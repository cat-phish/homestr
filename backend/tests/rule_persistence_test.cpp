#include <memory>
#include <optional>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include "homestr/services/home_str.hpp"

namespace homestr {
namespace {

AutomationRule persistedRule() {
  AutomationRule rule{"rule-1", "Bedtime", TriggerType::Manual, {{"note", "keep this payload"}}};
  rule.addAction({"light-1", "setBrightness", {{"level", 20}}});
  rule.addAction({"light-1", "turnOff", nlohmann::json::object()});
  return rule;
}

TEST(RuleDatabaseTest, RoundTripsFullConfigurationAndFiltersByHousehold) {
  InMemoryDatabase database;
  database.connect();
  auto rule = persistedRule();
  rule.disable();
  ASSERT_TRUE(database.saveRule(RuleRecord::fromRule(rule, "house-1")));
  const auto records = database.listRules("house-1");
  ASSERT_TRUE(records);
  ASSERT_EQ(records->size(), 1U);
  EXPECT_EQ(records->front().toRule().toJson(), rule.toJson());
  const auto other = database.listRules("house-2");
  ASSERT_TRUE(other);
  EXPECT_TRUE(other->empty());
}

TEST(RuleDatabaseTest, UpdatesReplaceActionsWithoutDuplicatesIncludingEmptyLists) {
  InMemoryDatabase database;
  database.connect();
  ASSERT_TRUE(database.saveRule(RuleRecord::fromRule(persistedRule(), "house-1")));
  AutomationRule replacement{"rule-1", "Updated", TriggerType::Schedule, {{"cron", "0 23 * * *"}}};
  replacement.disable();
  ASSERT_TRUE(database.saveRule(RuleRecord::fromRule(replacement, "house-1")));
  const auto records = database.listRules("house-1");
  ASSERT_TRUE(records);
  ASSERT_EQ(records->size(), 1U);
  EXPECT_EQ(records->front().toRule().toJson(), replacement.toJson());
}

TEST(RuleDatabaseTest, CannotOverwriteOrDeleteAnotherHouseholdsRule) {
  InMemoryDatabase database;
  database.connect();
  ASSERT_TRUE(database.saveRule(RuleRecord::fromRule(persistedRule(), "house-1")));
  EXPECT_FALSE(database.saveRule(RuleRecord::fromRule(persistedRule(), "house-2")));
  EXPECT_FALSE(database.deleteRule("house-2", "rule-1"));
  ASSERT_TRUE(database.listRules("house-1"));
  EXPECT_EQ(database.listRules("house-1")->size(), 1U);
  EXPECT_TRUE(database.deleteRule("house-1", "rule-1"));
  EXPECT_TRUE(database.listRules("house-1")->empty());
  EXPECT_FALSE(database.deleteRule("house-1", "rule-1"));
}

class RulePersistenceTest : public ::testing::Test {
 protected:
  class TestDatabase : public InMemoryDatabase {
   public:
    bool failWrites = false;
    bool failReads = false;

    bool saveRule(const RuleRecord& record) override {
      return !failWrites && InMemoryDatabase::saveRule(record);
    }

    std::optional<std::vector<RuleRecord>> listRules(const std::string& household) override {
      return failReads ? std::nullopt : InMemoryDatabase::listRules(household);
    }
  };

  void SetUp() override {
    auto database = std::make_unique<TestDatabase>();
    database_ = database.get();
    hub_ = std::make_unique<HomeStr>(std::move(database));
    ASSERT_TRUE(hub_->start("house-1"));
    DeviceRecord device;
    device.id = "light-1";
    device.householdId = "house-1";
    device.name = "Kitchen";
    device.ipAddress = "127.0.0.1";
    device.deviceType = "light";
    device.status = "ONLINE";
    ASSERT_TRUE(hub_->devices().registerDevice(device));
  }

  TestDatabase* database_ = nullptr;
  std::unique_ptr<HomeStr> hub_;
};

TEST_F(RulePersistenceTest, RestoresDisabledRuleAndOrderedActionsOnRepeatedStartup) {
  ASSERT_TRUE(hub_->addRule(persistedRule()));
  ASSERT_TRUE(hub_->setRuleEnabled("rule-1", false));
  const auto expected = hub_->rules().front().toJson();
  ASSERT_TRUE(hub_->start("house-1"));
  ASSERT_TRUE(hub_->start("house-1"));
  ASSERT_EQ(hub_->rules().size(), 1U);
  EXPECT_EQ(hub_->rules().front().toJson(), expected);
  TriggerEvent event{"manual", "", {{"ruleId", "rule-1"}}};
  EXPECT_TRUE(hub_->handleEvent(event).empty());
  ASSERT_TRUE(hub_->setRuleEnabled("rule-1", true));
  ASSERT_TRUE(hub_->start("house-1"));
  EXPECT_TRUE(hub_->rules().front().enabled());
  EXPECT_EQ(hub_->handleEvent(event).size(), 1U);
  EXPECT_FALSE(hub_->devices().findById("light-1")->isOn());
  EXPECT_EQ(hub_->devices().findById("light-1")->toJson().at("brightness"), 20);
}

TEST_F(RulePersistenceTest, DoesNotPublishACreateThatCouldNotBeSaved) {
  database_->failWrites = true;
  EXPECT_EQ(hub_->addRule(persistedRule()).status, RuleWriteStatus::StorageError);
  EXPECT_TRUE(hub_->rules().empty());
  EXPECT_TRUE(database_->listRules("house-1")->empty());
}

TEST_F(RulePersistenceTest, DoesNotChangeEnabledStateWhenSavingFails) {
  ASSERT_TRUE(hub_->addRule(persistedRule()));
  database_->failWrites = true;
  EXPECT_EQ(hub_->setRuleEnabled("rule-1", false).status, RuleWriteStatus::StorageError);
  EXPECT_TRUE(hub_->rules().front().enabled());
  EXPECT_TRUE(database_->listRules("house-1")->front().enabled);
  EXPECT_EQ(hub_->setRuleEnabled("unknown", false).status, RuleWriteStatus::NotFound);
}

TEST_F(RulePersistenceTest, DuplicateCreationDoesNotOverwriteSavedConfiguration) {
  ASSERT_TRUE(hub_->addRule(persistedRule()));
  auto duplicate = persistedRule();
  duplicate.disable();
  EXPECT_EQ(hub_->addRule(duplicate).status, RuleWriteStatus::Duplicate);
  ASSERT_TRUE(hub_->start("house-1"));
  EXPECT_TRUE(hub_->rules().front().enabled());
}

TEST_F(RulePersistenceTest, StartupFailsInsteadOfPublishingAnEmptyListOnReadFailure) {
  ASSERT_TRUE(hub_->addRule(persistedRule()));
  database_->failReads = true;
  EXPECT_FALSE(hub_->start("house-1"));
  EXPECT_EQ(hub_->rules().size(), 1U);
}

TEST_F(RulePersistenceTest, SwitchingHouseholdReplacesTheRulesSnapshot) {
  ASSERT_TRUE(hub_->addRule(persistedRule()));
  ASSERT_TRUE(hub_->start("house-2"));
  EXPECT_TRUE(hub_->rules().empty());
  ASSERT_TRUE(hub_->start("house-1"));
  EXPECT_EQ(hub_->rules().size(), 1U);
}

}  // namespace
}  // namespace homestr
