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

DeviceManager::DeviceManager(Database& database) : database_(database) {}

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
  devices_.reserve(records.size());
  for (const DeviceRecord& record : records) {
    devices_.push_back(materialise(record));
  }
  return devices_.size();
}

std::vector<DeviceRecord> DeviceManager::discover() {
  // TODO(backend): replace the cache with an mDNS + SSDP sweep of the LAN.
  // Spec: specs/domain-model.md "Device discovery" workflow.
  const std::lock_guard<std::mutex> lock(mutex_);
  return discovered_;
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
  }
  return database_.deleteDevice(deviceId);
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
                                         const nlohmann::json& params, const std::string& actorId) {
  CommandResult result;
  DeviceRecord record;

  {
    const std::lock_guard<std::mutex> lock(mutex_);
    const auto match = std::find_if(devices_.begin(), devices_.end(),
                                    [&deviceId](const std::unique_ptr<SmartDevice>& device) {
                                      return device->id() == deviceId;
                                    });
    if (match == devices_.end()) {
      return CommandResult::failure("unknown device " + deviceId);
    }
    result = apply(**match, command, params);
    record = project(**match);
  }

  // Persist outside the lock: the device state is already settled.
  if (result.ok) {
    database_.saveDevice(record);
  }
  database_.recordEvent(deviceId, actorId.empty() ? "SYSTEM" : "USER", actorId, command, params,
                        result.ok);
  return result;
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
