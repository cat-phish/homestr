#include "homestr/services/device_manager.hpp"

#include <string>

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include "homestr/models/light.hpp"
#include "homestr/models/smart_speaker.hpp"
#include "homestr/services/database.hpp"

namespace homestr {
namespace {

DeviceRecord makeLightRecord(std::string id = "light-1") {
  DeviceRecord record;
  record.id = std::move(id);
  record.householdId = "house-1";
  record.name = "Kitchen Ceiling";
  record.ipAddress = "192.168.1.40";
  record.deviceType = "light";
  record.status = "ONLINE";
  record.attributes = {{"brightness", 80}, {"color", "#FFAA00"}};
  return record;
}

DeviceRecord makeSpeakerRecord() {
  DeviceRecord record;
  record.id = "speaker-1";
  record.householdId = "house-1";
  record.name = "Living Room Speaker";
  record.ipAddress = "192.168.1.41";
  record.deviceType = "speaker";
  record.status = "ONLINE";
  record.attributes = {{"connected", true}, {"volume", 25}};
  return record;
}

class DeviceManagerTest : public ::testing::Test {
 protected:
  void SetUp() override { database_.connect(); }

  InMemoryDatabase database_;
  DeviceManager manager_{database_};
};

TEST_F(DeviceManagerTest, RegisterAddsDeviceAndPersistsIt) {
  ASSERT_TRUE(manager_.registerDevice(makeLightRecord()));

  EXPECT_EQ(manager_.size(), 1U);
  EXPECT_TRUE(database_.findDevice("light-1").has_value());
}

TEST_F(DeviceManagerTest, RegisterRejectsDuplicateId) {
  ASSERT_TRUE(manager_.registerDevice(makeLightRecord()));

  EXPECT_FALSE(manager_.registerDevice(makeLightRecord()));
  EXPECT_EQ(manager_.size(), 1U);
}

TEST_F(DeviceManagerTest, MaterialisesLightSubclassWithItsAttributes) {
  ASSERT_TRUE(manager_.registerDevice(makeLightRecord()));

  auto* light = dynamic_cast<Light*>(manager_.findById("light-1"));
  ASSERT_NE(light, nullptr);
  EXPECT_EQ(light->brightness(), 80);
  EXPECT_EQ(light->color(), "#FFAA00");
}

TEST_F(DeviceManagerTest, MaterialisesSpeakerSubclassWithItsAttributes) {
  ASSERT_TRUE(manager_.registerDevice(makeSpeakerRecord()));

  auto* speaker = dynamic_cast<SmartSpeaker*>(manager_.findById("speaker-1"));
  ASSERT_NE(speaker, nullptr);
  EXPECT_TRUE(speaker->connected());
  EXPECT_EQ(speaker->volume(), 25);
}

TEST_F(DeviceManagerTest, SendCommandAppliesStateAndAudits) {
  ASSERT_TRUE(manager_.registerDevice(makeLightRecord()));

  const CommandResult result =
      manager_.sendCommand("light-1", "setBrightness", {{"level", 42}}, "user-1");

  ASSERT_TRUE(result.ok) << result.error;
  EXPECT_EQ(dynamic_cast<Light*>(manager_.findById("light-1"))->brightness(), 42);
  EXPECT_EQ(database_.findDevice("light-1")->attributes.at("brightness"), 42);
  EXPECT_EQ(database_.event_count(), 1U);
}

TEST_F(DeviceManagerTest, SendCommandClampsOutOfRangeBrightness) {
  ASSERT_TRUE(manager_.registerDevice(makeLightRecord()));

  ASSERT_TRUE(manager_.sendCommand("light-1", "setBrightness", {{"level", 500}}, "user-1").ok);

  EXPECT_EQ(dynamic_cast<Light*>(manager_.findById("light-1"))->brightness(), 100);
}

TEST_F(DeviceManagerTest, SendCommandRejectsUnknownDevice) {
  const CommandResult result = manager_.sendCommand("nope", "turnOn", {}, "user-1");

  EXPECT_FALSE(result.ok);
  EXPECT_NE(result.error.find("unknown device"), std::string::npos);
}

TEST_F(DeviceManagerTest, SendCommandRejectsCommandForWrongDeviceType) {
  ASSERT_TRUE(manager_.registerDevice(makeLightRecord()));

  const CommandResult result = manager_.sendCommand("light-1", "play", {}, "user-1");

  EXPECT_FALSE(result.ok);
  EXPECT_NE(result.error.find("unsupported command"), std::string::npos);
}

TEST_F(DeviceManagerTest, FailedCommandIsStillAudited) {
  ASSERT_TRUE(manager_.registerDevice(makeLightRecord()));

  ASSERT_FALSE(manager_.sendCommand("light-1", "setColor", {{"color", "red"}}, "user-1").ok);

  EXPECT_EQ(database_.event_count(), 1U);
}

TEST_F(DeviceManagerTest, UnregisterRemovesFromMemoryAndDatabase) {
  ASSERT_TRUE(manager_.registerDevice(makeLightRecord()));

  ASSERT_TRUE(manager_.unregisterDevice("light-1"));

  EXPECT_EQ(manager_.size(), 0U);
  EXPECT_FALSE(database_.findDevice("light-1").has_value());
  EXPECT_EQ(manager_.findById("light-1"), nullptr);
}

TEST_F(DeviceManagerTest, LoadHouseholdRehydratesOnlyThatHousehold) {
  DeviceRecord other = makeLightRecord("light-2");
  other.householdId = "house-2";
  ASSERT_TRUE(database_.saveDevice(makeLightRecord()));
  ASSERT_TRUE(database_.saveDevice(other));

  EXPECT_EQ(manager_.loadHousehold("house-1"), 1U);
  EXPECT_NE(manager_.findById("light-1"), nullptr);
  EXPECT_EQ(manager_.findById("light-2"), nullptr);
}

TEST_F(DeviceManagerTest, SnapshotReportsEveryLoadedDevice) {
  ASSERT_TRUE(manager_.registerDevice(makeLightRecord()));
  ASSERT_TRUE(manager_.registerDevice(makeSpeakerRecord()));

  const nlohmann::json snapshot = manager_.snapshot();

  ASSERT_TRUE(snapshot.is_array());
  EXPECT_EQ(snapshot.size(), 2U);
}

TEST(OfflineDeviceTest, CommandsFailWhileDeviceIsOffline) {
  InMemoryDatabase database;
  database.connect();
  DeviceManager manager{database};
  DeviceRecord record = makeLightRecord();
  record.status = "OFFLINE";
  ASSERT_TRUE(manager.registerDevice(record));

  const CommandResult result = manager.sendCommand("light-1", "turnOn", {}, "user-1");

  EXPECT_FALSE(result.ok);
  EXPECT_EQ(result.error, "device unreachable");
}

}  // namespace
}  // namespace homestr
