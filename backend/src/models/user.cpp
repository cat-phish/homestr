#include "homestr/models/user.hpp"

#include <cstddef>
#include <string>
#include <string_view>
#include <utility>

#include <nlohmann/json.hpp>

namespace homestr {

std::string_view toString(Role role) {
  return role == Role::Admin ? "admin" : "user";
}

Role parseRole(std::string_view text) {
  return text == "admin" ? Role::Admin : Role::User;
}

User::User(std::string id, std::string name, std::string email, std::string passwordHash, Role role)
    : id_(std::move(id)),
      name_(std::move(name)),
      email_(std::move(email)),
      passwordHash_(std::move(passwordHash)),
      role_(role) {}

bool User::matchesPasswordHash(std::string_view candidateHash) const {
  // Constant-time comparison: never short-circuit on the first differing byte,
  // and never leak the digest length through an early return.
  if (candidateHash.size() != passwordHash_.size()) {
    return false;
  }
  unsigned char difference = 0;
  for (std::size_t index = 0; index < passwordHash_.size(); ++index) {
    difference |= static_cast<unsigned char>(passwordHash_[index] ^ candidateHash[index]);
  }
  return difference == 0;
}

nlohmann::json User::toJson() const {
  return {{"id", id_}, {"name", name_}, {"email", email_}, {"role", toString(role_)}};
}

}  // namespace homestr
