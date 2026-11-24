#include "homestr/controllers/api_controller.hpp"

#include <chrono>
#include <iostream>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <utility>

#include <crow.h>

#include <nlohmann/json.hpp>

#include "homestr/models/automation_rule.hpp"
#include "homestr/services/database.hpp"

namespace homestr {
namespace {

// CROW_ROUTE needs a literal, so the /api/v1 prefix is spelled out per route
// rather than composed from a constant.
crow::response jsonResponse(int code, const nlohmann::json& body) {
  crow::response response(code, body.dump());
  response.set_header("Content-Type", "application/json");
  return response;
}

crow::response errorResponse(int code, const std::string& message) {
  return jsonResponse(code, {{"error", message}});
}

// Parses a request body, returning nullopt (and letting the caller answer 400)
// on malformed JSON.
std::optional<nlohmann::json> parseBody(const crow::request& request) {
  nlohmann::json parsed = nlohmann::json::parse(request.body, nullptr, false);
  if (parsed.is_discarded() || !parsed.is_object()) {
    return std::nullopt;
  }
  return parsed;
}

}  // namespace

void ApiController::broadcast(const std::string& payload) {
  const std::lock_guard<std::mutex> lock(clientsMutex_);
  for (const auto& [client, hash] : clients_) {
    if (auth_.validateHash(hash)) {
      client->send_text(payload);
    } else {
      client->close("Session expired", 4001);
    }
  }
}

void ApiController::expireClients() {
  const std::lock_guard<std::mutex> lock(clientsMutex_);
  for (const auto& [client, hash] : clients_) {
    if (!auth_.validateHash(hash)) {
      client->close("Session expired", 4001);
    }
  }
}

void ApiController::registerRoutes(App& app) {
  app.get_middleware<AuthMiddleware>().auth = &auth_;
  hub_.setEventSink([this](const nlohmann::json& event) { broadcast(event.dump()); });
  app.tick(std::chrono::seconds(1), [this] {
    expireClients();
    try {
      hub_.tickSchedules(Schedule::now());
    } catch (const std::exception& error) {
      std::cerr << "[scheduler] " << error.what() << '\n';
    }
  });
  // --- Health -------------------------------------------------------------
  CROW_ROUTE(app, "/healthz")
  ([this] {
    return jsonResponse(
        200, {{"status", "ok"}, {"database", hub_.database().isConnected() ? "up" : "down"}});
  });

  // --- Login --------------------------------------------------------------
  CROW_ROUTE(app, "/api/v1/auth/login")
      .methods(crow::HTTPMethod::POST)([this](const crow::request& request) {
        const std::optional<nlohmann::json> body = parseBody(request);
        if (!body) {
          return errorResponse(400, "malformed JSON body");
        }
        if (!body->contains("email") || !(*body)["email"].is_string() ||
            !body->contains("password") || !(*body)["password"].is_string()) {
          return errorResponse(400, "email and password must be strings");
        }
        const std::string email = body->value("email", std::string{});
        const std::string password = body->value("password", std::string{});
        if (email.empty() || email.size() > 255 || password.size() < 8 || password.size() > 1024) {
          return errorResponse(400, "email and a password of 8 to 1024 bytes are required");
        }

        const auto session = auth_.login(email, password);
        if (!session) {
          return errorResponse(401, "invalid credentials");
        }
        auto response = jsonResponse(
            200, {{"token", session->token}, {"user", session->user.toUser().toJson()}});
        response.set_header("Cache-Control", "no-store");
        return response;
      });

  CROW_ROUTE(app, "/api/v1/auth/me")
  ([&app](const crow::request& request) {
    auto response =
        jsonResponse(200, app.get_context<AuthMiddleware>(request).user->toUser().toJson());
    response.set_header("Cache-Control", "no-store");
    return response;
  });

  CROW_ROUTE(app, "/api/v1/auth/logout")
      .methods(crow::HTTPMethod::POST)([this](const crow::request& request) {
        if (!auth_.logout(requestToken(request))) {
          return errorResponse(503, "could not close session");
        }
        expireClients();
        return crow::response(204);
      });

  // --- Dashboard ----------------------------------------------------------
  CROW_ROUTE(app, "/api/v1/dashboard")
  ([this] { return jsonResponse(200, hub_.dashboard()); });

  // --- Devices ------------------------------------------------------------
  CROW_ROUTE(app, "/api/v1/devices")
  ([this] { return jsonResponse(200, {{"devices", hub_.devices().snapshot()}}); });

  CROW_ROUTE(app, "/api/v1/devices/discover")
  ([this] {
    nlohmann::json discovered = nlohmann::json::array();
    for (const DeviceRecord& record : hub_.devices().discover()) {
      discovered.push_back({{"id", record.id},
                            {"name", record.name},
                            {"ipAddress", record.ipAddress},
                            {"type", record.deviceType},
                            {"status", record.status},
                            {"attributes", record.attributes},
                            {"metadata", record.metadata}});
    }
    return jsonResponse(200, {{"discovered", std::move(discovered)}});
  });

  // Device registration (admin operation in the UML model).
  CROW_ROUTE(app, "/api/v1/devices")
      .methods(crow::HTTPMethod::POST)([this](const crow::request& request) {
        const std::optional<nlohmann::json> body = parseBody(request);
        if (!body) {
          return errorResponse(400, "malformed JSON body");
        }

        DeviceRecord record;
        record.id = body->value("id", std::string{});
        record.householdId = body->value("householdId", std::string{});
        record.name = body->value("name", std::string{});
        record.ipAddress = body->value("ipAddress", std::string{});
        record.deviceType = body->value("type", std::string{"generic"});
        record.status = body->value("status", std::string{"OFFLINE"});
        record.attributes = body->value("attributes", nlohmann::json::object());
        record.metadata = body->value("metadata", nlohmann::json::object());
        if (!record.metadata.is_object()) {
          return errorResponse(400, "metadata must be an object");
        }

        if (record.id.empty() || record.name.empty() || record.ipAddress.empty()) {
          return errorResponse(400, "id, name and ipAddress are required");
        }
        if (!hub_.devices().registerDevice(record)) {
          return errorResponse(409, "device " + record.id + " is already registered");
        }
        return jsonResponse(201, {{"registered", record.id}});
      });

  CROW_ROUTE(app, "/api/v1/devices/<string>")
  ([this](const std::string& deviceId) {
    const auto device = hub_.devices().state(deviceId);
    if (!device) {
      return errorResponse(404, "unknown device " + deviceId);
    }
    return jsonResponse(200, *device);
  });

  CROW_ROUTE(app, "/api/v1/devices/<string>")
      .methods(crow::HTTPMethod::PATCH)(
          [this, &app](const crow::request& request, const std::string& deviceId) {
            const std::optional<nlohmann::json> body = parseBody(request);
            if (!body || !body->contains("name") || !body->at("name").is_string()) {
              return errorResponse(400, "'name' must be a string");
            }
            const auto& user = app.get_context<AuthMiddleware>(request).user;
            switch (hub_.renameDevice(deviceId, body->at("name").get<std::string>(), user->id)) {
              case RenameResult::Renamed:
                break;
              case RenameResult::UnknownDevice:
                return errorResponse(404, "unknown device " + deviceId);
              case RenameResult::InvalidName:
                return errorResponse(400, "name must be 1-100 characters");
              case RenameResult::StorageFailed:
                return errorResponse(503, "could not save the new name");
            }
            return jsonResponse(200, hub_.devices().state(deviceId).value_or(nlohmann::json{}));
          });

  CROW_ROUTE(app, "/api/v1/devices/<string>")
      .methods(crow::HTTPMethod::DELETE)([this](const std::string& deviceId) {
        if (!hub_.removeDevice(deviceId)) {
          return errorResponse(404, "unknown device " + deviceId);
        }
        return crow::response(204);  // 204 carries no body
      });

  // --- Device control -----------------------------------------------------
  CROW_ROUTE(app, "/api/v1/devices/<string>/commands")
      .methods(crow::HTTPMethod::POST)(
          [this, &app](const crow::request& request, const std::string& deviceId) {
            const std::optional<nlohmann::json> body = parseBody(request);
            if (!body) {
              return errorResponse(400, "malformed JSON body");
            }
            const std::string command = body->value("command", std::string{});
            if (command.empty()) {
              return errorResponse(400, "'command' is required");
            }
            const nlohmann::json params = body->value("params", nlohmann::json::object());

            const auto& user = app.get_context<AuthMiddleware>(request).user;
            const CommandResult result = hub_.sendCommand(deviceId, command, params, user->id);
            if (!result.ok) {
              const int code = result.error.starts_with("unknown device") ? 404 : 409;
              return errorResponse(code, result.error);
            }

            const auto device = hub_.devices().state(deviceId);
            const nlohmann::json state = device.value_or(nlohmann::json::object());
            return jsonResponse(200, state);
          });

  // --- Automation rules ---------------------------------------------------
  CROW_ROUTE(app, "/api/v1/rules")
  ([this] {
    nlohmann::json rules = nlohmann::json::array();
    for (const AutomationRule& rule : hub_.rules()) {
      rules.push_back(hub_.ruleJson(rule));
    }
    return jsonResponse(200, {{"rules", std::move(rules)}});
  });

  CROW_ROUTE(app, "/api/v1/rules")
      .methods(crow::HTTPMethod::POST)([this](const crow::request& request) {
        const std::optional<nlohmann::json> body = parseBody(request);
        if (!body) {
          return errorResponse(400, "malformed JSON body");
        }
        if (!body->contains("id") || !body->at("id").is_string() || !body->contains("name") ||
            !body->at("name").is_string() ||
            (body->contains("enabled") && !body->at("enabled").is_boolean()) ||
            (body->contains("triggerType") && !body->at("triggerType").is_string()) ||
            (body->contains("trigger") && !body->at("trigger").is_object()) ||
            (body->contains("actions") && !body->at("actions").is_array())) {
          return errorResponse(400, "invalid rule fields");
        }
        const auto type = body->value("triggerType", std::string{"MANUAL"});
        if (type != "MANUAL" && type != "SCHEDULE" && type != "DEVICE_STATE") {
          return errorResponse(400, "unsupported trigger type");
        }
        for (const auto& action : body->value("actions", nlohmann::json::array())) {
          if (!action.is_object() || !action.contains("deviceId") ||
              !action.at("deviceId").is_string() || !action.contains("command") ||
              !action.at("command").is_string() ||
              (action.contains("params") && !action.at("params").is_object())) {
            return errorResponse(400, "invalid rule action");
          }
        }
        const std::string id = body->value("id", std::string{});
        const std::string name = body->value("name", std::string{});
        if (id.empty() || name.empty()) {
          return errorResponse(400, "id and name are required");
        }

        AutomationRule rule{id, name,
                            parseTriggerType(body->value("triggerType", std::string{"MANUAL"})),
                            body->value("trigger", nlohmann::json::object())};
        for (const nlohmann::json& entry : body->value("actions", nlohmann::json::array())) {
          rule.addAction(RuleAction{entry.value("deviceId", std::string{}),
                                    entry.value("command", std::string{}),
                                    entry.value("params", nlohmann::json::object())});
        }
        if (!body->value("enabled", true)) {
          rule.disable();
        }

        const auto result = hub_.addRule(rule);
        if (result.status == RuleWriteStatus::InvalidTrigger) {
          return errorResponse(400, result.error);
        }
        if (result.status == RuleWriteStatus::Duplicate) {
          return errorResponse(409, "rule " + id + " already exists");
        }
        if (!result) {
          return errorResponse(503, "could not save rule");
        }
        return jsonResponse(201, hub_.ruleJson(rule));
      });

  CROW_ROUTE(app, "/api/v1/rules/<string>/enabled")
      .methods(crow::HTTPMethod::PUT)(
          [this](const crow::request& request, const std::string& ruleId) {
            const std::optional<nlohmann::json> body = parseBody(request);
            if (!body || !body->contains("enabled") || !body->at("enabled").is_boolean()) {
              return errorResponse(400, "'enabled' boolean is required");
            }
            const bool enabled = body->at("enabled").get<bool>();
            const auto result = hub_.setRuleEnabled(ruleId, enabled);
            if (result.status == RuleWriteStatus::NotFound) {
              return errorResponse(404, "unknown rule " + ruleId);
            }
            if (!result) {
              return errorResponse(503, "could not save rule");
            }
            return jsonResponse(200, {{"id", ruleId}, {"enabled", enabled}});
          });

  // Manual trigger: fires the rule's actions immediately.
  CROW_ROUTE(app, "/api/v1/rules/<string>/trigger")
      .methods(crow::HTTPMethod::POST)([this](const std::string& ruleId) {
        TriggerEvent event;
        event.kind = "manual";
        event.data["ruleId"] = ruleId;
        const std::vector<std::string> fired = hub_.handleEvent(event);
        if (fired.empty()) {
          return errorResponse(404, "rule " + ruleId + " is unknown or disabled");
        }
        return jsonResponse(200, {{"fired", fired}});
      });

  // --- WebSocket ----------------------------------------------------------
  CROW_WEBSOCKET_ROUTE(app, "/api/v1/ws")
      .subprotocols({"homestr"})
      .onaccept([this](const crow::request& request, void** data) {
        const std::string token = requestToken(request);
        if (!auth_.validateSession(token)) {
          return false;
        }
        *data = new std::string(AuthService::tokenHash(token));
        return true;
      })
      .onopen([this](crow::websocket::connection& connection) {
        std::unique_ptr<std::string> hash{static_cast<std::string*>(connection.userdata())};
        connection.userdata(nullptr);
        if (!hash || !auth_.validateHash(*hash)) {
          connection.close("Session expired", 4001);
          return;
        }
        {
          const std::lock_guard<std::mutex> lock(clientsMutex_);
          clients_.emplace(&connection, *hash);
        }
        // Send the current state so a reconnecting client needs no extra fetch.
        connection.send_text(
            nlohmann::json{{"type", "snapshot"}, {"payload", hub_.dashboard()}}.dump());
      })
      .onclose([this](crow::websocket::connection& connection, const std::string&, uint16_t) {
        const std::lock_guard<std::mutex> lock(clientsMutex_);
        clients_.erase(&connection);
      })
      .onmessage([this](crow::websocket::connection& connection, const std::string& data, bool) {
        const std::lock_guard<std::mutex> lock(clientsMutex_);
        const auto found = clients_.find(&connection);
        if (found == clients_.end() || !auth_.validateHash(found->second)) {
          connection.close("Session expired", 4001);
          return;
        }
        // The WebSocket channel is push-only; "ping" keeps proxies from idling
        // the connection out.
        if (data == "ping") {
          connection.send_text(R"({"type":"pong"})");
        }
      });
}

}  // namespace homestr
