#include "homestr/services/database.hpp"

#include <cstdlib>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include "homestr/models/user.hpp"

namespace homestr {
namespace {

UserRecord makeUserRecord(std::string email = "ada@homestr.local", std::string role = "user") {
  UserRecord record;
  record.id = "user-1";
  record.name = "Ada";
  record.email = std::move(email);
  record.passwordHash = "$argon2id$placeholder";
  record.role = std::move(role);
  return record;
}

DeviceRecord makeDeviceRecord(std::string id = "dev-1", std::string householdId = "house-1") {
  DeviceRecord record;
  record.id = std::move(id);
  record.householdId = std::move(householdId);
  record.name = "Kitchen Ceiling";
  record.ipAddress = "192.168.1.40";
  record.deviceType = "light";
  record.status = "ONLINE";
  record.powerOn = true;
  record.attributes = {{"brightness", 80}, {"color", "#FFAA00"}};
  return record;
}

// Sets or clears an environment variable for the lifetime of the object and
// restores the previous value afterwards, so tests cannot leak into each other.
class ScopedEnv {
 public:
  ScopedEnv(std::string name, const char* value) : name_(std::move(name)) {
    const char* previous = std::getenv(name_.c_str());
    if (previous != nullptr) {
      hadPrevious_ = true;
      previous_ = previous;
    }
    apply(value);
  }

  ~ScopedEnv() { apply(hadPrevious_ ? previous_.c_str() : nullptr); }

  ScopedEnv(const ScopedEnv&) = delete;
  ScopedEnv& operator=(const ScopedEnv&) = delete;

 private:
  void apply(const char* value) const {
#ifdef _WIN32
    _putenv_s(name_.c_str(), value != nullptr ? value : "");
#else
    if (value != nullptr) {
      setenv(name_.c_str(), value, 1);
    } else {
      unsetenv(name_.c_str());
    }
#endif
  }

