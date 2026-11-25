#include <algorithm>
#include <filesystem>
#include <format>
#include <fstream>
#include <sstream>

#include "check.h"
#include "world.h"

using namespace fleet_test;

int main() {
  {  // update, then nothing to do
    World w;
    Updater u{w.config, w.fleet, w.control, NoSleep};
    w.Assign("2", "v2 image");
    u.RunOnce();
    CHECK_EQ(Read(w.image), std::string{"v2 image"});
    CHECK_EQ(Read(w.image + ".prev"), std::string{"v1 image"});
    CHECK(!fs::exists(w.image + ".new"));
    CHECK_EQ(w.Echo()["state"].get<std::string>(), std::string{"updated"});
    CHECK_EQ(w.Echo()["version"].get<std::string>(), std::string{"2"});
    CHECK(w.control.calls ==
          (std::vector<std::string>{"stop echo", "start echo"}));
    u.RunOnce();
    CHECK_EQ(w.Echo()["state"].get<std::string>(), std::string{"current"});
    CHECK_EQ(w.fleet.downloads, 1);
    // A restarted agent remembers the version.
    Updater again{w.config, w.fleet, w.control, NoSleep};
    again.RunOnce();
    CHECK_EQ(w.Echo()["state"].get<std::string>(), std::string{"current"});
    CHECK_EQ(w.fleet.downloads, 1);
  }
  {  // a crashing version is rolled back and not tried again
    World w;
    Updater u{w.config, w.fleet, w.control, NoSleep};
    w.Assign("3", "crash image");
    u.RunOnce();
    CHECK_EQ(Read(w.image), std::string{"v1 image"});
    CHECK_EQ(w.control.state["echo"], std::string{"running"});
    CHECK_EQ(w.Echo()["state"].get<std::string>(), std::string{"rolled-back"});
    CHECK(w.Echo()["failed_versions"] == nlohmann::json::array({"3"}));
    u.RunOnce();
    CHECK_EQ(w.Echo()["state"].get<std::string>(), std::string{"failed"});
    CHECK_EQ(w.fleet.downloads, 1);
    w.Assign("4", "v4 image");  // a new version is tried
    u.RunOnce();
    CHECK_EQ(Read(w.image), std::string{"v4 image"});
    CHECK_EQ(w.Echo()["state"].get<std::string>(), std::string{"updated"});
  }
  {  // what is refused before anything is touched
    struct Case {
      const char* what;
      std::function<void(World&)> assign;
    };
    const Case cases[] = {
        {"bad signature", [](World& w) { w.Assign("2", "v2", kArch, false); }},
        {"other arch",
         [](World& w) { w.Assign("2", "v2", "aarch64-poky-linux"); }},
        {"corrupt download",
         [](World& w) { w.Assign("2", "v2 image", kArch, true, "v2 imagE"); }},
    };
    for (const auto& c : cases) {
      World w;
      Updater u{w.config, w.fleet, w.control, NoSleep};
      c.assign(w);
      u.RunOnce();
      CHECK_EQ(Read(w.image), std::string{"v1 image"});
      CHECK(w.control.calls.empty());
      CHECK(!fs::exists(w.image + ".new"));
      if (w.Echo()["state"] != "error") std::cerr << c.what << '\n';
      CHECK_EQ(w.Echo()["state"].get<std::string>(), std::string{"error"});
    }
  }
  {  // a container the device doesn't have
    World w;
    Updater u{w.config, w.fleet, w.control, NoSleep};
    w.Assign("2", "v2");
    w.fleet.desired.containers[0].name = "nope";
    u.RunOnce();
    bool found = false;
    for (const auto& c : w.fleet.reports.back()["containers"]) {
      if (c["name"] == "nope") {
        found = true;
        CHECK_EQ(c["state"].get<std::string>(), std::string{"error"});
      }
    }
    CHECK(found);
  }
  {  // the image never goes away, and the update is saved before the stop
    World w;
    Updater u{w.config, w.fleet, w.control, NoSleep};
    w.Assign("2", "v2 image");
    bool checked = false;
    w.control.on_stop = [&] {
      if (checked) return;
      checked = true;
      CHECK_EQ(Read(w.image), std::string{"v1 image"});
      CHECK_EQ(Read(w.image + ".prev"), std::string{"v1 image"});
      const auto state = nlohmann::json::parse(Read(w.config.state_file));
      CHECK_EQ(state["containers"]["echo"]["pending"].get<std::string>(),
               std::string{"2"});
    };
    u.RunOnce();
    CHECK(checked);
    CHECK_EQ(Read(w.image), std::string{"v2 image"});
    const auto state = nlohmann::json::parse(Read(w.config.state_file));
    CHECK(!state["containers"]["echo"].contains("pending"));
  }
  {  // Regression (spec/tla/FleetUpdate.tla, GoodKept): an agent stopped in
     // the health check of a crashing version; the next one used to move the
     // crashing image to .prev and lose the good one for good.
    World w;
    {
      Updater u{w.config, w.fleet, w.control, Stopping};
      w.Assign("3", "crash image");
      u.RunOnce();
      CHECK_EQ(w.Echo()["state"].get<std::string>(),
               std::string{"interrupted"});
    }
    Updater again{w.config, w.fleet, w.control, NoSleep};
    again.RunOnce();  // rolled back first, tried again, rolled back
    CHECK_EQ(Read(w.image), std::string{"v1 image"});
    CHECK_EQ(w.control.state["echo"], std::string{"running"});
    CHECK_EQ(w.Echo()["state"].get<std::string>(), std::string{"rolled-back"});
    CHECK(w.Echo()["failed_versions"] == nlohmann::json::array({"3"}));
  }
  {  // interrupted kMaxInterrupts times: failed, not tried again
    World w;
    w.Assign("2", "v2 image");
    for (int i = 0; i < kMaxInterrupts; ++i) {
      Updater u{w.config, w.fleet, w.control, Stopping};
      u.RunOnce();
    }
    Updater last{w.config, w.fleet, w.control, NoSleep};
    last.RunOnce();
    CHECK_EQ(Read(w.image), std::string{"v1 image"});
    CHECK_EQ(w.Echo()["state"].get<std::string>(), std::string{"failed"});
    CHECK_EQ(w.fleet.downloads, kMaxInterrupts);
  }
  {  // Regression (spec/tla/FleetUpdate.tla, ImageAlwaysThere): a power loss
     // between the two renames of the old agent left only .prev; every later
     // update then failed and the container never came back.
    World w;
    fs::rename(w.image, w.image + ".prev");
    w.control.state["echo"] = "failed";
    Updater u{w.config, w.fleet, w.control, NoSleep};
    u.RunOnce();
    CHECK_EQ(Read(w.image), std::string{"v1 image"});
    CHECK_EQ(w.control.state["echo"], std::string{"running"});
  }
  {  // Regression: a broken entry in containers.json or the agent's own
     // state.json (a power cut, a hand edit) threw out of RunOnce and the
     // agent crash-looped (found by fuzz_agent_files). Now it is skipped.
    World w;
    std::ofstream{w.config.containers_json}
        << R"({"containers":[{"name":1,"container":{"squashfs":"/x"}},)"
           R"({"name":"y","container":"z"},"text",)"
           R"({"name":"echo","container":{"squashfs":"/var/lib/aember/echo/echo.squashfs"}}]})";
    fs::create_directories(fs::path{w.config.state_file}.parent_path());
    std::ofstream{w.config.state_file}
        << R"({"containers":{"echo":{"version":"1","failed":[3,"4"],)"
           R"("pending":5,"interrupted":["2"]},"x":7}})";
    Updater u{w.config, w.fleet, w.control, NoSleep};
    w.Assign("4", "v4 image");
    u.RunOnce();
    CHECK_EQ(w.Echo()["state"].get<std::string>(), std::string{"failed"});
    w.Assign("2", "v2 image");
    u.RunOnce();
    CHECK_EQ(Read(w.image), std::string{"v2 image"});
  }
  // Regression: a poll_interval too big for an int was undefined behaviour
  // (found by fuzz_desired); it is clamped, and a non-number is the default.
  CHECK_EQ(ParseDesired(nlohmann::json::parse(
                            R"({"poll_interval":1e300,"containers":[]})"))
               ->poll_interval,
           86400);
  CHECK_EQ(ParseDesired(nlohmann::json::parse(
                            R"({"poll_interval":"5","containers":[]})"))
               ->poll_interval,
           60);
  {  // fleet.json
    ReleaseKey key;
    const auto pub = Base64Encode(key.pub);
    auto c = ParseAgentConfig(
        {{"release_public_key", pub}, {"arch", kArch}, {"health_seconds", 5}});
    CHECK(c.has_value() && c->health_seconds == 5 && c->arch == kArch);
    CHECK(!ParseAgentConfig(nlohmann::json::array()).has_value());
    CHECK(!ParseAgentConfig({{"release_public_key", pub}}).has_value());
    CHECK(!ParseAgentConfig({{"release_public_key", 1}, {"arch", kArch}})
               .has_value());
    // Regression: a health_seconds that wasn't a number threw out of main()
    // (found by fuzz_agent_config); now it is the default, and clamped.
    c = ParseAgentConfig({{"release_public_key", pub},
                          {"arch", kArch},
                          {"health_seconds", "20"}});
    CHECK(c.has_value() && c->health_seconds == 20);
    c = ParseAgentConfig(
        {{"release_public_key", pub}, {"arch", kArch}, {"health_seconds", -4}});
    CHECK(c.has_value() && c->health_seconds == 1);
  }
  {
    // Regression: the state file's rename wasn't synced (only the file
    // was), so after a power cut during an update the "pending" mark was
    // gone and the interrupted update wasn't counted (found by the
    // fleet-power-loss end-to-end test). Every rename that must survive a
    // power loss syncs its directory, the pending state before the stop.
    World w;
    Updater u{w.config, w.fleet, w.control, NoSleep};
    std::vector<std::string> synced;
    u.sync_dir = [&](const std::string& d) { synced.push_back(d); };
    const auto state_dir = fs::path{w.config.state_file}.parent_path().string();
    const auto image_dir = fs::path{w.image}.parent_path().string();
    bool pending_synced = false;
    w.control.on_stop = [&] {
      pending_synced =
          Read(w.config.state_file).contains(R"("pending": "2")") &&
          !synced.empty() && synced.back() == state_dir;
    };
    w.Assign("2", "v2 image");
    u.RunOnce();
    CHECK(pending_synced);
    CHECK(std::ranges::count(synced, image_dir) >= 2);  // .prev, the swap
    CHECK_EQ(Read(w.image), std::string{"v2 image"});
  }
  CHECK(!ParseDesired(nlohmann::json{{"containers", {{{"name", "x"}}}}})
             .has_value());
  CHECK(ParseDesired(nlohmann::json{{"containers", nlohmann::json::array()}})
            .has_value());
  return Failures();
}
