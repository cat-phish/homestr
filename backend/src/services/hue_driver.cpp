#include "homestr/services/hue_driver.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <httplib.h>
#include <sodium.h>

#include <nlohmann/json.hpp>

#include "homestr/models/light.hpp"

namespace homestr {
namespace {

constexpr int HUE_TIMEOUT_SECONDS = 2;
constexpr int HUE_MAX_BRIGHTNESS = 254;

std::string environment(const char* name) {
  const char* value = std::getenv(name);
  return value == nullptr ? std::string{} : std::string{value};
}

double linearise(double channel) {
  return channel > 0.04045 ? std::pow((channel + 0.055) / 1.055, 2.4) : channel / 12.92;
}

// Full state for turnOn, so brightness/colour set while off take effect.
nlohmann::json fullState(int brightness, std::string_view hex, bool color) {
  nlohmann::json body = {{"on", true}, {"bri", hueBrightness(brightness)}};
  if (const auto xy = hexToXy(hex); color && xy) {
    body["xy"] = {xy->first, xy->second};
  }
  return body;
}

httplib::Client bridgeClient(const HueConfig& config) {
  httplib::Client client{config.bridgeUrl};
  client.set_connection_timeout(HUE_TIMEOUT_SECONDS);
  client.set_read_timeout(HUE_TIMEOUT_SECONDS);
  client.set_write_timeout(HUE_TIMEOUT_SECONDS);
  return client;
}

}  // namespace

HueConfig HueConfig::fromEnvironment() {
  HueConfig config;
  config.bridgeUrl = environment("HUE_BRIDGE_URL");
  config.username = environment("HUE_USERNAME");
  while (!config.bridgeUrl.empty() && config.bridgeUrl.back() == '/') {
    config.bridgeUrl.pop_back();
  }
  return config;
}

int hueBrightness(int level) {
  const int clamped = std::clamp(level, 0, 100);
  const int scaled = static_cast<int>(std::lround(clamped * HUE_MAX_BRIGHTNESS / 100.0));
  return std::max(1, scaled);
}

std::optional<std::pair<double, double>> hexToXy(std::string_view hex) {
  if (!Light::isValidHexColor(hex)) {
    return std::nullopt;
  }
  const auto channel = [hex](std::size_t offset) {
    const std::string digits{hex.substr(offset, 2)};
    return linearise(static_cast<double>(std::strtol(digits.c_str(), nullptr, 16)) / 255.0);
  };
  const double red = channel(1);
  const double green = channel(3);
  const double blue = channel(5);

  const double x = red * 0.664511 + green * 0.154324 + blue * 0.162028;
  const double y = red * 0.283881 + green * 0.668433 + blue * 0.047685;
  const double z = red * 0.000088 + green * 0.072310 + blue * 0.986039;
  const double sum = x + y + z;
  if (sum <= 0.0) {
    // Black has no chromaticity; use the D65 white point.
    return std::make_pair(0.3127, 0.3290);
  }
  const auto round4 = [](double value) { return std::round(value * 10000.0) / 10000.0; };
  return std::make_pair(round4(x / sum), round4(y / sum));
}

HueRequest hueStateRequest(const Light& light, std::string_view command,
                           const nlohmann::json& params, bool color) {
  HueRequest request;
  if (command == "turnOn") {
    request.needed = true;
    request.body = fullState(light.brightness(), light.color(), color);
    return request;
  }
  if (command == "turnOff") {
    request.needed = true;
    request.body = {{"on", false}};
    return request;
  }
  if (command == "setBrightness") {
    if (!params.contains("level") || !params.at("level").is_number_integer()) {
      request.error = "setBrightness requires an integer 'level'";
      return request;
    }
    request.needed = light.isOn();
    request.body = {{"bri", hueBrightness(params.at("level").get<int>())}};
    return request;
  }
  if (command == "setColor") {
    if (!color) {
      request.error = "this light does not support colour";
      return request;
    }
    const auto xy = params.contains("color") && params.at("color").is_string()
                        ? hexToXy(params.at("color").get<std::string>())
                        : std::nullopt;
    if (!xy) {
      request.error = "setColor requires a '#RRGGBB' 'color'";
      return request;
    }
    request.needed = light.isOn();
    request.body = {{"xy", {xy->first, xy->second}}};
    return request;
  }
  request.error = "unsupported command '" + std::string{command} + "' for type light";
  return request;
}

std::optional<std::string> hueResponseError(std::string_view body) {
  const nlohmann::json parsed = nlohmann::json::parse(body, nullptr, false);
  if (parsed.is_discarded()) {
    return "malformed response from bridge";
  }
  if (!parsed.is_array()) {
    return "unexpected response from bridge";
  }
  for (const nlohmann::json& element : parsed) {
    if (element.is_object() && element.contains("error")) {
      const nlohmann::json& error = element.at("error");
      return error.is_object() ? error.value("description", std::string{"bridge error"})
                               : std::string{"bridge error"};
    }
  }
  return std::nullopt;
}

std::string hueBridgeHost(std::string_view url) {
  if (const auto scheme = url.find("://"); scheme != std::string_view::npos) {
    url.remove_prefix(scheme + 3);
  }
  url = url.substr(0, url.find_first_of(":/"));
  return std::string{url};
}

std::string hueDeviceId(std::string_view uniqueId) {
  const std::string input = "hue:" + std::string{uniqueId};
  std::array<unsigned char, 16> hash{};
  crypto_generichash(hash.data(), hash.size(), reinterpret_cast<const unsigned char*>(input.data()),
                     input.size(), nullptr, 0);
  // RFC 9562 version 8 (custom) and the RFC variant, so it parses as a UUID.
  hash[6] = static_cast<unsigned char>((hash[6] & 0x0F) | 0x80);
  hash[8] = static_cast<unsigned char>((hash[8] & 0x3F) | 0x80);

  std::string id;
  id.reserve(36);
  std::array<char, 3> digits{};
  for (std::size_t index = 0; index < hash.size(); ++index) {
    if (index == 4 || index == 6 || index == 8 || index == 10) {
      id.push_back('-');
    }
    std::snprintf(digits.data(), digits.size(), "%02x", hash[index]);
    id.append(digits.data(), 2);
  }
  return id;
}

std::vector<DeviceRecord> hueCandidates(const HueConfig& config, const nlohmann::json& lights) {
  std::vector<DeviceRecord> candidates;
  if (!lights.is_object()) {
    return candidates;
  }
  const std::string host = hueBridgeHost(config.bridgeUrl);
  for (const auto& [lightId, light] : lights.items()) {
    if (!light.is_object() || !light.contains("state") || !light.at("state").is_object()) {
      continue;
    }
    const nlohmann::json& state = light.at("state");
    if (!state.contains("bri") || !state.at("bri").is_number_integer()) {
      continue;  // on/off only: no driver for it yet
    }
    const std::string uniqueId = light.value("uniqueid", std::string{host + "/" + lightId});

    DeviceRecord record;
    record.id = hueDeviceId(uniqueId);
    record.name = light.value("name", std::string{"Hue light " + lightId});
    record.ipAddress = host;
    record.deviceType = "light";
    record.status = state.value("reachable", false) ? "ONLINE" : "OFFLINE";
    record.powerOn = state.value("on", false);
    const int bri = state.at("bri").get<int>();
    record.attributes = {
        {"brightness", static_cast<int>(std::lround(std::clamp(bri, 0, HUE_MAX_BRIGHTNESS) * 100.0 /
                                                    HUE_MAX_BRIGHTNESS))},
        {"color", "#FFFFFF"}};
    record.metadata = {{"driver", "hue"}, {"lightId", lightId}, {"color", state.contains("xy")}};
    candidates.push_back(std::move(record));
  }
  return candidates;
}

std::vector<DeviceRecord> discoverHueLights(const HueConfig& config) {
  if (!config.valid()) {
    return {};
  }
  httplib::Client client = bridgeClient(config);
  // The path embeds the username; never log it.
  const httplib::Result response = client.Get("/api/" + config.username + "/lights");
  if (!response) {
    std::cerr << "[discovery] hue bridge unreachable: " << httplib::to_string(response.error())
              << '\n';
    return {};
  }
  if (response->status != 200) {
    std::cerr << "[discovery] hue bridge returned HTTP " << response->status << '\n';
    return {};
  }
  const nlohmann::json lights = nlohmann::json::parse(response->body, nullptr, false);
  if (lights.is_array()) {
    // An error array, e.g. an unauthorized username.
    std::cerr << "[discovery] hue: " << hueResponseError(response->body).value_or("bridge error")
              << '\n';
    return {};
  }
  return hueCandidates(config, lights);
}

HueDriver::HueDriver(HueConfig config, std::string lightId, bool color)
    : config_(std::move(config)), lightId_(std::move(lightId)), color_(color) {}

DriverResult HueDriver::send(const SmartDevice& device, std::string_view command,
                             const nlohmann::json& params) {
  const auto* light = dynamic_cast<const Light*>(&device);
  if (light == nullptr) {
    return DriverResult::rejected("hue driver only controls lights");
  }
  const HueRequest request = hueStateRequest(*light, command, params, color_);
  if (!request.error.empty()) {
    return DriverResult::rejected(request.error);
  }
  if (!request.needed) {
    return DriverResult::sent();
  }

  httplib::Client client = bridgeClient(config_);

  // The path embeds the username; never log it.
  const std::string path = "/api/" + config_.username + "/lights/" + lightId_ + "/state";
  const httplib::Result response = client.Put(path, request.body.dump(), "application/json");
  if (!response) {
    return DriverResult::unreachable("hue bridge unreachable: " +
                                     httplib::to_string(response.error()));
  }
  if (response->status != 200) {
    return DriverResult::unreachable("hue bridge returned HTTP " +
                                     std::to_string(response->status));
  }
  if (const auto error = hueResponseError(response->body); error) {
    return DriverResult::unreachable("hue: " + *error);
  }
  return DriverResult::sent();
}

}  // namespace homestr
