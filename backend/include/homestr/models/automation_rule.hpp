#pragma once

#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

namespace homestr {

enum class TriggerType { Schedule, DeviceState, Manual };

std::string_view toString(TriggerType type);

TriggerType parseTriggerType(std::string_view text);

// One step of AutomationRule::execute(), mapped to a row of
// `automation_actions`.
struct RuleAction {
  std::string deviceId;
  std::string command;
  nlohmann::json params = nlohmann::json::object();
};

// An event offered to evaluate(): either a clock tick or a device state change.
struct TriggerEvent {
  std::string kind;  // "schedule" | "device_state"
  std::string deviceId;
  nlohmann::json data = nlohmann::json::object();
};

// `AutomationRule` from the class diagram.
class AutomationRule {
 public:
  AutomationRule(std::string id, std::string name, TriggerType triggerType,
                 nlohmann::json triggerPayload);

  const std::string& id() const noexcept { return id_; }

  const std::string& name() const noexcept { return name_; }

  bool enabled() const noexcept { return enabled_; }

  TriggerType triggerType() const noexcept { return triggerType_; }

  const nlohmann::json& triggerPayload() const noexcept { return triggerPayload_; }

  const std::vector<RuleAction>& actions() const noexcept { return actions_; }

  void enable() noexcept { enabled_ = true; }

  void disable() noexcept { enabled_ = false; }

  void addAction(RuleAction action);

  // True when `event` satisfies the trigger. A disabled rule never matches, so
  // callers can evaluate every rule unconditionally.
  bool evaluate(const TriggerEvent& event) const;

  // Commands to dispatch when the rule fires. Empty for a disabled rule.
  std::vector<RuleAction> execute() const;

  nlohmann::json toJson() const;

 private:
  std::string id_;
  std::string name_;
  bool enabled_ = true;
  TriggerType triggerType_;
  nlohmann::json triggerPayload_;
  std::vector<RuleAction> actions_;
};

}  // namespace homestr
