#include <array>
#include <format>

#include <sys/socket.h>
#include <sys/time.h>
#include <sys/un.h>
#include <unistd.h>

#include <aember-fleet/clients.h>

namespace aember::fleet {

namespace {

int ConnectUnix(const std::string& path, int type) {
  sockaddr_un a{};
  if (path.size() >= sizeof(a.sun_path)) return -1;
  const int fd = ::socket(AF_UNIX, type | SOCK_CLOEXEC, 0);
  if (fd < 0) return -1;
  a.sun_family = AF_UNIX;
  path.copy(a.sun_path, sizeof(a.sun_path) - 1);
  timeval tv{120, 0};  // a stop can take its stop_timeout; a download a while
  ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
  if (::connect(fd, reinterpret_cast<sockaddr*>(&a), sizeof(a)) != 0) {
    ::close(fd);
    return -1;
  }
  return fd;
}

struct Fd {
  int fd;
  ~Fd() {
    if (fd >= 0) ::close(fd);
  }
};

/// @p j[@p key] is true (and a boolean).
bool IsTrue(const nlohmann::json& j, const char* key) {
  return j.is_object() && j.contains(key) && j[key].is_boolean() &&
         j[key].get<bool>();
}

/// @p j[@p key] if it is a string, else @p fallback.
std::string Text(const nlohmann::json& j, const char* key,
                 std::string fallback = {}) {
  if (!j.is_object() || !j.contains(key) || !j[key].is_string())
    return fallback;
  return j[key].get<std::string>();
}

/// A call's HTTP status as success (200) or an error.
std::expected<void, std::string> Ok(const std::expected<int, std::string>& r) {
  if (!r) return std::unexpected(r.error());
  if (*r != 200) return std::unexpected(std::format("HTTP {}", *r));
  return {};
}

}  // namespace

std::expected<int, std::string> AuthSocketFleet::Call(
    std::string_view method, std::string_view path, const nlohmann::json* body,
    const Sink& sink) {
  Fd fd{ConnectUnix(socket_, SOCK_STREAM)};
  if (fd.fd < 0)
    return std::unexpected(std::format("cannot reach {}", socket_));
  nlohmann::json req{{"op", "request"}, {"method", method}, {"path", path}};
  if (body) req["body"] = *body;
  const auto line = req.dump() + "\n";
  if (::send(fd.fd, line.data(), line.size(), MSG_NOSIGNAL) !=
      static_cast<ssize_t>(line.size())) {
    return std::unexpected("aember-auth: send failed");
  }
  std::string head;
  std::array<char, 64 * 1024> buf{};
  bool in_body = false;
  int status = 0;
  while (true) {
    const ssize_t n = ::recv(fd.fd, buf.data(), buf.size(), 0);
    if (n < 0) return std::unexpected("aember-auth: read failed");
    if (n == 0) break;
    std::string_view chunk{buf.data(), static_cast<std::size_t>(n)};
    if (!in_body) {
      const auto nl = chunk.find('\n');
      head.append(chunk.substr(0, nl));
      if (nl == std::string_view::npos) continue;
      const auto h = nlohmann::json::parse(head, nullptr, false);
      if (!IsTrue(h, "ok")) {
        return std::unexpected(
            "aember-auth: " +
            (h.is_object() ? Text(h, "error", "failed") : head));
      }
      status = h.contains("status") && h["status"].is_number_integer()
                   ? h["status"].get<int>()
                   : 0;
      in_body = true;
      chunk.remove_prefix(nl + 1);
    }
    if (!chunk.empty() && sink && !sink(chunk)) {
      return std::unexpected("transfer aborted");
    }
  }
  if (!in_body) return std::unexpected("aember-auth: no answer");
  return status;
}

std::expected<Desired, std::string> AuthSocketFleet::GetDesired() {
  std::string body;
  auto status = Call("GET",
                     "/api/v1/device/desired?arch=" + arch_,
                     nullptr,
                     [&](std::string_view c) {
                       body.append(c);
                       return body.size() < 4 * 1024 * 1024;
                     });
  if (!status) return std::unexpected(status.error());
  if (*status != 200) {
    return std::unexpected(
        std::format("desired state: HTTP {} {}", *status, body));
  }
  const auto j = nlohmann::json::parse(body, nullptr, false);
  if (j.is_discarded()) return std::unexpected("desired state: not JSON");
  return ParseDesired(j);
}

std::expected<void, std::string> AuthSocketFleet::Download(
    const std::string& artifact, const Sink& sink) {
  // A non-200 body (an error message) also reaches the sink; the updater
  // checks size and SHA-256 against the manifest anyway.
  return Ok(Call("GET", "/api/v1/device/artifacts/" + artifact, nullptr, sink));
}

std::expected<void, std::string> AuthSocketFleet::Report(
    const nlohmann::json& status) {
  return Ok(Call("POST", "/api/v1/device/status", &status, {}));
}

std::expected<nlohmann::json, std::string> ControlSocket::Ask(
    const nlohmann::json& request) {
  Fd fd{ConnectUnix(socket_, SOCK_SEQPACKET)};
  if (fd.fd < 0)
    return std::unexpected(std::format("cannot reach {}", socket_));
  const auto out = request.dump();
  if (::send(fd.fd, out.data(), out.size(), MSG_NOSIGNAL) !=
      static_cast<ssize_t>(out.size())) {
    return std::unexpected("containerd: send failed");
  }
  std::array<char, 64 * 1024> buf{};
  const ssize_t n = ::recv(fd.fd, buf.data(), buf.size(), 0);
  if (n <= 0) return std::unexpected("containerd: no answer");
  auto j = nlohmann::json::parse(
      std::string_view{buf.data(), static_cast<size_t>(n)}, nullptr, false);
  if (!j.is_object()) return std::unexpected("containerd: bad answer");
  if (!IsTrue(j, "ok")) {
    return std::unexpected("containerd: " + Text(j, "error", "failed"));
  }
  return j;
}

std::expected<std::string, std::string> ControlSocket::State(
    const std::string& name) {
  auto j = Ask({{"op", "list"}});
  if (!j) return std::unexpected(j.error());
  if (j->contains("containers") && (*j)["containers"].is_array()) {
    for (const auto& c : (*j)["containers"]) {
      if (Text(c, "name") == name) return Text(c, "state");
    }
  }
  return std::unexpected("containerd: no container " + name);
}

std::expected<void, std::string> ControlSocket::Do(std::string_view op,
                                                   const std::string& name) {
  auto j = Ask({{"op", op}, {"name", name}});
  if (!j) return std::unexpected(j.error());
  return {};
}

std::expected<void, std::string> ControlSocket::Stop(const std::string& name) {
  return Do("stop", name);
}

std::expected<void, std::string> ControlSocket::Start(const std::string& name) {
  return Do("start", name);
}

}  // namespace aember::fleet
