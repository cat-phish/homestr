#include <chrono>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <string>
#include <thread>

#include <crow.h>
#include <sodium.h>
#include <termios.h>
#include <unistd.h>

#include "homestr/controllers/api_controller.hpp"
#include "homestr/services/database.hpp"
#include "homestr/services/home_str.hpp"

namespace {

constexpr int DEFAULT_PORT = 8080;
constexpr int DB_CONNECT_ATTEMPTS = 30;
constexpr std::chrono::seconds DB_RETRY_DELAY{2};

// Seeded household for local development; multi-household selection comes from
// the session once multi-household selection is implemented.
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

int main(int argc, char** argv) {
  const bool setPassword = argc == 2 && std::string{argv[1]} == "--set-admin-password";
  if (argc > 1 && !setPassword) {
    std::cerr << "Usage: homestr_server [--set-admin-password]\n";
    return 1;
  }
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

  if (setPassword) {
    termios original{};
    const bool terminal = isatty(STDIN_FILENO) != 0;
    if (terminal) {
      if (tcgetattr(STDIN_FILENO, &original) != 0) {
        return 1;
      }
      termios hidden = original;
      hidden.c_lflag &= static_cast<tcflag_t>(~ECHO);
      if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &hidden) != 0) {
        return 1;
      }
    }
    std::string password;
    std::string confirmation;
    std::cerr << "New administrator password: ";
    std::getline(std::cin, password);
    std::cerr << "\nConfirm password: ";
    std::getline(std::cin, confirmation);
    if (terminal) {
      tcsetattr(STDIN_FILENO, TCSAFLUSH, &original);
    }
    std::cerr << '\n';
    bool saved = false;
    try {
      if (!std::cin || password != confirmation) {
        std::cerr << "Passwords do not match or input ended.\n";
      } else {
        const auto admin = hub.database().findUserByEmail("admin@homestr.local");
        if (admin && admin->role == "admin") {
          saved = hub.database().updatePasswordHash(admin->email,
                                                    homestr::AuthService::hashPassword(password));
        }
      }
    } catch (const std::exception& error) {
      std::cerr << error.what() << '\n';
    }
    sodium_memzero(password.data(), password.size());
    sodium_memzero(confirmation.data(), confirmation.size());
    std::cerr << (saved ? "Administrator password updated; sessions revoked.\n"
                        : "Administrator password was not changed.\n");
    return saved ? 0 : 1;
  }

  homestr::App app;
  homestr::ApiController controller{hub};
  controller.registerRoutes(app);

  const int port = portFromEnvironment();
  std::cout << "[hub] listening on 0.0.0.0:" << port << '\n';
  app.bindaddr("0.0.0.0").port(static_cast<std::uint16_t>(port)).multithreaded().run();
  return 0;
}
