#pragma once

#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include <nlohmann/json.hpp>

#include "homestr/models/smart_device.hpp"
#include "homestr/services/command_result.hpp"
#include "homestr/services/database.hpp"
#include "homestr/services/device_driver.hpp"

namespace homestr {

enum class RenameResult { Renamed, UnknownDevice, InvalidName, StorageFailed };

// Longest name `devices.name` (VARCHAR(100)) holds.
constexpr std::size_t MAX_DEVICE_NAME_LENGTH = 100;

// `DeviceManager` from the class diagram: owns the live SmartDevice instances
// and is the only component that talks to the devices themselves.
// Thread-safe: the HTTP handlers, the WebSocket broadcaster and the automation
// engine all call it concurrently.
class DeviceManager {
 public:
  // `driverFactory` picks a transport per device from its metadata; devices it
  // returns nullptr for stay simulated. `discovery` supplies discover().
  explicit DeviceManager(Database& database, DriverFactory driverFactory = defaultDriverFactory(),
                         DiscoverySource discovery = defaultDiscoverySource());

  // Asks the discovery source for candidates (outside the lock) and drops those
  // already registered, by id or by driver + lightId.
  std::vector<DeviceRecord> discover();

  // Registers a discovered device into a household and persists it. Fails if
  // the id is already registered.
  bool registerDevice(const DeviceRecord& record);

  bool unregisterDevice(const std::string& deviceId);

  // Trims `name`, persists it with the rest of the device (metadata included)
  // and audits a `rename` event. A failed write restores the old name.
  RenameResult renameDevice(const std::string& deviceId, std::string_view name,
                            const std::string& actorId);

  // Borrowed pointer, nullptr when unknown. Valid until the device is
  // unregistered.
  SmartDevice* findById(const std::string& deviceId);

  // Rehydrates the in-memory devices for one household from the database.
  std::size_t loadHousehold(const std::string& householdId);

  // Dispatches a domain command ("turnOn", "setBrightness", "play", ...) and
  // persists the resulting state plus an audit event. A driver-backed device
  // is sent the command first and the model changes only if the device
  // accepted it; a transport failure marks the device ERROR.
  // Known limitation: the driver call runs under the manager lock.
  CommandResult sendCommand(const std::string& deviceId, const std::string& command,
                            const nlohmann::json& params, const std::string& actorId,
                            const std::string& actorType = "USER");

  using StateObserver = std::function<void(const nlohmann::json&, const nlohmann::json&)>;
  void setStateObserver(StateObserver observer);
  std::optional<nlohmann::json> state(const std::string& deviceId) const;

  // snapshot of every loaded device, as served by GET /devices.
  nlohmann::json snapshot() const;

  std::size_t size() const;

 private:
  // Builds a Light, SmartSpeaker or plain SmartDevice from `record`.
  static std::unique_ptr<SmartDevice> materialise(const DeviceRecord& record);

  // Applies `command` to `device`. Caller holds mutex_.
  static CommandResult apply(SmartDevice& device, const std::string& command,
                             const nlohmann::json& params);

  // Projects live state back into a persistable record. Caller holds mutex_.
  DeviceRecord project(const SmartDevice& device) const;

  // Stores the record's metadata and its driver, if any. Caller holds mutex_.
  void bind(const DeviceRecord& record);

  struct Binding {
    nlohmann::json metadata = nlohmann::json::object();
    std::unique_ptr<DeviceDriver> driver;  // null: simulated
  };

  Database& database_;
  DriverFactory driverFactory_;
  DiscoverySource discovery_;
  std::unordered_map<std::string, Binding> bindings_;
  mutable std::mutex mutex_;
  std::vector<std::unique_ptr<SmartDevice>> devices_;
  std::string householdId_;
  StateObserver observer_;
};

}  // namespace homestr
