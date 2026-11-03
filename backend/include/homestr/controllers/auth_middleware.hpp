#pragma once

#include <optional>
#include <sstream>
#include <string>

#include <crow.h>

#include "homestr/services/auth_service.hpp"

namespace homestr {

inline std::string requestToken(const crow::request& request) {
  const std::string authorization = request.get_header_value("Authorization");
  if (authorization.starts_with("Bearer ")) {
    return authorization.substr(7);
  }
  if (request.url == "/api/v1/ws") {
    std::istringstream protocols(request.get_header_value("Sec-WebSocket-Protocol"));
    std::string protocol;
    while (std::getline(protocols, protocol, ',')) {
      const auto start = protocol.find_first_not_of(" \t");
      const auto end = protocol.find_last_not_of(" \t");
      if (start == std::string::npos) {
        continue;
      }
      protocol = protocol.substr(start, end - start + 1);
      if (protocol.starts_with("bearer.")) {
        return protocol.substr(7);
      }
    }
  }
  return {};
}

struct AuthMiddleware {
  struct context {
    std::optional<UserRecord> user;
  };

  AuthService* auth = nullptr;

  void before_handle(crow::request& request, crow::response& response, context& ctx) {
    if (request.method == crow::HTTPMethod::OPTIONS || !request.url.starts_with("/api/v1/") ||
        request.url == "/api/v1/auth/login") {
      return;
    }
    ctx.user = auth ? auth->validateSession(requestToken(request)) : std::nullopt;
    int code = 0;
    if (!ctx.user) {
      code = 401;
    } else if (ctx.user->role != "admin" &&
               ((request.url == "/api/v1/devices" && request.method == crow::HTTPMethod::POST) ||
                request.url == "/api/v1/devices/discover" ||
                (request.url.starts_with("/api/v1/devices/") &&
                 request.method == crow::HTTPMethod::DELETE))) {
      code = 403;
    }
    if (code != 0) {
      response.code = code;
      response.set_header("Content-Type", "application/json");
      if (code == 401) {
        response.set_header("WWW-Authenticate", "Bearer");
      }
      response.body = code == 401 ? R"({"error":"authentication required"})"
                                  : R"({"error":"administrator permission required"})";
      response.end();
    }
  }

  void after_handle(crow::request&, crow::response&, context&) {}
};

}  // namespace homestr
