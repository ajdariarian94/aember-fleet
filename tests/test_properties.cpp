// Property-based tests (tests/property.h): base64, release manifests, the
// desired state, the state file, and the updater against the invariants of
// spec/tla/FleetUpdate.tla under random assignments, stops and power
// losses.
#include <algorithm>
#include <set>
#include <sstream>

#include "check.h"
#include "property.h"
#include "world.h"
#include <aember-fleet/base64.h>

using namespace fleet_test;
using property::ForAll;
using property::Gen;

namespace {

std::vector<std::uint8_t> ToBytes(const std::string& s) {
  return {s.begin(), s.end()};
}

/// A name the agent accepts: no '/', no "..".
std::string Name(Gen& g) {
  return g.Text("abcdefghijklmnopqrstuvwxyz0123456789-_", 1, 12);
}

/// Any text: quotes, backslashes, control characters, UTF-8 (valid: JSON
/// from the server always is).
std::string AnyText(Gen& g) {
  static const std::vector<std::string> pieces{
      "a", "b", "1", ".", "\"", "\\", "\n", "\t", "\x01", " ", "\xc3\xa9"};
  std::string s;
  for (int i = g.Size(10); i > 0; --i) s += g.Pick(pieces);
  return s;
}

/// A power loss (or kill) at a chosen call into aember-containerd.
struct PowerLoss {};

}  // namespace

