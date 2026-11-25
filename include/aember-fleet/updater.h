/**
 * @file updater.h
 * @brief The agent's work: bring the device's containers to the versions the
 *        fleet server assigns, one container at a time, and roll back what
 *        doesn't stay up.
 *
 * For each assigned container whose version differs from the one on the
 * device: check the release signature and the manifest, download the image
 * next to the current one (<image>.new, size and SHA-256 checked), keep the
 * current image as <image>.prev (a hard link: the image itself never goes
 * away), save the update as pending, stop the container, put the new image
 * in place (one rename), start it, and watch it for health_seconds. A
 * container that isn't running at any moment of that window is rolled back
 * to <image>.prev and the version is marked failed (not tried again until
 * the server assigns another one). Every round ends with a status report.
 *
 * Power loss (or the agent stopped) at any step: the next agent finds the
 * pending update in its state file and rolls it back before anything else
 * (the new image never proved itself); an update interrupted
 * kMaxInterrupts times counts as failed. Model: spec/tla/FleetUpdate.tla.
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

/// Updates of one version interrupted this often count as failed.
inline constexpr int kMaxInterrupts = 2;

/// fleet.json: release_public_key (base64), arch, health_seconds (1..3600,
/// default 20); errors name the bad field.
std::expected<AgentConfig, std::string> ParseAgentConfig(
    const nlohmann::json& j);

/// fsync of a directory: makes a rename in it survive a power loss (the
/// file's own fsync doesn't cover its directory entry).
void SyncDir(const std::string& dir);

class Updater {
 public:
  /// Waits; returns false when the agent is being stopped (SIGTERM): a
  /// health check cut short decides nothing.
  using Sleep = std::function<bool(std::chrono::seconds)>;

  Updater(AgentConfig config, FleetApi& fleet, ContainerControl& control,
          Sleep sleep);

  /// One round: desired state, updates, status report. Returns the poll
  /// interval to wait before the next round.
  int RunOnce();

  /// What the last round did, per container (also the status report).
  [[nodiscard]] nlohmann::json Status() const;

  /// Called with a directory after each rename in it that must survive a
  /// power loss (state file, image swap); SyncDir. Tests record the calls.
  std::function<void(const std::string&)> sync_dir = SyncDir;

 private:
  struct Entry {
    std::string version;           ///< installed through the fleet, or ""
    std::set<std::string> failed;  ///< versions that were rolled back
    std::string pending;  ///< version being installed (saved before the swap)
    std::map<std::string, int> interrupted;  ///< version -> interruptions
    std::string state;                       ///< "current", "updated", ...
    std::string error;
  };
  enum class Health { kHealthy, kUnhealthy, kInterrupted };

  void LoadImages();
  void LoadState();
  void SaveState() const;
  void Recover();
  void Update(const DesiredContainer& want);
  std::expected<void, std::string> Install(const DesiredContainer& want,
                                           const Manifest& m,
                                           const std::string& image, Entry& e);
  void RollBack(const std::string& name, const std::string& image);
  /// fs::rename, then sync_dir of the target's directory.
  void DurableRename(const std::string& from, const std::string& to,
                     std::error_code& ec) const;
  Health Check(const std::string& name);

  AgentConfig config_;
  FleetApi& fleet_;
  ContainerControl& control_;
  Sleep sleep_;
  std::map<std::string, std::string> images_;  ///< container -> squashfs path
  std::map<std::string, Entry> entries_;
  bool recovered_ = false;
};

}  // namespace aember::fleet
