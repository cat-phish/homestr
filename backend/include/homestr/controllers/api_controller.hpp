#pragma once

#include <mutex>
#include <string>
#include <unordered_map>

#include <crow.h>
#include <crow/middlewares/cors.h>

#include "homestr/controllers/auth_middleware.hpp"
#include "homestr/services/home_str.hpp"

namespace homestr {

// CORS is required because the Vite dev server runs on a different origin
// (localhost:5173) from the API (localhost:8080).
using App = crow::App<crow::CORSHandler, AuthMiddleware>;

// REST + WebSocket surface described by specs/api-spec.yaml. The controller
// owns authentication and WebSocket subscriptions; domain behavior lives in
// HomeStr.
class ApiController {
 public:
  explicit ApiController(HomeStr& hub) : hub_(hub), auth_(hub.database()) {}

  // Binds every route onto `app`. Call once, before app.run().
  void registerRoutes(App& app);

  // Pushes a device-state frame to every open WebSocket client.
  void broadcast(const std::string& payload);

 private:
  void expireClients();
  HomeStr& hub_;
  AuthService auth_;
  std::mutex clientsMutex_;
  std::unordered_map<crow::websocket::connection*, std::string> clients_;
};

}  // namespace homestr
