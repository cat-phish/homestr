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

AutomationRule RuleRecord::toRule() const {
  AutomationRule rule{id, name, parseTriggerType(triggerType), triggerPayload};
  rule.setLastFiredAt(lastFiredAt);
  for (const auto& action : actions) {
    rule.addAction(action);
  }
  if (!enabled) {
    rule.disable();
  }
  return rule;
}

RuleRecord RuleRecord::fromRule(const AutomationRule& rule, const std::string& householdId) {
  return {rule.id(),
          householdId,
          rule.name(),
          rule.enabled(),
          std::string{toString(rule.triggerType())},
          rule.triggerPayload(),
          rule.actions(),
          rule.lastFiredAt()};
}

// ---------------------------------------------------------------------------
// InMemoryDatabase
// ---------------------------------------------------------------------------

bool InMemoryDatabase::connect() {
  const std::lock_guard<std::recursive_mutex> lock(mutex_);
  connected_ = true;
  return true;
}

bool InMemoryDatabase::isConnected() const {
  const std::lock_guard<std::recursive_mutex> lock(mutex_);
  return connected_;
}

void InMemoryDatabase::addUser(UserRecord record) {
  const std::lock_guard<std::recursive_mutex> lock(mutex_);
  users_.push_back(std::move(record));
}

std::optional<UserRecord> InMemoryDatabase::findUserByEmail(const std::string& email) {
  const std::lock_guard<std::recursive_mutex> lock(mutex_);
  const auto match = std::find_if(users_.begin(), users_.end(),
                                  [&email](const UserRecord& user) { return user.email == email; });
  if (match == users_.end()) {
    return std::nullopt;
  }
  return *match;
}

bool InMemoryDatabase::updatePasswordHash(const std::string& email, const std::string& hash) {
  const std::lock_guard<std::recursive_mutex> lock(mutex_);
  for (auto& user : users_) {
    if (user.email == email) {
      user.passwordHash = hash;
      std::erase_if(sessions_, [&user](const auto& session) { return session.userId == user.id; });
      return true;
    }
  }
  return false;
}

bool InMemoryDatabase::saveSession(const SessionRecord& session, const std::string& verifiedHash) {
  const std::lock_guard<std::recursive_mutex> lock(mutex_);
  const bool current = std::any_of(users_.begin(), users_.end(), [&](const auto& user) {
    return user.id == session.userId && user.passwordHash == verifiedHash;
  });
  if (!current) {
    return false;
  }
  sessions_.push_back(session);
  return true;
}

std::optional<UserRecord> InMemoryDatabase::findSessionUser(const std::string& tokenHash,
                                                            std::int64_t now) {
  const std::lock_guard<std::recursive_mutex> lock(mutex_);
  for (const auto& session : sessions_) {
    if (session.tokenHash == tokenHash && session.expiresAt > now) {
      for (const auto& user : users_) {
        if (user.id == session.userId) {
          return user;
        }
      }
    }
  }
  return std::nullopt;
}

bool InMemoryDatabase::deleteSession(const std::string& tokenHash) {
  const std::lock_guard<std::recursive_mutex> lock(mutex_);
  std::erase_if(sessions_,
                [&tokenHash](const auto& session) { return session.tokenHash == tokenHash; });
  return true;
}

std::optional<std::vector<RuleRecord>> InMemoryDatabase::listRules(const std::string& householdId) {
  const std::lock_guard<std::recursive_mutex> lock(mutex_);
  std::vector<RuleRecord> records;
  for (const auto& record : rules_) {
    if (record.householdId == householdId) {
      records.push_back(record);
    }
  }
  std::sort(records.begin(), records.end(),
            [](const auto& left, const auto& right) { return left.id < right.id; });
  return records;
}

bool InMemoryDatabase::saveRule(const RuleRecord& record) {
  const std::lock_guard<std::recursive_mutex> lock(mutex_);
  const auto match = std::find_if(rules_.begin(), rules_.end(),
                                  [&](const auto& existing) { return existing.id == record.id; });
  if (match == rules_.end()) {
    rules_.push_back(record);
  } else if (match->householdId == record.householdId) {
    const auto lastFiredAt = match->lastFiredAt;
    *match = record;
    match->lastFiredAt = lastFiredAt;
  } else {
    return false;
  }
  return true;
}

bool InMemoryDatabase::claimScheduledRun(const std::string& householdId, const std::string& ruleId,
                                         std::int64_t minute) {
  const std::lock_guard<std::recursive_mutex> lock(mutex_);
  for (auto& rule : rules_) {
    if (rule.householdId == householdId && rule.id == ruleId && rule.enabled &&
        rule.triggerType == "SCHEDULE" && (!rule.lastFiredAt || *rule.lastFiredAt < minute)) {
      rule.lastFiredAt = minute;
      return true;
    }
  }
  return false;
}

