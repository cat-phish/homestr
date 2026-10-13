#pragma once

#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "homestr/models/smart_device.hpp"
#include "homestr/services/database.hpp"

namespace homestr {

// Result of a command dispatch, mapped straight onto the HTTP response and the
// `device_events` audit row.
struct CommandResult {
  bool ok = false;
  std::string error;  // empty when ok

  static CommandResult success() { return {true, {}}; }

  static CommandResult failure(std::string reason) { return {false, std::move(reason)}; }
};

// `DeviceManager` from the class diagram: owns the live SmartDevice instances
// and is the only component that talks to the devices themselves.
// Thread-safe: the HTTP handlers, the WebSocket broadcaster and the automation
// engine all call it concurrently.
class DeviceManager {
 public:
  explicit DeviceManager(Database& database);

  // Probes the local network (mDNS/SSDP) for unregistered devices. The current
  // implementation returns the discovery cache; the network scan is a separate
  // work item.
  std::vector<DeviceRecord> discover();

  // Registers a discovered device into a household and persists it. Fails if
  // the id is already registered.
  bool registerDevice(const DeviceRecord& record);

  bool unregisterDevice(const std::string& deviceId);

  // Borrowed pointer, nullptr when unknown. Valid until the device is
  // unregistered.
  SmartDevice* findById(const std::string& deviceId);

  // Rehydrates the in-memory devices for one household from the database.
  std::size_t loadHousehold(const std::string& householdId);

  // Dispatches a domain command ("turnOn", "setBrightness", "play", ...) and
  // persists the resulting state plus an audit event.
  CommandResult sendCommand(const std::string& deviceId, const std::string& command,
                            const nlohmann::json& params, const std::string& actorId);

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

  Database& database_;
  mutable std::mutex mutex_;
  std::vector<std::unique_ptr<SmartDevice>> devices_;
  std::vector<DeviceRecord> discovered_;
  std::string householdId_;
};

}  // namespace homestr
