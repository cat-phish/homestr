#include "homestr/services/auth_service.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <stdexcept>
#include <utility>

#include <sodium.h>

namespace homestr {
namespace {

void initializeSodium() {
  static const int initialized = sodium_init();
  if (initialized < 0) {
    throw std::runtime_error("Could not initialize cryptography");
  }
}

std::string hex(const unsigned char* data, std::size_t size) {
  std::string result(size * 2 + 1, '\0');
  sodium_bin2hex(result.data(), result.size(), data, size);
  result.pop_back();
  return result;
}

}  // namespace

AuthService::AuthService(Database& database, Clock clock)
    : database_(database),
      clock_(std::move(clock)),
      dummyHash_(hashPassword("unused-dummy-password")) {}

std::int64_t AuthService::currentTime() {
  return std::chrono::duration_cast<std::chrono::seconds>(
             std::chrono::system_clock::now().time_since_epoch())
      .count();
}

std::string AuthService::hashPassword(const std::string& password) {
  initializeSodium();
  if (password.size() < 8 || password.size() > 1024) {
    throw std::invalid_argument("Password must contain 8 to 1024 bytes");
  }
  std::array<char, crypto_pwhash_STRBYTES> hash{};
  if (crypto_pwhash_str_alg(hash.data(), password.data(), password.size(),
                            crypto_pwhash_OPSLIMIT_INTERACTIVE, crypto_pwhash_MEMLIMIT_INTERACTIVE,
                            crypto_pwhash_ALG_ARGON2ID13) != 0) {
    throw std::runtime_error("Could not hash password");
  }
  return hash.data();
}

bool AuthService::verifyPassword(const std::string& password, const std::string& hash) {
  initializeSodium();
  return !password.empty() && password.size() <= 1024 && hash.starts_with("$argon2id$") &&
         hash.find('\0') == std::string::npos &&
         crypto_pwhash_str_verify(hash.c_str(), password.data(), password.size()) == 0;
}

std::string AuthService::tokenHash(const std::string& token) {
  initializeSodium();
  std::array<unsigned char, crypto_hash_sha256_BYTES> hash{};
  crypto_hash_sha256(hash.data(), reinterpret_cast<const unsigned char*>(token.data()),
                     token.size());
  return hex(hash.data(), hash.size());
}

std::optional<LoginSession> AuthService::login(const std::string& email,
                                               const std::string& password) {
  const auto user = database_.findUserByEmail(email);
  // Use a real hash for unknown/unconfigured accounts to avoid a cheap timing distinction.
  const std::string& hash =
      user && user->passwordHash.starts_with("$argon2id$") ? user->passwordHash : dummyHash_;
  const bool verified = verifyPassword(password, hash);
  if (!user || hash == dummyHash_ || !verified) {
    return std::nullopt;
  }
  std::array<unsigned char, 32> bytes{};
  randombytes_buf(bytes.data(), bytes.size());
  const std::string token = hex(bytes.data(), bytes.size());
  if (!database_.saveSession({tokenHash(token), user->id, clock_() + 24 * 60 * 60},
                             user->passwordHash)) {
    return std::nullopt;
  }
  return LoginSession{token, *user};
}

std::optional<UserRecord> AuthService::validateSession(const std::string& token) {
  if (token.size() != 64 || !std::all_of(token.begin(), token.end(), [](char c) {
        return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
      })) {
    return std::nullopt;
  }
  return validateHash(tokenHash(token));
}

std::optional<UserRecord> AuthService::validateHash(const std::string& hash) {
  return database_.findSessionUser(hash, clock_());
}

bool AuthService::logout(const std::string& token) {
  return database_.deleteSession(tokenHash(token));
}

}  // namespace homestr
