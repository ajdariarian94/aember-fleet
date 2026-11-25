/**
 * @file main.cpp
 * @brief aember-fleet-agent: keeps the device's containers at the versions
 *        the fleet server assigns (signed releases, rollback on failure).
 *
 *   aember-fleet-agent [--config /etc/aember-fleet/fleet.json] [--once]
 *
 * fleet.json: {"release_public_key": "<base64>", "arch": "x64-poky-linux",
 *              "health_seconds": 20}
 */
#include <atomic>
#include <csignal>
#include <fstream>
#include <iostream>
#include <thread>

#include <aember-fleet/clients.h>

namespace {

std::atomic<bool> g_stop{false};

void OnSignal(int) {
  g_stop = true;
}

bool Sleep(std::chrono::seconds s) {
  const auto until = std::chrono::steady_clock::now() + s;
  while (!g_stop && std::chrono::steady_clock::now() < until) {
    std::this_thread::sleep_for(std::chrono::milliseconds{200});
  }
  return !g_stop;
}

}  // namespace

int main(int argc, char** argv) {
  using namespace aember::fleet;
  std::string config_path = "/etc/aember-fleet/fleet.json";
  bool once = false;
  for (int i = 1; i < argc; ++i) {
    const std::string_view a = argv[i];
    if (a == "--config" && i + 1 < argc) {
      config_path = argv[++i];
    } else if (a == "--once") {
      once = true;
    } else {
      std::cerr << "usage: aember-fleet-agent [--config FILE] [--once]\n";
      return 2;
    }
  }
  std::signal(SIGTERM, OnSignal);
  std::signal(SIGINT, OnSignal);
  std::signal(SIGPIPE, SIG_IGN);

  std::ifstream in{config_path};
  const auto j = nlohmann::json::parse(in, nullptr, false);
  if (!j.is_object()) {
    std::cerr << "aember-fleet-agent: cannot read " << config_path << '\n';
    return 1;
  }
  auto parsed = ParseAgentConfig(j);
  if (!parsed) {
    std::cerr << "aember-fleet-agent: " << config_path << ": " << parsed.error()
              << '\n';
    return 1;
  }
  const AgentConfig config = *parsed;

  AuthSocketFleet fleet{config.arch};
  ControlSocket control;
  Updater updater{config, fleet, control, Sleep};
  std::cerr << "aember-fleet-agent: " << config.arch << ", health check "
            << config.health_seconds << " s\n";
  while (!g_stop) {
    const int interval = updater.RunOnce();
    if (once) break;
    Sleep(std::chrono::seconds{interval});
  }
  return 0;
}
