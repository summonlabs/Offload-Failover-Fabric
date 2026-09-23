// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// Independent-process transport proof. The daemon and the CLI run as real
// child processes over real loopback sockets; synchronisation is protocol
// completion (the READY line, the handshake, the response frame), never a
// sleep or a timeout.

#include <cstdio>
#include <filesystem>
#include <string>
#include <thread>
#include <vector>

#include "fixture.hpp"
#include "off/off.hpp"
#include "testing.hpp"

#ifdef _WIN32
#define OFF_POPEN _popen
#define OFF_PCLOSE _pclose
#else
#define OFF_POPEN popen
#define OFF_PCLOSE pclose
#endif

namespace {

using namespace offtest;

std::string& daemon_path() {
  static std::string path;
  return path;
}

std::string& cli_path() {
  static std::string path;
  return path;
}

std::string trim(const std::string& text) {
  std::size_t end = text.size();
  while (end > 0 && (text[end - 1] == '\n' || text[end - 1] == '\r' || text[end - 1] == ' ')) {
    --end;
  }
  return text.substr(0, end);
}

/// A running offd child process. The READY line on its stdout is the barrier:
/// the constructor does not return until the daemon has bound its socket.
struct Daemon {
  std::FILE* pipe{nullptr};
  std::string host{"127.0.0.1"};
  off::u16 port{0};

  ~Daemon() { stop(); }

  bool start(const std::string& store, std::size_t max_frame = off::kDefaultMaxFramePayload) {
    // The command is handed to cmd.exe, so the program name is reduced to its
    // base name and invoked from the harness working directory. That keeps the
    // launch correct even when the build tree lives under a path with spaces.
    const std::filesystem::path executable(daemon_path());
    std::string program = executable.filename().string();
    if (program.empty()) {
      program = daemon_path();
    }
    std::string command = "\"" + program + "\"";
    command += " --listen 127.0.0.1:0 --ready --allow-remote-shutdown";
    command += " --max-frame-bytes " + off::to_decimal(max_frame);
    if (!store.empty()) {
      command += " --store \"" + store + "\"";
    }
    // cmd.exe strips the first and last quote of /c payloads that both begin
    // and end with one, which would corrupt a command whose last argument is
    // quoted. Wrapping the whole payload in one more pair defeats that rule.
    const std::string wrapped = "\"" + command + "\"";
    pipe = OFF_POPEN(wrapped.c_str(), "r");
    if (pipe == nullptr) {
      return false;
    }
    char buffer[512];
    if (std::fgets(buffer, sizeof(buffer), pipe) == nullptr) {
      return false;
    }
    const std::string line = trim(buffer);
    if (line.rfind("READY ", 0) != 0) {
      return false;
    }
    const std::size_t colon = line.rfind(':');
    if (colon == std::string::npos) {
      return false;
    }
    host = line.substr(6, colon - 6);
    off::u64 parsed = 0;
    if (!off::parse_decimal(line.substr(colon + 1), parsed)) {
      return false;
    }
    port = static_cast<off::u16>(parsed);
    return port != 0;
  }

  /// Requests an orderly shutdown over the protocol, then reaps the child. The
  /// request is best effort: if the daemon already stopped, the wait still
  /// returns because the process is gone.
  void stop() {
    if (pipe == nullptr) {
      return;
    }
    off::SocketRuntime runtime;
    std::string error;
    std::optional<off::Socket> socket = off::Socket::connect_to(host, port, error);
    if (socket.has_value()) {
      off::FrameChannel channel(std::move(*socket), off::kDefaultMaxFramePayload);
      off::Frame hello;
      hello.kind = off::WireMessage::Hello;
      off::CanonicalWriter writer;
      writer.put_u16(off::kProtocolVersion);
      writer.put_text("stopper");
      hello.payload = writer.buffer();
      if (channel.send_frame(hello)) {
        off::Frame welcome;
        off::Reason code = off::Reason::Ok;
        if (channel.recv_frame(welcome, code)) {
          off::Frame request;
          request.kind = off::WireMessage::Request;
          request.correlation = 99;
          off::WireRequest wire;
          wire.kind = off::WireRequestKind::Shutdown;
          std::vector<off::u8> payload;
          if (off::encode_request(wire, payload) == off::Reason::Ok) {
            request.payload = std::move(payload);
            (void)channel.send_frame(request);
            off::Frame reply;
            (void)channel.recv_frame(reply, code);
          }
        }
      }
      channel.socket().close();
    }
    OFF_PCLOSE(pipe);
    pipe = nullptr;
  }
};

/// A framed client connection to the daemon.
struct Client {
  off::SocketRuntime runtime{};
  std::optional<off::FrameChannel> channel{};
  off::u64 correlation{1};

