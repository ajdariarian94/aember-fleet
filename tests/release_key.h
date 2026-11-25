// A release key for the tests (the real one is the admin's, aember-fleet's
// server CLI signs with it). Ed25519 through Monocypher, like the agent.
#pragma once

#include <array>
#include <cstdint>
#include <string>

#include <sys/random.h>

#include <aember-fleet/base64.h>
#include <aember-fleet/manifest.h>

extern "C" {
#include <monocypher-ed25519.h>
#include <monocypher.h>
}

struct ReleaseKey {
  std::array<std::uint8_t, 64> secret{};
  aember::fleet::PublicKey pub{};
  ReleaseKey() {
    std::array<std::uint8_t, 32> seed{};
    for (std::size_t got = 0; got < seed.size();) {
      const ssize_t n = ::getrandom(seed.data() + got, seed.size() - got, 0);
      if (n > 0) got += static_cast<std::size_t>(n);
    }
    crypto_ed25519_key_pair(secret.data(), pub.data(), seed.data());
  }
  ~ReleaseKey() { crypto_wipe(secret.data(), secret.size()); }
  ReleaseKey(const ReleaseKey&) = delete;
  ReleaseKey& operator=(const ReleaseKey&) = delete;
  std::string Sign(const std::string& m) {
    std::array<std::uint8_t, 64> sig{};
    crypto_ed25519_sign(sig.data(), secret.data(),
                        reinterpret_cast<const std::uint8_t*>(m.data()),
                        m.size());
    return aember::fleet::Base64Encode(sig);
  }
};
