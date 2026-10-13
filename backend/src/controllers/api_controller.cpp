#include "homestr/controllers/api_controller.hpp"

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
  for (crow::websocket::connection* client : clients_) {
    client->send_text(payload);
  }
}

void ApiController::registerRoutes(App& app) {
  // --- Health -------------------------------------------------------------
  CROW_ROUTE(app, "/healthz")
  ([this] {
    return jsonResponse(
        200, {{"status", "ok"}, {"database", hub_.database().isConnected() ? "up" : "down"}});
  });

  // --- Login --------------------------------------------------------------
  // Password verification is stubbed: hashing belongs to the auth service,
  // which is the next spec item. Until then the endpoint only proves the user
  // exists and never accepts an empty password.
  CROW_ROUTE(app, "/api/v1/auth/login")
      .methods(crow::HTTPMethod::POST)([this](const crow::request& request) {
        const std::optional<nlohmann::json> body = parseBody(request);
        if (!body) {
          return errorResponse(400, "malformed JSON body");
        }
        const std::string email = body->value("email", std::string{});
        const std::string password = body->value("password", std::string{});
        if (email.empty() || password.empty()) {
          return errorResponse(400, "email and password are required");
        }

        const std::optional<UserRecord> record = hub_.database().findUserByEmail(email);
        if (!record) {
          return errorResponse(401, "invalid credentials");
        }

        // TODO(backend): verify `password` against record->passwordHash with
        // bcrypt and persist the session in `sessions`.
        return jsonResponse(
            200, {{"token", "dev-token-" + record->id}, {"user", record->toUser().toJson()}});
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
                            {"type", record.deviceType}});
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
    const SmartDevice* device = hub_.devices().findById(deviceId);
    if (device == nullptr) {
      return errorResponse(404, "unknown device " + deviceId);
    }
    return jsonResponse(200, device->toJson());
  });

  CROW_ROUTE(app, "/api/v1/devices/<string>")
      .methods(crow::HTTPMethod::DELETE)([this](const std::string& deviceId) {
        if (!hub_.devices().unregisterDevice(deviceId)) {
          return errorResponse(404, "unknown device " + deviceId);
        }
        return jsonResponse(204, nlohmann::json::object());
      });

  // --- Device control -----------------------------------------------------
  CROW_ROUTE(app, "/api/v1/devices/<string>/commands")
      .methods(crow::HTTPMethod::POST)(
          [this](const crow::request& request, const std::string& deviceId) {
            const std::optional<nlohmann::json> body = parseBody(request);
            if (!body) {
              return errorResponse(400, "malformed JSON body");
            }
            const std::string command = body->value("command", std::string{});
            if (command.empty()) {
              return errorResponse(400, "'command' is required");
            }
            const nlohmann::json params = body->value("params", nlohmann::json::object());

            // TODO(backend): take the actor id from the bearer token once the
            // auth middleware lands.
            const CommandResult result = hub_.devices().sendCommand(deviceId, command, params, {});
            if (!result.ok) {
              const int code = result.error.starts_with("unknown device") ? 404 : 409;
              return errorResponse(code, result.error);
            }

            const SmartDevice* device = hub_.devices().findById(deviceId);
            const nlohmann::json state =
                device != nullptr ? device->toJson() : nlohmann::json::object();
            broadcast(nlohmann::json{{"type", "device_state"}, {"device", state}}.dump());
            return jsonResponse(200, state);
          });

  // --- Automation rules ---------------------------------------------------
  CROW_ROUTE(app, "/api/v1/rules")
  ([this] {
    nlohmann::json rules = nlohmann::json::array();
    for (const AutomationRule& rule : hub_.rules()) {
      rules.push_back(rule.toJson());
    }
    return jsonResponse(200, {{"rules", std::move(rules)}});
  });

  CROW_ROUTE(app, "/api/v1/rules")
      .methods(crow::HTTPMethod::POST)([this](const crow::request& request) {
        const std::optional<nlohmann::json> body = parseBody(request);
        if (!body) {
          return errorResponse(400, "malformed JSON body");
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

        nlohmann::json created = rule.toJson();
        if (!hub_.addRule(std::move(rule))) {
          return errorResponse(409, "rule " + id + " already exists");
        }
        return jsonResponse(201, created);
      });

  CROW_ROUTE(app, "/api/v1/rules/<string>/enabled")
      .methods(crow::HTTPMethod::PUT)(
          [this](const crow::request& request, const std::string& ruleId) {
            const std::optional<nlohmann::json> body = parseBody(request);
            if (!body || !body->contains("enabled")) {
              return errorResponse(400, "'enabled' boolean is required");
            }
            const bool enabled = body->at("enabled").get<bool>();
            if (!hub_.setRuleEnabled(ruleId, enabled)) {
              return errorResponse(404, "unknown rule " + ruleId);
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
      .onopen([this](crow::websocket::connection& connection) {
        {
          const std::lock_guard<std::mutex> lock(clientsMutex_);
          clients_.insert(&connection);
        }
        // Send the current state so a reconnecting client needs no extra fetch.
        connection.send_text(
            nlohmann::json{{"type", "snapshot"}, {"payload", hub_.dashboard()}}.dump());
      })
      .onclose([this](crow::websocket::connection& connection, const std::string&, uint16_t) {
        const std::lock_guard<std::mutex> lock(clientsMutex_);
        clients_.erase(&connection);
      })
      .onmessage([](crow::websocket::connection& connection, const std::string& data, bool) {
        // The WebSocket channel is push-only; "ping" keeps proxies from idling
        // the connection out.
        if (data == "ping") {
          connection.send_text(R"({"type":"pong"})");
        }
      });
}

}  // namespace homestr