  bool connect(const std::string& host, off::u16 port) {
    std::string error;
    std::optional<off::Socket> socket = off::Socket::connect_to(host, port, error);
    if (!socket.has_value()) {
      return false;
    }
    channel.emplace(std::move(*socket), off::kDefaultMaxFramePayload);
    off::Frame hello;
    hello.kind = off::WireMessage::Hello;
    hello.correlation = correlation++;
    off::CanonicalWriter writer;
    writer.put_u16(off::kProtocolVersion);
    writer.put_text("test-client");
    hello.payload = writer.buffer();
    if (!channel->send_frame(hello)) {
      return false;
    }
    off::Frame welcome;
    off::Reason code = off::Reason::Ok;
    return channel->recv_frame(welcome, code) && welcome.kind == off::WireMessage::Welcome;
  }

  bool call(const off::WireRequest& request, off::WireResponse& response) {
    off::Frame frame;
    frame.kind = off::WireMessage::Request;
    frame.correlation = correlation++;
    std::vector<off::u8> payload;
    if (off::encode_request(request, payload) != off::Reason::Ok) {
      return false;
    }
    frame.payload = std::move(payload);
    if (!channel->send_frame(frame)) {
      return false;
    }
    off::Frame reply;
    off::Reason code = off::Reason::Ok;
    if (!channel->recv_frame(reply, code) || reply.kind != off::WireMessage::Response) {
      return false;
    }
    return off::decode_response(reply.payload.data(), reply.payload.size(), response) ==
           off::Reason::Ok;
  }

