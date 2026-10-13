#pragma once

#include <string>
#include <string_view>

#include <nlohmann/json.hpp>

#include "homestr/models/smart_device.hpp"

namespace homestr {

// `Light extends SmartDevice`: adds brightness and colour.
class Light : public SmartDevice {
 public:
  Light(std::string id, std::string name, std::string ipAddress);

  std::string_view type() const noexcept override { return "light"; }

  int brightness() const noexcept { return brightness_; }

  const std::string& color() const noexcept { return color_; }

  // Clamps `level` into [0, 100]; returns false if the device is unreachable.
  bool setBrightness(int level);

  // `hex` must be #RRGGBB. Returns false on a malformed value, leaving the
  // current colour untouched.
  bool setColor(std::string_view hex);

  nlohmann::json toJson() const override;

  static bool isValidHexColor(std::string_view hex);

 private:
  int brightness_ = 100;
  std::string color_ = "#FFFFFF";
};

}  // namespace homestr
