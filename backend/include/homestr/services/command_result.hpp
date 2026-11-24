#pragma once

#include <string>
#include <utility>

namespace homestr {

// Result of a command dispatch, mapped straight onto the HTTP response and the
// `device_events` audit row.
struct CommandResult {
  bool ok = false;
  std::string error;  // empty when ok

  static CommandResult success() { return {true, {}}; }

  static CommandResult failure(std::string reason) { return {false, std::move(reason)}; }
};

}  // namespace homestr
