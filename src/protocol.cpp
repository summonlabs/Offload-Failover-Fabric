// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
#include "off/protocol.hpp"

#include <algorithm>
#include <cstring>
#include <utility>

#include "detail/codec_util.hpp"

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace off {
namespace {

#ifdef _WIN32
using socket_handle = SOCKET;
constexpr socket_handle kInvalidSocketHandle = INVALID_SOCKET;
[[nodiscard]] int socket_last_error() noexcept { return ::WSAGetLastError(); }
void socket_close(socket_handle handle) noexcept { ::closesocket(handle); }
#else
using socket_handle = int;
constexpr socket_handle kInvalidSocketHandle = -1;
[[nodiscard]] int socket_last_error() noexcept { return errno; }
void socket_close(socket_handle handle) noexcept { ::close(handle); }
#endif

[[nodiscard]] socket_handle to_handle(std::uintptr_t raw) noexcept {
  return static_cast<socket_handle>(raw);
}

[[nodiscard]] std::uintptr_t to_raw(socket_handle handle) noexcept {
  return static_cast<std::uintptr_t>(handle);
}

[[nodiscard]] bool handle_valid(std::uintptr_t raw) noexcept {
  return raw != to_raw(kInvalidSocketHandle) && raw != 0;
}

void put_u16_at(std::vector<u8>& buffer, std::size_t offset, u16 value) {
  buffer[offset] = static_cast<u8>((value >> 8U) & 0xFFU);
  buffer[offset + 1] = static_cast<u8>(value & 0xFFU);
}

void put_u32_at(std::vector<u8>& buffer, std::size_t offset, u32 value) {
  for (int index = 0; index < 4; ++index) {
    buffer[offset + static_cast<std::size_t>(index)] =
        static_cast<u8>((value >> static_cast<unsigned>(24 - (index * 8))) & 0xFFU);
  }
}

void put_u64_at(std::vector<u8>& buffer, std::size_t offset, u64 value) {
  for (int index = 0; index < 8; ++index) {
    buffer[offset + static_cast<std::size_t>(index)] =
        static_cast<u8>((value >> static_cast<unsigned>(56 - (index * 8))) & 0xFFU);
  }
}

[[nodiscard]] u16 read_u16(const u8* data) noexcept {
  return static_cast<u16>((static_cast<u16>(data[0]) << 8U) | static_cast<u16>(data[1]));
}

[[nodiscard]] u32 read_u32(const u8* data) noexcept {
  u32 value = 0;
  for (int index = 0; index < 4; ++index) {
    value = (value << 8U) | static_cast<u32>(data[index]);
  }
  return value;
}

[[nodiscard]] u64 read_u64(const u8* data) noexcept {
  u64 value = 0;
  for (int index = 0; index < 8; ++index) {
    value = (value << 8U) | static_cast<u64>(data[index]);
  }
  return value;
}

[[nodiscard]] JsonValue reason_json(Reason code) {
  JsonValue object;
  object.set("code", JsonValue(std::string(reason_code_text(code))));
  object.set("family", JsonValue(std::string(reason_family(code))));
  object.set("summary", JsonValue(reason_summary(code)));
  return object;
}

struct ReasonEntry {
  WireMessage value;
  std::string_view text;
};

constexpr ReasonEntry kWireMessages[] = {
    {WireMessage::Hello, "hello"},     {WireMessage::Welcome, "welcome"},
    {WireMessage::Request, "request"}, {WireMessage::Response, "response"},
    {WireMessage::Error, "error"},     {WireMessage::Shutdown, "shutdown"},
    {WireMessage::Cancel, "cancel"},
};

struct RequestEntry {
  WireRequestKind value;
  std::string_view text;
};

constexpr RequestEntry kWireRequests[] = {
    {WireRequestKind::QueryView, "query_view"},
    {WireRequestKind::Export, "export"},
    {WireRequestKind::Explain, "explain"},
    {WireRequestKind::Stats, "stats"},
    {WireRequestKind::Recovery, "recovery"},
    {WireRequestKind::PullIntent, "pull_intent"},
    {WireRequestKind::EnrollSource, "enroll_source"},
    {WireRequestKind::RegisterService, "register_service"},
    {WireRequestKind::RegisterTarget, "register_target"},
    {WireRequestKind::SetTopologyGeneration, "set_topology_generation"},
    {WireRequestKind::SetPolicy, "set_policy"},
    {WireRequestKind::WithdrawService, "withdraw_service"},
    {WireRequestKind::EstablishPlacement, "establish_placement"},
    {WireRequestKind::IngestCapability, "ingest_capability"},
    {WireRequestKind::IngestFailure, "ingest_failure"},
    {WireRequestKind::IngestDependency, "ingest_dependency"},
    {WireRequestKind::RequestFailover, "request_failover"},
    {WireRequestKind::ResolveAmbiguity, "resolve_ambiguity"},
    {WireRequestKind::ResumeRecovery, "resume_recovery"},
    {WireRequestKind::RequestFailback, "request_failback"},
    {WireRequestKind::AcknowledgeIntent, "acknowledge_intent"},
    {WireRequestKind::ReportEffect, "report_effect"},
    {WireRequestKind::VerifyFence, "verify_fence"},
    {WireRequestKind::CancelRequest, "cancel_request"},
    {WireRequestKind::Shutdown, "shutdown"},
};

[[nodiscard]] WireResponse refusal_response(Reason code) {
  WireResponse response;
  response.status = code;
  response.accepted = false;
  JsonValue object;
  object.set("accepted", JsonValue(false));
  object.set("status", reason_json(code));
  response.body = object.dump();
  return response;
}

[[nodiscard]] WireResponse reason_response(Reason code, const Explanation& explanation) {
  WireResponse response;
  response.status = code;
  response.accepted = reason_is_accept(code);
  JsonValue object;
  object.set("accepted", JsonValue(response.accepted));
  object.set("explanation", explanation.to_json());
  object.set("status", reason_json(code));
  response.body = object.dump();
  return response;
}

[[nodiscard]] WireResponse decision_response(const RecoveryDecision& decision) {
  WireResponse response;
  response.status = decision.outcome;
  response.accepted = decision.accepted;
  response.duplicate = decision.duplicate;
  response.body = decision.to_json().dump();
  return response;
}

[[nodiscard]] Explanation blank_explanation() { return Explanation{}; }

}  // namespace

std::string_view wire_message_text(WireMessage value) noexcept {
  for (const ReasonEntry& entry : kWireMessages) {
    if (entry.value == value) {
      return entry.text;
    }
  }
  return "unknown";
}

bool wire_message_parse(std::string_view text, WireMessage& out) noexcept {
  for (const ReasonEntry& entry : kWireMessages) {
    if (entry.text == text) {
      out = entry.value;
      return true;
    }
  }
  return false;
}

std::string_view wire_request_text(WireRequestKind value) noexcept {
  for (const RequestEntry& entry : kWireRequests) {
    if (entry.value == value) {
      return entry.text;
    }
  }
  return "unknown";
}

bool wire_request_parse(std::string_view text, WireRequestKind& out) noexcept {
  for (const RequestEntry& entry : kWireRequests) {
    if (entry.text == text) {
      out = entry.value;
      return true;
    }
  }
  return false;
}

// ---------------------------------------------------------------------------
// Frame codec
// ---------------------------------------------------------------------------

Reason encode_frame(const Frame& frame, std::vector<u8>& out, std::size_t max_payload) {
  const std::size_t ceiling = (std::min)(max_payload, kAbsoluteMaxFramePayload);
  if (frame.payload.size() > ceiling) {
    out.clear();
    return Reason::Oversized;
  }
  u32 length = 0;
  if (narrow(static_cast<u64>(frame.payload.size()), length)) {
    out.clear();
    return Reason::Oversized;
  }
  out.assign(kWireHeaderBytes + frame.payload.size(), 0);
  put_u32_at(out, 0, kWireMagic);
  put_u16_at(out, 4, kProtocolVersion);
  put_u16_at(out, 6, static_cast<u16>(frame.kind));
  put_u64_at(out, 8, frame.correlation);
  put_u32_at(out, 16, length);
  put_u32_at(out, 20,
             frame.payload.empty() ? 0U : crc32c(frame.payload.data(), frame.payload.size()));
  if (!frame.payload.empty()) {
    std::memcpy(out.data() + kWireHeaderBytes, frame.payload.data(), frame.payload.size());
  }
  return Reason::Ok;
}

Reason decode_frame(const u8* data, std::size_t size, Frame& out, std::size_t& consumed,
                    std::size_t max_payload) {
  consumed = 0;
  if (size < kWireHeaderBytes) {
    return Reason::Truncated;
  }
  if (read_u32(data) != kWireMagic) {
    return Reason::Malformed;
  }
  const u16 version = read_u16(data + 4);
  if (version != kProtocolVersion) {
    return Reason::ProtocolVersionMismatch;
  }
  const u16 kind_raw = read_u16(data + 6);
  if (kind_raw == 0 || kind_raw > 7) {
    return Reason::UnsupportedSemantics;
  }
  const u32 length = read_u32(data + 16);
  const std::size_t ceiling = (std::min)(max_payload, kAbsoluteMaxFramePayload);
  if (static_cast<std::size_t>(length) > ceiling) {
    return Reason::FrameTooLarge;
  }
  if (size < kWireHeaderBytes + static_cast<std::size_t>(length)) {
    return Reason::Truncated;
  }
  const u32 expected_crc = read_u32(data + 20);
  const u32 actual_crc =
      length == 0 ? 0U : crc32c(data + kWireHeaderBytes, static_cast<std::size_t>(length));
  if (expected_crc != actual_crc) {
    return Reason::ChecksumMismatch;
  }
  out.kind = static_cast<WireMessage>(kind_raw);
  out.correlation = read_u64(data + 8);
  out.payload.assign(data + kWireHeaderBytes,
                     data + kWireHeaderBytes + static_cast<std::size_t>(length));
  consumed = kWireHeaderBytes + static_cast<std::size_t>(length);
  return Reason::Ok;
}

// ---------------------------------------------------------------------------
// Request and response codecs
// ---------------------------------------------------------------------------

Reason encode_request(const WireRequest& request, std::vector<u8>& out, std::size_t max_payload) {
  CanonicalWriter writer;
  writer.put_u16(static_cast<u16>(request.kind));
  writer.put_u64(request.key);
  writer.put_blob(request.body.empty() ? nullptr : request.body.data(), request.body.size());
  if (!writer.ok()) {
    out.clear();
    return Reason::Oversized;
  }
  if (writer.size() > (std::min)(max_payload, kAbsoluteMaxFramePayload)) {
    out.clear();
    return Reason::Oversized;
  }
  out = writer.buffer();
  return Reason::Ok;
}

Reason decode_request(const u8* data, std::size_t size, WireRequest& out) {
  CanonicalReader reader(data, size);
  u16 kind = 0;
  if (!reader.get_u16(kind) || !reader.get_u64(out.key)) {
    return reader.ok() ? Reason::Truncated : reader.error();
  }
  if (kind == 0 || kind > kWireRequestKindMax) {
    return Reason::UnsupportedSemantics;
  }
  out.kind = static_cast<WireRequestKind>(kind);
  if (!reader.get_blob(out.body)) {
    return reader.error();
  }
  if (!reader.at_end()) {
    return Reason::Malformed;
  }
  return Reason::Ok;
}

Reason encode_response(const WireResponse& response, std::vector<u8>& out, std::size_t max_payload) {
  CanonicalWriter writer;
  writer.put_u16(static_cast<u16>(response.status));
  writer.put_bool(response.accepted);
  writer.put_bool(response.duplicate);
  writer.put_text(response.body);
  if (!writer.ok()) {
    out.clear();
    return Reason::Oversized;
  }
  if (writer.size() > (std::min)(max_payload, kAbsoluteMaxFramePayload)) {
    out.clear();
    return Reason::Oversized;
  }
  out = writer.buffer();
  return Reason::Ok;
}

Reason decode_response(const u8* data, std::size_t size, WireResponse& out) {
  CanonicalReader reader(data, size);
  u16 status = 0;
  if (!reader.get_u16(status) || !reader.get_bool(out.accepted) ||
      !reader.get_bool(out.duplicate) || !reader.get_text(out.body)) {
    return reader.ok() ? Reason::Truncated : reader.error();
  }
  if (reason_code_text(static_cast<Reason>(status)) == "UNRECOGNIZED_REASON") {
    return Reason::UnsupportedSemantics;
  }
  out.status = static_cast<Reason>(status);
  if (!reader.at_end()) {
    return Reason::Malformed;
  }
  return Reason::Ok;
}

// ---------------------------------------------------------------------------
// Request execution
// ---------------------------------------------------------------------------

Reason execute_request(Fabric& fabric, const WireRequest& request, WireResponse& response) {
  CanonicalReader reader(request.body.data(), request.body.size());
  const RequestKey key = RequestKey::from_value(request.key);

  switch (request.kind) {
    case WireRequestKind::QueryView: {
      if (!reader.at_end()) {
        return Reason::Malformed;
      }
      response.status = Reason::Ok;
      response.accepted = true;
      response.body = fabric.view().canonical_text(false);
      return Reason::Ok;
    }
    case WireRequestKind::Export: {
      if (!reader.at_end()) {
        return Reason::Malformed;
      }
      response.status = Reason::Ok;
      response.accepted = true;
      response.body = fabric.export_canonical(false);
      return Reason::Ok;
    }
    case WireRequestKind::Stats: {
      if (!reader.at_end()) {
        return Reason::Malformed;
      }
      response.status = Reason::Ok;
      response.accepted = true;
      response.body = fabric.stats().to_json().dump();
      return Reason::Ok;
    }
    case WireRequestKind::Recovery: {
      if (!reader.at_end()) {
        return Reason::Malformed;
      }
      response.status = Reason::Ok;
      response.accepted = true;
      response.body = fabric.restart_summary().to_json().dump();
      return Reason::Ok;
    }
    case WireRequestKind::Explain: {
      ServiceName service;
      if (!detail::decode_name(reader, service) || !reader.at_end()) {
        return reader.ok() ? Reason::Malformed : reader.error();
      }
      JsonValue array{JsonValue::Array{}};
      for (const Explanation& entry : fabric.explain(service)) {
        array.push(entry.to_json());
      }
      response.status = Reason::Ok;
      response.accepted = true;
      response.body = array.dump();
      return Reason::Ok;
    }
    case WireRequestKind::PullIntent: {
      if (!reader.at_end()) {
        return Reason::Malformed;
      }
      FailoverIntent intent;
      JsonValue object;
      if (fabric.take_intent(intent)) {
        object.set("intent", to_json(intent));
        object.set("pending", JsonValue(true));
      } else {
        object.set("intent", JsonValue(nullptr));
        object.set("pending", JsonValue(false));
      }
      response.status = Reason::Ok;
      response.accepted = true;
      response.body = object.dump();
      return Reason::Ok;
    }
    case WireRequestKind::EnrollSource: {
      SourceName source;
      u8 role = 0;
      if (!detail::decode_name(reader, source) || !reader.get_u8(role) || !reader.at_end()) {
        return reader.ok() ? Reason::Malformed : reader.error();
      }
      if (role >= kSourceRoleCount) {
        return Reason::UnsupportedSemantics;
      }
      Explanation explanation;
      const Reason code = fabric.enroll_source(source, static_cast<SourceRole>(role), &explanation);
      response = reason_response(code, explanation);
      return code;
    }
    case WireRequestKind::RegisterService: {
      SourceName registrar;
      ServiceDescriptor descriptor;
      if (!detail::decode_name(reader, registrar) || !decode(reader, descriptor) ||
          !reader.at_end()) {
        return reader.ok() ? Reason::Malformed : reader.error();
      }
      Explanation explanation;
      const Reason code = fabric.register_service(descriptor, registrar, &explanation);
      response = reason_response(code, explanation);
      return code;
    }
    case WireRequestKind::RegisterTarget: {
      SourceName registrar;
      TargetRecord target;
      if (!detail::decode_name(reader, registrar) || !decode(reader, target) || !reader.at_end()) {
        return reader.ok() ? Reason::Malformed : reader.error();
      }
      Explanation explanation;
      const Reason code = fabric.register_target(target, registrar, &explanation);
      response = reason_response(code, explanation);
      return code;
    }
    case WireRequestKind::SetTopologyGeneration: {
      u64 generation = 0;
      SourceName reporter;
      if (!reader.get_u64(generation) || !detail::decode_name(reader, reporter) ||
          !reader.at_end()) {
        return reader.ok() ? Reason::Malformed : reader.error();
      }
      Explanation explanation;
      const Reason code = fabric.set_topology_generation(TopologyGeneration::from_value(generation),
                                                         reporter, &explanation);
      response = reason_response(code, explanation);
      return code;
    }
    case WireRequestKind::SetPolicy: {
      PolicyDescriptor policy;
      if (!decode(reader, policy) || !reader.at_end()) {
        return reader.ok() ? Reason::Malformed : reader.error();
      }
      Explanation explanation;
      const Reason code = fabric.set_policy(policy, &explanation);
      response = reason_response(code, explanation);
      return code;
    }
    case WireRequestKind::WithdrawService: {
      ServiceName service;
      SourceName requester;
      if (!detail::decode_name(reader, service) || !detail::decode_name(reader, requester) ||
          !reader.at_end()) {
        return reader.ok() ? Reason::Malformed : reader.error();
      }
      Explanation explanation;
      const Reason code = fabric.withdraw_service(service, requester, &explanation);
      response = reason_response(code, explanation);
      return code;
    }
    case WireRequestKind::EstablishPlacement: {
      ServiceName service;
      TargetName target;
      SourceName requester;
      if (!detail::decode_name(reader, service) || !detail::decode_name(reader, target) ||
          !detail::decode_name(reader, requester) || !reader.at_end()) {
        return reader.ok() ? Reason::Malformed : reader.error();
      }
      PlacementRequest placement;
      placement.key = key;
      placement.service = service;
      placement.target = target;
      placement.requester = requester;
      const RecoveryDecision decision = fabric.establish_placement(placement);
      response = decision_response(decision);
      return decision.outcome;
    }
    case WireRequestKind::IngestCapability: {
      CapabilityEvidence evidence;
      if (!decode(reader, evidence) || !reader.at_end()) {
        return reader.ok() ? Reason::Malformed : reader.error();
      }
      Explanation explanation;
      const Reason code = fabric.ingest_capability(evidence, &explanation);
      response = reason_response(code, explanation);
      return code;
    }
    case WireRequestKind::IngestFailure: {
      FailureEvidence evidence;
      if (!decode(reader, evidence) || !reader.at_end()) {
        return reader.ok() ? Reason::Malformed : reader.error();
      }
      Explanation explanation;
      const Reason code = fabric.ingest_failure(evidence, &explanation);
      response = reason_response(code, explanation);
      return code;
    }
    case WireRequestKind::IngestDependency: {
      DependencyObservation observation;
      if (!decode(reader, observation) || !reader.at_end()) {
        return reader.ok() ? Reason::Malformed : reader.error();
      }
      Explanation explanation;
      const Reason code = fabric.ingest_dependency(observation, &explanation);
      response = reason_response(code, explanation);
      return code;
    }
    case WireRequestKind::RequestFailover:
    case WireRequestKind::ResumeRecovery: {
      ServiceName service;
      u8 trigger = 0;
      u64 evidence = 0;
      SourceName requester;
      bool authorized = false;
      if (!detail::decode_name(reader, service) || !reader.get_u8(trigger) ||
          !reader.get_u64(evidence) || !detail::decode_name(reader, requester) ||
          !reader.get_bool(authorized) || !reader.at_end()) {
        return reader.ok() ? Reason::Malformed : reader.error();
      }
      if (trigger >= kRecoveryTriggerCount) {
        return Reason::UnsupportedSemantics;
      }
      FailoverRequest failover;
      failover.key = key;
      failover.service = service;
      failover.trigger = static_cast<RecoveryTrigger>(trigger);
      failover.evidence = EvidenceId::from_value(evidence);
      failover.requester = requester;
      failover.operator_authorized = authorized;
      const RecoveryDecision decision = request.kind == WireRequestKind::ResumeRecovery
                                            ? fabric.resume_recovery(failover)
                                            : fabric.request_failover(failover);
      response = decision_response(decision);
      return decision.outcome;
    }
    case WireRequestKind::ResolveAmbiguity: {
      ServiceName service;
      u64 evidence = 0;
      bool confirm = false;
      SourceName operator_name;
      if (!detail::decode_name(reader, service) || !reader.get_u64(evidence) ||
          !reader.get_bool(confirm) || !detail::decode_name(reader, operator_name) ||
          !reader.at_end()) {
        return reader.ok() ? Reason::Malformed : reader.error();
      }
      AmbiguityResolution resolution;
      resolution.key = key;
      resolution.service = service;
      resolution.evidence = EvidenceId::from_value(evidence);
      resolution.confirm_failure = confirm;
      resolution.resolved_by = operator_name;
      const RecoveryDecision decision = fabric.resolve_ambiguity(resolution);
      response = decision_response(decision);
      return decision.outcome;
    }
    case WireRequestKind::RequestFailback: {
      ServiceName service;
      TargetName target;
      u64 topology = 0;
      u64 policy = 0;
      SourceName requester;
      if (!detail::decode_name(reader, service) || !detail::decode_name(reader, target) ||
          !reader.get_u64(topology) || !reader.get_u64(policy) ||
          !detail::decode_name(reader, requester) || !reader.at_end()) {
        return reader.ok() ? Reason::Malformed : reader.error();
      }
      FailbackRequest failback;
      failback.key = key;
      failback.service = service;
      failback.preferred_target = target;
      failback.observed_topology_generation = TopologyGeneration::from_value(topology);
      failback.observed_policy_generation = PolicyGeneration::from_value(policy);
      failback.requester = requester;
      const RecoveryDecision decision = fabric.request_failback(failback);
      response = decision_response(decision);
      return decision.outcome;
    }
    case WireRequestKind::AcknowledgeIntent: {
      u64 attempt = 0;
      u64 fence = 0;
      u64 generation = 0;
      bool accepted = false;
      SourceName executor;
      if (!reader.get_u64(attempt) || !reader.get_u64(fence) || !reader.get_u64(generation) ||
          !reader.get_bool(accepted) || !detail::decode_name(reader, executor) ||
          !reader.at_end()) {
        return reader.ok() ? Reason::Malformed : reader.error();
      }
      IntentAck ack;
      ack.key = key;
      ack.attempt = AttemptId::from_value(attempt);
      ack.fence = FenceToken::from_value(fence);
      ack.generation = FailoverGeneration::from_value(generation);
      ack.accepted = accepted;
      ack.executor = executor;
      const RecoveryDecision decision = fabric.acknowledge_intent(ack);
      response = decision_response(decision);
      return decision.outcome;
    }
    case WireRequestKind::ReportEffect: {
      EffectReport report;
      if (!decode(reader, report) || !reader.at_end()) {
        return reader.ok() ? Reason::Malformed : reader.error();
      }
      const RecoveryDecision decision = fabric.report_effect(report);
      response = decision_response(decision);
      return decision.outcome;
    }
    case WireRequestKind::VerifyFence: {
      ServiceName service;
      TargetRef holder;
      u64 token = 0;
      if (!detail::decode_name(reader, service) || !decode(reader, holder) ||
          !reader.get_u64(token) || !reader.at_end()) {
        return reader.ok() ? Reason::Malformed : reader.error();
      }
      FenceCheck check;
      check.service = service;
      check.holder = holder;
      check.presented = FenceToken::from_value(token);
      const FenceVerdict verdict = fabric.verify_fence(check);
      response.status = verdict.code;
      response.accepted = verdict.authorized;
      response.body = verdict.to_json().dump();
      return verdict.code;
    }
    case WireRequestKind::CancelRequest: {
      u64 target_key = 0;
      if (!reader.get_u64(target_key) || !reader.at_end()) {
        return reader.ok() ? Reason::Malformed : reader.error();
      }
      Explanation explanation;
      const Reason code = fabric.cancel(RequestKey::from_value(target_key), &explanation);
      response = reason_response(code, explanation);
      return code;
    }
    case WireRequestKind::Shutdown: {
      if (!reader.at_end()) {
        return Reason::Malformed;
      }
      response.status = Reason::Ok;
      response.accepted = true;
      response.body = "{\"accepted\":true,\"status\":{\"code\":\"OK\"}}";
      return Reason::Ok;
    }
    default:
      return Reason::RequestUnknown;
  }
}

// ---------------------------------------------------------------------------
// Sockets
// ---------------------------------------------------------------------------

SocketRuntime::SocketRuntime() {
#ifdef _WIN32
  WSADATA data{};
  (void)::WSAStartup(MAKEWORD(2, 2), &data);
#endif
}

SocketRuntime::~SocketRuntime() {
#ifdef _WIN32
  (void)::WSACleanup();
#endif
}

Socket::~Socket() { close(); }

Socket::Socket(Socket&& other) noexcept : handle_(other.handle_) { other.handle_ = 0; }

Socket& Socket::operator=(Socket&& other) noexcept {
  if (this != &other) {
    close();
    handle_ = other.handle_;
    other.handle_ = 0;
  }
  return *this;
}

bool Socket::valid() const noexcept { return handle_valid(handle_); }

void Socket::close() noexcept {
  if (handle_valid(handle_)) {
    socket_close(to_handle(handle_));
    handle_ = 0;
  }
}

void Socket::interrupt() noexcept { close(); }

void Socket::shutdown_send() noexcept {
  if (handle_valid(handle_)) {
#ifdef _WIN32
    (void)::shutdown(to_handle(handle_), SD_SEND);
#else
    (void)::shutdown(to_handle(handle_), SHUT_WR);
#endif
  }
}

std::optional<Socket> Socket::listen_on(const std::string& host, u16 port, u16& bound_port,
                                        std::string& error) {
  bound_port = 0;
  error.clear();
  addrinfo hints{};
  hints.ai_family = AF_INET;
  hints.ai_socktype = SOCK_STREAM;
  hints.ai_protocol = IPPROTO_TCP;
  hints.ai_flags = AI_PASSIVE;
  addrinfo* results = nullptr;
  const std::string service = to_decimal(port);
  if (::getaddrinfo(host.empty() ? nullptr : host.c_str(), service.c_str(), &hints, &results) != 0) {
    error = "getaddrinfo failed";
    return std::nullopt;
  }
  socket_handle handle = kInvalidSocketHandle;
  for (addrinfo* entry = results; entry != nullptr; entry = entry->ai_next) {
    handle = ::socket(entry->ai_family, entry->ai_socktype, entry->ai_protocol);
    if (handle == kInvalidSocketHandle) {
      continue;
    }
    int reuse = 1;
    (void)::setsockopt(handle, SOL_SOCKET, SO_REUSEADDR,
                       reinterpret_cast<const char*>(&reuse), static_cast<int>(sizeof(reuse)));
    if (::bind(handle, entry->ai_addr, static_cast<int>(entry->ai_addrlen)) == 0 &&
        ::listen(handle, SOMAXCONN) == 0) {
      break;
    }
    socket_close(handle);
    handle = kInvalidSocketHandle;
  }
  ::freeaddrinfo(results);
  if (handle == kInvalidSocketHandle) {
    error = "bind or listen failed: " + to_decimal(static_cast<u64>(socket_last_error()));
    return std::nullopt;
  }
  sockaddr_in address{};
  int length = static_cast<int>(sizeof(address));
  if (::getsockname(handle, reinterpret_cast<sockaddr*>(&address), &length) != 0) {
    socket_close(handle);
    error = "getsockname failed";
    return std::nullopt;
  }
  bound_port = ntohs(address.sin_port);
  return Socket(to_raw(handle));
}

std::optional<Socket> Socket::connect_to(const std::string& host, u16 port, std::string& error) {
  error.clear();
  addrinfo hints{};
  hints.ai_family = AF_INET;
  hints.ai_socktype = SOCK_STREAM;
  hints.ai_protocol = IPPROTO_TCP;
  addrinfo* results = nullptr;
  const std::string service = to_decimal(port);
  if (::getaddrinfo(host.c_str(), service.c_str(), &hints, &results) != 0) {
    error = "getaddrinfo failed";
    return std::nullopt;
  }
  socket_handle handle = kInvalidSocketHandle;
  for (addrinfo* entry = results; entry != nullptr; entry = entry->ai_next) {
    handle = ::socket(entry->ai_family, entry->ai_socktype, entry->ai_protocol);
    if (handle == kInvalidSocketHandle) {
      continue;
    }
    if (::connect(handle, entry->ai_addr, static_cast<int>(entry->ai_addrlen)) == 0) {
      break;
    }
    socket_close(handle);
    handle = kInvalidSocketHandle;
  }
  ::freeaddrinfo(results);
  if (handle == kInvalidSocketHandle) {
    error = "connect failed: " + to_decimal(static_cast<u64>(socket_last_error()));
    return std::nullopt;
  }
  return Socket(to_raw(handle));
}

std::optional<Socket> Socket::accept(std::string& error) const {
  error.clear();
  if (!handle_valid(handle_)) {
    error = "socket is closed";
    return std::nullopt;
  }
  sockaddr_in address{};
  int length = static_cast<int>(sizeof(address));
  for (;;) {
    const socket_handle accepted =
        ::accept(to_handle(handle_), reinterpret_cast<sockaddr*>(&address), &length);
    if (accepted == kInvalidSocketHandle) {
      const int code = socket_last_error();
#ifdef _WIN32
      if (code == WSAEINTR || code == WSAECONNRESET) {
        continue;
      }
      if (code == WSAEINVAL || code == WSAENOTSOCK || code == WSAEOPNOTSUPP) {
        error = "listening socket was closed";
        return std::nullopt;
      }
#else
      if (code == EINTR || code == ECONNABORTED) {
        continue;
      }
      if (code == EINVAL || code == EBADF) {
        error = "listening socket was closed";
        return std::nullopt;
      }
#endif
      error = "accept failed: " + to_decimal(static_cast<u64>(code));
      return std::nullopt;
    }
    return Socket(to_raw(accepted));
  }
}

bool Socket::send_all(const u8* data, std::size_t size) noexcept {
  if (!handle_valid(handle_)) {
    return false;
  }
  std::size_t sent = 0;
  while (sent < size) {
    const std::size_t chunk = (std::min)(size - sent, static_cast<std::size_t>(1U << 20U));
    const int written = ::send(to_handle(handle_), reinterpret_cast<const char*>(data + sent),
                               static_cast<int>(chunk), 0);
    if (written <= 0) {
      return false;
    }
    sent += static_cast<std::size_t>(written);
  }
  return true;
}

bool Socket::recv_exact(u8* data, std::size_t size) noexcept {
  if (!handle_valid(handle_)) {
    return false;
  }
  std::size_t received = 0;
  while (received < size) {
    const int got = ::recv(to_handle(handle_), reinterpret_cast<char*>(data + received),
                           static_cast<int>(size - received), 0);
    if (got <= 0) {
      return false;
    }
    received += static_cast<std::size_t>(got);
  }
  return true;
}

bool FrameChannel::fill(std::size_t needed) {
  if (buffer_.size() - begin_ >= needed) {
    return true;
  }
  if (begin_ > 0) {
    buffer_.erase(buffer_.begin(), buffer_.begin() + static_cast<std::ptrdiff_t>(begin_));
    begin_ = 0;
  }
  if (needed > max_payload_ + kWireHeaderBytes) {
    return false;
  }
  const std::size_t current = buffer_.size();
  buffer_.resize(needed);
  if (!socket_.recv_exact(buffer_.data() + current, needed - current)) {
    buffer_.resize(current);
    return false;
  }
  return true;
}

bool FrameChannel::send_frame(const Frame& frame) {
  std::vector<u8> encoded;
  if (encode_frame(frame, encoded, max_payload_) != Reason::Ok) {
    return false;
  }
  return socket_.send_all(encoded.data(), encoded.size());
}

bool FrameChannel::recv_frame(Frame& frame, Reason& error) {
  error = Reason::Ok;
  if (!fill(kWireHeaderBytes)) {
    error = Reason::TransportClosed;
    return false;
  }
  const u8* header = buffer_.data() + begin_;
  if (read_u32(header) != kWireMagic) {
    error = Reason::Malformed;
    return false;
  }
  const u16 version = read_u16(header + 4);
  if (version != kProtocolVersion) {
    error = Reason::ProtocolVersionMismatch;
    return false;
  }
  const u32 length = read_u32(header + 16);
  if (static_cast<std::size_t>(length) > (std::min)(max_payload_, kAbsoluteMaxFramePayload)) {
    error = Reason::FrameTooLarge;
    return false;
  }
  const std::size_t total = kWireHeaderBytes + static_cast<std::size_t>(length);
  if (!fill(total)) {
    error = Reason::TransportClosed;
    return false;
  }
  std::size_t consumed = 0;
  const Reason code =
      decode_frame(buffer_.data() + begin_, buffer_.size() - begin_, frame, consumed, max_payload_);
  if (code != Reason::Ok) {
    error = code;
    return false;
  }
  begin_ += consumed;
  return true;
}

}  // namespace off