bool InMemoryDatabase::deleteRule(const std::string& householdId, const std::string& ruleId) {
  const std::lock_guard<std::recursive_mutex> lock(mutex_);
  return std::erase_if(rules_, [&](const auto& rule) {
           return rule.id == ruleId && rule.householdId == householdId;
         }) > 0;
}

std::vector<DeviceRecord> InMemoryDatabase::listDevices(const std::string& householdId) {
  const std::lock_guard<std::recursive_mutex> lock(mutex_);
  std::vector<DeviceRecord> result;
  for (const DeviceRecord& device : devices_) {
    if (householdId.empty() || device.householdId == householdId) {
      result.push_back(device);
    }
  }
  return result;
}

std::optional<DeviceRecord> InMemoryDatabase::findDevice(const std::string& deviceId) {
  const std::lock_guard<std::recursive_mutex> lock(mutex_);
  const auto match =
      std::find_if(devices_.begin(), devices_.end(),
                   [&deviceId](const DeviceRecord& device) { return device.id == deviceId; });
  if (match == devices_.end()) {
    return std::nullopt;
  }
  return *match;
}

bool InMemoryDatabase::saveDevice(const DeviceRecord& record) {
  const std::lock_guard<std::recursive_mutex> lock(mutex_);
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
  const std::lock_guard<std::recursive_mutex> lock(mutex_);
  const auto removed =
      std::remove_if(devices_.begin(), devices_.end(),
                     [&deviceId](const DeviceRecord& device) { return device.id == deviceId; });
  if (removed == devices_.end()) {
    return false;
  }
  devices_.erase(removed, devices_.end());
  // Mirror the schema's ON DELETE CASCADE on automation_actions.device_id.
  for (RuleRecord& rule : rules_) {
    std::erase_if(rule.actions,
                  [&deviceId](const RuleAction& action) { return action.deviceId == deviceId; });
  }
  return true;
}

