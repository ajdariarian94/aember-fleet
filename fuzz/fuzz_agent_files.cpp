// The files the agent reads on the device: containers.json (the board's,
// for the image paths) and its own state.json, which a power cut can leave
// broken. Then one round with nothing assigned.
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>

#include <fuzzer/FuzzedDataProvider.h>
#include <unistd.h>

#include <aember-fleet/updater.h>

namespace {

using namespace aember::fleet;

struct NoFleet final : FleetApi {
  std::expected<Desired, std::string> GetDesired() override {
    return Desired{};
  }
  std::expected<void, std::string> Download(const std::string&,
                                            const Sink&) override {
    return std::unexpected("no");
  }
  std::expected<void, std::string> Report(const nlohmann::json& s) override {
    s.dump();  // what goes over the socket
    return {};
  }
};

struct NoControl final : ContainerControl {
  std::expected<std::string, std::string> State(const std::string&) override {
    return "running";
  }
  std::expected<void, std::string> Stop(const std::string&) override {
    return {};
  }
  std::expected<void, std::string> Install(const std::string&) override {
    return {};
  }
  std::expected<void, std::string> Rollback(const std::string&) override {
    return {};
  }
  std::expected<void, std::string> Start(const std::string&) override {
    return {};
  }
};

}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data,
                                      std::size_t size) {
  static const auto dir = [] {
    auto d = std::filesystem::temp_directory_path() /
             ("aember-fleet-fuzz-" + std::to_string(::getpid()));
    std::filesystem::create_directories(d);
    return d;
  }();
  FuzzedDataProvider fdp{data, size};
  std::ofstream{dir / "containers.json"} << fdp.ConsumeRandomLengthString();
  std::ofstream{dir / "state.json"} << fdp.ConsumeRemainingBytesAsString();
  AgentConfig config;
  config.arch = "x64-poky-linux";
  config.containers_json = (dir / "containers.json").string();
  config.host_root = dir.string();
  config.state_file = (dir / "state.json").string();
  NoFleet fleet;
  NoControl control;
  Updater u{config, fleet, control, [](std::chrono::seconds) { return true; }};
  u.RunOnce();
  return 0;
}
