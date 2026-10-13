#include "homestr/services/database.hpp"

#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include "homestr/models/user.hpp"

#if HOMESTR_HAS_MARIADB
#include <mariadb/conncpp.hpp>
#endif

namespace homestr {
namespace {

std::string envOr(const char* key, std::string fallback) {
  const char* value = std::getenv(key);
  return (value != nullptr && *value != '\0') ? std::string{value} : std::move(fallback);
}

}  // namespace

std::string DatabaseConfig::connectionUrl() const {
  return "jdbc:mariadb://" + host + ":" + std::to_string(port) + "/" + schema;
}

DatabaseConfig DatabaseConfig::fromEnvironment() {
  DatabaseConfig config;
  config.host = envOr("DB_HOST", config.host);
  config.port = std::atoi(envOr("DB_PORT", "3306").c_str());
  config.schema = envOr("MARIADB_DATABASE", config.schema);
  config.user = envOr("MARIADB_USER", config.user);
  config.password = envOr("MARIADB_PASSWORD", config.password);
  return config;
}

User UserRecord::toUser() const {
  return User{id, name, email, passwordHash, parseRole(role)};
}

// ---------------------------------------------------------------------------
// InMemoryDatabase
// ---------------------------------------------------------------------------

bool InMemoryDatabase::connect() {
  connected_ = true;
  return true;
}

bool InMemoryDatabase::isConnected() const {
  return connected_;
}

void InMemoryDatabase::addUser(UserRecord record) {
  users_.push_back(std::move(record));
}

std::optional<UserRecord> InMemoryDatabase::findUserByEmail(const std::string& email) {
  const auto match = std::find_if(users_.begin(), users_.end(),
                                  [&email](const UserRecord& user) { return user.email == email; });
  if (match == users_.end()) {
    return std::nullopt;
  }
  return *match;
}

std::vector<DeviceRecord> InMemoryDatabase::listDevices(const std::string& householdId) {
  std::vector<DeviceRecord> result;
  for (const DeviceRecord& device : devices_) {
    if (householdId.empty() || device.householdId == householdId) {
      result.push_back(device);
    }
  }
  return result;
}

std::optional<DeviceRecord> InMemoryDatabase::findDevice(const std::string& deviceId) {
  const auto match =
      std::find_if(devices_.begin(), devices_.end(),
                   [&deviceId](const DeviceRecord& device) { return device.id == deviceId; });
  if (match == devices_.end()) {
    return std::nullopt;
  }
  return *match;
}

bool InMemoryDatabase::saveDevice(const DeviceRecord& record) {
  const auto match =
      std::find_if(devices_.begin(), devices_.end(),
                   [&record](const DeviceRecord& device) { return device.id == record.id; });
  if (match == devices_.end()) {
    devices_.push_back(record);
  } else {
    *match = record;
  }
  return true;
}

bool InMemoryDatabase::deleteDevice(const std::string& deviceId) {
  const auto removed =
      std::remove_if(devices_.begin(), devices_.end(),
                     [&deviceId](const DeviceRecord& device) { return device.id == deviceId; });
  if (removed == devices_.end()) {
    return false;
  }
  devices_.erase(removed, devices_.end());
  return true;
}

bool InMemoryDatabase::recordEvent(const std::string& deviceId, const std::string& actorType,
                                   const std::string& actorId, const std::string& command,
                                   const nlohmann::json& payload, bool succeeded) {
  events_.push_back({{"deviceId", deviceId},
                     {"actorType", actorType},
                     {"actorId", actorId},
                     {"command", command},
                     {"payload", payload},
                     {"succeeded", succeeded}});
  return true;
}

#if HOMESTR_HAS_MARIADB
// ---------------------------------------------------------------------------
// MariaDbDatabase -- MariaDB Connector/C++ implementation.
// ---------------------------------------------------------------------------
namespace {

class MariaDbDatabase : public Database {
 public:
  explicit MariaDbDatabase(DatabaseConfig config) : config_(std::move(config)) {}

  bool connect() override {
    try {
      sql::Driver* driver = sql::mariadb::get_driver_instance();
      sql::Properties properties({{"user", config_.user}, {"password", config_.password}});
      connection_.reset(driver->connect(config_.connectionUrl(), properties));
      return connection_ != nullptr;
    } catch (const sql::SQLException& error) {
      std::cerr << "[db] connect failed: " << error.what() << '\n';
      connection_.reset();
      return false;
    }
  }

