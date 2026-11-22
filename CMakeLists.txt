cmake_minimum_required(VERSION 3.25)
project(aember-fleet-agent VERSION 0.1.0 LANGUAGES CXX)

# The device agent (the server is Python, in server/). Cross builds: the
# aember-resources SDK's toolchain; the unit tests build with the x64 SDK
# (wolfSSL, nlohmann/json) and run through its loader.
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(BUILD_TESTS "Build the unit tests" OFF)

add_compile_options(-Wall -Wextra -Wpedantic -Werror)

find_package(PkgConfig REQUIRED)
pkg_check_modules(WOLFSSL REQUIRED IMPORTED_TARGET wolfssl)
find_package(nlohmann_json 3 REQUIRED)

add_library(aember-fleet-agent-lib STATIC src/base64.cpp src/manifest.cpp
                                          src/updater.cpp src/clients.cpp)
target_include_directories(aember-fleet-agent-lib PUBLIC include)
target_link_libraries(aember-fleet-agent-lib PUBLIC PkgConfig::WOLFSSL
                                                    nlohmann_json::nlohmann_json)

add_executable(aember-fleet-agent src/main.cpp)
target_link_libraries(aember-fleet-agent PRIVATE aember-fleet-agent-lib)
if(CMAKE_BUILD_TYPE STREQUAL "Release")
  target_link_options(aember-fleet-agent PRIVATE -s)
endif()

include(GNUInstallDirs)
install(TARGETS aember-fleet-agent RUNTIME DESTINATION usr/bin)

if(BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()
