// Offload Failover Fabric - framed bounded protocol and socket transport.
// The same request executor backs the offd service surface and the CLI local
// mode, so both paths behave identically.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "off/canonical.hpp"
#include "off/fabric.hpp"
#include "off/reason.hpp"

namespace off {

inline constexpr u32 kWireMagic = 0x4F464652U;  // 'OFFR'
inline constexpr std::size_t kWireHeaderBytes = 24;
inline constexpr std::size_t kDefaultMaxFramePayload = 1U << 20;
inline constexpr std::size_t kAbsoluteMaxFramePayload = 64U << 20;

enum class WireMessage : u16 {
  Hello = 1,
  Welcome = 2,
  Request = 3,
  Response = 4,
  Error = 5,
  Shutdown = 6,
  Cancel = 7,
};

[[nodiscard]] std::string_view wire_message_text(WireMessage value) noexcept;
[[nodiscard]] bool wire_message_parse(std::string_view text, WireMessage& out) noexcept;

/// Request verbs. Numeric values are part of the wire contract.
enum class WireRequestKind : u16 {
  QueryView = 1,
  Export = 2,
  Explain = 3,
  Stats = 4,
  Recovery = 5,
  PullIntent = 6,
  EnrollSource = 10,
  RegisterService = 11,
  RegisterTarget = 12,
  SetTopologyGeneration = 13,
  SetPolicy = 14,
  WithdrawService = 15,
  EstablishPlacement = 16,
  IngestCapability = 20,
  IngestFailure = 21,
  IngestDependency = 22,
  RequestFailover = 30,
  ResolveAmbiguity = 31,
  ResumeRecovery = 32,
  RequestFailback = 33,
  AcknowledgeIntent = 34,
  ReportEffect = 35,
  VerifyFence = 40,
  CancelRequest = 41,
  Shutdown = 50,
};

inline constexpr u16 kWireRequestKindMax = 50;
[[nodiscard]] std::string_view wire_request_text(WireRequestKind value) noexcept;
[[nodiscard]] bool wire_request_parse(std::string_view text, WireRequestKind& out) noexcept;

struct Frame {
  WireMessage kind{WireMessage::Request};
  u64 correlation{0};
  std::vector<u8> payload{};

  friend bool operator==(const Frame&, const Frame&) noexcept = default;
};

/// Encodes one frame. Fails with Reason::Oversized when the payload exceeds the
/// negotiated ceiling; nothing is truncated.
[[nodiscard]] Reason encode_frame(const Frame& frame, std::vector<u8>& out,
                                  std::size_t max_payload = kDefaultMaxFramePayload);

/// Decodes exactly one frame from the front of a buffer. The consumed count
/// reports how many bytes the frame occupied so a stream can be advanced
/// without copying.
[[nodiscard]] Reason decode_frame(const u8* data, std::size_t size, Frame& out,
                                  std::size_t& consumed,
                                  std::size_t max_payload = kDefaultMaxFramePayload);

struct WireRequest {
  WireRequestKind kind{WireRequestKind::QueryView};
  u64 key{0};
  std::vector<u8> body{};

  friend bool operator==(const WireRequest&, const WireRequest&) noexcept = default;
};

struct WireResponse {
  Reason status{Reason::Ok};
  bool accepted{false};
  bool duplicate{false};
  std::string body{};
};

[[nodiscard]] Reason encode_request(const WireRequest& request, std::vector<u8>& out,
                                    std::size_t max_payload = kDefaultMaxFramePayload);
[[nodiscard]] Reason decode_request(const u8* data, std::size_t size, WireRequest& out);
[[nodiscard]] Reason encode_response(const WireResponse& response, std::vector<u8>& out,
                                     std::size_t max_payload = kDefaultMaxFramePayload);
[[nodiscard]] Reason decode_response(const u8* data, std::size_t size, WireResponse& out);

/// Executes one decoded request against a fabric. Both the daemon and the CLI
/// local mode call this, so the two surfaces cannot drift apart.
[[nodiscard]] Reason execute_request(Fabric& fabric, const WireRequest& request,
                                     WireResponse& response);

/// Bounded socket wrapper. All operations are blocking; there are no timeouts
/// anywhere in the transport, so a test synchronises on protocol completion.
class Socket {
 public:
  Socket() = default;
  ~Socket();
  Socket(const Socket&) = delete;
  Socket& operator=(const Socket&) = delete;
  Socket(Socket&& other) noexcept;
  Socket& operator=(Socket&& other) noexcept;

  [[nodiscard]] bool valid() const noexcept;
  void close() noexcept;
  /// Closes from another thread to unblock a blocking accept or receive.
  void interrupt() noexcept;

  [[nodiscard]] static std::optional<Socket> listen_on(const std::string& host, u16 port,
                                                       u16& bound_port, std::string& error);
  [[nodiscard]] static std::optional<Socket> connect_to(const std::string& host, u16 port,
                                                        std::string& error);
  [[nodiscard]] std::optional<Socket> accept(std::string& error) const;

  [[nodiscard]] bool send_all(const u8* data, std::size_t size) noexcept;
  /// Reads exactly size bytes. Returns false on EOF or error, so a caller can
  /// distinguish a complete frame from a truncated stream.
  [[nodiscard]] bool recv_exact(u8* data, std::size_t size) noexcept;
  void shutdown_send() noexcept;

 private:
  explicit Socket(std::uintptr_t handle) : handle_(handle) {}

  std::uintptr_t handle_{0};
};

/// Blocking frame channel over a socket.
class FrameChannel {
 public:
  FrameChannel(Socket socket, std::size_t max_payload)
      : socket_(std::move(socket)), max_payload_(max_payload) {}

  [[nodiscard]] bool send_frame(const Frame& frame);
  /// Reads one frame. Returns false at clean EOF or on a protocol violation;
  /// the error out-parameter carries the reason code in the failure case.
  [[nodiscard]] bool recv_frame(Frame& frame, Reason& error);
  void interrupt() noexcept { socket_.interrupt(); }
  [[nodiscard]] Socket& socket() noexcept { return socket_; }

 private:
  [[nodiscard]] bool fill(std::size_t needed);

  Socket socket_{};
  std::size_t max_payload_{kDefaultMaxFramePayload};
  std::vector<u8> buffer_{};
  std::size_t begin_{0};
};

/// Enables Winsock for the lifetime of the object. Constructing it more than
/// once is harmless.
class SocketRuntime {
 public:
  SocketRuntime();
  ~SocketRuntime();
  SocketRuntime(const SocketRuntime&) = delete;
  SocketRuntime& operator=(const SocketRuntime&) = delete;
};

}  // namespace off
