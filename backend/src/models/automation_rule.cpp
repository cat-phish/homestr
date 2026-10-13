#include "homestr/models/automation_rule.hpp"

#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

namespace homestr {

std::string_view toString(TriggerType type) {
  switch (type) {
    case TriggerType::Schedule:
      return "SCHEDULE";
    case TriggerType::DeviceState:
      return "DEVICE_STATE";
    case TriggerType::Manual:
      return "MANUAL";
  }
  return "MANUAL";
}

TriggerType parseTriggerType(std::string_view text) {
  if (text == "SCHEDULE") {
    return TriggerType::Schedule;
  }
  if (text == "DEVICE_STATE") {
    return TriggerType::DeviceState;
  }
  return TriggerType::Manual;
}

AutomationRule::AutomationRule(std::string id, std::string name, TriggerType triggerType,
                               nlohmann::json triggerPayload)
    : id_(std::move(id)),
      name_(std::move(name)),
      triggerType_(triggerType),
      triggerPayload_(std::move(triggerPayload)) {}

void AutomationRule::addAction(RuleAction action) {
  actions_.push_back(std::move(action));
}

bool AutomationRule::evaluate(const TriggerEvent& event) const {
  if (!enabled_) {
    return false;
  }

  switch (triggerType_) {
    case TriggerType::Schedule:
      // The scheduler is the only source of schedule ticks; it has already
      // matched the cron expression against the clock before calling in.
      return event.kind == "schedule" && event.data.value("ruleId", std::string{}) == id_;

    case TriggerType::DeviceState: {
      if (event.kind != "device_state") {
        return false;
      }
      if (triggerPayload_.value("deviceId", std::string{}) != event.deviceId) {
        return false;
      }
      // An absent "equals" clause matches any state change of that device.
      if (!triggerPayload_.contains("equals")) {
        return true;
      }
      const std::string attribute = triggerPayload_.value("attribute", std::string{"status"});
      return event.data.contains(attribute) &&
             event.data.at(attribute) == triggerPayload_.at("equals");
    }

    case TriggerType::Manual:
      return event.kind == "manual" && event.data.value("ruleId", std::string{}) == id_;
  }

  return false;
}

std::vector<RuleAction> AutomationRule::execute() const {
  if (!enabled_) {
    return {};
  }
  return actions_;
}

nlohmann::json AutomationRule::toJson() const {
  nlohmann::json actions = nlohmann::json::array();
  for (const RuleAction& action : actions_) {
    actions.push_back(
        {{"deviceId", action.deviceId}, {"command", action.command}, {"params", action.params}});
  }

  return {{"id", id_},
          {"name", name_},
          {"enabled", enabled_},
          {"triggerType", toString(triggerType_)},
          {"trigger", triggerPayload_},
          {"actions", std::move(actions)}};
}

}  // namespace homestr
