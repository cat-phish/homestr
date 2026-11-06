#include "homestr/controllers/auth_middleware.hpp"

#include <gtest/gtest.h>

namespace homestr {
namespace {

class AuthMiddlewareTest : public ::testing::Test {
 protected:
  AuthMiddlewareTest() : auth_(database_) {
    database_.connect();
    database_.addUser({"u1", "Member", "member@example.com",
                       AuthService::hashPassword("member-password"), "user"});
    token_ = auth_.login("member@example.com", "member-password")->token;
    middleware_.auth = &auth_;
  }

  InMemoryDatabase database_;
  AuthService auth_;
  AuthMiddleware middleware_;
  std::string token_;
};

TEST_F(AuthMiddlewareTest, ProtectsEveryApiRouteExceptLogin) {
  for (const auto* path : {"/api/v1/dashboard", "/api/v1/devices", "/api/v1/rules",
                           "/api/v1/auth/me", "/api/v1/auth/logout", "/api/v1/ws"}) {
    crow::request request;
    request.url = path;
    crow::response response;
    AuthMiddleware::context context;
    middleware_.before_handle(request, response, context);
    EXPECT_EQ(response.code, 401) << path;
    EXPECT_TRUE(response.is_completed());
  }
}

TEST_F(AuthMiddlewareTest, LeavesHealthLoginAndPreflightPublic) {
  for (const auto* path : {"/healthz", "/api/v1/auth/login"}) {
    crow::request request;
    request.url = path;
    crow::response response;
    AuthMiddleware::context context;
    middleware_.before_handle(request, response, context);
    EXPECT_FALSE(response.is_completed());
  }
  crow::request request;
  request.url = "/api/v1/devices";
  request.method = crow::HTTPMethod::OPTIONS;
  crow::response response;
  AuthMiddleware::context context;
  middleware_.before_handle(request, response, context);
  EXPECT_FALSE(response.is_completed());
}

TEST_F(AuthMiddlewareTest, ResolvesTheUserAndEnforcesAdminOperations) {
  for (const auto method : {crow::HTTPMethod::GET, crow::HTTPMethod::POST, crow::HTTPMethod::DELETE,
                            crow::HTTPMethod::PATCH}) {
    crow::request request;
    request.url = method == crow::HTTPMethod::DELETE || method == crow::HTTPMethod::PATCH
                      ? "/api/v1/devices/device-1"
                      : "/api/v1/devices";
    request.method = method;
    request.add_header("Authorization", "Bearer " + token_);
    crow::response response;
    AuthMiddleware::context context;
    middleware_.before_handle(request, response, context);
    ASSERT_TRUE(context.user);
    EXPECT_EQ(context.user->id, "u1");
    if (method == crow::HTTPMethod::GET) {
      EXPECT_FALSE(response.is_completed());
    } else {
      EXPECT_EQ(response.code, 403);
    }
  }
}

TEST_F(AuthMiddlewareTest, AcceptsBrowserWebsocketCredentialsOnlyOnWebsocketRoute) {
  crow::request request;
  request.url = "/api/v1/ws";
  request.add_header("Sec-WebSocket-Protocol", "homestr, bearer." + token_);
  crow::response response;
  AuthMiddleware::context context;
  middleware_.before_handle(request, response, context);
  EXPECT_TRUE(context.user);
  EXPECT_FALSE(response.is_completed());
  request.url = "/api/v1/dashboard";
  crow::response denied;
  AuthMiddleware::context other;
  middleware_.before_handle(request, denied, other);
  EXPECT_EQ(denied.code, 401);
}

}  // namespace
}  // namespace homestr
