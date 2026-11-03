#pragma once

#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "homestr/models/automation_rule.hpp"
#include "homestr/services/database.hpp"
#include "homestr/services/device_manager.hpp"

namespace homestr {

enum class RuleWriteStatus { Saved, Duplicate, NotFound, StorageError };

struct RuleWriteResult {
  RuleWriteStatus status;

  explicit operator bool() const noexcept { return status == RuleWriteStatus::Saved; }
};

// `HomeStr` central controller: owns the Database and the DeviceManager and
// coordinates the automation rules between them. The controllers hold a
// reference to this object and nothing else.
class HomeStr {
 public:
  explicit HomeStr(std::unique_ptr<Database> database);

  // Connects the database and loads the household's devices and rules.
  bool start(const std::string& householdId);

  DeviceManager& devices() noexcept { return deviceManager_; }

  Database& database() noexcept { return *database_; }

  std::vector<AutomationRule> rules() const;

  // Persist before publishing changes; distinguish conflicts from storage failures.
  RuleWriteResult addRule(AutomationRule rule);

  RuleWriteResult setRuleEnabled(const std::string& ruleId, bool enabled);

  // Evaluates every rule against `event` and dispatches the actions of those
  // that match. Returns the ids of the rules that fired.
  std::vector<std::string> handleEvent(const TriggerEvent& event);

  // Payload of GET /dashboard: devices plus rule summaries.
  nlohmann::json dashboard() const;

 private:
  std::unique_ptr<Database> database_;
  DeviceManager deviceManager_;
  mutable std::mutex rulesMutex_;
  std::vector<AutomationRule> rules_;
  std::string householdId_;
};

}  // namespace homestr
