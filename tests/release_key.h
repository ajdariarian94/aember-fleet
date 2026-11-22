// A release key for the tests (the real one is the admin's, aember-fleet's
// server CLI signs with it).
#pragma once

#include <string>

#include <wolfssl/options.h>
#include <wolfssl/wolfcrypt/random.h>

#include <aember-fleet/base64.h>
#include <aember-fleet/manifest.h>
#include <wolfssl/wolfcrypt/ed25519.h>

struct ReleaseKey {
  ed25519_key key{};
  aember::fleet::PublicKey pub{};
  ReleaseKey() {
    WC_RNG rng{};
    wc_InitRng(&rng);
    wc_ed25519_init(&key);
    wc_ed25519_make_key(&rng, 32, &key);
    wc_FreeRng(&rng);
    word32 len = pub.size();
    wc_ed25519_export_public(&key, pub.data(), &len);
  }
  ~ReleaseKey() { wc_ed25519_free(&key); }
  ReleaseKey(const ReleaseKey&) = delete;
  ReleaseKey& operator=(const ReleaseKey&) = delete;
  std::string Sign(const std::string& m) {
    std::array<std::uint8_t, 64> sig{};
    word32 len = sig.size();
    wc_ed25519_sign_msg(reinterpret_cast<const byte*>(m.data()),
                        m.size(),
                        sig.data(),
                        &len,
                        &key);
    return aember::fleet::Base64Encode(sig);
  }
};
