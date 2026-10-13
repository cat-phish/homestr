#include "homestr/models/smart_device.hpp"

#include <string>
#include <string_view>
#include <utility>

#include <nlohmann/json.hpp>

namespace homestr {

std::string_view toString(DeviceStatus status) {
  switch (status) {
    case DeviceStatus::Online:
      return "ONLINE";
    case DeviceStatus::Offline:
      return "OFFLINE";
    case DeviceStatus::Error:
      return "ERROR";
  }
  return "OFFLINE";
}

DeviceStatus parseDeviceStatus(std::string_view text) {
  if (text == "ONLINE") {
    return DeviceStatus::Online;
  }
  if (text == "ERROR") {
    return DeviceStatus::Error;
  }
  return DeviceStatus::Offline;
}

SmartDevice::SmartDevice(std::string id, std::string name, std::string ipAddress)
    : id_(std::move(id)), name_(std::move(name)), ipAddress_(std::move(ipAddress)) {}

bool SmartDevice::turnOn() {
  if (!reachable()) {
    return false;
  }
  powerOn_ = true;
  return true;
}

bool SmartDevice::turnOff() {
  if (!reachable()) {
    return false;
  }
  powerOn_ = false;
  return true;
}

nlohmann::json SmartDevice::toJson() const {
  return {{"id", id_},
          {"name", name_},
          {"ipAddress", ipAddress_},
          {"type", type()},
          {"status", toString(status_)},
          {"powerOn", powerOn_}};
}

GenericDevice::GenericDevice(std::string id, std::string name, std::string ipAddress)
    : SmartDevice(std::move(id), std::move(name), std::move(ipAddress)) {}

}  // namespace homestr
