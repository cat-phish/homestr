#pragma once

#include <string>
#include <string_view>

#include <nlohmann/json.hpp>

namespace homestr {

enum class DeviceStatus { Online, Offline, Error };

std::string_view toString(DeviceStatus status);

DeviceStatus parseDeviceStatus(std::string_view text);

// Abstract base class of the device hierarchy (see specs/domain-model.md).
// Everything shared by every registered device lives here; `type()` is pure
// virtual, so only a concrete specialisation can be instantiated. Use
// GenericDevice for a device with no richer capabilities than on/off.
class SmartDevice {
 public:
  SmartDevice(std::string id, std::string name, std::string ipAddress);

  virtual ~SmartDevice() = default;

  SmartDevice(const SmartDevice&) = delete;
  SmartDevice& operator=(const SmartDevice&) = delete;

  const std::string& id() const noexcept { return id_; }

  const std::string& name() const noexcept { return name_; }

  const std::string& ipAddress() const noexcept { return ipAddress_; }

  DeviceStatus status() const noexcept { return status_; }

  bool isOn() const noexcept { return powerOn_; }

  void setStatus(DeviceStatus status) noexcept { status_ = status; }

  // Device type discriminator persisted in `devices.device_type`.
  virtual std::string_view type() const noexcept = 0;

  // Commands. Return false when the device is not reachable, so the caller can
  // record the failure in `device_events` instead of reporting a false success.
  virtual bool turnOn();
  virtual bool turnOff();

  // Full state as returned by GET /devices/{id}. Subclasses extend the object
  // produced by SmartDevice::toJson().
  virtual nlohmann::json toJson() const;

 protected:
  bool reachable() const noexcept { return status_ == DeviceStatus::Online; }

 private:
  std::string id_;
  std::string name_;
  std::string ipAddress_;
  DeviceStatus status_ = DeviceStatus::Offline;
  bool powerOn_ = false;
};

// A registered device whose only capability is power on/off: the concrete
// stand-in for `device_type = 'generic'`.
class GenericDevice : public SmartDevice {
 public:
  GenericDevice(std::string id, std::string name, std::string ipAddress);

  std::string_view type() const noexcept override { return "generic"; }
};

}  // namespace homestr
