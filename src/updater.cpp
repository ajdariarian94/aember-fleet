#include <cstdio>
#include <filesystem>
#include <format>
#include <fstream>
#include <iostream>

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
    entries_[name] = entry;
  }
}

void Updater::SaveState() const {
  nlohmann::json j{{"containers", nlohmann::json::object()}};
  for (const auto& [name, e] : entries_) {
    j["containers"][name] = {{"version", e.version}, {"failed", e.failed}};
  }
  std::error_code ec;
  fs::create_directories(fs::path{config_.state_file}.parent_path(), ec);
  const std::string tmp = config_.state_file + ".tmp";
  {
    std::ofstream out{tmp};
    out << j.dump(2) << '\n';
  }
  fs::rename(tmp, config_.state_file, ec);
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
  if (auto r = Install(want, *m, image->second); !r) {
    fail("error", r.error());
    return;
  }
  if (Healthy(want.name)) {
    e.version = want.version;
    e.state = "updated";
    std::cerr << "aember-fleet-agent: " << want.name << " " << want.version
              << " runs\n";
    return;
  }
  // Roll back to the previous image.
  std::cerr << "aember-fleet-agent: " << want.name << " " << want.version
            << " didn't stay up: rolling back\n";
  std::ignore = control_.Stop(want.name);
  std::error_code ec;
  fs::rename(image->second + ".prev", image->second, ec);
  std::ignore = control_.Start(want.name);
  e.failed.insert(want.version);
  e.state = "rolled-back";
  e.error = std::format("version {} didn't stay running", want.version);
}

std::expected<void, std::string> Updater::Install(const DesiredContainer& want,
                                                  const Manifest& m,
                                                  const std::string& image) {
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
  if (auto r = control_.Stop(want.name); !r) {
    fs::remove(next);
    return std::unexpected("stop: " + r.error());
  }
  std::error_code ec;
  fs::rename(image, image + ".prev", ec);
  if (ec) {
    fs::remove(next);
    std::ignore = control_.Start(want.name);
    return std::unexpected("cannot keep the previous image: " + ec.message());
  }
  fs::rename(next, image, ec);
  if (ec) {
    fs::rename(image + ".prev", image, ec);
    std::ignore = control_.Start(want.name);
    return std::unexpected("cannot put the new image in place");
  }
  std::ignore = control_.Start(want.name);  // a failed start: Healthy says no
  return {};
}

bool Updater::Healthy(const std::string& name) {
  for (int i = 0; i < std::max(1, config_.health_seconds); ++i) {
    sleep_(std::chrono::seconds{1});
    const auto state = control_.State(name);
    if (!state || *state != "running") return false;
  }
  return true;
}

}  // namespace aember::fleet
