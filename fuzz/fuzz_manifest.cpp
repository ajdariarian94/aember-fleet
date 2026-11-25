// VerifyManifest: any manifest and signature, and (to reach the JSON checks)
// any manifest signed with the release key. A verified manifest is for a
// sane container name and has a 64-digit hex SHA-256.
#include <cstdint>
#include <cstdlib>

#include <fuzzer/FuzzedDataProvider.h>

#include "release_key.h"
#include <aember-fleet/manifest.h>

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data,
                                      std::size_t size) {
  using namespace aember::fleet;
  static ReleaseKey key;
  FuzzedDataProvider fdp{data, size};
  const bool sign = fdp.ConsumeBool();
  const auto signature = fdp.ConsumeRandomLengthString();
  const auto manifest = fdp.ConsumeRemainingBytesAsString();
  const auto m =
      VerifyManifest(key.pub, manifest, sign ? key.Sign(manifest) : signature);
  if (!m) return 0;
  if (m->sha256.size() != 64 || m->container.empty() ||
      m->container.find('/') != std::string::npos ||
      m->container.find("..") != std::string::npos) {
    std::abort();
  }
  ParsePublicKey(signature);
  return 0;
}
