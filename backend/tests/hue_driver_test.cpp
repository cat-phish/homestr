#include "homestr/services/hue_driver.hpp"

#include <string>
#include <thread>
#include <utility>

#include <httplib.h>

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include "homestr/models/light.hpp"

namespace homestr {
namespace {

// An online light at 50% red. Lights are not movable, so tests hold one in place.
struct TestLight {
  explicit TestLight(bool on) {
    light.setStatus(DeviceStatus::Online);
    light.setBrightness(50);
    light.setColor("#FF0000");
    if (on) {
      light.turnOn();
    }
  }

  Light light{"light-1", "Desk", "192.168.1.40"};
};

TEST(HueDriverTest, BrightnessMapsOntoHueRange) {
  EXPECT_EQ(hueBrightness(0), 1);
  EXPECT_EQ(hueBrightness(50), 127);
  EXPECT_EQ(hueBrightness(100), 254);
  EXPECT_EQ(hueBrightness(250), 254);
  EXPECT_EQ(hueBrightness(-5), 1);
}

TEST(HueDriverTest, HexToXyConvertsPrimariesAndRejectsMalformed) {
  const auto red = hexToXy("#FF0000");
  ASSERT_TRUE(red.has_value());
  EXPECT_NEAR(red->first, 0.7006, 0.001);
  EXPECT_NEAR(red->second, 0.2993, 0.001);

  const auto white = hexToXy("#FFFFFF");
  ASSERT_TRUE(white.has_value());
  EXPECT_NEAR(white->first, 0.3227, 0.002);
  EXPECT_NEAR(white->second, 0.329, 0.002);

  EXPECT_FALSE(hexToXy("red").has_value());
  EXPECT_FALSE(hexToXy("#GG0000").has_value());
}

TEST(HueDriverTest, TurnOnSendsFullStateSoChangesWhileOffApply) {
  const TestLight off{false};
  const HueRequest request = hueStateRequest(off.light, "turnOn", nlohmann::json::object());

  ASSERT_TRUE(request.needed);
  EXPECT_EQ(request.body.at("on"), true);
  EXPECT_EQ(request.body.at("bri"), 127);
  ASSERT_TRUE(request.body.contains("xy"));
}

TEST(HueDriverTest, TurnOffSendsOnFalse) {
  const TestLight on{true};
  const HueRequest request = hueStateRequest(on.light, "turnOff", nlohmann::json::object());

  ASSERT_TRUE(request.needed);
  EXPECT_EQ(request.body, (nlohmann::json{{"on", false}}));
}

TEST(HueDriverTest, BrightnessAndColourAreSentOnlyWhileOn) {
  const TestLight lit{true};
  const TestLight dark{false};
  const nlohmann::json level = {{"level", 100}};
  const HueRequest on = hueStateRequest(lit.light, "setBrightness", level);
  ASSERT_TRUE(on.needed);
  EXPECT_EQ(on.body, (nlohmann::json{{"bri", 254}}));

  const HueRequest off = hueStateRequest(dark.light, "setBrightness", level);
  EXPECT_FALSE(off.needed);
  EXPECT_TRUE(off.error.empty());

  const HueRequest color = hueStateRequest(lit.light, "setColor", {{"color", "#0000FF"}});
  ASSERT_TRUE(color.needed);
  ASSERT_TRUE(color.body.contains("xy"));
}

TEST(HueDriverTest, InvalidOrUnsupportedCommandsAreRejected) {
  const TestLight subject{true};
  const Light& light = subject.light;
  EXPECT_FALSE(hueStateRequest(light, "setBrightness", {{"level", "high"}}).error.empty());
  EXPECT_FALSE(hueStateRequest(light, "setColor", {{"color", "blue"}}).error.empty());
  EXPECT_FALSE(hueStateRequest(light, "play", nlohmann::json::object()).error.empty());
}

TEST(HueDriverTest, ResponseErrorsAreDetectedInsideA200) {
  EXPECT_FALSE(hueResponseError(R"([{"success":{"/lights/1/state/on":true}}])").has_value());
  EXPECT_EQ(hueResponseError(R"([{"error":{"type":1,"description":"unauthorized user"}}])"),
            "unauthorized user");
  EXPECT_TRUE(hueResponseError("not json").has_value());
  EXPECT_TRUE(hueResponseError(R"({"on":true})").has_value());
}

TEST(HueDriverTest, UnreachableBridgeReportsUnreachable) {
  // Port 9 on localhost (discard) is closed in any sane test environment.
  HueDriver driver{{"http://127.0.0.1:9", "test-user"}, "1"};
  const TestLight off{false};
  const DriverResult result = driver.send(off.light, "turnOn", nlohmann::json::object());

  EXPECT_EQ(result.status, DriverStatus::Unreachable);
  EXPECT_FALSE(result.error.empty());
}

// A local stand-in for the bridge, answering PUT .../lights/<id>/state.
class FakeBridge {
 public:
  explicit FakeBridge(std::string reply) : reply_(std::move(reply)) {
    server_.Get(R"(/api/([^/]+)/lights)",
                [this](const httplib::Request& request, httplib::Response& response) {
                  path_ = request.path;
                  response.set_content(reply_, "application/json");
                });
    server_.Put(R"(/api/([^/]+)/lights/([^/]+)/state)",
                [this](const httplib::Request& request, httplib::Response& response) {
                  path_ = request.path;
                  body_ = nlohmann::json::parse(request.body, nullptr, false);
                  response.set_content(reply_, "application/json");
                });
    port_ = server_.bind_to_any_port("127.0.0.1");
    thread_ = std::thread([this] { server_.listen_after_bind(); });
    server_.wait_until_ready();
  }

