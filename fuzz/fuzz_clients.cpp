// The agent's two sockets answering anything: aember-auth's (a JSON head
// line, then the fleet server's body) and aember-containerd's control
// socket (one JSON message). First bytes pick the call.
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <thread>

#include <fuzzer/FuzzedDataProvider.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <aember-fleet/clients.h>

namespace {

int Listen(const std::string& path, int type) {
  ::unlink(path.c_str());
  const int fd = ::socket(AF_UNIX, type | SOCK_CLOEXEC, 0);
  sockaddr_un a{};
  a.sun_family = AF_UNIX;
  path.copy(a.sun_path, sizeof(a.sun_path) - 1);
  if (::bind(fd, reinterpret_cast<sockaddr*>(&a), sizeof(a)) != 0 ||
      ::listen(fd, 4) != 0) {
    std::abort();
  }
  return fd;
}

struct Sockets {
  std::string auth, control;
  int auth_fd, control_fd;
};

const Sockets& Get() {
  static const Sockets s = [] {
    const auto dir = std::filesystem::temp_directory_path() /
                     ("aember-fleet-fuzz-sock-" + std::to_string(::getpid()));
    std::filesystem::create_directories(dir);
    Sockets s{
        (dir / "auth.sock").string(), (dir / "control.sock").string(), -1, -1};
    s.auth_fd = Listen(s.auth, SOCK_STREAM);
    s.control_fd = Listen(s.control, SOCK_SEQPACKET);
    return s;
  }();
  return s;
}

/// One connection on @p listen_fd: read the request, send @p answer, close.
void ServeOne(int listen_fd, const std::string& answer, bool stream) {
  const int fd = ::accept4(listen_fd, nullptr, nullptr, SOCK_CLOEXEC);
  if (fd < 0) return;
  char buf[65536];
  ::recv(fd, buf, sizeof(buf), 0);
  if (stream) {
    std::string_view out{answer};
    while (!out.empty()) {
      const ssize_t n = ::send(fd, out.data(), out.size(), MSG_NOSIGNAL);
      if (n <= 0) break;
      out.remove_prefix(static_cast<std::size_t>(n));
    }
  } else if (!answer.empty()) {
    ::send(fd,
           answer.data(),
           std::min<std::size_t>(answer.size(), 65536),
           MSG_NOSIGNAL);
  }
  ::close(fd);
}

}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data,
                                      std::size_t size) {
  using namespace aember::fleet;
  const auto& s = Get();
  FuzzedDataProvider fdp{data, size};
  const int call = fdp.ConsumeIntegralInRange(0, 5);
  const auto answer = fdp.ConsumeRemainingBytesAsString();
  const bool auth = call < 3;
  std::thread server{ServeOne, auth ? s.auth_fd : s.control_fd, answer, auth};
  AuthSocketFleet fleet{"x64-poky-linux", s.auth};
  ControlSocket control{s.control};
  std::size_t got = 0;
  switch (call) {
    case 0:
      std::ignore = fleet.GetDesired();
      break;
    case 1:
      std::ignore = fleet.Download("a-1", [&](std::string_view c) {
        got += c.size();
        return true;
      });
      if (got > answer.size()) std::abort();
      break;
    case 2:
      std::ignore = fleet.Report({{"containers", nlohmann::json::array()}});
      break;
    case 3:
      std::ignore = control.State("echo");
      break;
    case 4:
      std::ignore = control.Stop("echo");
      break;
    default:
      std::ignore = control.Start("echo");
  }
  server.join();
  return 0;
}
