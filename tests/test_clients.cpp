// The agent's socket clients against stand-ins for aember-auth's socket
// and aember-containerd's control socket that answer what we tell them.
#include <filesystem>
#include <string>
#include <thread>

#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include "check.h"
#include <aember-fleet/clients.h>

using namespace aember::fleet;

namespace {

/// Answers the next connection on @p path with @p answer, then closes.
class OneShot {
 public:
  OneShot(const std::string& path, int type, std::string answer) {
    ::unlink(path.c_str());
    fd_ = ::socket(AF_UNIX, type | SOCK_CLOEXEC, 0);
    sockaddr_un a{};
    a.sun_family = AF_UNIX;
    path.copy(a.sun_path, sizeof(a.sun_path) - 1);
    ::bind(fd_, reinterpret_cast<sockaddr*>(&a), sizeof(a));
    ::listen(fd_, 1);
    thread_ = std::thread{[this, answer = std::move(answer)] {
      const int c = ::accept(fd_, nullptr, nullptr);
      char buf[4096];
      ::recv(c, buf, sizeof(buf), 0);
      ::send(c, answer.data(), answer.size(), MSG_NOSIGNAL);
      ::close(c);
    }};
  }
  ~OneShot() {
    thread_.join();
    ::close(fd_);
  }

 private:
  int fd_ = -1;
  std::thread thread_;
};

}  // namespace

int main() {
  char d[] = "/tmp/aember-fleet-clients-XXXXXX";
  const std::string dir = ::mkdtemp(d);
  const std::string auth = dir + "/auth.sock", control = dir + "/control.sock";
  AuthSocketFleet fleet{"x64-poky-linux", auth};
  ControlSocket containerd{control};

  {
    OneShot s{
        auth, SOCK_STREAM, "{\"ok\":true,\"status\":200}\n{\"containers\":[]}"};
    auto r = fleet.GetDesired();
    CHECK(r.has_value() && r->containers.empty());
  }
  {
    OneShot s{auth, SOCK_STREAM, "{\"ok\":false,\"error\":\"not enrolled\"}\n"};
    auto r = fleet.GetDesired();
    CHECK(!r.has_value() && r.error() == "aember-auth: not enrolled");
  }
  // Regression: an answer whose fields had other types ("ok" not a boolean,
  // "error" or "status" not what they should be) threw out of the agent
  // (found by fuzz_clients). Now it is an error, or status 0.
  for (const char* head :
       {"{\"ok\":\"yes\",\"status\":200}\n", "{\"ok\":false,\"error\":7}\n"}) {
    OneShot s{auth, SOCK_STREAM, head};
    CHECK(!fleet.GetDesired().has_value());
  }
  {
    OneShot s{auth, SOCK_STREAM, "{\"ok\":true,\"status\":\"200\"}\n{}"};
    auto r = fleet.Report({});
    CHECK(!r.has_value() && r.error() == "HTTP 0");
  }
  {
    OneShot s{
        control,
        SOCK_SEQPACKET,
        R"({"ok":true,"containers":[{"name":"echo","state":"running"}]})"};
    auto r = containerd.State("echo");
    CHECK(r.has_value() && *r == "running");
  }
  for (const char* answer :
       {R"({"ok":1})",
        R"({"ok":false,"error":["x"]})",
        R"({"ok":true,"containers":[3,{"name":4},{"name":"echo","state":5}]})",
        R"({"ok":true,"containers":{"name":"echo"}})"}) {
    OneShot s{control, SOCK_SEQPACKET, answer};
    auto r = containerd.State("echo");
    CHECK(!r.has_value() || r->empty());
  }
  std::filesystem::remove_all(dir);
  return Failures();
}
