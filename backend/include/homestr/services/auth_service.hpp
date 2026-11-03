#pragma once

#include <cstdint>
#include <functional>
#include <optional>
#include <string>

#include "homestr/services/database.hpp"

namespace homestr {

struct LoginSession {
  std::string token;
  UserRecord user;
};

class AuthService {
 public:
  using Clock = std::function<std::int64_t()>;
  explicit AuthService(Database& database, Clock clock = currentTime);
  static std::int64_t currentTime();
  static std::string hashPassword(const std::string& password);
  static bool verifyPassword(const std::string& password, const std::string& hash);
  static std::string tokenHash(const std::string& token);
  std::optional<LoginSession> login(const std::string& email, const std::string& password);
  std::optional<UserRecord> validateSession(const std::string& token);
  std::optional<UserRecord> validateHash(const std::string& hash);
  bool logout(const std::string& token);

 private:
  Database& database_;
  Clock clock_;
  std::string dummyHash_;
};

}  // namespace homestr
