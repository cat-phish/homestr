#include <memory>

#include <gtest/gtest.h>

#include "homestr/services/home_str.hpp"
#include "homestr/services/schedule.hpp"

namespace homestr {
namespace {

TEST(ScheduleTest, ValidatesCronAndTimezoneWithoutChangingProcessTimezone) {
  EXPECT_TRUE(Schedule::valid({{"cron", "0 22 * * *"}, {"timezone", "America/Los_Angeles"}}));
  EXPECT_FALSE(Schedule::valid({{"cron", "bad"}}));
  EXPECT_FALSE(Schedule::valid({{"cron", "256 * * * *"}}));
  EXPECT_FALSE(Schedule::valid({{"cron", "*/0 * * * *"}}));
  EXPECT_FALSE(Schedule::valid({{"cron", "0 22 * * *"}, {"timezone", "Not/AZone"}}));
}

TEST(ScheduleTest, ConvertsLocalSchedulesAndHandlesDaylightSavingGapsAndRepeats) {
  const nlohmann::json gap = {{"cron", "30 2 * * *"}, {"timezone", "America/Los_Angeles"}};
  // March 8, 2026 has no 02:30 in Los Angeles: next is March 9 09:30 UTC.
  EXPECT_EQ(Schedule::next(gap, 1772956800), 1773048600);
  const nlohmann::json repeat = {{"cron", "30 1 * * *"}, {"timezone", "America/Los_Angeles"}};
  EXPECT_TRUE(Schedule::due(repeat, 1793521800));   // Nov 1 08:30 UTC, first 01:30
  EXPECT_FALSE(Schedule::due(repeat, 1793525400));  // Nov 1 09:30 UTC, repeated 01:30
  EXPECT_EQ(Schedule::next(repeat, 1793521800), 1793611800);
}

class TriggerEngineTest : public ::testing::Test {
 protected:
  void SetUp() override {
    auto database = std::make_unique<InMemoryDatabase>();
    database_ = database.get();
    hub_ = std::make_unique<HomeStr>(std::move(database), [this] { return now_; });
    ASSERT_TRUE(hub_->start("home"));
    for (const auto* id : {"a", "b"}) {
      DeviceRecord device;
      device.id = id;
      device.householdId = "home";
      device.name = id;
      device.ipAddress = "127.0.0.1";
      device.deviceType = "light";
      device.status = "ONLINE";
      ASSERT_TRUE(hub_->devices().registerDevice(device));
    }
    hub_->setEventSink([this](const nlohmann::json& event) { events_.push_back(event); });
  }

  AutomationRule stateRule(std::string id, std::string source, bool expected, std::string target,
                           std::string command) {
    AutomationRule rule{std::move(id),
                        "State rule",
                        TriggerType::DeviceState,
                        {{"deviceId", source}, {"attribute", "powerOn"}, {"equals", expected}}};
    rule.addAction({std::move(target), std::move(command), nlohmann::json::object()});
    return rule;
  }

