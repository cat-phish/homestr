#include <chrono>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <string>
#include <thread>

#include <crow.h>

#include "homestr/controllers/api_controller.hpp"
#include "homestr/services/database.hpp"
#include "homestr/services/home_str.hpp"

namespace {

constexpr int DEFAULT_PORT = 8080;
constexpr int DB_CONNECT_ATTEMPTS = 30;
constexpr std::chrono::seconds DB_RETRY_DELAY{2};

// Seeded household for local development; multi-household selection comes from
// the session once the auth middleware lands.
constexpr const char* DEFAULT_HOUSEHOLD = "00000000-0000-0000-0000-0000000000a1";

int portFromEnvironment() {
  const char* value = std::getenv("HOMESTR_HTTP_PORT");
  if (value == nullptr || *value == '\0') {
    return DEFAULT_PORT;
  }
  const int port = std::atoi(value);
  return (port > 0 && port < 65536) ? port : DEFAULT_PORT;
}

}  // namespace

int main() {
  const homestr::DatabaseConfig config = homestr::DatabaseConfig::fromEnvironment();
  homestr::HomeStr hub{homestr::makeDatabase(config)};

  // MariaDB's own init scripts run on first boot, so the backend can outrace
  // the database even with a compose healthcheck. Retry instead of
  // crash-looping.
  bool started = false;
  for (int attempt = 1; attempt <= DB_CONNECT_ATTEMPTS && !started; ++attempt) {
    started = hub.start(DEFAULT_HOUSEHOLD);
    if (!started) {
      std::cerr << "[hub] database not ready (attempt " << attempt << "/" << DB_CONNECT_ATTEMPTS
                << "), retrying in " << DB_RETRY_DELAY.count() << "s\n";
      std::this_thread::sleep_for(DB_RETRY_DELAY);
    }
  }
  if (!started) {
    std::cerr << "[hub] giving up: cannot reach " << config.connectionUrl() << '\n';
    return 1;
  }

  homestr::App app;
  homestr::ApiController controller{hub};
  controller.registerRoutes(app);

  const int port = portFromEnvironment();
  std::cout << "[hub] listening on 0.0.0.0:" << port << '\n';
  app.bindaddr("0.0.0.0").port(static_cast<std::uint16_t>(port)).multithreaded().run();
  return 0;
}
