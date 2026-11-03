#include "homestr/services/home_str.hpp"

#include <algorithm>
#include <iostream>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

namespace homestr {

HomeStr::HomeStr(std::unique_ptr<Database> database)
    : database_(std::move(database)), deviceManager_(*database_) {}

bool HomeStr::start(const std::string& householdId) {
  if (!database_->isConnected() && !database_->connect()) {
    return false;
  }
  const std::lock_guard<std::mutex> lock(rulesMutex_);
  const auto records = database_->listRules(householdId);
  if (!records) {
    return false;
  }
  std::vector<AutomationRule> restored;
  for (const auto& record : *records) {
    restored.push_back(record.toRule());
  }
  householdId_ = householdId;
  rules_ = std::move(restored);
  const std::size_t loaded = deviceManager_.loadHousehold(householdId);
  std::cout << "[hub] household " << householdId << ": " << loaded << " device(s) loaded; "
            << rules_.size() << " rule(s) loaded\n";
  return true;
}

std::vector<AutomationRule> HomeStr::rules() const {
  const std::lock_guard<std::mutex> lock(rulesMutex_);
  return rules_;
}

RuleWriteResult HomeStr::addRule(AutomationRule rule) {
  const std::lock_guard<std::mutex> lock(rulesMutex_);
  const bool known =
      std::any_of(rules_.begin(), rules_.end(),
                  [&rule](const AutomationRule& existing) { return existing.id() == rule.id(); });
  if (known) {
    return {RuleWriteStatus::Duplicate};
  }
  if (!database_->saveRule(RuleRecord::fromRule(rule, householdId_))) {
    return {RuleWriteStatus::StorageError};
  }
  rules_.push_back(std::move(rule));
  return {RuleWriteStatus::Saved};
}

RuleWriteResult HomeStr::setRuleEnabled(const std::string& ruleId, bool enabled) {
  const std::lock_guard<std::mutex> lock(rulesMutex_);
  const auto match =
      std::find_if(rules_.begin(), rules_.end(),
                   [&ruleId](const AutomationRule& rule) { return rule.id() == ruleId; });
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
  *match = std::move(updated);
  return {RuleWriteStatus::Saved};
}

std::vector<std::string> HomeStr::handleEvent(const TriggerEvent& event) {
  std::vector<std::string> fired;
  for (const AutomationRule& rule : rules()) {
    if (!rule.evaluate(event)) {
      continue;
    }
    for (const RuleAction& action : rule.execute()) {
      // actorId stays empty so the audit row is attributed to the rule, not a
      // user; the rule id goes into the payload.
      const CommandResult result =
          deviceManager_.sendCommand(action.deviceId, action.command, action.params, {});
      if (!result.ok) {
        std::cerr << "[rule " << rule.id() << "] " << action.command << " on " << action.deviceId
                  << " failed: " << result.error << '\n';
      }
    }
    fired.push_back(rule.id());
  }
  return fired;
}

nlohmann::json HomeStr::dashboard() const {
  const std::lock_guard<std::mutex> lock(rulesMutex_);
  nlohmann::json rules = nlohmann::json::array();
  for (const AutomationRule& rule : rules_) {
    rules.push_back(rule.toJson());
  }

  return {{"householdId", householdId_},
          {"devices", deviceManager_.snapshot()},
          {"rules", std::move(rules)}};
}

}  // namespace homestr
