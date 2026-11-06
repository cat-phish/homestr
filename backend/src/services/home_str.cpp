#include "homestr/services/home_str.hpp"

#include <algorithm>
#include <iostream>
#include <regex>
#include <unordered_set>
#include <utility>

namespace homestr {

HomeStr::HomeStr(std::unique_ptr<Database> database, Clock clock)
    : clock_(std::move(clock)), database_(std::move(database)), deviceManager_(*database_) {
  deviceManager_.setStateObserver(
      [this](const nlohmann::json& before, const nlohmann::json& after) {
        const std::lock_guard<std::recursive_mutex> engine(engineMutex_);
        publish({{"type", "device_state"}, {"device", after}});
        handleEvent({"device_state", after.at("id").get<std::string>(), after, before});
      });
}

bool HomeStr::start(const std::string& householdId) {
  const std::lock_guard<std::recursive_mutex> engine(engineMutex_);
  if (!database_->isConnected() && !database_->connect()) {
    return false;
  }
  const auto records = database_->listRules(householdId);
  if (!records) {
    return false;
  }
  const std::lock_guard<std::mutex> lock(rulesMutex_);
  householdId_ = householdId;
  rules_.clear();
  nextRuns_.clear();
  for (const auto& record : *records) {
    rules_.push_back(record.toRule());
    if (record.enabled && record.triggerType == "SCHEDULE") {
      const auto next =
          Schedule::next(record.triggerPayload, std::max(clock_(), record.lastFiredAt.value_or(0)));
      if (next) {
        nextRuns_[record.id] = *next;
      }
    }
  }
  const auto loaded = deviceManager_.loadHousehold(householdId);
  std::cout << "[hub] household " << householdId << ": " << loaded << " device(s) loaded; "
            << rules_.size() << " rule(s) loaded\n";
  return true;
}

RenameResult HomeStr::renameDevice(const std::string& deviceId, std::string_view name,
                                   const std::string& actorId) {
  const RenameResult result = deviceManager_.renameDevice(deviceId, name, actorId);
  if (result == RenameResult::Renamed) {
    if (const auto state = deviceManager_.state(deviceId); state) {
      publish({{"type", "device_state"}, {"device", *state}});
    }
  }
  return result;
}

bool HomeStr::removeDevice(const std::string& deviceId) {
  const std::lock_guard<std::recursive_mutex> engine(engineMutex_);
  if (!deviceManager_.unregisterDevice(deviceId)) {
    return false;
  }
  {
    const std::lock_guard<std::mutex> lock(rulesMutex_);
    for (AutomationRule& rule : rules_) {
      RuleRecord record = RuleRecord::fromRule(rule, householdId_);
      const auto before = record.actions.size();
      std::erase_if(record.actions,
                    [&deviceId](const RuleAction& action) { return action.deviceId == deviceId; });
      if (record.actions.size() != before) {
        rule = record.toRule();
      }
    }
  }
  publishSnapshot();
  return true;
}

std::vector<AutomationRule> HomeStr::rules() const {
  const std::lock_guard<std::mutex> lock(rulesMutex_);
  return rules_;
}

std::string HomeStr::validateTrigger(const AutomationRule& rule) const {
  const auto& trigger = rule.triggerPayload();
  if (!trigger.is_object()) {
    return "trigger must be an object";
  }
  if (rule.triggerType() == TriggerType::Manual) {
    return {};
  }
  if (rule.triggerType() == TriggerType::Schedule) {
    return Schedule::valid(trigger) ? "" : "use a valid five-field cron schedule and IANA timezone";
  }
  if (!trigger.contains("deviceId") || !trigger.at("deviceId").is_string()) {
    return "select a registered trigger device";
  }
  const auto source = deviceManager_.state(trigger.at("deviceId").get<std::string>());
  if (!source) {
    return "trigger device is not registered";
  }
  if (!trigger.contains("attribute")) {
    return trigger.contains("equals") ? "select a property for the condition" : "";
  }
  if (!trigger.at("attribute").is_string()) {
    return "trigger attribute must be a string";
  }
  const std::string attribute = trigger.at("attribute").get<std::string>();
  const std::unordered_set<std::string> supported = {
      "powerOn", "status", "brightness", "color", "volume", "connected", "playbackState"};
  if (!supported.contains(attribute) || !source->contains(attribute)) {
    return "unsupported trigger property";
  }
  if (!trigger.contains("equals")) {
    return {};
  }
  const auto& expected = trigger.at("equals");
  if (attribute == "powerOn" || attribute == "connected") {
    return expected.is_boolean() ? "" : "condition must be true or false";
  }
  if (attribute == "brightness" || attribute == "volume") {
    return expected.is_number_integer() && expected >= 0 && expected <= 100
               ? ""
               : "condition must be a whole number from 0 to 100";
  }
  if (!expected.is_string()) {
    return "condition must be a string";
  }
  const auto value = expected.get<std::string>();
  if (attribute == "color") {
    return std::regex_match(value, std::regex{"^#[0-9a-fA-F]{6}$"})
               ? ""
               : "choose a valid condition color";
  }
  if (attribute == "status") {
    return value == "ONLINE" || value == "OFFLINE" || value == "ERROR" ? ""
                                                                       : "invalid device status";
  }
  return value == "PLAYING" || value == "PAUSED" || value == "STOPPED" ? ""
                                                                       : "invalid playback state";
}

RuleWriteResult HomeStr::addRule(AutomationRule rule) {
  const std::lock_guard<std::recursive_mutex> engine(engineMutex_);
  {
    const std::lock_guard<std::mutex> lock(rulesMutex_);
    if (std::any_of(rules_.begin(), rules_.end(),
                    [&](const auto& existing) { return existing.id() == rule.id(); })) {
      return {RuleWriteStatus::Duplicate};
    }
  }
  const auto error = validateTrigger(rule);
  if (!error.empty()) {
    return {RuleWriteStatus::InvalidTrigger, error};
  }
  {
    const std::lock_guard<std::mutex> lock(rulesMutex_);
    if (!database_->saveRule(RuleRecord::fromRule(rule, householdId_))) {
      return {RuleWriteStatus::StorageError};
    }
    if (rule.enabled() && rule.triggerType() == TriggerType::Schedule) {
      const auto next = Schedule::next(rule.triggerPayload(), clock_());
      if (next) {
        nextRuns_[rule.id()] = *next;
      }
    }
    rules_.push_back(std::move(rule));
  }
  publishSnapshot();
  return {RuleWriteStatus::Saved};
}

RuleWriteResult HomeStr::setRuleEnabled(const std::string& ruleId, bool enabled) {
  const std::lock_guard<std::recursive_mutex> engine(engineMutex_);
  {
    const std::lock_guard<std::mutex> lock(rulesMutex_);
    const auto match = std::find_if(rules_.begin(), rules_.end(),
                                    [&](const auto& rule) { return rule.id() == ruleId; });
    if (match == rules_.end()) {
      return {RuleWriteStatus::NotFound};
    }
    AutomationRule updated = *match;
    if (enabled) {
      updated.enable();
    } else {
      updated.disable();
    }
    if (!database_->saveRule(RuleRecord::fromRule(updated, householdId_))) {
      return {RuleWriteStatus::StorageError};
    }
    nextRuns_.erase(ruleId);
    if (enabled && updated.triggerType() == TriggerType::Schedule) {
      const auto next = Schedule::next(updated.triggerPayload(),
                                       std::max(clock_(), updated.lastFiredAt().value_or(0)));
      if (next) {
        nextRuns_[ruleId] = *next;
      }
    }
    *match = std::move(updated);
  }
  publishSnapshot();
  return {RuleWriteStatus::Saved};
}

CommandResult HomeStr::sendCommand(const std::string& deviceId, const std::string& command,
                                   const nlohmann::json& params, const std::string& actorId) {
  const std::lock_guard<std::recursive_mutex> engine(engineMutex_);
  return deviceManager_.sendCommand(deviceId, command, params, actorId);
}

std::vector<std::string> HomeStr::handleEvent(const TriggerEvent& event) {
  const std::lock_guard<std::recursive_mutex> engine(engineMutex_);
  eventQueue_.push_back(event);
  if (dispatching_) {
    return {};
  }
  dispatching_ = true;

  struct Reset {
    bool& active;
    std::deque<TriggerEvent>& queue;

    ~Reset() {
      active = false;
      queue.clear();
    }
  } reset{dispatching_, eventQueue_};

  std::vector<std::string> fired;
  std::unordered_set<std::string> visited;
  std::size_t actions = 0;
  while (!eventQueue_.empty()) {
    const auto current = eventQueue_.front();
    eventQueue_.pop_front();
    for (const auto& rule : rules()) {
      if (visited.contains(rule.id())) {
        continue;
      }
      bool matches = false;
      try {
        matches = rule.evaluate(current);
      } catch (const nlohmann::json::exception&) {
        std::cerr << "[rule] invalid stored trigger\n";
      }
      if (!matches) {
        continue;
      }
      if (visited.size() >= 64) {
        std::cerr << "[rule] chain rule limit reached\n";
        return fired;
      }
      visited.insert(rule.id());
      fired.push_back(rule.id());
      for (const auto& action : rule.execute()) {
        if (++actions > 256) {
          std::cerr << "[rule] chain action limit reached\n";
          return fired;
        }
        const auto result = deviceManager_.sendCommand(action.deviceId, action.command,
                                                       action.params, rule.id(), "RULE");
        if (!result.ok) {
          std::cerr << "[rule " << rule.id() << "] " << action.command << " on " << action.deviceId
                    << " failed: " << result.error << '\n';
        }
      }
    }
  }
  return fired;
}

void HomeStr::tickSchedules(std::int64_t now) {
  const std::lock_guard<std::recursive_mutex> engine(engineMutex_);
  const std::int64_t minute = now - now % 60;
  std::vector<std::string> due;
  bool changed = false;
  {
    const std::lock_guard<std::mutex> lock(rulesMutex_);
    for (auto& rule : rules_) {
      const auto planned = nextRuns_.find(rule.id());
      if (!rule.enabled() || rule.triggerType() != TriggerType::Schedule ||
          planned == nextRuns_.end() || planned->second > minute) {
        continue;
      }
      const auto expected = planned->second;
      nextRuns_.erase(planned);
      const auto next =
          Schedule::next(rule.triggerPayload(), std::max(now, rule.lastFiredAt().value_or(0)));
      if (next) {
        nextRuns_[rule.id()] = *next;
      }
      changed = true;
      if (expected == minute && Schedule::due(rule.triggerPayload(), minute) &&
          database_->claimScheduledRun(householdId_, rule.id(), minute)) {
        rule.setLastFiredAt(minute);
        due.push_back(rule.id());
      }
    }
  }
  for (const auto& id : due) {
    handleEvent({"schedule", "", {{"ruleId", id}}});
  }
  if (changed) {
    publishSnapshot();
  }
}

void HomeStr::setEventSink(EventSink sink) {
  const std::lock_guard<std::mutex> lock(sinkMutex_);
  eventSink_ = std::move(sink);
}

void HomeStr::publish(const nlohmann::json& event) {
  EventSink sink;
  {
    const std::lock_guard<std::mutex> lock(sinkMutex_);
    sink = eventSink_;
  }
  if (sink) {
    sink(event);
  }
}

void HomeStr::publishSnapshot() {
  publish({{"type", "snapshot"}, {"payload", dashboard()}});
}

nlohmann::json HomeStr::ruleJsonLocked(const AutomationRule& rule) const {
  auto result = rule.toJson();
  if (rule.triggerType() == TriggerType::Schedule) {
    const auto next = nextRuns_.find(rule.id());
    result["nextRunAt"] = rule.enabled() && next != nextRuns_.end()
                              ? nlohmann::json(Schedule::format(next->second))
                              : nlohmann::json(nullptr);
  }
  return result;
}

nlohmann::json HomeStr::ruleJson(const AutomationRule& rule) const {
  const std::lock_guard<std::mutex> lock(rulesMutex_);
  return ruleJsonLocked(rule);
}

nlohmann::json HomeStr::dashboard() const {
  const std::lock_guard<std::mutex> lock(rulesMutex_);
  nlohmann::json rules = nlohmann::json::array();
  for (const auto& rule : rules_) {
    rules.push_back(ruleJsonLocked(rule));
  }
  return {{"householdId", householdId_},
          {"devices", deviceManager_.snapshot()},
          {"rules", std::move(rules)}};
}

}  // namespace homestr