  bool isConnected() const override { return connection_ != nullptr && !connection_->isClosed(); }

  std::optional<UserRecord> findUserByEmail(const std::string& email) override {
    try {
      std::unique_ptr<sql::PreparedStatement> statement(connection_->prepareStatement(
          "SELECT id, name, email, password_hash, role FROM users WHERE "
          "email = ?"));
      statement->setString(1, email);
      std::unique_ptr<sql::ResultSet> rows(statement->executeQuery());
      if (!rows->next()) {
        return std::nullopt;
      }
      UserRecord record;
      record.id = rows->getString("id").c_str();
      record.name = rows->getString("name").c_str();
      record.email = rows->getString("email").c_str();
      record.passwordHash = rows->getString("password_hash").c_str();
      record.role = rows->getString("role").c_str();
      return record;
    } catch (const sql::SQLException& error) {
      std::cerr << "[db] findUserByEmail: " << error.what() << '\n';
      return std::nullopt;
    }
  }

  std::vector<DeviceRecord> listDevices(const std::string& householdId) override {
    std::vector<DeviceRecord> result;
    try {
      std::unique_ptr<sql::PreparedStatement> statement(
          connection_->prepareStatement(DEVICE_SELECT + std::string{" WHERE d.household_id = ?"}));
      statement->setString(1, householdId);
      std::unique_ptr<sql::ResultSet> rows(statement->executeQuery());
      while (rows->next()) {
        result.push_back(readDevice(*rows));
      }
    } catch (const sql::SQLException& error) {
      std::cerr << "[db] listDevices: " << error.what() << '\n';
    }
    return result;
  }

  std::optional<DeviceRecord> findDevice(const std::string& deviceId) override {
    try {
      std::unique_ptr<sql::PreparedStatement> statement(
          connection_->prepareStatement(DEVICE_SELECT + std::string{" WHERE d.id = ?"}));
      statement->setString(1, deviceId);
      std::unique_ptr<sql::ResultSet> rows(statement->executeQuery());
      if (!rows->next()) {
        return std::nullopt;
      }
      return readDevice(*rows);
    } catch (const sql::SQLException& error) {
      std::cerr << "[db] findDevice: " << error.what() << '\n';
      return std::nullopt;
    }
  }

  bool saveDevice(const DeviceRecord& record) override {
    try {
      std::unique_ptr<sql::PreparedStatement> statement(connection_->prepareStatement(
          "INSERT INTO devices (id, household_id, name, ip_address, "
          "device_type, status, "
          "power_on, metadata) VALUES (?, ?, ?, ?, ?, ?, ?, ?) "
          "ON DUPLICATE KEY UPDATE household_id = VALUES(household_id), "
          "name = VALUES(name), "
          "ip_address = VALUES(ip_address), status = VALUES(status), "
          "power_on = VALUES(power_on), metadata = VALUES(metadata)"));
      statement->setString(1, record.id);
      statement->setString(2, record.householdId);
      statement->setString(3, record.name);
      statement->setString(4, record.ipAddress);
      statement->setString(5, record.deviceType);
      statement->setString(6, record.status);
      statement->setBoolean(7, record.powerOn);
      statement->setString(8, record.attributes.dump());
      statement->executeUpdate();
      return saveSubtype(record);
    } catch (const sql::SQLException& error) {
      std::cerr << "[db] saveDevice: " << error.what() << '\n';
      return false;
    }
  }

  bool deleteDevice(const std::string& deviceId) override {
    try {
      // The subtype rows and audit events cascade from devices.id.
      std::unique_ptr<sql::PreparedStatement> statement(
          connection_->prepareStatement("DELETE FROM devices WHERE id = ?"));
      statement->setString(1, deviceId);
      return statement->executeUpdate() > 0;
    } catch (const sql::SQLException& error) {
      std::cerr << "[db] deleteDevice: " << error.what() << '\n';
      return false;
    }
  }

