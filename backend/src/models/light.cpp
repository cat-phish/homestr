#include "homestr/models/light.hpp"

#include <algorithm>
#include <cctype>
#include <string>
#include <string_view>
#include <utility>

#include <nlohmann/json.hpp>

namespace homestr {

Light::Light(std::string id, std::string name, std::string ipAddress)
    : SmartDevice(std::move(id), std::move(name), std::move(ipAddress)) {}

bool Light::isValidHexColor(std::string_view hex) {
  if (hex.size() != 7 || hex.front() != '#') {
    return false;
  }
  return std::all_of(hex.begin() + 1, hex.end(),
                     [](unsigned char character) { return std::isxdigit(character) != 0; });
}

bool Light::setBrightness(int level) {
  if (!reachable()) {
    return false;
  }
  brightness_ = std::clamp(level, 0, 100);
  return true;
}

bool Light::setColor(std::string_view hex) {
  if (!isValidHexColor(hex)) {
    return false;
  }
  if (!reachable()) {
    return false;
  }
  color_.assign(hex);
  return true;
}

nlohmann::json Light::toJson() const {
  nlohmann::json payload = SmartDevice::toJson();
  payload["brightness"] = brightness_;
  payload["color"] = color_;
  return payload;
}

}  // namespace homestr
