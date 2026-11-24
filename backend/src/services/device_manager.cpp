#include "homestr/services/device_manager.hpp"

#include <algorithm>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include "homestr/models/light.hpp"
#include "homestr/models/smart_device.hpp"
#include "homestr/models/smart_speaker.hpp"

namespace homestr {

DeviceManager::DeviceManager(Database& database, DriverFactory driverFactory,
                             DiscoverySource discovery)
    : database_(database),
      driverFactory_(std::move(driverFactory)),
      discovery_(std::move(discovery)) {}

void DeviceManager::bind(const DeviceRecord& record) {
  Binding binding;
  binding.metadata = record.metadata.is_object() ? record.metadata : nlohmann::json::object();
  if (driverFactory_) {
    binding.driver = driverFactory_(record);
  }
  bindings_[record.id] = std::move(binding);
}

std::unique_ptr<SmartDevice> DeviceManager::materialise(const DeviceRecord& record) {
  std::unique_ptr<SmartDevice> device;

  if (record.deviceType == "light") {
    auto light = std::make_unique<Light>(record.id, record.name, record.ipAddress);
    light->setStatus(parseDeviceStatus(record.status));
    light->setBrightness(record.attributes.value("brightness", 100));
    light->setColor(record.attributes.value("color", std::string{"#FFFFFF"}));
    device = std::move(light);
  } else if (record.deviceType == "speaker") {
    auto speaker = std::make_unique<SmartSpeaker>(record.id, record.name, record.ipAddress);
    speaker->setStatus(parseDeviceStatus(record.status));
    if (record.attributes.value("connected", false)) {
      speaker->connect();
    }
    speaker->setVolume(record.attributes.value("volume", 30));
    device = std::move(speaker);
  } else {
    device = std::make_unique<GenericDevice>(record.id, record.name, record.ipAddress);
    device->setStatus(parseDeviceStatus(record.status));
  }

  if (record.powerOn) {
    device->turnOn();
  }
  return device;
}

DeviceRecord DeviceManager::project(const SmartDevice& device) const {
  DeviceRecord record;
  record.id = device.id();
  record.householdId = householdId_;
  record.name = device.name();
  record.ipAddress = device.ipAddress();
  record.deviceType = std::string{device.type()};
  record.status = std::string{toString(device.status())};
  record.powerOn = device.isOn();
  // Carry the driver configuration through: saveDevice is an upsert.
  if (const auto binding = bindings_.find(device.id()); binding != bindings_.end()) {
    record.metadata = binding->second.metadata;
  }

  // Keep the subtype attributes out of toJson()'s transport keys: the record
  // mirrors the subtype tables.
  const nlohmann::json state = device.toJson();
  if (record.deviceType == "light") {
    record.attributes["brightness"] = state.value("brightness", 100);
    record.attributes["color"] = state.value("color", std::string{"#FFFFFF"});
  } else if (record.deviceType == "speaker") {
    record.attributes["connected"] = state.value("connected", false);
    record.attributes["volume"] = state.value("volume", 30);
    record.attributes["playbackState"] = state.value("playbackState", std::string{"STOPPED"});
    record.attributes["currentTrack"] = state.value("currentTrack", std::string{});
  }
  return record;
}

std::size_t DeviceManager::loadHousehold(const std::string& householdId) {
  std::vector<DeviceRecord> records = database_.listDevices(householdId);

  const std::lock_guard<std::mutex> lock(mutex_);
  householdId_ = householdId;
  devices_.clear();
  bindings_.clear();
  devices_.reserve(records.size());
  for (const DeviceRecord& record : records) {
    devices_.push_back(materialise(record));
    bind(record);
  }
  return devices_.size();
}

std::vector<DeviceRecord> DeviceManager::discover() {
  // The source may hit the network; keep that outside the lock.
  std::vector<DeviceRecord> found = discovery_ ? discovery_() : std::vector<DeviceRecord>{};

  const std::lock_guard<std::mutex> lock(mutex_);
  const auto registered = [this](const DeviceRecord& candidate) {
    if (bindings_.count(candidate.id) != 0) {
      return true;
    }
    // A light added by hand under another id still counts as registered.
    const std::string driver = candidate.metadata.value("driver", std::string{});
    const std::string lightId = candidate.metadata.value("lightId", std::string{});
    if (driver.empty() || lightId.empty()) {
      return false;
    }
    return std::any_of(bindings_.begin(), bindings_.end(), [&](const auto& entry) {
      const nlohmann::json& metadata = entry.second.metadata;
      return metadata.value("driver", std::string{}) == driver &&
             metadata.value("lightId", std::string{}) == lightId;
    });
  };
  found.erase(std::remove_if(found.begin(), found.end(), registered), found.end());
  return found;
}

bool DeviceManager::registerDevice(const DeviceRecord& record) {
  {
    const std::lock_guard<std::mutex> lock(mutex_);
    const bool known = std::any_of(devices_.begin(), devices_.end(),
                                   [&record](const std::unique_ptr<SmartDevice>& device) {
                                     return device->id() == record.id;
                                   });
    if (known) {
      return false;
    }
  }

  if (!database_.saveDevice(record)) {
    return false;
  }

  const std::lock_guard<std::mutex> lock(mutex_);
  if (householdId_.empty()) {
    householdId_ = record.householdId;
  }
  devices_.push_back(materialise(record));
  bind(record);
  return true;
}

bool DeviceManager::unregisterDevice(const std::string& deviceId) {
  {
    const std::lock_guard<std::mutex> lock(mutex_);
    const auto removed = std::remove_if(devices_.begin(), devices_.end(),
                                        [&deviceId](const std::unique_ptr<SmartDevice>& device) {
                                          return device->id() == deviceId;
                                        });
    if (removed == devices_.end()) {
      return false;
    }
    devices_.erase(removed, devices_.end());
    bindings_.erase(deviceId);
  }
  return database_.deleteDevice(deviceId);
}

RenameResult DeviceManager::renameDevice(const std::string& deviceId, std::string_view name,
                                         const std::string& actorId) {
  const auto first = name.find_first_not_of(" \t\r\n");
  const std::string trimmed =
      first == std::string_view::npos
          ? std::string{}
          : std::string{name.substr(first, name.find_last_not_of(" \t\r\n") - first + 1)};
  if (trimmed.empty() || trimmed.size() > MAX_DEVICE_NAME_LENGTH) {
    return RenameResult::InvalidName;
  }

  std::string previous;
  DeviceRecord record;
  {
    const std::lock_guard<std::mutex> lock(mutex_);
    const auto match = std::find_if(devices_.begin(), devices_.end(),
                                    [&deviceId](const std::unique_ptr<SmartDevice>& device) {
                                      return device->id() == deviceId;
                                    });
    if (match == devices_.end()) {
      return RenameResult::UnknownDevice;
    }
    previous = (*match)->name();
    (*match)->setName(trimmed);
    record = project(**match);
  }

  const bool saved = database_.saveDevice(record);
  database_.recordEvent(deviceId, actorId.empty() ? "SYSTEM" : "USER", actorId, "rename",
                        {{"name", trimmed}}, saved);
  if (!saved) {
    const std::lock_guard<std::mutex> lock(mutex_);
    for (const auto& device : devices_) {
      if (device->id() == deviceId) {
        device->setName(previous);
      }
    }
    return RenameResult::StorageFailed;
  }
  return RenameResult::Renamed;
}

SmartDevice* DeviceManager::findById(const std::string& deviceId) {
  const std::lock_guard<std::mutex> lock(mutex_);
  const auto match = std::find_if(
      devices_.begin(), devices_.end(),
      [&deviceId](const std::unique_ptr<SmartDevice>& device) { return device->id() == deviceId; });
  return match == devices_.end() ? nullptr : match->get();
}

CommandResult DeviceManager::apply(SmartDevice& device, const std::string& command,
                                   const nlohmann::json& params) {
  if (command == "turnOn") {
    return device.turnOn() ? CommandResult::success()
                           : CommandResult::failure("device unreachable");
  }
  if (command == "turnOff") {
    return device.turnOff() ? CommandResult::success()
                            : CommandResult::failure("device unreachable");
  }

  if (auto* light = dynamic_cast<Light*>(&device); light != nullptr) {
    if (command == "setBrightness") {
      if (!params.contains("level") || !params.at("level").is_number_integer()) {
        return CommandResult::failure("setBrightness requires an integer 'level'");
      }
      return light->setBrightness(params.at("level").get<int>())
                 ? CommandResult::success()
                 : CommandResult::failure("device unreachable");
    }
    if (command == "setColor") {
      if (!params.contains("color") || !params.at("color").is_string()) {
        return CommandResult::failure("setColor requires a '#RRGGBB' 'color'");
      }
      return light->setColor(params.at("color").get<std::string>())
                 ? CommandResult::success()
                 : CommandResult::failure("invalid colour or device unreachable");
    }
  }

  if (auto* speaker = dynamic_cast<SmartSpeaker*>(&device); speaker != nullptr) {
    if (command == "play") {
      return speaker->play(params.value("track", std::string{}))
                 ? CommandResult::success()
                 : CommandResult::failure("speaker not connected");
    }
    if (command == "pause") {
      return speaker->pause() ? CommandResult::success()
                              : CommandResult::failure("speaker not connected");
    }
    if (command == "setVolume") {
      if (!params.contains("level") || !params.at("level").is_number_integer()) {
        return CommandResult::failure("setVolume requires an integer 'level'");
      }
      return speaker->setVolume(params.at("level").get<int>())
                 ? CommandResult::success()
                 : CommandResult::failure("speaker not connected");
    }
    if (command == "connect") {
      return speaker->connect() ? CommandResult::success()
                                : CommandResult::failure("device unreachable");
    }
  }

  return CommandResult::failure("unsupported command '" + command + "' for type " +
                                std::string{device.type()});
}

CommandResult DeviceManager::sendCommand(const std::string& deviceId, const std::string& command,
                                         const nlohmann::json& params, const std::string& actorId,
                                         const std::string& actorType) {
  CommandResult result;
  DeviceRecord record;
  nlohmann::json before;
  nlohmann::json after;
  StateObserver observer;

  {
    const std::lock_guard<std::mutex> lock(mutex_);
    const auto match = std::find_if(devices_.begin(), devices_.end(),
                                    [&deviceId](const std::unique_ptr<SmartDevice>& device) {
                                      return device->id() == deviceId;
                                    });
    if (match == devices_.end()) {
      return CommandResult::failure("unknown device " + deviceId);
    }
    SmartDevice& device = **match;
    before = device.toJson();
    const auto binding = bindings_.find(deviceId);
    DeviceDriver* driver = binding == bindings_.end() ? nullptr : binding->second.driver.get();
    if (driver == nullptr) {
      result = apply(device, command, params);
    } else {
      const DriverResult sent = driver->send(device, command, params);
      switch (sent.status) {
        case DriverStatus::Sent:
          // The device answered, so it is reachable whatever was stored.
          device.setStatus(DeviceStatus::Online);
          result = apply(device, command, params);
          break;
        case DriverStatus::Unreachable:
          device.setStatus(DeviceStatus::Error);
          result = CommandResult::failure(sent.error);
          break;
        case DriverStatus::Rejected:
          result = CommandResult::failure(sent.error);
          break;
      }
    }
    after = device.toJson();
    observer = observer_;
    record = project(**match);
  }

  // Persist outside the lock: the device state is already settled. A failed
  // command can still change the status (driver unreachable -> ERROR).
  if (result.ok || before.value("status", "") != after.value("status", "")) {
    database_.saveDevice(record);
  }
  database_.recordEvent(deviceId, actorId.empty() ? "SYSTEM" : actorType, actorId, command, params,
                        result.ok);
  if (result.ok && before != after && observer) {
    observer(before, after);
  }
  return result;
}

void DeviceManager::setStateObserver(StateObserver observer) {
  const std::lock_guard<std::mutex> lock(mutex_);
  observer_ = std::move(observer);
}

std::optional<nlohmann::json> DeviceManager::state(const std::string& deviceId) const {
  const std::lock_guard<std::mutex> lock(mutex_);
  for (const auto& device : devices_) {
    if (device->id() == deviceId) {
      return device->toJson();
    }
  }
  return std::nullopt;
}

nlohmann::json DeviceManager::snapshot() const {
  nlohmann::json devices = nlohmann::json::array();
  const std::lock_guard<std::mutex> lock(mutex_);
  for (const std::unique_ptr<SmartDevice>& device : devices_) {
    devices.push_back(device->toJson());
  }
  return devices;
}

std::size_t DeviceManager::size() const {
  const std::lock_guard<std::mutex> lock(mutex_);
  return devices_.size();
}

}  // namespace homestr
