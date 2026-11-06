#include "homestr/services/device_manager.hpp"

#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include "homestr/models/light.hpp"
#include "homestr/models/smart_speaker.hpp"
#include "homestr/services/database.hpp"
#include "homestr/services/device_driver.hpp"

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

// Records what DeviceManager asked it to send and answers with a preset status.
class FakeDriver : public DeviceDriver {
 public:
  explicit FakeDriver(DriverResult answer, std::vector<std::string>* sent)
      : answer_(std::move(answer)), sent_(sent) {}

  DriverResult send(const SmartDevice& /*device*/, std::string_view command,
                    const nlohmann::json& /*params*/) override {
    sent_->emplace_back(command);
    return answer_;
  }

 private:
  DriverResult answer_;
  std::vector<std::string>* sent_;
};

class DriverBackedDeviceTest : public ::testing::Test {
 protected:
  void SetUp() override { database_.connect(); }

  DeviceManager managerAnswering(DriverResult answer) {
    return DeviceManager{
        database_, [this, answer](const DeviceRecord& record) -> std::unique_ptr<DeviceDriver> {
          if (record.metadata.value("driver", std::string{}) != "fake") {
            return nullptr;
          }
          return std::make_unique<FakeDriver>(answer, &sent_);
        }};
  }

  static DeviceRecord drivenLight(std::string status = "ONLINE") {
    DeviceRecord record = makeLightRecord();
    record.status = std::move(status);
    record.metadata = {{"driver", "fake"}, {"lightId", "3"}};
    return record;
  }

  InMemoryDatabase database_;
  std::vector<std::string> sent_;
};

TEST_F(DriverBackedDeviceTest, SentCommandMutatesModelAndPersists) {
  DeviceManager manager = managerAnswering(DriverResult::sent());
  ASSERT_TRUE(manager.registerDevice(drivenLight()));

  const CommandResult result = manager.sendCommand("light-1", "turnOn", {}, "user-1");

  ASSERT_TRUE(result.ok) << result.error;
  EXPECT_EQ(sent_, std::vector<std::string>{"turnOn"});
  EXPECT_TRUE(manager.state("light-1")->at("powerOn").get<bool>());
  EXPECT_TRUE(database_.findDevice("light-1")->powerOn);
}

TEST_F(DriverBackedDeviceTest, MetadataSurvivesStateWrites) {
  DeviceManager manager = managerAnswering(DriverResult::sent());
  ASSERT_TRUE(manager.registerDevice(drivenLight()));

  ASSERT_TRUE(manager.sendCommand("light-1", "setBrightness", {{"level", 20}}, "user-1").ok);

  const auto stored = database_.findDevice("light-1");
  ASSERT_TRUE(stored.has_value());
  EXPECT_EQ(stored->metadata.value("driver", std::string{}), "fake");
  EXPECT_EQ(stored->metadata.value("lightId", std::string{}), "3");
}

TEST_F(DriverBackedDeviceTest, UnreachableLeavesStateMarksErrorAndAudits) {
  DeviceManager manager = managerAnswering(DriverResult::unreachable("bridge down"));
  ASSERT_TRUE(manager.registerDevice(drivenLight()));

  const CommandResult result = manager.sendCommand("light-1", "turnOn", {}, "user-1");

  EXPECT_FALSE(result.ok);
  EXPECT_EQ(result.error, "bridge down");
  const auto state = manager.state("light-1");
  EXPECT_FALSE(state->at("powerOn").get<bool>());
  EXPECT_EQ(state->at("status"), "ERROR");
  EXPECT_EQ(database_.findDevice("light-1")->status, "ERROR");
  EXPECT_EQ(database_.event_count(), 1U);
}

TEST_F(DriverBackedDeviceTest, SuccessfulSendRecoversAnErroredDevice) {
  DeviceManager manager = managerAnswering(DriverResult::sent());
  ASSERT_TRUE(manager.registerDevice(drivenLight("ERROR")));

  ASSERT_TRUE(manager.sendCommand("light-1", "turnOn", {}, "user-1").ok);

  EXPECT_EQ(manager.state("light-1")->at("status"), "ONLINE");
}

TEST_F(DriverBackedDeviceTest, RejectedCommandKeepsStatus) {
  DeviceManager manager = managerAnswering(DriverResult::rejected("unsupported"));
  ASSERT_TRUE(manager.registerDevice(drivenLight()));

  EXPECT_FALSE(manager.sendCommand("light-1", "play", {}, "user-1").ok);
  EXPECT_EQ(manager.state("light-1")->at("status"), "ONLINE");
  EXPECT_EQ(database_.event_count(), 1U);
}

TEST_F(DriverBackedDeviceTest, DevicesWithoutDriverStaySimulated) {
  DeviceManager manager = managerAnswering(DriverResult::unreachable("never called"));
  ASSERT_TRUE(manager.registerDevice(makeLightRecord()));

  EXPECT_TRUE(manager.sendCommand("light-1", "turnOn", {}, "user-1").ok);
  EXPECT_TRUE(sent_.empty());
}

DeviceRecord hueCandidate(std::string id, std::string lightId) {
  DeviceRecord record = makeLightRecord(std::move(id));
  record.householdId = "";
  record.metadata = {{"driver", "hue"}, {"lightId", std::move(lightId)}};
  return record;
}

DeviceManager managerDiscovering(Database& database, std::vector<DeviceRecord> found) {
  return DeviceManager{database, nullptr, [found] { return found; }};
}

TEST(DiscoveryTest, ReturnsWhatTheSourceFinds) {
  InMemoryDatabase database;
  database.connect();
  DeviceManager manager = managerDiscovering(database, {hueCandidate("a", "1")});

  const auto found = manager.discover();

  ASSERT_EQ(found.size(), 1U);
  EXPECT_EQ(found.at(0).metadata.at("lightId"), "1");
}

TEST(DiscoveryTest, DropsCandidatesRegisteredUnderTheSameId) {
  InMemoryDatabase database;
  database.connect();
  DeviceManager manager =
      managerDiscovering(database, {hueCandidate("a", "1"), hueCandidate("b", "2")});
  DeviceRecord registered = hueCandidate("a", "1");
  registered.householdId = "house-1";
  ASSERT_TRUE(manager.registerDevice(registered));

  const auto found = manager.discover();

  ASSERT_EQ(found.size(), 1U);
  EXPECT_EQ(found.at(0).id, "b");
}

TEST(DiscoveryTest, DropsLightsRegisteredByHandUnderAnotherId) {
  InMemoryDatabase database;
  database.connect();
  DeviceManager manager = managerDiscovering(database, {hueCandidate("discovered-id", "3")});
  DeviceRecord manual = hueCandidate("hand-made-uuid", "3");
  manual.householdId = "house-1";
  ASSERT_TRUE(manager.registerDevice(manual));

  EXPECT_TRUE(manager.discover().empty());
}

TEST(DiscoveryTest, NoSourceMeansNoCandidates) {
  InMemoryDatabase database;
  database.connect();
  DeviceManager manager{database, nullptr, nullptr};

  EXPECT_TRUE(manager.discover().empty());
}

}  // namespace
}  // namespace homestr
