// fleet.json (/etc/aember/fleet/fleet.json) as main() reads it.
#include <cstdint>
#include <cstdlib>

#include <aember-fleet/updater.h>

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data,
                                      std::size_t size) {
  const auto j = nlohmann::json::parse(data, data + size, nullptr, false);
  const auto c = aember::fleet::ParseAgentConfig(j);
  if (c &&
      (c->arch.empty() || c->health_seconds < 1 || c->health_seconds > 3600)) {
    std::abort();
  }
  return 0;
}