  ~FakeBridge() {
    server_.stop();
    thread_.join();
  }

  FakeBridge(const FakeBridge&) = delete;
  FakeBridge& operator=(const FakeBridge&) = delete;

  std::string url() const { return "http://127.0.0.1:" + std::to_string(port_); }

  // Read after send() returns; the handler has finished by then.
  const std::string& path() const { return path_; }

  const nlohmann::json& body() const { return body_; }

 private:
  std::string reply_;
  httplib::Server server_;
  int port_ = 0;
  std::thread thread_;
  std::string path_;
  nlohmann::json body_;
};

TEST(HueDriverTest, SendsStateToTheBridgeOverHttp) {
  FakeBridge bridge{R"([{"success":{"/lights/3/state/on":true}}])"};
  HueDriver driver{{bridge.url(), "test-user"}, "3"};
  const TestLight off{false};

  const DriverResult result = driver.send(off.light, "turnOn", nlohmann::json::object());

  ASSERT_EQ(result.status, DriverStatus::Sent) << result.error;
  EXPECT_EQ(bridge.path(), "/api/test-user/lights/3/state");
  EXPECT_EQ(bridge.body().at("on"), true);
  EXPECT_EQ(bridge.body().at("bri"), 127);
}

TEST(HueDriverTest, BridgeErrorInsideA200IsUnreachable) {
  FakeBridge bridge{R"([{"error":{"type":1,"description":"unauthorized user"}}])"};
  HueDriver driver{{bridge.url(), "wrong-user"}, "3"};
  const TestLight on{true};

  const DriverResult result = driver.send(on.light, "turnOff", nlohmann::json::object());

  EXPECT_EQ(result.status, DriverStatus::Unreachable);
  EXPECT_EQ(result.error, "hue: unauthorized user");
}

// Trimmed from a real bridge's GET /api/<user>/lights.
const nlohmann::json BRIDGE_LIGHTS = nlohmann::json::parse(R"({
  "1": {"name": "Sofa lamp", "type": "Extended color light",
        "uniqueid": "00:17:88:01:00:aa:bb:01-0b",
        "state": {"on": true, "bri": 254, "xy": [0.45, 0.41], "reachable": true}},
  "2": {"name": "Hallway", "type": "Dimmable light",
        "uniqueid": "00:17:88:01:00:aa:bb:02-0b",
        "state": {"on": false, "bri": 127, "reachable": false}},
  "5": {"name": "Plug", "type": "On/Off plug-in unit",
        "uniqueid": "00:17:88:01:00:aa:bb:05-0b",
        "state": {"on": false, "reachable": true}}
})");

TEST(HueDiscoveryTest, BridgeHostDropsSchemePortAndPath) {
  EXPECT_EQ(hueBridgeHost("http://10.44.44.5"), "10.44.44.5");
  EXPECT_EQ(hueBridgeHost("http://10.44.44.5:8080/"), "10.44.44.5");
  EXPECT_EQ(hueBridgeHost("bridge.local"), "bridge.local");
}

TEST(HueDiscoveryTest, DeviceIdIsStableUuidShaped) {
  const std::string id = hueDeviceId("00:17:88:01:00:aa:bb:01-0b");

  EXPECT_EQ(id, hueDeviceId("00:17:88:01:00:aa:bb:01-0b"));
  EXPECT_NE(id, hueDeviceId("00:17:88:01:00:aa:bb:02-0b"));
  ASSERT_EQ(id.size(), 36U);
  EXPECT_EQ(id[8], '-');
  EXPECT_EQ(id[14], '8');                                          // version 8
  EXPECT_NE(std::string{"89ab"}.find(id[19]), std::string::npos);  // RFC variant
}

TEST(HueDiscoveryTest, CandidatesCarryDriverMetadataAndSkipPlugs) {
  const auto candidates = hueCandidates({"http://10.44.44.5", "user"}, BRIDGE_LIGHTS);

  ASSERT_EQ(candidates.size(), 2U);
  const DeviceRecord& sofa = candidates.at(0);
  EXPECT_EQ(sofa.name, "Sofa lamp");
  EXPECT_EQ(sofa.deviceType, "light");
  EXPECT_EQ(sofa.ipAddress, "10.44.44.5");
  EXPECT_EQ(sofa.status, "ONLINE");
  EXPECT_TRUE(sofa.powerOn);
  EXPECT_EQ(sofa.attributes.at("brightness"), 100);
  EXPECT_EQ(sofa.metadata, (nlohmann::json{{"driver", "hue"}, {"lightId", "1"}, {"color", true}}));

  const DeviceRecord& hallway = candidates.at(1);
  EXPECT_EQ(hallway.status, "OFFLINE");
  EXPECT_EQ(hallway.attributes.at("brightness"), 50);
  EXPECT_EQ(hallway.metadata.at("color"), false);
}

TEST(HueDiscoveryTest, MalformedBodyYieldsNoCandidates) {
  EXPECT_TRUE(hueCandidates({"http://h", "u"}, nlohmann::json::array()).empty());
  EXPECT_TRUE(
      hueCandidates({"http://h", "u"}, nlohmann::json{{"1", {{"name", "no state"}}}}).empty());
}

TEST(HueDiscoveryTest, WhiteOnlyLightsNeverGetColour) {
  const TestLight off{false};
  const HueRequest on = hueStateRequest(off.light, "turnOn", nlohmann::json::object(), false);
  ASSERT_TRUE(on.needed);
  EXPECT_FALSE(on.body.contains("xy"));

  const HueRequest color = hueStateRequest(off.light, "setColor", {{"color", "#0000FF"}}, false);
  EXPECT_FALSE(color.error.empty());
}

TEST(HueDiscoveryTest, DiscoversLightsFromTheBridgeOverHttp) {
  FakeBridge bridge{BRIDGE_LIGHTS.dump()};

  const auto candidates = discoverHueLights({bridge.url(), "test-user"});

  EXPECT_EQ(bridge.path(), "/api/test-user/lights");
  EXPECT_EQ(candidates.size(), 2U);
}

TEST(HueDiscoveryTest, BridgeErrorsAndMissingConfigYieldNothing) {
  FakeBridge bridge{R"([{"error":{"type":1,"description":"unauthorized user"}}])"};

  EXPECT_TRUE(discoverHueLights({bridge.url(), "wrong-user"}).empty());
  EXPECT_TRUE(discoverHueLights({"", ""}).empty());
  EXPECT_TRUE(discoverHueLights({"http://127.0.0.1:9", "user"}).empty());
}

}  // namespace
}  // namespace homestr