  void close() {
    if (channel.has_value()) {
      channel->socket().close();
    }
  }
};

off::CanonicalWriter& name(off::CanonicalWriter& writer, const char* text) {
  writer.put_text(text);
  return writer;
}

off::Reason enroll(Client& client, const char* source, off::SourceRole role) {
  off::WireRequest request;
  request.kind = off::WireRequestKind::EnrollSource;
  off::CanonicalWriter writer;
  name(writer, source);
  writer.put_u8(static_cast<off::u8>(role));
  request.body = writer.buffer();
  off::WireResponse response;
  if (!client.call(request, response)) {
    return off::Reason::TransportClosed;
  }
  return response.status;
}

off::Reason register_target(Client& client, const char* target, const char* host,
                            const char* device, off::DeviceKind kind, const char* scope) {
  off::TargetRecord record;
  record.name = target_name(target);
  record.host = host_name(host);
  record.device = device_name(device);
  record.kind = kind;
  record.scope = scope_name(scope);
  record.incarnation.host = off::Incarnation::from_value(1);
  record.incarnation.device = off::Incarnation::from_value(1);
  off::WireRequest request;
  request.kind = off::WireRequestKind::RegisterTarget;
  off::CanonicalWriter writer;
  name(writer, "topo");
  encode(writer, record);
  request.body = writer.buffer();
  off::WireResponse response;
  if (!client.call(request, response)) {
    return off::Reason::TransportClosed;
  }
  return response.status;
}

off::Reason register_service(Client& client, const char* service, const char* scope) {
  off::ServiceDescriptor descriptor;
  descriptor.name = service_name(service);
  descriptor.scope = scope_name(scope);
  descriptor.required_capabilities = capabilities({off::CapabilityCode::IPv4Forward});
  off::WireRequest request;
  request.kind = off::WireRequestKind::RegisterService;
  off::CanonicalWriter writer;
  name(writer, "topo");
  encode(writer, descriptor);
  request.body = writer.buffer();
  off::WireResponse response;
  if (!client.call(request, response)) {
    return off::Reason::TransportClosed;
  }
  return response.status;
}

off::Reason add_capability(Client& client, const char* target, const char* scope) {
  off::CapabilityEvidence evidence;
  evidence.target = target_name(target);
  evidence.generation = off::CapabilityGeneration::from_value(1);
  evidence.capabilities = capabilities({off::CapabilityCode::IPv4Forward});
  evidence.topology_generation = off::TopologyGeneration::from_value(1);
  evidence.incarnation.host = off::Incarnation::from_value(1);
  evidence.incarnation.device = off::Incarnation::from_value(1);
  evidence.source = source_name("cap");
  off::WireRequest request;
  request.kind = off::WireRequestKind::IngestCapability;
  off::CanonicalWriter writer;
  encode(writer, evidence);
  request.body = writer.buffer();
  off::WireResponse response;
  if (!client.call(request, response)) {
    return off::Reason::TransportClosed;
  }
  (void)scope;
  return response.status;
}

off::Reason set_topology(Client& client, off::u64 generation) {
  off::WireRequest request;
  request.kind = off::WireRequestKind::SetTopologyGeneration;
  off::CanonicalWriter writer;
  writer.put_u64(generation);
  name(writer, "topo");
  request.body = writer.buffer();
  off::WireResponse response;
  if (!client.call(request, response)) {
    return off::Reason::TransportClosed;
  }
  return response.status;
}

off::Reason prepare(Client& client) {
  struct Role {
    const char* name;
    off::SourceRole role;
  };
  const Role roles[] = {
      {"op", off::SourceRole::Operator},
      {"topo", off::SourceRole::TopologyReporter},
      {"failrep", off::SourceRole::FailureReporter},
      {"cap", off::SourceRole::CapabilityReporter},
      {"exec", off::SourceRole::EffectReporter},
  };
  for (const Role& role : roles) {
    const off::Reason code = enroll(client, role.name, role.role);
    if (!off::reason_is_accept(code)) {
      return code;
    }
  }
  off::Reason code = register_target(client, "t1", "h1", "nic1", off::DeviceKind::SmartNic, "edge");
  if (!off::reason_is_accept(code)) {
    return code;
  }
  code = register_target(client, "t2", "h2", "dpu1", off::DeviceKind::Dpu, "edge");
  if (!off::reason_is_accept(code)) {
    return code;
  }
  code = set_topology(client, 1);
  if (!off::reason_is_accept(code)) {
    return code;
  }
  code = add_capability(client, "t1", "edge");
  if (!off::reason_is_accept(code)) {
    return code;
  }
  code = add_capability(client, "t2", "edge");
  if (!off::reason_is_accept(code)) {
    return code;
  }
  code = register_service(client, "svc1", "edge");
  return code;
}

off::Reason place(Client& client, const char* target, off::u64 key) {
  off::WireRequest request;
  request.kind = off::WireRequestKind::EstablishPlacement;
  request.key = key;
  off::CanonicalWriter writer;
  name(writer, "svc1");
  name(writer, target);
  name(writer, "op");
  request.body = writer.buffer();
  off::WireResponse response;
  if (!client.call(request, response)) {
    return off::Reason::TransportClosed;
  }
  return response.status;
}

off::Reason fail(Client& client, const char* target, off::u64 sequence, off::u64 epoch,
                 off::u64 boot) {
  off::FailureEvidence evidence;
  evidence.service = service_name("svc1");
  evidence.target.target = target_name(target);
  evidence.target.incarnation.host = off::Incarnation::from_value(1);
  evidence.target.incarnation.device = off::Incarnation::from_value(1);
  evidence.failure_class = off::FailureClass::TargetUnresponsive;
  evidence.provenance.source = source_name("failrep");
  evidence.provenance.sequence = off::EvidenceSeq::from_value(sequence);
  evidence.provenance.epoch = off::CoordinatorEpoch::from_value(epoch);
  evidence.provenance.boot = off::BootId::from_value(boot);
  evidence.topology_generation = off::TopologyGeneration::from_value(1);
  off::WireRequest request;
  request.kind = off::WireRequestKind::IngestFailure;
  off::CanonicalWriter writer;
  encode(writer, evidence);
  request.body = writer.buffer();
  off::WireResponse response;
  if (!client.call(request, response)) {
    return off::Reason::TransportClosed;
  }
  return response.status;
}

bool current_epoch(Client& client, off::u64& epoch, off::u64& boot) {
  off::WireRequest request;
  request.kind = off::WireRequestKind::Recovery;
  off::WireResponse response;
  if (!client.call(request, response)) {
    return false;
  }
  // The recovery body is JSON; the epoch is also visible in the welcome frame,
  // so query the view for the authoritative values.
  off::WireRequest view_request;
  view_request.kind = off::WireRequestKind::QueryView;
  off::WireResponse view_response;
  if (!client.call(view_request, view_response)) {
    return false;
  }
  const std::string& body = view_response.body;
  const std::size_t epoch_at = body.find("\"epoch\":");
  const std::size_t boot_at = body.find("\"boot\":");
  if (epoch_at == std::string::npos || boot_at == std::string::npos) {
    return false;
  }
  return off::parse_decimal(
             std::string_view(body).substr(epoch_at + 8, body.find(',', epoch_at) - epoch_at - 8),
             epoch) &&
         off::parse_decimal(
             std::string_view(body).substr(boot_at + 7, body.find(',', boot_at) - boot_at - 7),
             boot);
}

off::Reason request_failover(Client& client, off::u64 key, const std::string& body_suffix,
                             off::WireResponse& response) {
  off::WireRequest request;
  request.kind = off::WireRequestKind::RequestFailover;
  request.key = key;
  request.body = std::vector<off::u8>(body_suffix.begin(), body_suffix.end());
  if (!client.call(request, response)) {
    return off::Reason::TransportClosed;
  }
  return response.status;
}

std::string failover_body(off::u64 evidence, bool authorized) {
  off::CanonicalWriter writer;
  name(writer, "svc1");
  writer.put_u8(static_cast<off::u8>(off::RecoveryTrigger::FailureEvidence));
  writer.put_u64(evidence);
  name(writer, "op");
  writer.put_bool(authorized);
  const std::vector<off::u8>& buffer = writer.buffer();
  return std::string(buffer.begin(), buffer.end());
}

OFF_TEST(process, daemon_serves_a_full_recovery_over_real_sockets) {
  const std::filesystem::path store = unique_directory("process-recovery");
  Daemon daemon;
  OFF_REQUIRE(daemon.start(store.string()));
  OFF_REQUIRE(daemon.port != 0);
  Client client;
  OFF_REQUIRE(client.connect(daemon.host, daemon.port));
  OFF_CHECK(off::reason_is_accept(prepare(client)));
  OFF_CHECK_REASON(place(client, "t1", 1), off::Reason::AcceptPlacementEstablished);

  off::u64 epoch = 0;
  off::u64 boot = 0;
  OFF_REQUIRE(current_epoch(client, epoch, boot));
  OFF_CHECK(epoch >= 1);
  OFF_CHECK(off::reason_is_accept(fail(client, "t1", 1, epoch, boot)));

  off::WireResponse response;
  OFF_CHECK_REASON(request_failover(client, 5, failover_body(1, false), response),
                   off::Reason::AcceptIntentEmitted);
  OFF_CHECK(response.accepted);
  OFF_CHECK(response.body.find("ACCEPT_INTENT_EMITTED") != std::string::npos);

  off::WireRequest duplicate;
  duplicate.kind = off::WireRequestKind::RequestFailover;
  duplicate.key = 5;
  const std::string body = failover_body(1, false);
  duplicate.body = std::vector<off::u8>(body.begin(), body.end());
  off::WireResponse duplicate_response;
  OFF_REQUIRE(client.call(duplicate, duplicate_response));
  OFF_CHECK(duplicate_response.duplicate);
  // The replay carries the same decision; only the duplicate marker differs.
  OFF_CHECK(duplicate_response.body.find("ACCEPT_INTENT_EMITTED") != std::string::npos);
  OFF_CHECK(duplicate_response.body.find("\"fence\":2") != std::string::npos);

  off::WireRequest pull;
  pull.kind = off::WireRequestKind::PullIntent;
  off::WireResponse pull_response;
  OFF_REQUIRE(client.call(pull, pull_response));
  OFF_CHECK(pull_response.body.find("\"intent\"") != std::string::npos);
  OFF_CHECK(pull_response.body.find("\"pending\":true") != std::string::npos);

  off::WireRequest fence;
  fence.kind = off::WireRequestKind::VerifyFence;
  off::CanonicalWriter writer;
  name(writer, "svc1");
  off::TargetRef holder;
  holder.target = target_name("t1");
  holder.incarnation.host = off::Incarnation::from_value(1);
  holder.incarnation.device = off::Incarnation::from_value(1);
  encode(writer, holder);
  writer.put_u64(1);
  fence.body = writer.buffer();
  off::WireResponse fence_response;
  OFF_REQUIRE(client.call(fence, fence_response));
  OFF_CHECK(!fence_response.accepted);
  OFF_CHECK(fence_response.body.find("FENCE_STALE") != std::string::npos);

  client.close();
  daemon.stop();
}

OFF_TEST(process, concurrent_clients_cannot_double_commit) {
  const std::filesystem::path store = unique_directory("process-concurrent");
  Daemon daemon;
  OFF_REQUIRE(daemon.start(store.string()));
  Client setup;
  OFF_REQUIRE(setup.connect(daemon.host, daemon.port));
  OFF_CHECK(off::reason_is_accept(prepare(setup)));
  OFF_CHECK_REASON(place(setup, "t1", 1), off::Reason::AcceptPlacementEstablished);
  off::u64 epoch = 0;
  off::u64 boot = 0;
  OFF_REQUIRE(current_epoch(setup, epoch, boot));
  OFF_CHECK(off::reason_is_accept(fail(setup, "t1", 1, epoch, boot)));

  constexpr int kClients = 4;
  std::vector<off::Reason> outcomes(kClients, off::Reason::Internal);
  std::vector<bool> accepted(kClients, false);
  std::vector<std::thread> workers;
  for (int index = 0; index < kClients; ++index) {
    workers.emplace_back([&daemon, &outcomes, &accepted, index]() {
      Client client;
      if (!client.connect(daemon.host, daemon.port)) {
        return;
      }
      off::WireResponse response;
      outcomes[index] = request_failover(client, 77, failover_body(1, false), response);
      accepted[index] = response.accepted;
      client.close();
    });
  }
  for (std::thread& worker : workers) {
    worker.join();
  }
  int committed = 0;
  for (int index = 0; index < kClients; ++index) {
    if (accepted[index]) {
      ++committed;
    }
    OFF_CHECK(outcomes[index] == off::Reason::AcceptIntentEmitted ||
              outcomes[index] == off::Reason::AcceptDuplicateReplayCached ||
              outcomes[index] == off::Reason::ProtocolViolation ||
              outcomes[index] == off::Reason::TransportClosed);
  }
  OFF_CHECK(committed >= 1);

  off::WireRequest view;
  view.kind = off::WireRequestKind::QueryView;
  off::WireResponse view_response;
  OFF_REQUIRE(setup.call(view, view_response));
  OFF_CHECK(view_response.body.find("\"fence\":2") != std::string::npos);
  setup.close();
  daemon.stop();
}

OFF_TEST(process, daemon_restart_recovers_persisted_state_conservatively) {
  const std::filesystem::path store = unique_directory("process-restart");
  off::u64 first_epoch = 0;
  {
    Daemon daemon;
    OFF_REQUIRE(daemon.start(store.string()));
    Client client;
    OFF_REQUIRE(client.connect(daemon.host, daemon.port));
    OFF_CHECK(off::reason_is_accept(prepare(client)));
    OFF_CHECK_REASON(place(client, "t1", 1), off::Reason::AcceptPlacementEstablished);
    off::u64 epoch = 0;
    off::u64 boot = 0;
    OFF_REQUIRE(current_epoch(client, epoch, boot));
    first_epoch = epoch;
    client.close();
    daemon.stop();
  }
  Daemon daemon;
  OFF_REQUIRE(daemon.start(store.string()));
  Client client;
  OFF_REQUIRE(client.connect(daemon.host, daemon.port));
  off::u64 epoch = 0;
  off::u64 boot = 0;
  OFF_REQUIRE(current_epoch(client, epoch, boot));
  OFF_CHECK(epoch > first_epoch);

  off::WireRequest view;
  view.kind = off::WireRequestKind::QueryView;
  off::WireResponse view_response;
  OFF_REQUIRE(client.call(view, view_response));
  OFF_CHECK(view_response.body.find("\"name\":\"svc1\"") != std::string::npos);
  OFF_CHECK(view_response.body.find("\"active\":{\"incarnation\"") != std::string::npos);
  OFF_CHECK(view_response.body.find("\"target\":\"t1\"") != std::string::npos);

  off::WireRequest recovery;
  recovery.kind = off::WireRequestKind::Recovery;
  off::WireResponse recovery_response;
  OFF_REQUIRE(client.call(recovery, recovery_response));
  OFF_CHECK(recovery_response.body.find("clean_reopen") != std::string::npos);
  client.close();
  daemon.stop();
}

OFF_TEST(process, cli_remote_mode_reports_the_daemon_state) {
  const std::filesystem::path store = unique_directory("process-cli");
  Daemon daemon;
  OFF_REQUIRE(daemon.start(store.string()));
  Client client;
  OFF_REQUIRE(client.connect(daemon.host, daemon.port));
  OFF_CHECK(off::reason_is_accept(prepare(client)));
  client.close();

  const std::string endpoint = daemon.host + ":" + off::to_decimal(daemon.port);
  const std::filesystem::path cli(cli_path());
  const std::string program = cli.filename().empty() ? cli_path() : cli.filename().string();
  const std::string command = "\"" + program + "\" --connect " + endpoint + " view";
  const std::string wrapped_command = "\"" + command + "\"";
  std::FILE* pipe = OFF_POPEN(wrapped_command.c_str(), "r");
  OFF_REQUIRE(pipe != nullptr);
  std::string output;
  char buffer[512];
  while (std::fgets(buffer, sizeof(buffer), pipe) != nullptr) {
    output += buffer;
  }
  OFF_PCLOSE(pipe);
  OFF_CHECK(output.find("\"services\"") != std::string::npos);
  OFF_CHECK(output.find("\"svc1\"") != std::string::npos);
  OFF_CHECK(output.find("\"runtime_version\":\"1.0.0\"") != std::string::npos);

  const std::string stats_command = "\"" + program + "\" --connect " + endpoint + " stats";
  const std::string wrapped_stats = "\"" + stats_command + "\"";
  std::FILE* stats_pipe = OFF_POPEN(wrapped_stats.c_str(), "r");
  OFF_REQUIRE(stats_pipe != nullptr);
  std::string stats_output;
  while (std::fgets(buffer, sizeof(buffer), stats_pipe) != nullptr) {
    stats_output += buffer;
  }
  OFF_PCLOSE(stats_pipe);
  OFF_CHECK(stats_output.find("\"sources_enrolled\":5") != std::string::npos);
  daemon.stop();
}

OFF_TEST(process, oversized_declared_frame_is_refused_and_the_connection_ends) {
  const std::filesystem::path store = unique_directory("process-oversize");
  Daemon daemon;
  OFF_REQUIRE(daemon.start(store.string(), 4096));
  Client client;
  OFF_REQUIRE(client.connect(daemon.host, daemon.port));

  std::vector<off::u8> header(off::kWireHeaderBytes, 0);
  header[0] = 0x4F;
  header[1] = 0x46;
  header[2] = 0x46;
  header[3] = 0x52;
  header[4] = 0x00;
  header[5] = 0x01;
  header[6] = 0x00;
  header[7] = 0x03;
  header[16] = 0x00;
  header[17] = 0x80;
  header[18] = 0x00;
  header[19] = 0x00;
  OFF_CHECK(client.channel->socket().send_all(header.data(), header.size()));

  off::Frame reply;
  off::Reason code = off::Reason::Ok;
  const bool received = client.channel->recv_frame(reply, code);
  if (received) {
    OFF_CHECK(reply.kind == off::WireMessage::Error);
  } else {
    OFF_CHECK(code == off::Reason::FrameTooLarge || code == off::Reason::TransportClosed);
  }
  client.close();

  Client after;
  OFF_REQUIRE(after.connect(daemon.host, daemon.port));
  off::WireRequest view;
  view.kind = off::WireRequestKind::QueryView;
  off::WireResponse response;
  OFF_CHECK(after.call(view, response));
  after.close();
  daemon.stop();
}

OFF_TEST(process, abrupt_client_disconnect_does_not_disturb_the_daemon) {
  const std::filesystem::path store = unique_directory("process-abrupt");
  Daemon daemon;
  OFF_REQUIRE(daemon.start(store.string()));
  {
    Client rude;
    OFF_REQUIRE(rude.connect(daemon.host, daemon.port));
    rude.channel->socket().close();
  }
  Client client;
  OFF_REQUIRE(client.connect(daemon.host, daemon.port));
  OFF_CHECK(off::reason_is_accept(prepare(client)));
  client.close();
  daemon.stop();
}

}  // namespace

int main(int argc, char** argv) {
  if (argc > 1) {
    daemon_path() = argv[1];
  }
  if (argc > 2) {
    cli_path() = argv[2];
  }
  if (daemon_path().empty() || cli_path().empty()) {
    std::fputs("test_process: offd and off paths are required\n", stderr);
    return 2;
  }
  return offtest::run_all("") == 0 ? 0 : 1;
}
