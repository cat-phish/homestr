#pragma once

#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "homestr/models/automation_rule.hpp"
#include "homestr/models/user.hpp"

namespace homestr {

// Connection parameters, normally read from the environment
// (DB_HOST/DB_PORT/...). See .env.example.
struct DatabaseConfig {
  std::string host = "db";
  int port = 3306;
  std::string schema = "homestr_db";
  std::string user = "homestr_user";
  std::string password;

  // Builds the Connector/C++ URL, e.g. "jdbc:mariadb://db:3306/homestr_db".
  std::string connectionUrl() const;

  // Reads DB_HOST, DB_PORT, MARIADB_DATABASE, MARIADB_USER, MARIADB_PASSWORD.
  static DatabaseConfig fromEnvironment();
};

// Flat projection of `devices` joined with its subtype table. The polymorphic
// SmartDevice objects are reconstructed by DeviceManager, which keeps the
// persistence layer free of the domain hierarchy.
struct DeviceRecord {
  std::string id;
  std::string householdId;
  std::string name;
  std::string ipAddress;
  std::string deviceType = "generic";  // light | speaker | generic
  std::string status = "OFFLINE";
  bool powerOn = false;
  nlohmann::json attributes = nlohmann::json::object();
};

struct UserRecord {
  std::string id;
  std::string name;
  std::string email;
  std::string passwordHash;
  std::string role = "user";

  User toUser() const;
};

struct SessionRecord {
  std::string tokenHash;
  std::string userId;
  std::int64_t expiresAt = 0;
};

// Flat rule projection; action vector order maps to sequence_no.
struct RuleRecord {
  std::string id;
  std::string householdId;
  std::string name;
  bool enabled = true;
  std::string triggerType = "MANUAL";
  nlohmann::json triggerPayload = nlohmann::json::object();
  std::vector<RuleAction> actions;

  AutomationRule toRule() const;
  static RuleRecord fromRule(const AutomationRule& rule, const std::string& householdId);
};

// Persistence boundary. Controllers and services depend on this interface only,
// so unit tests can substitute the in-memory implementation.
class Database {
 public:
  virtual ~Database() = default;

  // Opens the connection (or validates the fake). Returns false on failure
  // instead of throwing, so main() can retry while MariaDB is still booting.
  virtual bool connect() = 0;

  virtual bool isConnected() const = 0;

  virtual std::optional<UserRecord> findUserByEmail(const std::string& email) = 0;

  virtual bool updatePasswordHash(const std::string& email, const std::string& hash) = 0;
  virtual bool saveSession(const SessionRecord& session, const std::string& verifiedHash) = 0;
  virtual std::optional<UserRecord> findSessionUser(const std::string& tokenHash,
                                                    std::int64_t now) = 0;
  virtual bool deleteSession(const std::string& tokenHash) = 0;

  // nullopt means a read failure, distinct from a successful empty household.
  virtual std::optional<std::vector<RuleRecord>> listRules(const std::string& householdId) = 0;
  virtual bool saveRule(const RuleRecord& record) = 0;
  virtual bool deleteRule(const std::string& householdId, const std::string& ruleId) = 0;

  virtual std::vector<DeviceRecord> listDevices(const std::string& householdId) = 0;

  virtual std::optional<DeviceRecord> findDevice(const std::string& deviceId) = 0;

  // Insert or update, keyed on DeviceRecord::id. Used by both registration and
  // state writes after a command.
  virtual bool saveDevice(const DeviceRecord& record) = 0;

  virtual bool deleteDevice(const std::string& deviceId) = 0;

  // Appends to `device_events`. Failures here must not fail the command itself.
  virtual bool recordEvent(const std::string& deviceId, const std::string& actorType,
                           const std::string& actorId, const std::string& command,
                           const nlohmann::json& payload, bool succeeded) = 0;
};

// Volatile Database used by the GoogleTest suites and by `docker compose` runs
// started with HOMESTR_WITH_MARIADB=OFF.
class InMemoryDatabase : public Database {
 public:
  bool connect() override;
  bool isConnected() const override;
  std::optional<UserRecord> findUserByEmail(const std::string& email) override;
  bool updatePasswordHash(const std::string& email, const std::string& hash) override;
  bool saveSession(const SessionRecord& session, const std::string& verifiedHash) override;
  std::optional<UserRecord> findSessionUser(const std::string& tokenHash,
                                            std::int64_t now) override;
  bool deleteSession(const std::string& tokenHash) override;
  std::optional<std::vector<RuleRecord>> listRules(const std::string& householdId) override;
  bool saveRule(const RuleRecord& record) override;
  bool deleteRule(const std::string& householdId, const std::string& ruleId) override;
  std::vector<DeviceRecord> listDevices(const std::string& householdId) override;
  std::optional<DeviceRecord> findDevice(const std::string& deviceId) override;
  bool saveDevice(const DeviceRecord& record) override;
  bool deleteDevice(const std::string& deviceId) override;
  bool recordEvent(const std::string& deviceId, const std::string& actorType,
                   const std::string& actorId, const std::string& command,
                   const nlohmann::json& payload, bool succeeded) override;

  void addUser(UserRecord record);

  std::size_t event_count() const noexcept {
    const std::lock_guard<std::recursive_mutex> lock(mutex_);
    return events_.size();
  }

 private:
  mutable std::recursive_mutex mutex_;
  std::vector<SessionRecord> sessions_;
  std::vector<RuleRecord> rules_;
  bool connected_ = false;
  std::vector<UserRecord> users_;
  std::vector<DeviceRecord> devices_;
  std::vector<nlohmann::json> events_;
};

// Returns a MariaDB-backed Database when the Connector/C++ is linked in
// (HOMESTR_HAS_MARIADB=1), otherwise an InMemoryDatabase.
std::unique_ptr<Database> makeDatabase(const DatabaseConfig& config);

}  // namespace homestr