  bool recordEvent(const std::string& deviceId, const std::string& actorType,
                   const std::string& actorId, const std::string& command,
                   const nlohmann::json& payload, bool succeeded) override {
    try {
      std::unique_ptr<sql::PreparedStatement> statement(connection_->prepareStatement(
          "INSERT INTO device_events (device_id, actor_type, actor_id, "
          "command, payload, "
          "succeeded) VALUES (?, ?, ?, ?, ?, ?)"));
      statement->setString(1, deviceId);
      statement->setString(2, actorType);
      statement->setString(3, actorId);
      statement->setString(4, command);
      statement->setString(5, payload.dump());
      statement->setBoolean(6, succeeded);
      statement->executeUpdate();
      return true;
    } catch (const sql::SQLException& error) {
      std::cerr << "[db] recordEvent: " << error.what() << '\n';
      return false;
    }
  }

 private:
  // Subtype columns come back NULL for the other device types; readDevice only
  // reads the ones matching device_type.
  static constexpr const char* DEVICE_SELECT =
      "SELECT d.id, d.household_id, d.name, d.ip_address, d.device_type, "
      "d.status, d.power_on, "
      "l.brightness, l.color, s.connected, s.volume, s.playback_state, "
      "s.current_track "
      "FROM devices d "
      "LEFT JOIN device_light l ON l.device_id = d.id "
      "LEFT JOIN device_speaker s ON s.device_id = d.id";

  static DeviceRecord readDevice(sql::ResultSet& rows) {
    DeviceRecord record;
    record.id = rows.getString("id").c_str();
    record.householdId = rows.isNull("household_id") ? "" : rows.getString("household_id").c_str();
    record.name = rows.getString("name").c_str();
    record.ipAddress = rows.getString("ip_address").c_str();
    record.deviceType = rows.getString("device_type").c_str();
    record.status = rows.getString("status").c_str();
    record.powerOn = rows.getBoolean("power_on");

    if (record.deviceType == "light" && !rows.isNull("brightness")) {
      record.attributes["brightness"] = rows.getInt("brightness");
      record.attributes["color"] = rows.getString("color").c_str();
    } else if (record.deviceType == "speaker" && !rows.isNull("volume")) {
      record.attributes["connected"] = rows.getBoolean("connected");
      record.attributes["volume"] = rows.getInt("volume");
      record.attributes["playbackState"] = rows.getString("playback_state").c_str();
      record.attributes["currentTrack"] =
          rows.isNull("current_track") ? "" : rows.getString("current_track").c_str();
    }
    return record;
  }

  bool saveSubtype(const DeviceRecord& record) {
    if (record.deviceType == "light") {
      std::unique_ptr<sql::PreparedStatement> statement(connection_->prepareStatement(
          "INSERT INTO device_light (device_id, brightness, color) VALUES "
          "(?, ?, ?) "
          "ON DUPLICATE KEY UPDATE brightness = VALUES(brightness), color "
          "= VALUES(color)"));
      statement->setString(1, record.id);
      statement->setInt(2, record.attributes.value("brightness", 100));
      statement->setString(3, record.attributes.value("color", std::string{"#FFFFFF"}));
      statement->executeUpdate();
      return true;
    }

    if (record.deviceType == "speaker") {
      std::unique_ptr<sql::PreparedStatement> statement(connection_->prepareStatement(
          "INSERT INTO device_speaker (device_id, connected, volume, "
          "playback_state, "
          "current_track) VALUES (?, ?, ?, ?, ?) "
          "ON DUPLICATE KEY UPDATE connected = VALUES(connected), volume = "
          "VALUES(volume), "
          "playback_state = VALUES(playback_state), current_track = "
          "VALUES(current_track)"));
      statement->setString(1, record.id);
      statement->setBoolean(2, record.attributes.value("connected", false));
      statement->setInt(3, record.attributes.value("volume", 30));
      statement->setString(4, record.attributes.value("playbackState", std::string{"STOPPED"}));
      statement->setString(5, record.attributes.value("currentTrack", std::string{}));
      statement->executeUpdate();
      return true;
    }

    return true;
  }

  DatabaseConfig config_;
  std::unique_ptr<sql::Connection> connection_;
};

}  // namespace
#endif  // HOMESTR_HAS_MARIADB

std::unique_ptr<Database> makeDatabase(const DatabaseConfig& config) {
#if HOMESTR_HAS_MARIADB
  return std::make_unique<MariaDbDatabase>(config);
#else
  (void)config;
  std::cerr << "[db] built without MariaDB Connector/C++; using the in-memory "
               "repository\n";
  return std::make_unique<InMemoryDatabase>();
#endif
}

}  // namespace homestr
