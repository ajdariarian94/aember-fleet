#include <cstdio>
#include <filesystem>
#include <format>
#include <fstream>
#include <iostream>

#include <unistd.h>

#include <aember-fleet/updater.h>

namespace aember::fleet {

namespace fs = std::filesystem;

std::expected<Desired, std::string> ParseDesired(const nlohmann::json& j) {
  Desired d;
  try {
    d.poll_interval = std::clamp(j.value("poll_interval", 60), 5, 86400);
    for (const auto& c : j.at("containers")) {
      d.containers.push_back({c.at("name").get<std::string>(),
                              c.at("version").get<std::string>(),
                              c.at("artifact").get<std::string>(),
                              c.at("manifest").get<std::string>(),
                              c.at("signature").get<std::string>()});
    }
  } catch (const nlohmann::json::exception& e) {
    return std::unexpected(std::format("desired state: {}", e.what()));
  }
  return d;
}

Updater::Updater(AgentConfig config, FleetApi& fleet, ContainerControl& control,
                 Sleep sleep)
    : config_(std::move(config)),
      fleet_(fleet),
      control_(control),
      sleep_(std::move(sleep)) {
  LoadState();
}

void Updater::LoadImages() {
  images_.clear();
  std::ifstream in{config_.containers_json};
  const auto j = nlohmann::json::parse(in, nullptr, false);
  if (!j.is_object() || !j.contains("containers")) return;
  for (const auto& c : j["containers"]) {
    if (!c.is_object() || !c.contains("name") || !c.contains("container"))
      continue;
    const auto path = c["container"].value("squashfs", std::string{});
    if (!path.empty()) {
      images_[c["name"].get<std::string>()] = config_.host_root + path;
    }
  }
}

void Updater::LoadState() {
  std::ifstream in{config_.state_file};
  const auto j = nlohmann::json::parse(in, nullptr, false);
  if (!j.is_object() || !j.contains("containers")) return;
  for (const auto& [name, e] : j["containers"].items()) {
    Entry entry;
    entry.version = e.value("version", std::string{});
    for (const auto& f : e.value("failed", nlohmann::json::array())) {
      entry.failed.insert(f.get<std::string>());
    }
    entry.pending = e.value("pending", std::string{});
    const auto interrupted = e.value("interrupted", nlohmann::json::object());
    for (const auto& [v, n] : interrupted.items()) {
      if (n.is_number_integer()) entry.interrupted[v] = n.get<int>();
    }
    entries_[name] = entry;
  }
}

void Updater::SaveState() const {
  nlohmann::json j{{"containers", nlohmann::json::object()}};
  for (const auto& [name, e] : entries_) {
    auto& c = j["containers"][name];
    c = {{"version", e.version}, {"failed", e.failed}};
    if (!e.pending.empty()) c["pending"] = e.pending;
    if (!e.interrupted.empty()) c["interrupted"] = e.interrupted;
  }
  std::error_code ec;
  fs::create_directories(fs::path{config_.state_file}.parent_path(), ec);
  const std::string tmp = config_.state_file + ".tmp";
  {
    std::ofstream out{tmp};
    out << j.dump(2) << '\n';
    out.flush();
  }
  // The pending update must be on disk before the container is stopped.
  if (FILE* f = std::fopen(tmp.c_str(), "r+")) {
    ::fsync(::fileno(f));
    std::fclose(f);
  }
  fs::rename(tmp, config_.state_file, ec);
}

void Updater::Recover() {
  // A power loss between the two renames of an older agent left no image,
  // only .prev: put it back.
  for (const auto& [name, image] : images_) {
    std::error_code ec;
    if (!fs::exists(image, ec) && fs::exists(image + ".prev", ec)) {
      std::cerr << "aember-fleet-agent: " << name
                << ": image missing, restoring " << image << ".prev\n";
      fs::rename(image + ".prev", image, ec);
      std::ignore = control_.Start(name);
    }
  }
  // An update cut short (power loss, agent stopped): its image never proved
  // itself. Counted first (and saved), so a recovery cut short counts too.
  for (auto& [name, e] : entries_) {
    if (e.pending.empty()) continue;
    const std::string version = e.pending;
    const int n = ++e.interrupted[version];
    if (n >= kMaxInterrupts) e.failed.insert(version);
    SaveState();
    std::cerr << "aember-fleet-agent: " << name << " " << version
              << ": update was interrupted (" << n << "), rolling back\n";
    if (const auto image = images_.find(name); image != images_.end()) {
      std::error_code ec;
      if (fs::exists(image->second + ".prev", ec)) {
        RollBack(name, image->second);
      }
    }
    e.pending.clear();
    SaveState();
  }
}

nlohmann::json Updater::Status() const {
  auto list = nlohmann::json::array();
  for (const auto& [name, e] : entries_) {
    nlohmann::json c{
        {"name", name}, {"version", e.version}, {"state", e.state}};
    if (!e.error.empty()) c["error"] = e.error;
    if (!e.failed.empty()) c["failed_versions"] = e.failed;
    list.push_back(std::move(c));
  }
  return {{"containers", std::move(list)}};
}

int Updater::RunOnce() {
  LoadImages();
  if (!recovered_) {
    Recover();
    recovered_ = true;
  }
  auto desired = fleet_.GetDesired();
  if (!desired) {
    std::cerr << "aember-fleet-agent: " << desired.error() << '\n';
    return 15;  // aember-auth may still be enrolling: ask again soon
  }
  for (const auto& want : desired->containers) Update(want);
  SaveState();
  if (auto r = fleet_.Report(Status()); !r) {
    std::cerr << "aember-fleet-agent: report: " << r.error() << '\n';
  }
  return desired->poll_interval;
}

void Updater::Update(const DesiredContainer& want) {
  auto& e = entries_[want.name];
  e.error.clear();
  const auto fail = [&e](std::string state, std::string error) {
    e.state = std::move(state);
    e.error = std::move(error);
  };
  if (e.version == want.version) {
    e.state = "current";
    return;
  }
  if (e.failed.contains(want.version)) {
    fail("failed",
         std::format("version {} was rolled back before", want.version));
    return;
  }
  if (want.name == "aember-fleet-agent") {
    fail("error",
         "the agent doesn't update itself (stopping it would cut the update "
         "off)");
    return;
  }
  const auto image = images_.find(want.name);
  if (image == images_.end()) {
    fail("error", "no such container on this device");
    return;
  }
  auto m = VerifyManifest(config_.release_key, want.manifest, want.signature);
  if (!m) {
    fail("error", m.error());
    return;
  }
  if (m->container != want.name || m->version != want.version ||
      m->arch != config_.arch) {
    fail("error",
         std::format(
             "manifest is for {} {} {}", m->container, m->version, m->arch));
    return;
  }
  std::cerr << "aember-fleet-agent: " << want.name << ": "
            << (e.version.empty() ? "image" : e.version) << " -> "
            << want.version << '\n';
  if (auto r = Install(want, *m, image->second, e); !r) {
    fail("error", r.error());
    return;
  }
  switch (Check(want.name)) {
    case Health::kHealthy:
      e.version = want.version;
      e.pending.clear();
      e.state = "updated";
      std::cerr << "aember-fleet-agent: " << want.name << " " << want.version
                << " runs\n";
      break;
    case Health::kUnhealthy:
      std::cerr << "aember-fleet-agent: " << want.name << " " << want.version
                << " didn't stay up: rolling back\n";
      RollBack(want.name, image->second);
      e.failed.insert(want.version);
      e.pending.clear();
      e.state = "rolled-back";
      e.error = std::format("version {} didn't stay running", want.version);
      break;
    case Health::kInterrupted:
      // Stopping: the next agent rolls it back (still pending).
      fail("interrupted", "the agent was stopped during the health check");
      return;
  }
  SaveState();
}

void Updater::RollBack(const std::string& name, const std::string& image) {
  std::ignore = control_.Stop(name);
  std::error_code ec;
  fs::rename(image + ".prev", image, ec);
  std::ignore = control_.Start(name);
}

std::expected<void, std::string> Updater::Install(const DesiredContainer& want,
                                                  const Manifest& m,
                                                  const std::string& image,
                                                  Entry& e) {
  const std::string next = image + ".new";
  {
    std::ofstream out{next, std::ios::binary | std::ios::trunc};
    if (!out) return std::unexpected("cannot write " + next);
    Sha256Hasher sha;
    std::uint64_t got = 0;
    auto r = fleet_.Download(want.artifact, [&](std::string_view chunk) {
      got += chunk.size();
      if (got > m.size) return false;
      sha.Update(chunk);
      out.write(chunk.data(), static_cast<std::streamsize>(chunk.size()));
      return static_cast<bool>(out);
    });
    out.close();
    if (!r || got != m.size || sha.HexDigest() != m.sha256) {
      fs::remove(next);
      return std::unexpected(
          !r ? "download: " + r.error()
             : std::string{"download doesn't match the manifest"});
    }
  }
  // .prev: a hard link to the current image (a copy where links don't
  // work), put in place with one rename. The image itself stays.
  std::error_code ec;
  const std::string prev = image + ".prev";
  const std::string prev_tmp = prev + ".tmp";
  fs::remove(prev_tmp, ec);
  fs::create_hard_link(image, prev_tmp, ec);
  if (ec) fs::copy_file(image, prev_tmp, ec);
  if (!ec) fs::rename(prev_tmp, prev, ec);
  if (ec) {
    fs::remove(prev_tmp, ec);
    fs::remove(next, ec);
    return std::unexpected("cannot keep the previous image");
  }
  // From here on a power loss leaves a pending update for Recover().
  e.pending = want.version;
  SaveState();
  if (auto r = control_.Stop(want.name); !r) {
    fs::remove(next, ec);
    e.pending.clear();
    SaveState();
    return std::unexpected("stop: " + r.error());
  }
  fs::rename(next, image, ec);  // atomic: the old image or the new one
  if (ec) {
    fs::remove(next, ec);
    std::ignore = control_.Start(want.name);
    e.pending.clear();
    SaveState();
    return std::unexpected("cannot put the new image in place");
  }
  std::ignore = control_.Start(want.name);  // a failed start: Check says no
  return {};
}

Updater::Health Updater::Check(const std::string& name) {
  for (int i = 0; i < std::max(1, config_.health_seconds); ++i) {
    if (!sleep_(std::chrono::seconds{1})) return Health::kInterrupted;
    const auto state = control_.State(name);
    if (!state || *state != "running") return Health::kUnhealthy;
  }
  return Health::kHealthy;
}

}  // namespace aember::fleet
