#include "homestr/services/device_driver.hpp"

#include <iostream>
#include <memory>
#include <string>
#include <vector>

#include "homestr/services/database.hpp"
#include "homestr/services/hue_driver.hpp"

namespace homestr {

DriverFactory defaultDriverFactory() {
  return [config = HueConfig::fromEnvironment()](
             const DeviceRecord& record) -> std::unique_ptr<DeviceDriver> {
    if (!record.metadata.is_object()) {
      return nullptr;
    }
    const std::string driver = record.metadata.value("driver", std::string{});
    if (driver.empty()) {
      return nullptr;
    }
    if (driver == "hue" && record.deviceType == "light") {
      const std::string lightId = record.metadata.value("lightId", std::string{});
      if (!config.valid() || lightId.empty()) {
        std::cerr << "[driver] device " << record.id
                  << ": hue needs HUE_BRIDGE_URL, HUE_USERNAME and metadata.lightId; "
                     "staying simulated\n";
        return nullptr;
      }
      return std::make_unique<HueDriver>(config, lightId, record.metadata.value("color", true));
    }
    std::cerr << "[driver] device " << record.id << ": unknown driver '" << driver << "' for "
              << record.deviceType << "; staying simulated\n";
    return nullptr;
  };
}

DiscoverySource defaultDiscoverySource() {
  return [config = HueConfig::fromEnvironment()]() -> std::vector<DeviceRecord> {
    return discoverHueLights(config);
  };
}

}  // namespace homestr
