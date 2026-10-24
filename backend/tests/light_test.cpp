#include "homestr/models/light.hpp"

#include <memory>
#include <string>

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

namespace homestr {
namespace {

std::unique_ptr<Light> makeLight(DeviceStatus status = DeviceStatus::Online) {
  auto light = std::make_unique<Light>("light-1", "Desk Lamp", "192.168.1.20");
  light->setStatus(status);
  return light;
}

TEST(LightTest, DefaultsToFullBrightnessWhite) {
  const Light light{"light-1", "Desk Lamp", "192.168.1.20"};

  EXPECT_EQ(light.type(), "light");
  EXPECT_EQ(light.brightness(), 100);
  EXPECT_EQ(light.color(), "#FFFFFF");
}

TEST(LightTest, SetBrightnessStoresValueInRange) {
  const auto light = makeLight();

  EXPECT_TRUE(light->setBrightness(42));
  EXPECT_EQ(light->brightness(), 42);
}

TEST(LightTest, SetBrightnessAcceptsBothBoundaries) {
  const auto light = makeLight();

  EXPECT_TRUE(light->setBrightness(0));
  EXPECT_EQ(light->brightness(), 0);
  EXPECT_TRUE(light->setBrightness(100));
  EXPECT_EQ(light->brightness(), 100);
}

TEST(LightTest, SetBrightnessClampsOutOfRangeValues) {
  const auto light = makeLight();

  EXPECT_TRUE(light->setBrightness(250));
  EXPECT_EQ(light->brightness(), 100);
  EXPECT_TRUE(light->setBrightness(-5));
  EXPECT_EQ(light->brightness(), 0);
}

TEST(LightTest, SetBrightnessFailsAndKeepsLevelWhenNotOnline) {
  for (const DeviceStatus status : {DeviceStatus::Offline, DeviceStatus::Error}) {
    const auto light = makeLight(status);

    EXPECT_FALSE(light->setBrightness(10)) << toString(status);
    EXPECT_EQ(light->brightness(), 100) << toString(status);
  }
}

TEST(LightTest, SetColorAcceptsUpperAndLowerCaseHex) {
  const auto light = makeLight();

  EXPECT_TRUE(light->setColor("#FFAA00"));
  EXPECT_EQ(light->color(), "#FFAA00");
  EXPECT_TRUE(light->setColor("#ffaa00"));
  EXPECT_EQ(light->color(), "#ffaa00");
}

TEST(LightTest, SetColorRejectsMalformedValuesAndLeavesColorUnchanged) {
  const auto light = makeLight();
  ASSERT_TRUE(light->setColor("#102030"));

  for (const char* bad :
       {"red", "", "#", "#FFF", "FFAA00", "#GGGGGG", "#FFAA0", "#FFAA000", "# FFAA0", "0xFFAA00"}) {
    EXPECT_FALSE(light->setColor(bad)) << '"' << bad << '"';
    EXPECT_EQ(light->color(), "#102030") << '"' << bad << '"';
  }
}

TEST(LightTest, SetColorFailsAndKeepsColorWhenNotOnline) {
  const auto light = makeLight(DeviceStatus::Offline);

  EXPECT_FALSE(light->setColor("#FF0000"));
  EXPECT_EQ(light->color(), "#FFFFFF");
}

TEST(LightTest, IsValidHexColorRequiresHashAndSixHexDigits) {
  EXPECT_TRUE(Light::isValidHexColor("#000000"));
  EXPECT_TRUE(Light::isValidHexColor("#abcdef"));
  EXPECT_TRUE(Light::isValidHexColor("#ABCDEF"));
  EXPECT_TRUE(Light::isValidHexColor("#0a1B2c"));

  EXPECT_FALSE(Light::isValidHexColor(""));
  EXPECT_FALSE(Light::isValidHexColor("#12345"));
  EXPECT_FALSE(Light::isValidHexColor("#1234567"));
  EXPECT_FALSE(Light::isValidHexColor("123456#"));
  EXPECT_FALSE(Light::isValidHexColor("#12345g"));
}

TEST(LightTest, InheritsPowerControlFromSmartDevice) {
  const auto light = makeLight();

  EXPECT_TRUE(light->turnOn());
  EXPECT_TRUE(light->isOn());
  EXPECT_TRUE(light->turnOff());
  EXPECT_FALSE(light->isOn());
}

TEST(LightTest, PowerAndBrightnessAreIndependent) {
  const auto light = makeLight();

  ASSERT_TRUE(light->setBrightness(30));
  ASSERT_TRUE(light->turnOn());
  ASSERT_TRUE(light->turnOff());

  EXPECT_EQ(light->brightness(), 30);
}

TEST(LightTest, ToJsonExtendsTheBasePayload) {
  const auto light = makeLight();
  ASSERT_TRUE(light->turnOn());
  ASSERT_TRUE(light->setBrightness(55));
  ASSERT_TRUE(light->setColor("#00FF00"));

  const nlohmann::json payload = light->toJson();

  EXPECT_EQ(payload.at("id"), "light-1");
  EXPECT_EQ(payload.at("name"), "Desk Lamp");
  EXPECT_EQ(payload.at("ipAddress"), "192.168.1.20");
  EXPECT_EQ(payload.at("type"), "light");
  EXPECT_EQ(payload.at("status"), "ONLINE");
  EXPECT_EQ(payload.at("powerOn"), true);
  EXPECT_EQ(payload.at("brightness"), 55);
  EXPECT_EQ(payload.at("color"), "#00FF00");
}

}  // namespace
}  // namespace homestr
