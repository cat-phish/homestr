#include "homestr/models/automation_rule.hpp"

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

namespace homestr {
namespace {

AutomationRule makeDeviceStateRule() {
  AutomationRule rule{"rule-1",
                      "Porch light on motion",
                      TriggerType::DeviceState,
                      {{"deviceId", "sensor-1"}, {"attribute", "status"}, {"equals", "ONLINE"}}};
  rule.addAction(RuleAction{"light-1", "turnOn", {}});
  return rule;
}

TriggerEvent makeDeviceStateEvent(const char* status) {
  TriggerEvent event;
  event.kind = "device_state";
  event.deviceId = "sensor-1";
  event.data = {{"status", status}};
  return event;
}

TEST(AutomationRuleTest, EvaluateMatchesTriggerCondition) {
  EXPECT_TRUE(makeDeviceStateRule().evaluate(makeDeviceStateEvent("ONLINE")));
}

TEST(AutomationRuleTest, EvaluateRejectsNonMatchingValue) {
  EXPECT_FALSE(makeDeviceStateRule().evaluate(makeDeviceStateEvent("OFFLINE")));
}

TEST(AutomationRuleTest, EvaluateRejectsOtherDevice) {
  TriggerEvent event = makeDeviceStateEvent("ONLINE");
  event.deviceId = "sensor-2";

  EXPECT_FALSE(makeDeviceStateRule().evaluate(event));
}

TEST(AutomationRuleTest, EvaluateRejectsWrongEventKind) {
  TriggerEvent event = makeDeviceStateEvent("ONLINE");
  event.kind = "schedule";

  EXPECT_FALSE(makeDeviceStateRule().evaluate(event));
}

TEST(AutomationRuleTest, DisabledRuleNeverMatches) {
  AutomationRule rule = makeDeviceStateRule();
  rule.disable();

  EXPECT_FALSE(rule.enabled());
  EXPECT_FALSE(rule.evaluate(makeDeviceStateEvent("ONLINE")));
  EXPECT_TRUE(rule.execute().empty());
}

TEST(AutomationRuleTest, ReEnablingRestoresMatching) {
  AutomationRule rule = makeDeviceStateRule();
  rule.disable();
  rule.enable();

  EXPECT_TRUE(rule.evaluate(makeDeviceStateEvent("ONLINE")));
}

TEST(AutomationRuleTest, TriggerWithoutEqualsMatchesAnyStateChange) {
  AutomationRule rule{"rule-2", "Any change", TriggerType::DeviceState, {{"deviceId", "sensor-1"}}};

  EXPECT_TRUE(rule.evaluate(makeDeviceStateEvent("ERROR")));
}

TEST(AutomationRuleTest, ScheduleRuleMatchesOnlyItsOwnTick) {
  AutomationRule rule{"rule-3", "Nightly", TriggerType::Schedule, {{"cron", "0 22 * * *"}}};
  TriggerEvent own;
  own.kind = "schedule";
  own.data = {{"ruleId", "rule-3"}};
  TriggerEvent other;
  other.kind = "schedule";
  other.data = {{"ruleId", "rule-9"}};

  EXPECT_TRUE(rule.evaluate(own));
  EXPECT_FALSE(rule.evaluate(other));
}

TEST(AutomationRuleTest, ExecuteReturnsActionsInInsertionOrder) {
  AutomationRule rule = makeDeviceStateRule();
  rule.addAction(RuleAction{"speaker-1", "setVolume", {{"level", 10}}});

  const std::vector<RuleAction> actions = rule.execute();

  ASSERT_EQ(actions.size(), 2U);
  EXPECT_EQ(actions[0].command, "turnOn");
  EXPECT_EQ(actions[1].command, "setVolume");
  EXPECT_EQ(actions[1].params.at("level"), 10);
}

TEST(AutomationRuleTest, ToJsonMatchesApiContract) {
  const nlohmann::json payload = makeDeviceStateRule().toJson();

  EXPECT_EQ(payload.at("id"), "rule-1");
  EXPECT_EQ(payload.at("enabled"), true);
  EXPECT_EQ(payload.at("triggerType"), "DEVICE_STATE");
  ASSERT_TRUE(payload.at("actions").is_array());
  EXPECT_EQ(payload.at("actions").at(0).at("deviceId"), "light-1");
}

TEST(AutomationRuleTest, ParseTriggerTypeRoundTrips) {
  EXPECT_EQ(parseTriggerType("SCHEDULE"), TriggerType::Schedule);
  EXPECT_EQ(parseTriggerType("DEVICE_STATE"), TriggerType::DeviceState);
  EXPECT_EQ(parseTriggerType("MANUAL"), TriggerType::Manual);
  // Unknown values fall back to MANUAL rather than throwing.
  EXPECT_EQ(parseTriggerType("nonsense"), TriggerType::Manual);
}

}  // namespace
}  // namespace homestr