int main() {
  ForAll("base64 round trip", 500, [](Gen& g) {
    const auto data = ToBytes(g.Bytes(100));
    const auto text = Base64Encode(data);
    CHECK_EQ(text.size(), (data.size() + 2) / 3 * 4);
    const auto back = Base64Decode(text);
    CHECK(back.has_value() && *back == data);
  });

  ForAll("base64: any text decodes to nothing or to bytes that encode back",
         1000,
         [](Gen& g) {
           const auto text = g.Text("AZaz09+/=-_ \n", 0, 24);
           const auto bytes = Base64Decode(text);
           if (!bytes) return;
           CHECK_EQ(text.size() % 4, 0u);
           // Not always the same text: unused low bits may be set.
           CHECK(Base64Decode(Base64Encode(*bytes)) == bytes);
         });

  ReleaseKey key;
  ForAll(
      "signed manifests verify and give back their fields", 200, [&](Gen& g) {
        Manifest m{Name(g),
                   AnyText(g),
                   AnyText(g),
                   g.Text("0123456789abcdef", 64, 64),
                   static_cast<std::uint64_t>(g.Int(0, 1 << 30))};
        // The server's canonical form (security.canonical_manifest).
        const std::string text = nlohmann::json{
            {"arch", m.arch},
            {"container", m.container},
            {"sha256", m.sha256},
            {"size", m.size},
            {"version", m.version}}.dump();
        const auto got = VerifyManifest(key.pub, text, key.Sign(text));
        CHECK(got.has_value());
        if (!got) return;
        CHECK_EQ(got->container, m.container);
        CHECK_EQ(got->version, m.version);
        CHECK_EQ(got->arch, m.arch);
        CHECK_EQ(got->sha256, m.sha256);
        CHECK_EQ(got->size, m.size);
      });

  ForAll("any changed byte of a manifest or its signature is refused",
         200,
         [&](Gen& g) {
           const std::string text =
               R"({"arch":"x64-poky-linux","container":")" + Name(g) +
               R"(","sha256":")" + std::string(64, 'a') + R"(","size":)" +
               std::to_string(g.Int(0, 1000)) + R"(,"version":")" + Name(g) +
               R"("})";
           const auto sig = key.Sign(text);
           auto changed = text;
           auto& c = changed[static_cast<std::size_t>(
               g.Int(0, static_cast<int>(text.size()) - 1))];
           c = static_cast<char>(c ^ g.Int(1, 255));
           CHECK(!VerifyManifest(key.pub, changed, sig).has_value());
           auto raw = *Base64Decode(sig);
           raw[static_cast<std::size_t>(g.Int(0, 63))] ^=
               static_cast<std::uint8_t>(g.Int(1, 255));
           CHECK(!VerifyManifest(key.pub, text, Base64Encode(raw)).has_value());
         });

  ForAll("desired state: every container comes back as sent", 200, [](Gen& g) {
    auto list = nlohmann::json::array();
    std::vector<DesiredContainer> want;
    for (int i = g.Size(5); i > 0; --i) {
      want.push_back({Name(g), AnyText(g), AnyText(g), AnyText(g), AnyText(g)});
      const auto& c = want.back();
      list.push_back({{"name", c.name},
                      {"version", c.version},
                      {"artifact", c.artifact},
                      {"manifest", c.manifest},
                      {"signature", c.signature}});
    }
    const int interval = g.Int(-100, 100000);
    const auto d = ParseDesired(nlohmann::json::parse(nlohmann::json{
        {"poll_interval", interval}, {"containers", list}}.dump()));
    CHECK(d.has_value());
    if (!d) return;
    CHECK_EQ(d->poll_interval, std::clamp(interval, 5, 86400));
    CHECK_EQ(d->containers.size(), want.size());
    for (std::size_t i = 0; i < want.size() && i < d->containers.size(); ++i) {
      CHECK_EQ(d->containers[i].name, want[i].name);
      CHECK_EQ(d->containers[i].version, want[i].version);
      CHECK_EQ(d->containers[i].signature, want[i].signature);
    }
  });

  ForAll("state file: what an agent saves, the next one reads back",
         100,
         [](Gen& g) {
           World w;
           nlohmann::json containers = nlohmann::json::object();
           for (int i = g.Size(4); i > 0; --i) {
             std::set<std::string> failed;
             for (int k = g.Size(3); k > 0; --k) failed.insert(AnyText(g));
             nlohmann::json e{{"version", AnyText(g)}, {"failed", failed}};
             std::map<std::string, int> interrupted;
             for (int k = g.Size(2); k > 0; --k)
               interrupted[AnyText(g)] = g.Int(1, kMaxInterrupts - 1);
             if (!interrupted.empty()) e["interrupted"] = interrupted;
             containers[Name(g)] = e;
           }
           const nlohmann::json state{{"containers", containers}};
           fs::create_directories(fs::path{w.config.state_file}.parent_path());
           std::ofstream{w.config.state_file} << state.dump();
           w.fleet.desired = {};  // nothing assigned: the round only saves
           Updater u{w.config, w.fleet, w.control, NoSleep};
           u.RunOnce();
           CHECK(nlohmann::json::parse(Read(w.config.state_file)) == state);
         });

  // The updater under random assignments of good and crashing versions,
  // agent stops in the health check and power losses at any call into
  // containerd, against FleetUpdate.tla's invariants:
  //  - the image is never missing; a good image is always on disk
  //    (the image or .prev);
  //  - after a round that ran to the end: the container runs a good image,
  //    and the assigned version is on disk unless it is marked failed;
  //  - a crashing version always ends up failed; failed stays failed.
  ForAll("updater invariants under stops and power losses", 300, [](Gen& g) {
    World w;
    std::map<std::string, std::string> data;  // version -> image
    std::vector<std::string> versions;
    for (int i = 2; i < 2 + g.Int(1, 4); ++i) {
      const auto v = std::to_string(i);
      versions.push_back(v);
      data[v] = (g.Chance(0.35) ? "crash v" : "v") + v + " image";
    }
    const auto good = [&](const std::string& path) {
      std::error_code ec;
      return fs::exists(path, ec) && !Read(path).starts_with("crash");
    };
    std::set<std::string> failed_before;
    std::string assigned;
    std::stringstream log;  // printed when the case fails
    const int failures = check::failures();
    for (int step = 0; step < 8; ++step) {
      if (assigned.empty() || g.Chance(0.5)) {
        assigned = g.Pick(versions);
        w.Assign(assigned, data[assigned]);
      }
      const int mode = g.Int(0, 3);  // 0, 1: to the end; 2: stop; 3: cut
      int cut_at = mode == 3 ? g.Int(1, 6) : -1;
      w.control.on_call = [&](std::string_view) {
        if (cut_at > 0 && --cut_at == 0) throw PowerLoss{};
      };
      bool cut = false;
      auto* const cerr = std::cerr.rdbuf(log.rdbuf());  // the agent's log
      try {
        Updater u{w.config, w.fleet, w.control, mode == 2 ? Stopping : NoSleep};
        u.RunOnce();
      } catch (const PowerLoss&) { cut = true; }
      std::cerr.rdbuf(cerr);
      w.control.on_call = nullptr;
      if (cut) {  // the device boots again: containerd installs what waits
                  // in its inbox, then starts the container
        if (fs::exists(w.inbox + "/echo.aember")) {
          std::ignore = w.control.Install("echo");
        }
        std::ignore = w.control.Start("echo");
        w.control.calls.clear();
      }
      CHECK(fs::exists(w.image));
      CHECK(good(w.image) || good(w.prev));
      if (mode > 1 || cut || w.fleet.reports.empty()) continue;

      const auto echo = w.Echo();
      std::set<std::string> failed;
      if (echo.contains("failed_versions"))
        for (const auto& f : echo["failed_versions"]) failed.insert(f);
      CHECK(std::ranges::includes(failed, failed_before));
      failed_before = failed;
      CHECK(good(w.image));
      CHECK_EQ(w.control.state["echo"], std::string{"running"});
      if (data[assigned].starts_with("crash")) {
        CHECK(failed.contains(assigned));
      } else if (!failed.contains(assigned)) {
        CHECK_EQ(Read(w.image), data[assigned]);
        CHECK_EQ(echo["version"].get<std::string>(), assigned);
      }
    }
    if (check::failures() != failures) std::cerr << "agent log:\n" << log.str();
  });
  return Failures();
}
