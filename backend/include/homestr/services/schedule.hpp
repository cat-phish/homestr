#pragma once

#include <cstdint>
#include <optional>
#include <string>

#include <nlohmann/json.hpp>

namespace homestr {

class Schedule {
 public:
  static std::int64_t now();
  static bool valid(const nlohmann::json& trigger);
  static bool due(const nlohmann::json& trigger, std::int64_t instant);
  static std::optional<std::int64_t> next(const nlohmann::json& trigger, std::int64_t after);
  static std::string format(std::int64_t instant);
};

}  // namespace homestr
