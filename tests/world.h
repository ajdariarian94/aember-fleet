// The updater's test world: a fake fleet server, a fake aember-containerd
// and a device directory with one container ("echo"). Shared by
// test_updater.cpp and test_properties.cpp.
#pragma once

#include <filesystem>
#include <format>
#include <fstream>
#include <functional>
#include <map>
#include <sstream>
#include <string>
#include <vector>

#include "release_key.h"
#include <aember-fleet/updater.h>

namespace fleet_test {

using namespace aember::fleet;
namespace fs = std::filesystem;

inline const std::string kArch = "x64-poky-linux";

inline std::string Sha(std::string_view data) {
  Sha256Hasher h;
  h.Update(data);
  return h.HexDigest();
}

inline std::string Read(const std::string& path) {
  std::ifstream in{path, std::ios::binary};
  std::stringstream s;
  s << in.rdbuf();
  return s.str();
}

struct FakeFleet final : FleetApi {
  Desired desired;
  std::map<std::string, std::string> artifacts;
  std::vector<nlohmann::json> reports;
  int downloads = 0;
  std::expected<Desired, std::string> GetDesired() override { return desired; }
  std::expected<void, std::string> Download(const std::string& id,
                                            const Sink& sink) override {
    ++downloads;
    const auto it = artifacts.find(id);
    if (it == artifacts.end()) return std::unexpected("404");
    if (!sink(it->second)) return std::unexpected("aborted");
    return {};
  }
  std::expected<void, std::string> Report(const nlohmann::json& s) override {
    reports.push_back(s);
    return {};
  }
};

/// containerd stand-in on the device directory: "install" moves the
/// package waiting in the inbox into place (the installed version to
/// <name>.prev, in one step like containerd's RENAME_EXCHANGE), "rollback"
/// swaps them back; a container whose image begins with "crash" fails when
/// it starts. on_call runs first in every call (a test throws from it to
/// cut the agent off there, e.g. a power loss).
struct FakeControl final : ContainerControl {
  std::string lib;  ///< <host>/var/lib/aember
  std::map<std::string, std::string> state;
  std::vector<std::string> calls;
  std::function<void(std::string_view op)> on_call;
  std::function<void()> on_install;
  std::string Image(const std::string& n) const {
    return lib + "/" + n + "/image.squashfs";
  }
  std::expected<std::string, std::string> State(const std::string& n) override {
    if (on_call) on_call("state");
    return state[n];
  }
  std::expected<void, std::string> Stop(const std::string& n) override {
    if (on_call) on_call("stop");
    calls.push_back("stop " + n);
    state[n] = "stopped";
    return {};
  }
  std::expected<void, std::string> Start(const std::string& n) override {
    if (on_call) on_call("start");
    calls.push_back("start " + n);
    state[n] = Read(Image(n)).starts_with("crash") ? "failed" : "running";
    return {};
  }
  void Swap(const std::string& n) {
    const auto dir = lib + "/" + n;
    fs::rename(dir, dir + ".swap");
    if (fs::exists(dir + ".prev")) fs::rename(dir + ".prev", dir);
    fs::rename(dir + ".swap", dir + ".prev");
  }
  std::expected<void, std::string> Install(const std::string& n) override {
    if (on_call) on_call("install");
    if (on_install) on_install();
    calls.push_back("install " + n);
    const auto package = lib + "/packages/" + n + ".aember";
    if (!fs::exists(package)) return std::unexpected("nothing to install");
    const auto next = lib + "/." + n + ".new";
    fs::create_directories(next);
    fs::rename(package, next + "/image.squashfs");
    fs::remove_all(lib + "/" + n + ".prev");
    fs::rename(lib + "/" + n, lib + "/" + n + ".prev");
    fs::rename(next, lib + "/" + n);
    state[n] = Read(Image(n)).starts_with("crash") ? "failed" : "running";
    return {};
  }
  std::expected<void, std::string> Rollback(const std::string& n) override {
    if (on_call) on_call("rollback");
    calls.push_back("rollback " + n);
    if (!fs::exists(lib + "/" + n + ".prev")) {
      return std::unexpected("no previous version");
    }
    Swap(n);
    state[n] = Read(Image(n)).starts_with("crash") ? "failed" : "running";
    return {};
  }
};

struct World {
  std::string dir;
  ReleaseKey key;
  FakeFleet fleet;
  FakeControl control;
  AgentConfig config;
  std::string image;  ///< the installed version's image
  std::string prev;   ///< the one before
  std::string inbox;  ///< where packages wait for containerd

  World() {
    char d[] = "/tmp/aember-fleet-agent-XXXXXX";
    dir = ::mkdtemp(d);
    fs::create_directories(dir + "/host/etc/aember");
    fs::create_directories(dir + "/host/var/lib/aember/echo");
    std::ofstream{dir + "/host/etc/aember/containers.json"}
        << R"({"containers":[{"name":"echo"}]})";
    control.lib = dir + "/host/var/lib/aember";
    image = control.Image("echo");
    prev = control.lib + "/echo.prev/image.squashfs";
    inbox = control.lib + "/packages";
    std::ofstream{image} << "v1 image";
    control.state["echo"] = "running";
    config.release_key = key.pub;
    config.arch = kArch;
    config.containers_json = dir + "/host/etc/aember/containers.json";
    config.host_root = dir + "/host";
    config.state_file = dir + "/state/state.json";
    config.health_seconds = 3;
  }
  ~World() { fs::remove_all(dir); }

  /// Assign @p version of echo with image @p data (signed unless told).
  void Assign(const std::string& version, const std::string& data,
              const std::string& arch = kArch, bool sign = true,
              std::string served = "") {
    const auto m = std::format(
        R"({{"arch":"{}","container":"echo","sha256":"{}","size":{},"version":"{}"}})",
        arch,
        Sha(data),
        data.size(),
        version);
    fleet.artifacts["art-" + version] = served.empty() ? data : served;
    fleet.desired.containers = {{"echo",
                                 version,
                                 "art-" + version,
                                 m,
                                 sign ? key.Sign(m) : key.Sign(m + "x")}};
  }

  nlohmann::json Echo() {
    for (const auto& c : fleet.reports.back()["containers"]) {
      if (c["name"] == "echo") return c;
    }
    return {};
  }
};

inline bool NoSleep(std::chrono::seconds) {
  return true;
}

/// The agent being stopped (SIGTERM) during the health check.
inline bool Stopping(std::chrono::seconds) {
  return false;
}

}  // namespace fleet_test
