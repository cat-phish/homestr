#include "homestr/services/home_str.hpp"

#include <memory>
#include <string>

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include "homestr/models/automation_rule.hpp"
#include "homestr/services/database.hpp"

namespace homestr {
namespace {

constexpr const char* HOUSEHOLD = "house-1";

AutomationRule makeRule(std::string id = "rule-1") {
  AutomationRule rule{std::move(id), "Lights out", TriggerType::Schedule, {{"cron", "0 22 * * *"}}};
  rule.addAction(RuleAction{"light-1", "turnOff", {}});
  return rule;
}

DeviceRecord makeLightRecord() {
  DeviceRecord record;
  record.id = "light-1";
  record.householdId = HOUSEHOLD;
  record.name = "Kitchen Ceiling";
  record.ipAddress = "192.168.1.40";
  record.deviceType = "light";
  record.status = "ONLINE";
  record.attributes = {{"brightness", 80}, {"color", "#FFAA00"}};
  return record;
}

class HomeStrTest : public ::testing::Test {
 protected:
  void SetUp() override {
    auto database = std::make_unique<InMemoryDatabase>();
    database_ = database.get();
    hub_ = std::make_unique<HomeStr>(std::move(database));
    ASSERT_TRUE(hub_->start(HOUSEHOLD));
  }

  InMemoryDatabase* database_ = nullptr;  // owned by hub_
  std::unique_ptr<HomeStr> hub_;
};

TEST_F(HomeStrTest, addRuleAcceptsANewRule) {
  EXPECT_TRUE(hub_->addRule(makeRule()));
  EXPECT_EQ(hub_->rules().size(), 1U);
}

// Regression: the rule list used to accept the same id repeatedly, so re-running
// the seed script stacked up duplicates and every one of them fired.
TEST_F(HomeStrTest, addRuleRejectsADuplicateId) {
  ASSERT_TRUE(hub_->addRule(makeRule()));

  EXPECT_FALSE(hub_->addRule(makeRule()));
  EXPECT_EQ(hub_->rules().size(), 1U);
}

TEST_F(HomeStrTest, addRuleAcceptsADifferentId) {
  ASSERT_TRUE(hub_->addRule(makeRule("rule-1")));

  EXPECT_TRUE(hub_->addRule(makeRule("rule-2")));
  EXPECT_EQ(hub_->rules().size(), 2U);
}

TEST_F(HomeStrTest, setRuleEnabledTogglesTheNamedRule) {
  ASSERT_TRUE(hub_->addRule(makeRule()));

  ASSERT_TRUE(hub_->setRuleEnabled("rule-1", false));
  EXPECT_FALSE(hub_->rules().front().enabled());

  ASSERT_TRUE(hub_->setRuleEnabled("rule-1", true));
  EXPECT_TRUE(hub_->rules().front().enabled());
}

TEST_F(HomeStrTest, setRuleEnabledReportsAnUnknownRule) {
  EXPECT_FALSE(hub_->setRuleEnabled("nope", false));
}

TEST_F(HomeStrTest, handleEventFiresAMatchingRuleAndDispatchesItsActions) {
  ASSERT_TRUE(hub_->devices().registerDevice(makeLightRecord()));
  ASSERT_TRUE(hub_->devices().findById("light-1")->turnOn());
  ASSERT_TRUE(hub_->addRule(makeRule()));

  TriggerEvent event;
  event.kind = "schedule";
  event.data = {{"ruleId", "rule-1"}};
  const std::vector<std::string> fired = hub_->handleEvent(event);

  ASSERT_EQ(fired.size(), 1U);
  EXPECT_EQ(fired.front(), "rule-1");
  EXPECT_FALSE(hub_->devices().findById("light-1")->isOn());
}

TEST_F(HomeStrTest, handleEventSkipsADisabledRule) {
  ASSERT_TRUE(hub_->devices().registerDevice(makeLightRecord()));
  ASSERT_TRUE(hub_->addRule(makeRule()));
  ASSERT_TRUE(hub_->setRuleEnabled("rule-1", false));

  TriggerEvent event;
  event.kind = "schedule";
  event.data = {{"ruleId", "rule-1"}};

  EXPECT_TRUE(hub_->handleEvent(event).empty());
}

TEST_F(HomeStrTest, dashboardReportsHouseholdDevicesAndRules) {
  ASSERT_TRUE(hub_->devices().registerDevice(makeLightRecord()));
  ASSERT_TRUE(hub_->addRule(makeRule()));

  const nlohmann::json payload = hub_->dashboard();

  EXPECT_EQ(payload.at("householdId"), HOUSEHOLD);
  ASSERT_TRUE(payload.at("devices").is_array());
  EXPECT_EQ(payload.at("devices").size(), 1U);
  ASSERT_TRUE(payload.at("rules").is_array());
  EXPECT_EQ(payload.at("rules").at(0).at("id"), "rule-1");
}

}  // namespace
}  // namespace homestr
