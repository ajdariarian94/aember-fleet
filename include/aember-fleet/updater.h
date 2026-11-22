/**
 * @file updater.h
 * @brief The agent's work: bring the device's containers to the versions the
 *        fleet server assigns, one container at a time, and roll back what
 *        doesn't stay up.
 *
 * For each assigned container whose version differs from the one on the
 * device: check the release signature and the manifest, download the image
 * next to the current one (<image>.new, size and SHA-256 checked), stop the
 * container, keep the current image as <image>.prev, put the new one in
 * place, start it, and watch it for health_seconds. A container that isn't
 * running at any moment of that window is rolled back to <image>.prev and
 * the version is marked failed (not tried again until the server assigns
 * another one). Every round ends with a status report.
 */
#pragma once

#include <nlohmann/json.hpp>

#include <chrono>
#include <expected>
#include <functional>
#include <map>
#include <set>
#include <string>
#include <vector>

#include <aember-fleet/manifest.h>

namespace aember::fleet {

struct DesiredContainer {
  std::string name;
  std::string version;
  std::string artifact;
  std::string manifest;
  std::string signature;
};

struct Desired {
  int poll_interval = 60;
  std::vector<DesiredContainer> containers;
};

/// Parse GET /api/v1/device/desired.
std::expected<Desired, std::string> ParseDesired(const nlohmann::json& j);

using Sink = std::function<bool(std::string_view chunk)>;

/// The fleet server (through aember-auth's socket on a device).
class FleetApi {
 public:
  virtual ~FleetApi() = default;
  virtual std::expected<Desired, std::string> GetDesired() = 0;
  virtual std::expected<void, std::string> Download(const std::string& artifact,
                                                    const Sink& sink) = 0;
  virtual std::expected<void, std::string> Report(
      const nlohmann::json& status) = 0;
};

/// aember-containerd (its control socket on a device).
class ContainerControl {
 public:
  virtual ~ContainerControl() = default;
  /// "running", "stopped", "failed", ...
  virtual std::expected<std::string, std::string> State(
      const std::string& name) = 0;
  virtual std::expected<void, std::string> Stop(const std::string& name) = 0;
  virtual std::expected<void, std::string> Start(const std::string& name) = 0;
};

struct AgentConfig {
  PublicKey release_key{};
  std::string arch;
  /// The device's containers.json (squashfs paths) and the prefix its
  /// paths have here (the device's root is bound at /host in the agent).
  std::string containers_json = "/host/etc/aember/containers.json";
  std::string host_root = "/host";
  std::string state_file = "/var/lib/aember-fleet/state.json";
  int health_seconds = 20;
};

class Updater {
 public:
  using Sleep = std::function<void(std::chrono::seconds)>;

  Updater(AgentConfig config, FleetApi& fleet, ContainerControl& control,
          Sleep sleep);

  /// One round: desired state, updates, status report. Returns the poll
  /// interval to wait before the next round.
  int RunOnce();

  /// What the last round did, per container (also the status report).
  [[nodiscard]] nlohmann::json Status() const;

 private:
  struct Entry {
    std::string version;           ///< installed through the fleet, or ""
    std::set<std::string> failed;  ///< versions that were rolled back
    std::string state;             ///< "current", "updated", "failed", ...
    std::string error;
  };

  void LoadImages();
  void LoadState();
  void SaveState() const;
  void Update(const DesiredContainer& want);
  std::expected<void, std::string> Install(const DesiredContainer& want,
                                           const Manifest& m,
                                           const std::string& image);
  bool Healthy(const std::string& name);

  AgentConfig config_;
  FleetApi& fleet_;
  ContainerControl& control_;
  Sleep sleep_;
  std::map<std::string, std::string> images_;  ///< container -> squashfs path
  std::map<std::string, Entry> entries_;
};

}  // namespace aember::fleet