bool InMemoryDatabase::recordEvent(const std::string& deviceId, const std::string& actorType,
                                   const std::string& actorId, const std::string& command,
                                   const nlohmann::json& payload, bool succeeded) {
  const std::lock_guard<std::recursive_mutex> lock(mutex_);
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
    const std::lock_guard<std::recursive_mutex> lock(mutex_);
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

  bool isConnected() const override {
    const std::lock_guard<std::recursive_mutex> lock(mutex_);
    return connection_ != nullptr && !connection_->isClosed();
  }

  std::optional<UserRecord> findUserByEmail(const std::string& email) override {
    const std::lock_guard<std::recursive_mutex> lock(mutex_);
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

  bool updatePasswordHash(const std::string& email, const std::string& hash) override {
    const std::lock_guard<std::recursive_mutex> lock(mutex_);
    try {
      connection_->setAutoCommit(false);
      std::unique_ptr<sql::PreparedStatement> update(
          connection_->prepareStatement("UPDATE users SET password_hash = ? WHERE email = ?"));
      update->setString(1, hash);
      update->setString(2, email);
      const bool changed = update->executeUpdate() > 0;
      std::unique_ptr<sql::PreparedStatement> revoke(connection_->prepareStatement(
          "DELETE FROM sessions WHERE user_id IN (SELECT id FROM users WHERE email = ?)"));
      revoke->setString(1, email);
      revoke->executeUpdate();
      connection_->commit();
      connection_->setAutoCommit(true);
      return changed;
    } catch (const sql::SQLException&) {
      try {
        connection_->rollback();
        connection_->setAutoCommit(true);
      } catch (const sql::SQLException&) {
      }
      std::cerr << "[db] password update failed\n";
      return false;
    }
  }

  bool saveSession(const SessionRecord& session, const std::string& verifiedHash) override {
    const std::lock_guard<std::recursive_mutex> lock(mutex_);
    try {
      std::unique_ptr<sql::PreparedStatement> statement(
          connection_->prepareStatement("INSERT INTO sessions (token_hash, user_id, expires_at) "
                                        "SELECT ?, id, FROM_UNIXTIME(?) FROM users WHERE id = ? "
                                        "AND BINARY password_hash = BINARY ?"));
      statement->setString(1, session.tokenHash);
      statement->setInt64(2, session.expiresAt);
      statement->setString(3, session.userId);
      statement->setString(4, verifiedHash);
      return statement->executeUpdate() == 1;
    } catch (const sql::SQLException&) {
      std::cerr << "[db] session write failed\n";
      return false;
    }
  }

  std::optional<UserRecord> findSessionUser(const std::string& tokenHash,
                                            std::int64_t now) override {
    const std::lock_guard<std::recursive_mutex> lock(mutex_);
    try {
      std::unique_ptr<sql::PreparedStatement> statement(connection_->prepareStatement(
          "SELECT u.id, u.name, u.email, u.password_hash, u.role FROM users u "
          "JOIN sessions s ON s.user_id = u.id WHERE s.token_hash = ? AND s.expires_at > "
          "FROM_UNIXTIME(?)"));
      statement->setString(1, tokenHash);
      statement->setInt64(2, now);
      std::unique_ptr<sql::ResultSet> rows(statement->executeQuery());
      if (!rows->next()) {
        return std::nullopt;
      }
      return UserRecord{rows->getString("id").c_str(), rows->getString("name").c_str(),
                        rows->getString("email").c_str(), rows->getString("password_hash").c_str(),
                        rows->getString("role").c_str()};
    } catch (const sql::SQLException&) {
      std::cerr << "[db] session lookup failed\n";
      return std::nullopt;
    }
  }

  bool deleteSession(const std::string& tokenHash) override {
    const std::lock_guard<std::recursive_mutex> lock(mutex_);
    try {
      std::unique_ptr<sql::PreparedStatement> statement(
          connection_->prepareStatement("DELETE FROM sessions WHERE token_hash = ?"));
      statement->setString(1, tokenHash);
      statement->executeUpdate();
      return true;
    } catch (const sql::SQLException&) {
      std::cerr << "[db] session deletion failed\n";
      return false;
    }
  }

  std::optional<std::vector<RuleRecord>> listRules(const std::string& householdId) override {
    const std::lock_guard<std::recursive_mutex> lock(mutex_);
    try {
      std::unique_ptr<sql::PreparedStatement> statement(connection_->prepareStatement(
          "SELECT r.id, r.household_id, r.name, r.enabled, r.trigger_type, r.trigger_payload, "
          "UNIX_TIMESTAMP(r.last_fired_at) AS last_fired_at, "
          "a.id AS action_id, a.device_id, a.command, a.command_params "
          "FROM automation_rules r LEFT JOIN automation_actions a ON a.rule_id = r.id "
          "WHERE r.household_id = ? ORDER BY r.id, a.sequence_no"));
      statement->setString(1, householdId);
      std::unique_ptr<sql::ResultSet> rows(statement->executeQuery());
      std::vector<RuleRecord> records;
      while (rows->next()) {
        const std::string id = rows->getString("id").c_str();
        if (records.empty() || records.back().id != id) {
          RuleRecord record;
          record.id = id;
          record.householdId = rows->getString("household_id").c_str();
          record.name = rows->getString("name").c_str();
          record.enabled = rows->getBoolean("enabled");
          if (!rows->isNull("last_fired_at")) {
            record.lastFiredAt = rows->getInt64("last_fired_at");
          }
          record.triggerType = rows->getString("trigger_type").c_str();
          record.triggerPayload = nlohmann::json::parse(rows->getString("trigger_payload").c_str());
          records.push_back(std::move(record));
        }
        if (!rows->isNull("action_id")) {
          records.back().actions.push_back(
              {rows->getString("device_id").c_str(), rows->getString("command").c_str(),
               rows->isNull("command_params")
                   ? nlohmann::json::object()
                   : nlohmann::json::parse(rows->getString("command_params").c_str())});
        }
      }
      return records;
    } catch (const sql::SQLException&) {
      std::cerr << "[db] rule read failed\n";
    } catch (const nlohmann::json::exception&) {
      std::cerr << "[db] invalid rule JSON\n";
    }
    return std::nullopt;
  }

  bool saveRule(const RuleRecord& record) override {
    const std::lock_guard<std::recursive_mutex> lock(mutex_);
    try {
      connection_->setAutoCommit(false);
      {
        std::unique_ptr<sql::PreparedStatement> check(connection_->prepareStatement(
            "SELECT household_id FROM automation_rules WHERE id = ? FOR UPDATE"));
        check->setString(1, record.id);
        std::unique_ptr<sql::ResultSet> rows(check->executeQuery());
        if (rows->next() &&
            std::string{rows->getString("household_id").c_str()} != record.householdId) {
          connection_->rollback();
          connection_->setAutoCommit(true);
          return false;
        }
      }
      std::unique_ptr<sql::PreparedStatement> rule(connection_->prepareStatement(
          "INSERT INTO automation_rules (id, household_id, name, enabled, trigger_type, "
          "trigger_payload) "
          "VALUES (?, ?, ?, ?, ?, ?) ON DUPLICATE KEY UPDATE name = VALUES(name), "
          "enabled = VALUES(enabled), trigger_type = VALUES(trigger_type), "
          "trigger_payload = VALUES(trigger_payload)"));
      rule->setString(1, record.id);
      rule->setString(2, record.householdId);
      rule->setString(3, record.name);
      rule->setBoolean(4, record.enabled);
      rule->setString(5, record.triggerType);
      rule->setString(6, record.triggerPayload.dump());
      rule->executeUpdate();
      std::unique_ptr<sql::PreparedStatement> clear(
          connection_->prepareStatement("DELETE FROM automation_actions WHERE rule_id = ?"));
      clear->setString(1, record.id);
      clear->executeUpdate();
      std::unique_ptr<sql::PreparedStatement> action(
          connection_->prepareStatement("INSERT INTO automation_actions (rule_id, device_id, "
                                        "sequence_no, command, command_params) "
                                        "VALUES (?, ?, ?, ?, ?)"));
      for (std::size_t index = 0; index < record.actions.size(); ++index) {
        const auto& entry = record.actions[index];
        action->setString(1, record.id);
        action->setString(2, entry.deviceId);
        action->setUInt64(3, index);
        action->setString(4, entry.command);
        action->setString(5, entry.params.dump());
        action->executeUpdate();
      }
      connection_->commit();
      connection_->setAutoCommit(true);
      return true;
    } catch (const sql::SQLException&) {
      try {
        connection_->rollback();
        connection_->setAutoCommit(true);
      } catch (const sql::SQLException&) {
      }
      std::cerr << "[db] rule write failed\n";
      return false;
    }
  }

  bool claimScheduledRun(const std::string& householdId, const std::string& ruleId,
                         std::int64_t minute) override {
    const std::lock_guard<std::recursive_mutex> lock(mutex_);
    try {
      std::unique_ptr<sql::PreparedStatement> statement(connection_->prepareStatement(
          "UPDATE automation_rules SET last_fired_at = FROM_UNIXTIME(?) "
          "WHERE id = ? AND household_id = ? AND enabled = TRUE AND trigger_type = 'SCHEDULE' "
          "AND (last_fired_at IS NULL OR last_fired_at < FROM_UNIXTIME(?))"));
      statement->setInt64(1, minute);
      statement->setString(2, ruleId);
      statement->setString(3, householdId);
      statement->setInt64(4, minute);
      return statement->executeUpdate() == 1;
    } catch (const sql::SQLException&) {
      std::cerr << "[db] scheduled run claim failed\n";
      return false;
    }
  }

  bool deleteRule(const std::string& householdId, const std::string& ruleId) override {
    const std::lock_guard<std::recursive_mutex> lock(mutex_);
    try {
      std::unique_ptr<sql::PreparedStatement> statement(connection_->prepareStatement(
          "DELETE FROM automation_rules WHERE id = ? AND household_id = ?"));
      statement->setString(1, ruleId);
      statement->setString(2, householdId);
      return statement->executeUpdate() > 0;
    } catch (const sql::SQLException&) {
      std::cerr << "[db] rule deletion failed\n";
      return false;
    }
  }

  std::vector<DeviceRecord> listDevices(const std::string& householdId) override {
    const std::lock_guard<std::recursive_mutex> lock(mutex_);
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
    const std::lock_guard<std::recursive_mutex> lock(mutex_);
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
    const std::lock_guard<std::recursive_mutex> lock(mutex_);
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
      statement->setString(8, record.metadata.is_object() ? record.metadata.dump() : "{}");
      statement->executeUpdate();
      return saveSubtype(record);
    } catch (const sql::SQLException& error) {
      std::cerr << "[db] saveDevice: " << error.what() << '\n';
      return false;
    }
  }

  bool deleteDevice(const std::string& deviceId) override {
    const std::lock_guard<std::recursive_mutex> lock(mutex_);
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
    const std::lock_guard<std::recursive_mutex> lock(mutex_);
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
      "d.status, d.power_on, d.metadata, "
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
    if (!rows.isNull("metadata")) {
      nlohmann::json metadata =
          nlohmann::json::parse(rows.getString("metadata").c_str(), nullptr, false);
      if (metadata.is_object()) {
        record.metadata = std::move(metadata);
      }
    }

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

  mutable std::recursive_mutex mutex_;
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
