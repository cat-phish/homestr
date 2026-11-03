#include "homestr/services/auth_service.hpp"

#include <memory>
#include <stdexcept>
#include <string>

#include <gtest/gtest.h>

namespace homestr {
namespace {

class AuthServiceTest : public ::testing::Test {
 protected:
  void SetUp() override {
    database_.connect();
    database_.addUser({"user-1", "Ada", "ada@example.com",
                       AuthService::hashPassword("correct-password"), "admin"});
    auth_ = std::make_unique<AuthService>(database_, [this] { return now_; });
  }

  InMemoryDatabase database_;
  std::int64_t now_ = 1'800'000'000;
  std::unique_ptr<AuthService> auth_;
};

TEST_F(AuthServiceTest, HashesAreSaltedAndVerifyOnlyTheCorrectPassword) {
  const auto first = AuthService::hashPassword("correct-password");
  const auto second = AuthService::hashPassword("correct-password");
  EXPECT_NE(first, second);
  EXPECT_TRUE(first.starts_with("$argon2id$"));
  EXPECT_TRUE(AuthService::verifyPassword("correct-password", first));
  EXPECT_FALSE(AuthService::verifyPassword("wrong", first));
  EXPECT_FALSE(AuthService::verifyPassword("anything", ""));
  EXPECT_FALSE(AuthService::verifyPassword("anything", "$2a$placeholder"));
}

TEST_F(AuthServiceTest, RejectsBadCredentialsAndMalformedTokens) {
  EXPECT_FALSE(auth_->login("ada@example.com", "incorrect-password"));
  EXPECT_FALSE(auth_->login("missing@example.com", "correct-password"));
  EXPECT_FALSE(auth_->validateSession("dev-token-user-1"));
  EXPECT_FALSE(auth_->validateSession(""));
  EXPECT_FALSE(auth_->validateSession(std::string(64, 'a')));
}

TEST_F(AuthServiceTest, StoresOnlyTheTokenHashAndRestoresSessionInAnotherService) {
  const auto session = auth_->login("ada@example.com", "correct-password");
  ASSERT_TRUE(session);
  EXPECT_EQ(session->token.size(), 64U);
  EXPECT_FALSE(database_.findSessionUser(session->token, now_));
  EXPECT_TRUE(database_.findSessionUser(AuthService::tokenHash(session->token), now_));
  AuthService restarted{database_, [this] { return now_; }};
  const auto user = restarted.validateSession(session->token);
  ASSERT_TRUE(user);
  EXPECT_EQ(user->id, "user-1");
  EXPECT_EQ(user->role, "admin");
  EXPECT_FALSE(user->toUser().toJson().contains("passwordHash"));
}

TEST_F(AuthServiceTest, SessionExpiresAtTheAbsoluteDeadline) {
  const auto session = auth_->login("ada@example.com", "correct-password");
  ASSERT_TRUE(session);
  now_ += 24 * 60 * 60 - 1;
  EXPECT_TRUE(auth_->validateSession(session->token));
  ++now_;
  EXPECT_FALSE(auth_->validateSession(session->token));
}

TEST_F(AuthServiceTest, LogoutRevokesOnlyTheCurrentSession) {
  const auto first = auth_->login("ada@example.com", "correct-password");
  const auto second = auth_->login("ada@example.com", "correct-password");
  ASSERT_TRUE(first);
  ASSERT_TRUE(second);
  EXPECT_NE(first->token, second->token);
  EXPECT_TRUE(auth_->logout(first->token));
  EXPECT_FALSE(auth_->validateSession(first->token));
  EXPECT_TRUE(auth_->validateSession(second->token));
}

TEST_F(AuthServiceTest, PasswordRotationRevokesAllUserSessions) {
  const auto session = auth_->login("ada@example.com", "correct-password");
  ASSERT_TRUE(session);
  EXPECT_TRUE(database_.updatePasswordHash("ada@example.com",
                                           AuthService::hashPassword("replacement-password")));
  EXPECT_FALSE(auth_->validateSession(session->token));
  EXPECT_FALSE(auth_->login("ada@example.com", "correct-password"));
  EXPECT_TRUE(auth_->login("ada@example.com", "replacement-password"));
  EXPECT_FALSE(database_.updatePasswordHash("missing@example.com", "hash"));
}

TEST_F(AuthServiceTest, DoesNotIssueASessionUsingACredentialSnapshotFromBeforeRotation) {
  const auto user = database_.findUserByEmail("ada@example.com");
  ASSERT_TRUE(user);
  ASSERT_TRUE(
      database_.updatePasswordHash(user->email, AuthService::hashPassword("replacement-password")));
  EXPECT_FALSE(database_.saveSession({"old-token-hash", user->id, now_ + 60}, user->passwordHash));
}

TEST_F(AuthServiceTest, RejectsOutOfBoundsPasswordsDuringSetup) {
  EXPECT_THROW(AuthService::hashPassword("short"), std::invalid_argument);
  EXPECT_THROW(AuthService::hashPassword(std::string(1025, 'x')), std::invalid_argument);
}

TEST(AuthFailureTest, DoesNotIssueATokenWhenPersistenceFails) {
  class FailingDatabase : public InMemoryDatabase {
   public:
    bool saveSession(const SessionRecord&, const std::string&) override { return false; }
  } database;

  database.connect();
  database.addUser(
      {"user-1", "Ada", "ada@example.com", AuthService::hashPassword("correct-password"), "admin"});
  AuthService auth{database};
  EXPECT_FALSE(auth.login("ada@example.com", "correct-password"));
}

}  // namespace
}  // namespace homestr
