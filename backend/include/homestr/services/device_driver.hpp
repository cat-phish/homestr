#pragma once

#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

#include "homestr/models/smart_device.hpp"
#include "homestr/services/database.hpp"

namespace homestr {

// What happened when a driver tried to deliver a command.
enum class DriverStatus {
  Sent,         // the device accepted it; apply it to the model
  Rejected,     // invalid or unsupported command; nothing was sent
  Unreachable,  // transport failure or device-side error; the device goes ERROR
};

struct DriverResult {
  DriverStatus status = DriverStatus::Rejected;
  std::string error;  // empty when Sent

  static DriverResult sent() { return {DriverStatus::Sent, {}}; }

  static DriverResult rejected(std::string reason) {
    return {DriverStatus::Rejected, std::move(reason)};
  }

  static DriverResult unreachable(std::string reason) {
    return {DriverStatus::Unreachable, std::move(reason)};
  }
};

// Transport to one physical device. DeviceManager calls `send` before it
// mutates the model, so the model only records state the device confirmed.
// `device` is the state before the command.
class DeviceDriver {
 public:
  virtual ~DeviceDriver() = default;

  virtual DriverResult send(const SmartDevice& device, std::string_view command,
                            const nlohmann::json& params) = 0;
};

// Builds the driver named by `record.metadata["driver"]`, or nullptr for a
// device without one, which stays simulated.
using DriverFactory = std::function<std::unique_ptr<DeviceDriver>(const DeviceRecord&)>;

// Knows the `hue` driver, configured from HUE_BRIDGE_URL and HUE_USERNAME.
DriverFactory defaultDriverFactory();

// Returns registration candidates, metadata included. May do network I/O, so
// DeviceManager calls it outside its lock.
using DiscoverySource = std::function<std::vector<DeviceRecord>()>;

// The Hue bridge when HUE_BRIDGE_URL and HUE_USERNAME are set, else nothing.
DiscoverySource defaultDiscoverySource();

}  // namespace homestr
