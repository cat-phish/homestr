#pragma once

#include <string>
#include <string_view>

#include <nlohmann/json.hpp>

namespace homestr {

enum class Role { User, Admin };

std::string_view toString(Role role);

Role parseRole(std::string_view text);

// `User` from the class diagram. The UML `Admin` specialisation is modelled by
// `role() == Role::Admin` rather than a subclass: the only difference is which
// operations are permitted, which the controllers enforce through
// `is_admin()`. Provisioning behaviour itself lives in HomeStr/DeviceManager.
class User {
 public:
  User(std::string id, std::string name, std::string email, std::string passwordHash,
       Role role = Role::User);

  const std::string& id() const noexcept { return id_; }

  const std::string& name() const noexcept { return name_; }

  const std::string& email() const noexcept { return email_; }

  Role role() const noexcept { return role_; }

  bool is_admin() const noexcept { return role_ == Role::Admin; }

  // Compares `candidateHash` against the stored digest in constant time.
  // Hashing itself belongs to the auth service; User never sees a plaintext
  // password.
  bool matchesPasswordHash(std::string_view candidateHash) const;

  // Never contains passwordHash: this payload is sent to the WebUI.
  nlohmann::json toJson() const;

 private:
  std::string id_;
  std::string name_;
  std::string email_;
  std::string passwordHash_;
  Role role_;
};

}  // namespace homestr
