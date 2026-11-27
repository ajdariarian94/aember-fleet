cmake_minimum_required(VERSION 3.25)
project(aember-fleet-agent VERSION 0.1.0 LANGUAGES C CXX)

# The device agent (the server is Python, in server/). Cross builds: the
# aember-resources SDK's toolchain; the unit tests build with the x64 SDK
# (its crypto library, nlohmann/json) and run through its loader.
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(BUILD_TESTS "Build the unit tests" OFF)

# Our C++ only: third_party/ (C) keeps its own warnings.
add_compile_options("$<$<COMPILE_LANGUAGE:CXX>:-Wall;-Wextra;-Wpedantic;-Werror>")

# SHA-256: mbedTLS (Apache-2.0) from the aember-resources SDK (v0.1.16+). The
# wolfSSL backend (GPL-2.0-only) is gone: a GPL library can't be linked into
# a binary sold under the Aember License (aember's doc/licensing/).

include(cmake/backends.cmake)
find_package(nlohmann_json 3 REQUIRED)

add_library(aember-fleet-agent-lib STATIC src/base64.cpp src/manifest.cpp
                                          src/updater.cpp src/clients.cpp
                                          ${AEMBER_FLEET_CRYPTO_SOURCE})
target_include_directories(aember-fleet-agent-lib PUBLIC include)
target_link_libraries(aember-fleet-agent-lib PUBLIC aember-fleet-crypto monocypher
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