  std::int64_t now_ = 1800000000;  // divisible by 60
  InMemoryDatabase* database_ = nullptr;
  std::unique_ptr<HomeStr> hub_;
  std::vector<nlohmann::json> events_;
};

TEST_F(TriggerEngineTest, StateTransitionsFireAndNoOpsOrUnrelatedUpdatesDoNot) {
  ASSERT_TRUE(hub_->addRule(stateRule("r", "a", true, "b", "turnOn")));
  ASSERT_TRUE(hub_->sendCommand("a", "turnOn", {}, "user").ok);
  EXPECT_TRUE(hub_->devices().state("b")->at("powerOn").get<bool>());
  hub_->sendCommand("b", "turnOff", {}, "user");
  hub_->sendCommand("a", "turnOn", {}, "user");
  hub_->sendCommand("a", "setBrightness", {{"level", 10}}, "user");
  EXPECT_FALSE(hub_->devices().state("b")->at("powerOn").get<bool>());
  hub_->sendCommand("a", "turnOff", {}, "user");
  hub_->sendCommand("a", "turnOn", {}, "user");
  EXPECT_TRUE(hub_->devices().state("b")->at("powerOn").get<bool>());
}

TEST_F(TriggerEngineTest, DisabledAndFailedCommandsDoNotFireRules) {
  ASSERT_TRUE(hub_->addRule(stateRule("r", "a", true, "b", "turnOn")));
  ASSERT_TRUE(hub_->setRuleEnabled("r", false));
  hub_->sendCommand("a", "turnOn", {}, "user");
  EXPECT_FALSE(hub_->devices().state("b")->at("powerOn").get<bool>());
  EXPECT_FALSE(hub_->sendCommand("a", "unsupported", {}, "user").ok);
}

TEST_F(TriggerEngineTest, CircularChainsTerminateAndIndependentChangesCanFireAgain) {
  ASSERT_TRUE(hub_->addRule(stateRule("on", "a", true, "a", "turnOff")));
  ASSERT_TRUE(hub_->addRule(stateRule("off", "a", false, "a", "turnOn")));
  hub_->sendCommand("a", "turnOn", {}, "user");
  EXPECT_EQ(database_->event_count(), 3U);
  hub_->sendCommand("a", "turnOff", {}, "user");
  EXPECT_EQ(database_->event_count(), 6U);
}

TEST_F(TriggerEngineTest, SchedulesFireOncePersistTheirClaimAndSkipMissedMinutes) {
  AutomationRule rule{"scheduled", "Every minute", TriggerType::Schedule, {{"cron", "* * * * *"}}};
  rule.addAction({"a", "turnOn", nlohmann::json::object()});
  ASSERT_TRUE(hub_->addRule(rule));
  EXPECT_TRUE(hub_->ruleJson(rule).at("nextRunAt").is_string());
  now_ += 60;
  hub_->tickSchedules(now_);
  EXPECT_TRUE(hub_->devices().state("a")->at("powerOn").get<bool>());
  EXPECT_EQ(database_->event_count(), 1U);
  hub_->tickSchedules(now_ + 1);
  EXPECT_EQ(database_->event_count(), 1U);
  ASSERT_TRUE(hub_->start("home"));
  hub_->tickSchedules(now_ + 2);
  EXPECT_EQ(database_->event_count(), 1U);
  EXPECT_EQ(database_->listRules("home")->front().lastFiredAt, now_);
  now_ += 180;
  hub_->tickSchedules(now_);
  EXPECT_EQ(database_->event_count(), 1U);  // skip missed planned minute
  now_ += 60;
  hub_->tickSchedules(now_);
  EXPECT_EQ(database_->event_count(), 2U);
  ASSERT_TRUE(hub_->setRuleEnabled("scheduled", false));
  EXPECT_TRUE(hub_->ruleJson(hub_->rules().front()).at("nextRunAt").is_null());
}

TEST_F(TriggerEngineTest, CapsActionsAndRulesInAChain) {
  AutomationRule manual{"many-actions", "Bounded", TriggerType::Manual, nlohmann::json::object()};
  for (int count = 0; count < 300; ++count) {
    manual.addAction({"b", "turnOn", nlohmann::json::object()});
  }
  ASSERT_TRUE(hub_->addRule(manual));
  hub_->handleEvent({"manual", "", {{"ruleId", "many-actions"}}});
  EXPECT_EQ(database_->event_count(), 256U);
  for (int count = 0; count < 65; ++count) {
    ASSERT_TRUE(
        hub_->addRule(stateRule("limit-" + std::to_string(count), "a", true, "b", "turnOff")));
  }
  hub_->sendCommand("a", "turnOn", {}, "user");
  EXPECT_EQ(database_->event_count(), 256U + 1U + 64U);
}

TEST_F(TriggerEngineTest, ScheduleClaimsFailClosedAndCannotBeClaimedTwice) {
  auto database = std::make_unique<InMemoryDatabase>();
  database->connect();
  const auto record = RuleRecord::fromRule(
      AutomationRule{"r", "Schedule", TriggerType::Schedule, {{"cron", "* * * * *"}}}, "home");
  ASSERT_TRUE(database->saveRule(record));
  EXPECT_TRUE(database->claimScheduledRun("home", "r", now_));
  EXPECT_FALSE(database->claimScheduledRun("home", "r", now_));
  EXPECT_FALSE(database->claimScheduledRun("other", "r", now_ + 60));
  EXPECT_TRUE(database->saveRule(record));  // saving configuration preserves the claim
  EXPECT_FALSE(database->claimScheduledRun("home", "r", now_));
}

TEST_F(TriggerEngineTest, RejectsInvalidTriggersAndTypedComparisons) {
  AutomationRule schedule{"bad", "Invalid", TriggerType::Schedule, {{"cron", "nope"}}};
  EXPECT_EQ(hub_->addRule(schedule).status, RuleWriteStatus::InvalidTrigger);
  AutomationRule state{"bad",
                       "Invalid",
                       TriggerType::DeviceState,
                       {{"deviceId", "a"}, {"attribute", "powerOn"}, {"equals", "true"}}};
  EXPECT_EQ(hub_->addRule(state).status, RuleWriteStatus::InvalidTrigger);
}

}  // namespace
}  // namespace homestr
