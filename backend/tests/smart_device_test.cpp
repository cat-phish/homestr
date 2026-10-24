#include "homestr/models/smart_device.hpp"

#include <memory>
#include <string>
#include <type_traits>

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include "homestr/models/light.hpp"
#include "homestr/models/smart_speaker.hpp"

namespace homestr {
namespace {

static_assert(std::is_abstract_v<SmartDevice>, "SmartDevice must not be instantiable");
static_assert(!std::is_copy_constructible_v<SmartDevice>, "devices are identities, not values");
static_assert(!std::is_copy_assignable_v<SmartDevice>, "devices are identities, not values");

std::unique_ptr<GenericDevice> makeDevice(DeviceStatus status = DeviceStatus::Online) {
  auto device = std::make_unique<GenericDevice>("dev-1", "Hall Plug", "192.168.1.10");
  device->setStatus(status);
  return device;
}

TEST(DeviceStatusTest, ToStringUsesTheDatabaseEnumSpelling) {
  EXPECT_EQ(toString(DeviceStatus::Online), "ONLINE");
  EXPECT_EQ(toString(DeviceStatus::Offline), "OFFLINE");
  EXPECT_EQ(toString(DeviceStatus::Error), "ERROR");
}

TEST(DeviceStatusTest, ParseRoundTripsEveryValue) {
  for (const DeviceStatus status :
       {DeviceStatus::Online, DeviceStatus::Offline, DeviceStatus::Error}) {
    EXPECT_EQ(parseDeviceStatus(toString(status)), status);
  }
}

TEST(DeviceStatusTest, ParseFallsBackToOfflineForUnknownText) {
  EXPECT_EQ(parseDeviceStatus("nonsense"), DeviceStatus::Offline);
  EXPECT_EQ(parseDeviceStatus(""), DeviceStatus::Offline);
}

TEST(SmartDeviceTest, NewDeviceIsOfflineAndPoweredDown) {
  const GenericDevice device{"dev-1", "Hall Plug", "192.168.1.10"};

  EXPECT_EQ(device.id(), "dev-1");
  EXPECT_EQ(device.name(), "Hall Plug");
  EXPECT_EQ(device.ipAddress(), "192.168.1.10");
  EXPECT_EQ(device.status(), DeviceStatus::Offline);
  EXPECT_FALSE(device.isOn());
}

TEST(SmartDeviceTest, SetStatusChangesStatus) {
  const auto device = makeDevice(DeviceStatus::Offline);

  device->setStatus(DeviceStatus::Error);

  EXPECT_EQ(device->status(), DeviceStatus::Error);
}

TEST(SmartDeviceTest, TurnOnAndTurnOffToggleWhileOnline) {
  const auto device = makeDevice();

  EXPECT_TRUE(device->turnOn());
  EXPECT_TRUE(device->isOn());
  EXPECT_TRUE(device->turnOff());
  EXPECT_FALSE(device->isOn());
}

TEST(SmartDeviceTest, TurnOnIsIdempotent) {
  const auto device = makeDevice();

  EXPECT_TRUE(device->turnOn());
  EXPECT_TRUE(device->turnOn());
  EXPECT_TRUE(device->isOn());
}

TEST(SmartDeviceTest, TurnOnFailsUnlessOnline) {
  for (const DeviceStatus status : {DeviceStatus::Offline, DeviceStatus::Error}) {
    const auto device = makeDevice(status);

    EXPECT_FALSE(device->turnOn()) << toString(status);
    EXPECT_FALSE(device->isOn()) << toString(status);
  }
}

TEST(SmartDeviceTest, TurnOffFailsAndKeepsStateWhenDeviceDropsOffline) {
  const auto device = makeDevice();
  ASSERT_TRUE(device->turnOn());
  device->setStatus(DeviceStatus::Offline);

  EXPECT_FALSE(device->turnOff());
  EXPECT_TRUE(device->isOn());
}

TEST(SmartDeviceTest, ToJsonCarriesTheBaseAttributes) {
  const auto device = makeDevice();
  ASSERT_TRUE(device->turnOn());

  const nlohmann::json payload = device->toJson();

  EXPECT_EQ(payload.at("id"), "dev-1");
  EXPECT_EQ(payload.at("name"), "Hall Plug");
  EXPECT_EQ(payload.at("ipAddress"), "192.168.1.10");
  EXPECT_EQ(payload.at("type"), "generic");
  EXPECT_EQ(payload.at("status"), "ONLINE");
  EXPECT_EQ(payload.at("powerOn"), true);
}

TEST(GenericDeviceTest, ReportsGenericTypeAndAddsNoExtraAttributes) {
  const auto device = makeDevice();

  EXPECT_EQ(device->type(), "generic");
  EXPECT_EQ(device->toJson().size(), 6U);
}

TEST(SmartDeviceTest, TypeDiscriminatorIsResolvedThroughTheBasePointer) {
  std::unique_ptr<SmartDevice> light = std::make_unique<Light>("l", "Lamp", "10.0.0.1");
  std::unique_ptr<SmartDevice> speaker = std::make_unique<SmartSpeaker>("s", "Echo", "10.0.0.2");
  std::unique_ptr<SmartDevice> generic = std::make_unique<GenericDevice>("g", "Plug", "10.0.0.3");

  EXPECT_EQ(light->type(), "light");
  EXPECT_EQ(speaker->type(), "speaker");
  EXPECT_EQ(generic->type(), "generic");
}

TEST(SmartDeviceTest, ToJsonIsDispatchedVirtuallyAndExtendsTheBasePayload) {
  std::unique_ptr<SmartDevice> light = std::make_unique<Light>("l", "Lamp", "10.0.0.1");

  const nlohmann::json payload = light->toJson();

  EXPECT_TRUE(payload.contains("brightness"));
  EXPECT_TRUE(payload.contains("color"));
  EXPECT_EQ(payload.at("id"), "l");
  EXPECT_EQ(payload.at("type"), "light");
}

}  // namespace
}  // namespace homestr
