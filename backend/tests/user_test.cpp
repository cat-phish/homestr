#include "homestr/models/user.hpp"

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

namespace homestr {
namespace {

constexpr const char* STORED_HASH = "$2a$12$abcdefghijklmnopqrstuv";

User makeUser(Role role = Role::User) {
  return User{"user-1", "Ada", "ada@homestr.local", STORED_HASH, role};
}

TEST(UserTest, AdminRoleIsReportedByIsAdmin) {
  EXPECT_TRUE(makeUser(Role::Admin).is_admin());
  EXPECT_FALSE(makeUser(Role::User).is_admin());
}

TEST(UserTest, MatchesPasswordHashAcceptsTheStoredDigest) {
  EXPECT_TRUE(makeUser().matchesPasswordHash(STORED_HASH));
}

TEST(UserTest, MatchesPasswordHashRejectsOtherDigests) {
  EXPECT_FALSE(makeUser().matchesPasswordHash("$2a$12$different.digest.value"));
  EXPECT_FALSE(makeUser().matchesPasswordHash(""));
}

TEST(UserTest, ToJsonNeverLeaksThePasswordHash) {
  const nlohmann::json payload = makeUser(Role::Admin).toJson();

  EXPECT_FALSE(payload.contains("password"));
  EXPECT_FALSE(payload.contains("passwordHash"));
  EXPECT_FALSE(payload.contains("passwordHash"));
  EXPECT_EQ(payload.at("role"), "admin");
  EXPECT_EQ(payload.at("email"), "ada@homestr.local");
}

TEST(UserTest, ParseRoleDefaultsToUser) {
  EXPECT_EQ(parseRole("admin"), Role::Admin);
  EXPECT_EQ(parseRole("user"), Role::User);
  EXPECT_EQ(parseRole("root"), Role::User);
}

}  // namespace
}  // namespace homestr
