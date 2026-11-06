#pragma once

#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include "homestr/models/light.hpp"
#include "homestr/services/database.hpp"
#include "homestr/services/device_driver.hpp"

namespace homestr {

// Philips Hue API v1 over plain HTTP. Works with a real bridge and with diyHue.
struct HueConfig {
  std::string bridgeUrl;  // e.g. "http://192.168.1.20"
  std::string username;   // whitelist key from POST /api; a credential, never log it

  bool valid() const noexcept { return !bridgeUrl.empty() && !username.empty(); }

  // Reads HUE_BRIDGE_URL and HUE_USERNAME.
  static HueConfig fromEnvironment();
};

// Maps 0-100 onto the Hue range 1-254.
int hueBrightness(int level);

// #RRGGBB to CIE 1931 xy (sRGB, gamma-corrected, wide-gamut D65 matrix).
std::optional<std::pair<double, double>> hexToXy(std::string_view hex);

// The body for PUT /api/<user>/lights/<id>/state. `needed` is false when the
// command needs no request: Hue rejects brightness/colour while a light is off,
// so those are recorded in the model and sent with the next turnOn.
struct HueRequest {
  bool needed = false;
  nlohmann::json body = nlohmann::json::object();
  std::string error;  // non-empty: reject the command
};

// `color` false (a dimmable white bulb): never send `xy`, reject setColor.
HueRequest hueStateRequest(const Light& light, std::string_view command,
                           const nlohmann::json& params, bool color = true);

// Hue answers 200 with `[{"error":{...}}]` on failure. Returns the first error
// description, or nullopt when every element is a success.
std::optional<std::string> hueResponseError(std::string_view body);

// "http://10.0.0.5:80/" -> "10.0.0.5".
std::string hueBridgeHost(std::string_view url);

// Stable UUID-shaped id for a bridge light, from its `uniqueid`.
std::string hueDeviceId(std::string_view uniqueId);

// Registration candidates from a GET /api/<user>/lights body. Lights without
// a brightness (on/off plugs) are skipped.
std::vector<DeviceRecord> hueCandidates(const HueConfig& config, const nlohmann::json& lights);

// Queries the bridge. Logs and returns nothing on any failure.
std::vector<DeviceRecord> discoverHueLights(const HueConfig& config);

class HueDriver : public DeviceDriver {
 public:
  HueDriver(HueConfig config, std::string lightId, bool color = true);

  DriverResult send(const SmartDevice& device, std::string_view command,
                    const nlohmann::json& params) override;

 private:
  HueConfig config_;
  std::string lightId_;
  bool color_;
};

}  // namespace homestr
