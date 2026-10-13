#pragma once

#include <mutex>
#include <string>
#include <unordered_set>

#include <crow.h>
#include <crow/middlewares/cors.h>

#include "homestr/services/home_str.hpp"

namespace homestr {

// CORS is required because the Vite dev server runs on a different origin
// (localhost:5173) from the API (localhost:8080).
using App = crow::App<crow::CORSHandler>;

// REST + WebSocket surface described by specs/api-spec.yaml. The controller
// holds no state of its own beyond the WebSocket subscriber set; all domain
// behaviour lives in HomeStr.
class ApiController {
 public:
  explicit ApiController(HomeStr& hub) : hub_(hub) {}

  // Binds every route onto `app`. Call once, before app.run().
  void registerRoutes(App& app);

  // Pushes a device-state frame to every open WebSocket client.
  void broadcast(const std::string& payload);

 private:
  HomeStr& hub_;
  std::mutex clientsMutex_;
  std::unordered_set<crow::websocket::connection*> clients_;
};

}  // namespace homestr
