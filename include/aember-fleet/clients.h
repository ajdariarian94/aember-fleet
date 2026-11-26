/**
 * @file clients.h
 * @brief The agent's real FleetApi and ContainerControl: aember-auth's
 *        socket (it adds the device's login, the agent never sees the device
 *        key or TLS) and aember-containerd's control socket.
 */
#pragma once

#include <string>

#include <aember-fleet/updater.h>

namespace aember::fleet {

class AuthSocketFleet final : public FleetApi {
 public:
  /// @p arch: the device's aember target (fleet.json), sent with each
  /// desired-state request: releases are per target.
  explicit AuthSocketFleet(std::string arch,
                           std::string socket = "/run/aember-auth/auth.sock")
      : arch_(std::move(arch)), socket_(std::move(socket)) {}
  std::expected<Desired, std::string> GetDesired() override;
  std::expected<void, std::string> Download(const std::string& artifact,
                                            const Sink& sink) override;
  std::expected<void, std::string> Report(
      const nlohmann::json& status) override;

 private:
  /// One request: the status code, the body to @p sink.
  std::expected<int, std::string> Call(std::string_view method,
                                       std::string_view path,
                                       const nlohmann::json* body,
                                       const Sink& sink);
  std::string arch_;
  std::string socket_;
};

class ControlSocket final : public ContainerControl {
 public:
  explicit ControlSocket(
      std::string socket = "/run/aember-containerd/control.sock")
      : socket_(std::move(socket)) {}
  std::expected<std::string, std::string> State(
      const std::string& name) override;
  std::expected<void, std::string> Stop(const std::string& name) override;
  std::expected<void, std::string> Start(const std::string& name) override;
  std::expected<void, std::string> Install(const std::string& name) override;
  std::expected<void, std::string> Rollback(const std::string& name) override;

 private:
  std::expected<nlohmann::json, std::string> Ask(const nlohmann::json& request);
  std::expected<void, std::string> Do(std::string_view op,
                                      const std::string& name);
  std::string socket_;
};

}  // namespace aember::fleet
