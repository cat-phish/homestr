#pragma once

#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include <nlohmann/json.hpp>

#include "homestr/models/automation_rule.hpp"
#include "homestr/services/database.hpp"
#include "homestr/services/device_manager.hpp"
#include "homestr/services/schedule.hpp"

namespace homestr {

enum class RuleWriteStatus { Saved, Duplicate, NotFound, StorageError, InvalidTrigger };

struct RuleWriteResult {
  RuleWriteStatus status;
  std::string error = {};

  explicit operator bool() const noexcept { return status == RuleWriteStatus::Saved; }
};

// `HomeStr` central controller: owns the Database and the DeviceManager and
// coordinates the automation rules between them. The controllers hold a
// reference to this object and nothing else.
class HomeStr {
 public:
  using Clock = std::function<std::int64_t()>;
  using EventSink = std::function<void(const nlohmann::json&)>;
  explicit HomeStr(std::unique_ptr<Database> database, Clock clock = Schedule::now);

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

  CommandResult sendCommand(const std::string& deviceId, const std::string& command,
                            const nlohmann::json& params, const std::string& actorId);
  void tickSchedules(std::int64_t now);
  void setEventSink(EventSink sink);
  nlohmann::json ruleJson(const AutomationRule& rule) const;

  // Payload of GET /dashboard: devices plus rule summaries.
  nlohmann::json dashboard() const;

 private:
  std::string validateTrigger(const AutomationRule& rule) const;
  nlohmann::json ruleJsonLocked(const AutomationRule& rule) const;
  void publish(const nlohmann::json& event);
  void publishSnapshot();
  Clock clock_;
  mutable std::mutex sinkMutex_;
  EventSink eventSink_;
  std::recursive_mutex engineMutex_;
  bool dispatching_ = false;
  std::deque<TriggerEvent> eventQueue_;
  std::unordered_map<std::string, std::int64_t> nextRuns_;
  std::unique_ptr<Database> database_;
  DeviceManager deviceManager_;
  mutable std::mutex rulesMutex_;
  std::vector<AutomationRule> rules_;
  std::string householdId_;
};

}  // namespace homestr
