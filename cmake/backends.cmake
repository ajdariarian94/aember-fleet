# The crypto pieces the agent links, shared by the SDK build (CMakeLists.txt)
# and the host fuzz build (fuzz/CMakeLists.txt). Needs AEMBER_FLEET_CRYPTO
# and the C language enabled.
#
# - monocypher: Ed25519 release signatures (third_party/, BSD-2-Clause).
# - aember-fleet-crypto: SHA-256 of the images, an INTERFACE target over
#   pkg-config; AEMBER_FLEET_CRYPTO_SOURCE is the matching src/sha256-*.cpp.
set(_root ${CMAKE_CURRENT_LIST_DIR}/..)

add_library(monocypher STATIC ${_root}/third_party/monocypher/monocypher.c
                              ${_root}/third_party/monocypher/monocypher-ed25519.c)
target_include_directories(monocypher SYSTEM PUBLIC ${_root}/third_party/monocypher)
set_target_properties(monocypher PROPERTIES C_STANDARD 99
                                            POSITION_INDEPENDENT_CODE ON)

find_package(PkgConfig REQUIRED)
add_library(aember-fleet-crypto INTERFACE)
if(AEMBER_FLEET_CRYPTO STREQUAL "mbedtls")
  pkg_check_modules(MBEDCRYPTO IMPORTED_TARGET mbedcrypto)
  if(MBEDCRYPTO_FOUND)
    target_link_libraries(aember-fleet-crypto INTERFACE PkgConfig::MBEDCRYPTO)
  else()
    find_library(MBEDCRYPTO_LIB mbedcrypto REQUIRED)
    target_link_libraries(aember-fleet-crypto INTERFACE ${MBEDCRYPTO_LIB})
  endif()
  set(AEMBER_FLEET_CRYPTO_SOURCE ${_root}/src/sha256-mbedtls.cpp)
elseif(AEMBER_FLEET_CRYPTO STREQUAL "wolfssl")
  pkg_check_modules(WOLFSSL REQUIRED IMPORTED_TARGET wolfssl)
  target_link_libraries(aember-fleet-crypto INTERFACE PkgConfig::WOLFSSL)
  set(AEMBER_FLEET_CRYPTO_SOURCE ${_root}/src/sha256-wolfssl.cpp)
else()
  message(FATAL_ERROR "AEMBER_FLEET_CRYPTO must be mbedtls or wolfssl, not '${AEMBER_FLEET_CRYPTO}'")
endif()
message(STATUS "aember-fleet-agent crypto backend: ${AEMBER_FLEET_CRYPTO}")
