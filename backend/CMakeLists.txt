cmake_minimum_required(VERSION 3.22)

project(
  homestr_backend
  VERSION 0.1.0
  DESCRIPTION "HomeStr IoT home automation hub (C++20 REST + WebSocket server)"
  LANGUAGES CXX)

# ---------------------------------------------------------------------------
# Toolchain
# ---------------------------------------------------------------------------
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON) # feeds clangd / clang-tidy

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE
      Debug
      CACHE STRING "Build type" FORCE)
endif()

option(HOMESTR_BUILD_TESTS "Build the GoogleTest suites" ON)
option(HOMESTR_WARNINGS_AS_ERRORS "Treat compiler warnings as errors" OFF)
option(HOMESTR_WITH_MARIADB "Link MariaDB Connector/C++ (off = in-memory repository)" ON)

add_library(homestr_warnings INTERFACE)
if(MSVC)
  target_compile_options(homestr_warnings INTERFACE /W4 /permissive-)
  if(HOMESTR_WARNINGS_AS_ERRORS)
    target_compile_options(homestr_warnings INTERFACE /WX)
  endif()
else()
  target_compile_options(
    homestr_warnings INTERFACE -Wall -Wextra -Wpedantic -Wshadow
                               -Wnon-virtual-dtor -Wold-style-cast -Wconversion)
  if(HOMESTR_WARNINGS_AS_ERRORS)
    target_compile_options(homestr_warnings INTERFACE -Werror)
  endif()
endif()

# ---------------------------------------------------------------------------
# Dependencies
#
# Each dependency is looked for on the system first; FetchContent is the
# fallback so a clean checkout builds without a package manager. The Docker
# image pre-installs them, so container builds stay cache-friendly.
# ---------------------------------------------------------------------------
include(FetchContent)
set(FETCHCONTENT_QUIET OFF)

# nlohmann/json -- JSON (de)serialisation for the REST layer.
find_package(nlohmann_json 3.11 QUIET)
if(NOT nlohmann_json_FOUND)
  FetchContent_Declare(
    nlohmann_json
    GIT_REPOSITORY https://github.com/nlohmann/json.git
    GIT_TAG v3.11.3
    GIT_SHALLOW TRUE)
  set(JSON_BuildTests OFF CACHE INTERNAL "")
  FetchContent_MakeAvailable(nlohmann_json)
endif()

# Crow -- HTTP routing plus the WebSocket endpoint the WebUI subscribes to.
# (cpp-httplib is the alternative, but it has no WebSocket support.)
find_package(Crow QUIET)
if(NOT Crow_FOUND)
  FetchContent_Declare(
    Crow
    GIT_REPOSITORY https://github.com/CrowCpp/Crow.git
    GIT_TAG v1.2.1.2
    GIT_SHALLOW TRUE)
  set(CROW_BUILD_EXAMPLES OFF CACHE INTERNAL "")
  set(CROW_BUILD_TESTS OFF CACHE INTERNAL "")
  set(CROW_INSTALL OFF CACHE INTERNAL "")
  FetchContent_MakeAvailable(Crow)
endif()

# Crow's headers do not survive -Wconversion, and they are not ours to fix.
# Marking the interface includes as SYSTEM silences them without weakening the
# warning set for HomeStr's own code.
foreach(crowTarget Crow crow)
  if(TARGET ${crowTarget})
    get_target_property(crowIncludes ${crowTarget} INTERFACE_INCLUDE_DIRECTORIES)
    if(crowIncludes)
      set_target_properties(
        ${crowTarget} PROPERTIES INTERFACE_SYSTEM_INCLUDE_DIRECTORIES
                                  "${crowIncludes}")
    endif()
  endif()
endforeach()

find_package(Threads REQUIRED)

# MariaDB Connector/C++ -- optional so unit tests need no database.
set(HOMESTR_MARIADB_AVAILABLE OFF)
if(HOMESTR_WITH_MARIADB)
  find_path(MARIADB_CPP_INCLUDE_DIR mariadb/conncpp.hpp)
  # The connector installs its shared object into a `mariadb` subdirectory.
  find_library(
    MARIADB_CPP_LIBRARY
    NAMES mariadbcpp
    PATH_SUFFIXES mariadb mariadb-connector-cpp)
  if(MARIADB_CPP_INCLUDE_DIR AND MARIADB_CPP_LIBRARY)
    set(HOMESTR_MARIADB_AVAILABLE ON)
    message(STATUS "MariaDB Connector/C++: ${MARIADB_CPP_LIBRARY}")
  else()
    message(
      WARNING
        "MariaDB Connector/C++ not found; building with the in-memory repository. "
        "Install mariadb-connector-cpp or configure with -DHOMESTR_WITH_MARIADB=OFF.")
  endif()
endif()

# ---------------------------------------------------------------------------
# homestr_core -- domain model and services, free of transport concerns.
# ---------------------------------------------------------------------------
add_library(
  homestr_core STATIC
  src/models/user.cpp
  src/models/smart_device.cpp
  src/models/light.cpp
  src/models/smart_speaker.cpp
  src/models/automation_rule.cpp
  src/services/device_manager.cpp
  src/services/database.cpp
  src/services/home_str.cpp)

target_include_directories(homestr_core PUBLIC include)
target_link_libraries(homestr_core PUBLIC nlohmann_json::nlohmann_json
                                          Threads::Threads
                                   PRIVATE homestr_warnings)

if(HOMESTR_MARIADB_AVAILABLE)
  target_compile_definitions(homestr_core PUBLIC HOMESTR_HAS_MARIADB=1)
  target_include_directories(homestr_core SYSTEM PUBLIC ${MARIADB_CPP_INCLUDE_DIR})
  target_link_libraries(homestr_core PUBLIC ${MARIADB_CPP_LIBRARY})
else()
  target_compile_definitions(homestr_core PUBLIC HOMESTR_HAS_MARIADB=0)
endif()

# ---------------------------------------------------------------------------
# homestr_server -- REST/WebSocket entry point.
# ---------------------------------------------------------------------------
add_executable(homestr_server src/main.cpp src/controllers/api_controller.cpp)

target_link_libraries(homestr_server PRIVATE homestr_core Crow::Crow
                                             homestr_warnings)

# ---------------------------------------------------------------------------
# Tests
# ---------------------------------------------------------------------------
if(HOMESTR_BUILD_TESTS)
  enable_testing()

  find_package(GTest QUIET)
  if(NOT GTest_FOUND)
    FetchContent_Declare(
      googletest
      GIT_REPOSITORY https://github.com/google/googletest.git
      GIT_TAG v1.15.2
      GIT_SHALLOW TRUE)
    set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
    set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
    FetchContent_MakeAvailable(googletest)
  endif()

  include(GoogleTest)

  add_executable(homestr_tests tests/device_manager_test.cpp
                               tests/automation_rule_test.cpp
                               tests/home_str_test.cpp
                               tests/user_test.cpp)

  target_link_libraries(homestr_tests PRIVATE homestr_core GTest::gtest_main
                                              GTest::gmock homestr_warnings)

  gtest_discover_tests(homestr_tests DISCOVERY_TIMEOUT 30)
endif()
