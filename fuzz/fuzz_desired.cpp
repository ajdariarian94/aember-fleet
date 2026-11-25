// GET /api/v1/device/desired as the agent parses it (the fleet server's
// JSON, passed through by aember-auth).
#include <cstdint>
#include <cstdlib>

#include <aember-fleet/updater.h>

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data,
                                      std::size_t size) {
  const auto j = nlohmann::json::parse(data, data + size, nullptr, false);
  if (j.is_discarded()) return 0;  // GetDesired stops at "not JSON"
  const auto d = aember::fleet::ParseDesired(j);
  if (d && (d->poll_interval < 5 || d->poll_interval > 86400)) std::abort();
  return 0;
}