  std::string name_;
  std::string previous_;
  bool hadPrevious_ = false;
};

// ---------------------------------------------------------------------------
// Records and configuration
// ---------------------------------------------------------------------------

TEST(DeviceRecordTest, DefaultsDescribeAnUnregisteredOfflineGenericDevice) {
  const DeviceRecord record;

  EXPECT_EQ(record.deviceType, "generic");
  EXPECT_EQ(record.status, "OFFLINE");
  EXPECT_FALSE(record.powerOn);
  EXPECT_TRUE(record.attributes.is_object());
  EXPECT_TRUE(record.attributes.empty());
}

TEST(UserRecordTest, DefaultsToTheUserRole) {
  EXPECT_EQ(UserRecord{}.role, "user");
}

TEST(UserRecordTest, ToUserCopiesEveryField) {
  const User user = makeUserRecord("ada@homestr.local", "admin").toUser();

  EXPECT_EQ(user.id(), "user-1");
  EXPECT_EQ(user.name(), "Ada");
  EXPECT_EQ(user.email(), "ada@homestr.local");
  EXPECT_EQ(user.role(), Role::Admin);
  EXPECT_TRUE(user.matchesPasswordHash("$argon2id$placeholder"));
}

TEST(UserRecordTest, ToUserFallsBackToUserForAnUnknownRole) {
  EXPECT_FALSE(makeUserRecord("ada@homestr.local", "superuser").toUser().is_admin());
}

TEST(UserRecordTest, ToUserNeverExposesThePasswordHashInJson) {
  const nlohmann::json payload = makeUserRecord().toUser().toJson();

  EXPECT_EQ(payload.dump().find("argon2id"), std::string::npos);
}

TEST(DatabaseConfigTest, DefaultsTargetTheComposeDatabaseService) {
  const DatabaseConfig config;

  EXPECT_EQ(config.host, "db");
  EXPECT_EQ(config.port, 3306);
  EXPECT_EQ(config.schema, "homestr_db");
  EXPECT_EQ(config.user, "homestr_user");
  EXPECT_TRUE(config.password.empty());
  EXPECT_EQ(config.connectionUrl(), "jdbc:mariadb://db:3306/homestr_db");
}

TEST(DatabaseConfigTest, ConnectionUrlUsesHostPortAndSchema) {
  DatabaseConfig config;
  config.host = "localhost";
  config.port = 3307;
  config.schema = "other_db";

  EXPECT_EQ(config.connectionUrl(), "jdbc:mariadb://localhost:3307/other_db");
}

TEST(DatabaseConfigTest, ConnectionUrlNeverContainsTheCredentials) {
  DatabaseConfig config;
  config.user = "svc";
  config.password = "s3cret";

  const std::string url = config.connectionUrl();

  EXPECT_EQ(url.find("s3cret"), std::string::npos);
  EXPECT_EQ(url.find("svc"), std::string::npos);
}

TEST(DatabaseConfigTest, FromEnvironmentReadsEveryVariable) {
  const ScopedEnv host("DB_HOST", "db.internal");
  const ScopedEnv port("DB_PORT", "3310");
  const ScopedEnv schema("MARIADB_DATABASE", "prod_db");
  const ScopedEnv user("MARIADB_USER", "svc");
  const ScopedEnv password("MARIADB_PASSWORD", "s3cret");

  const DatabaseConfig config = DatabaseConfig::fromEnvironment();

  EXPECT_EQ(config.host, "db.internal");
  EXPECT_EQ(config.port, 3310);
  EXPECT_EQ(config.schema, "prod_db");
  EXPECT_EQ(config.user, "svc");
  EXPECT_EQ(config.password, "s3cret");
}

TEST(DatabaseConfigTest, FromEnvironmentKeepsDefaultsWhenVariablesAreUnsetOrEmpty) {
  const ScopedEnv host("DB_HOST", nullptr);
  const ScopedEnv port("DB_PORT", "");
  const ScopedEnv schema("MARIADB_DATABASE", nullptr);
  const ScopedEnv user("MARIADB_USER", "");
  const ScopedEnv password("MARIADB_PASSWORD", nullptr);

  const DatabaseConfig config = DatabaseConfig::fromEnvironment();

  EXPECT_EQ(config.host, "db");
  EXPECT_EQ(config.port, 3306);
  EXPECT_EQ(config.schema, "homestr_db");
  EXPECT_EQ(config.user, "homestr_user");
  EXPECT_TRUE(config.password.empty());
}

// ---------------------------------------------------------------------------
// InMemoryDatabase
// ---------------------------------------------------------------------------

class InMemoryDatabaseTest : public ::testing::Test {
 protected:
  void SetUp() override { ASSERT_TRUE(database_.connect()); }

  InMemoryDatabase database_;
};

TEST(InMemoryDatabaseConnectionTest, IsDisconnectedUntilConnectIsCalled) {
  InMemoryDatabase database;

  EXPECT_FALSE(database.isConnected());
  EXPECT_TRUE(database.connect());
  EXPECT_TRUE(database.isConnected());
}

TEST(InMemoryDatabaseConnectionTest, ConnectIsIdempotent) {
  InMemoryDatabase database;

  EXPECT_TRUE(database.connect());
  EXPECT_TRUE(database.connect());
  EXPECT_TRUE(database.isConnected());
}

TEST_F(InMemoryDatabaseTest, FindUserByEmailReturnsTheStoredRecord) {
  database_.addUser(makeUserRecord("ada@homestr.local", "admin"));

  const std::optional<UserRecord> found = database_.findUserByEmail("ada@homestr.local");

  ASSERT_TRUE(found.has_value());
  EXPECT_EQ(found->id, "user-1");
  EXPECT_EQ(found->name, "Ada");
  EXPECT_EQ(found->role, "admin");
  EXPECT_EQ(found->passwordHash, "$argon2id$placeholder");
}

TEST_F(InMemoryDatabaseTest, FindUserByEmailReturnsNothingForAnUnknownEmail) {
  database_.addUser(makeUserRecord());

  EXPECT_FALSE(database_.findUserByEmail("nobody@homestr.local").has_value());
}

TEST_F(InMemoryDatabaseTest, FindUserByEmailOnAnEmptyStoreReturnsNothing) {
  EXPECT_FALSE(database_.findUserByEmail("ada@homestr.local").has_value());
}

TEST_F(InMemoryDatabaseTest, FindUserByEmailPicksTheMatchingUserAmongSeveral) {
  UserRecord first = makeUserRecord("first@homestr.local");
  first.id = "user-1";
  UserRecord second = makeUserRecord("second@homestr.local");
  second.id = "user-2";
  database_.addUser(first);
  database_.addUser(second);

  const std::optional<UserRecord> found = database_.findUserByEmail("second@homestr.local");

  ASSERT_TRUE(found.has_value());
  EXPECT_EQ(found->id, "user-2");
}

TEST_F(InMemoryDatabaseTest, SaveDeviceInsertsANewDevice) {
  ASSERT_TRUE(database_.saveDevice(makeDeviceRecord()));

  const std::optional<DeviceRecord> found = database_.findDevice("dev-1");

  ASSERT_TRUE(found.has_value());
  EXPECT_EQ(found->householdId, "house-1");
  EXPECT_EQ(found->name, "Kitchen Ceiling");
  EXPECT_EQ(found->ipAddress, "192.168.1.40");
  EXPECT_EQ(found->deviceType, "light");
  EXPECT_EQ(found->status, "ONLINE");
  EXPECT_TRUE(found->powerOn);
  EXPECT_EQ(found->attributes.at("brightness"), 80);
  EXPECT_EQ(found->attributes.at("color"), "#FFAA00");
}

TEST_F(InMemoryDatabaseTest, SaveDeviceWithAnExistingIdUpdatesInPlace) {
  ASSERT_TRUE(database_.saveDevice(makeDeviceRecord()));
  DeviceRecord changed = makeDeviceRecord();
  changed.name = "Kitchen Island";
  changed.status = "OFFLINE";
  changed.powerOn = false;
  changed.attributes = {{"brightness", 10}, {"color", "#000000"}};

  ASSERT_TRUE(database_.saveDevice(changed));

  EXPECT_EQ(database_.listDevices("house-1").size(), 1U);
  const std::optional<DeviceRecord> found = database_.findDevice("dev-1");
  ASSERT_TRUE(found.has_value());
  EXPECT_EQ(found->name, "Kitchen Island");
  EXPECT_EQ(found->status, "OFFLINE");
  EXPECT_FALSE(found->powerOn);
  EXPECT_EQ(found->attributes.at("brightness"), 10);
}

TEST_F(InMemoryDatabaseTest, SaveDeviceCanMoveADeviceBetweenHouseholds) {
  ASSERT_TRUE(database_.saveDevice(makeDeviceRecord("dev-1", "house-1")));
  ASSERT_TRUE(database_.saveDevice(makeDeviceRecord("dev-1", "house-2")));

  EXPECT_TRUE(database_.listDevices("house-1").empty());
  EXPECT_EQ(database_.listDevices("house-2").size(), 1U);
}

TEST_F(InMemoryDatabaseTest, FindDeviceReturnsNothingForAnUnknownId) {
  ASSERT_TRUE(database_.saveDevice(makeDeviceRecord()));

  EXPECT_FALSE(database_.findDevice("missing").has_value());
}

TEST_F(InMemoryDatabaseTest, ListDevicesReturnsOnlyTheRequestedHousehold) {
  ASSERT_TRUE(database_.saveDevice(makeDeviceRecord("dev-1", "house-1")));
  ASSERT_TRUE(database_.saveDevice(makeDeviceRecord("dev-2", "house-1")));
  ASSERT_TRUE(database_.saveDevice(makeDeviceRecord("dev-3", "house-2")));

  const std::vector<DeviceRecord> devices = database_.listDevices("house-1");

  ASSERT_EQ(devices.size(), 2U);
  EXPECT_EQ(devices[0].id, "dev-1");
  EXPECT_EQ(devices[1].id, "dev-2");
}

TEST_F(InMemoryDatabaseTest, ListDevicesForAHouseholdWithNoDevicesIsEmpty) {
  ASSERT_TRUE(database_.saveDevice(makeDeviceRecord()));

  EXPECT_TRUE(database_.listDevices("house-404").empty());
}

TEST_F(InMemoryDatabaseTest, DeleteDeviceRemovesOnlyTheTargetedDevice) {
  ASSERT_TRUE(database_.saveDevice(makeDeviceRecord("dev-1")));
  ASSERT_TRUE(database_.saveDevice(makeDeviceRecord("dev-2")));

  EXPECT_TRUE(database_.deleteDevice("dev-1"));

  EXPECT_FALSE(database_.findDevice("dev-1").has_value());
  EXPECT_TRUE(database_.findDevice("dev-2").has_value());
}

TEST_F(InMemoryDatabaseTest, DeleteDeviceReportsFalseForAnUnknownId) {
  ASSERT_TRUE(database_.saveDevice(makeDeviceRecord()));

  EXPECT_FALSE(database_.deleteDevice("missing"));
  EXPECT_TRUE(database_.findDevice("dev-1").has_value());
}

TEST_F(InMemoryDatabaseTest, DeleteDeviceTwiceSucceedsOnlyTheFirstTime) {
  ASSERT_TRUE(database_.saveDevice(makeDeviceRecord()));

  EXPECT_TRUE(database_.deleteDevice("dev-1"));
  EXPECT_FALSE(database_.deleteDevice("dev-1"));
}

TEST_F(InMemoryDatabaseTest, RecordEventAppendsOneRowPerCall) {
  EXPECT_EQ(database_.event_count(), 0U);

  EXPECT_TRUE(database_.recordEvent("dev-1", "USER", "user-1", "turnOn", {}, true));
  EXPECT_TRUE(database_.recordEvent("dev-1", "USER", "user-1", "turnOff", {}, true));

  EXPECT_EQ(database_.event_count(), 2U);
}

TEST_F(InMemoryDatabaseTest, RecordEventKeepsFailedAttemptsToo) {
  EXPECT_TRUE(database_.recordEvent("dev-1", "SYSTEM", "", "setColor", {{"color", "red"}}, false));

  EXPECT_EQ(database_.event_count(), 1U);
}

TEST_F(InMemoryDatabaseTest, RecordEventDoesNotRequireTheDeviceToBeStored) {
  EXPECT_TRUE(database_.recordEvent("never-saved", "USER", "user-1", "turnOn", {}, false));
}

TEST_F(InMemoryDatabaseTest, DeletingADeviceDoesNotDiscardItsAuditTrail) {
  ASSERT_TRUE(database_.saveDevice(makeDeviceRecord()));
  ASSERT_TRUE(database_.recordEvent("dev-1", "USER", "user-1", "turnOn", {}, true));

  ASSERT_TRUE(database_.deleteDevice("dev-1"));

  EXPECT_EQ(database_.event_count(), 1U);
}

TEST_F(InMemoryDatabaseTest, WorksThroughTheAbstractDatabaseInterface) {
  Database& database = database_;

  EXPECT_TRUE(database.isConnected());
  EXPECT_TRUE(database.saveDevice(makeDeviceRecord()));
  EXPECT_TRUE(database.findDevice("dev-1").has_value());
}

// ---------------------------------------------------------------------------
// makeDatabase
// ---------------------------------------------------------------------------

TEST(MakeDatabaseTest, ReturnsAUsableDatabase) {
  const std::unique_ptr<Database> database = makeDatabase(DatabaseConfig{});

  ASSERT_NE(database, nullptr);
  EXPECT_FALSE(database->isConnected());
}

#if !HOMESTR_HAS_MARIADB
TEST(MakeDatabaseTest, FallsBackToTheInMemoryRepositoryWithoutMariaDb) {
  const std::unique_ptr<Database> database = makeDatabase(DatabaseConfig{});

  ASSERT_TRUE(database->connect());
  EXPECT_TRUE(database->saveDevice(makeDeviceRecord()));
  EXPECT_TRUE(database->findDevice("dev-1").has_value());
}
#endif

}  // namespace
}  // namespace homestr
