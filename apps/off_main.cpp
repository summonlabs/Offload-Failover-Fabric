// off - inspection and control CLI for the Offload Failover Fabric.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// The same request executor backs the local (in-process) and remote (socket)
// modes, so a command has exactly one meaning whichever surface it uses.

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "off/off.hpp"

namespace {

using off::u16;
using off::u64;
using off::u8;

/// Names are canonically encoded as length-prefixed text; the CLI builds the
/// same bytes the runtime would so the wire path is exercised end to end.
template <class Tag>
void put_name(off::CanonicalWriter& writer, const off::Name<Tag>& name) {
  writer.put_text(name.view());
}

struct Context {
  bool remote{false};
  std::string host{"127.0.0.1"};
  u16 port{0};
  std::string store_directory{};
  std::size_t max_frame{off::kDefaultMaxFramePayload};
  std::optional<off::Socket> socket{};
  std::optional<off::FrameChannel> channel{};
  std::optional<off::Fabric> fabric{};
  u64 next_correlation{1};
};

int fail(const std::string& message) {
  std::fprintf(stderr, "off: %s\n", message.c_str());
  return 1;
}

bool parse_u64(const std::string& text, u64& out) { return off::parse_decimal(text, out); }

template <class Tag>
bool parse_name(const std::string& text, off::Name<Tag>& out) {
  const std::optional<off::Name<Tag>> parsed = off::Name<Tag>::parse(text);
  if (!parsed.has_value()) {
    return false;
  }
  out = *parsed;
  return true;
}

std::vector<std::string> split(const std::string& text, char separator) {
  std::vector<std::string> out;
  std::string current;
  for (const char c : text) {
    if (c == separator) {
      out.push_back(current);
      current.clear();
      continue;
    }
    current.push_back(c);
  }
  out.push_back(current);
  return out;
}

bool parse_capabilities(const std::string& text, off::CapabilitySet& out) {
  if (text.empty() || text == "-") {
    return true;
  }
  for (const std::string& token : split(text, ',')) {
    off::CapabilityCode code{};
    if (!off::capability_code_parse(token, code)) {
      return false;
    }
    out.insert(code);
  }
  return true;
}

bool parse_preconditions(const std::string& text, off::PreconditionSet& out) {
  if (text.empty() || text == "-") {
    return true;
  }
  for (const std::string& token : split(text, ',')) {
    off::Precondition value{};
    if (!off::precondition_parse(token, value)) {
      return false;
    }
    out.insert(value);
  }
  return true;
}

bool parse_dependencies(const std::string& text, std::vector<off::DependencySpec>& out) {
  if (text.empty() || text == "-") {
    return true;
  }
  for (const std::string& token : split(text, ',')) {
    const std::vector<std::string> parts = split(token, ':');
    if (parts.size() != 2) {
      return false;
    }
    off::DependencySpec spec;
    if (!parse_name(parts[0], spec.service)) {
      return false;
    }
    if (!off::dependency_kind_parse(parts[1], spec.kind)) {
      return false;
    }
    out.push_back(spec);
  }
  return true;
}

struct Args {
  std::vector<std::string> positional{};
  std::vector<std::pair<std::string, std::string>> flags{};

  [[nodiscard]] bool has(const std::string& name) const {
    for (const auto& flag : flags) {
      if (flag.first == name) {
        return true;
      }
    }
    return false;
  }

  [[nodiscard]] bool get(const std::string& name, std::string& value) const {
    for (const auto& flag : flags) {
      if (flag.first == name) {
        value = flag.second;
        return true;
      }
    }
    return false;
  }

  [[nodiscard]] u64 number(const std::string& name, u64 fallback, bool& ok) const {
    std::string value;
    if (!get(name, value)) {
      return fallback;
    }
    u64 parsed = 0;
    if (!parse_u64(value, parsed)) {
      ok = false;
      return fallback;
    }
    return parsed;
  }
};

bool parse_args(const std::vector<std::string>& tokens, Args& out) {
  for (std::size_t index = 0; index < tokens.size(); ++index) {
    const std::string& token = tokens[index];
    if (token.size() > 2 && token[0] == '-' && token[1] == '-') {
      const std::size_t equals = token.find('=');
      if (equals != std::string::npos) {
        out.flags.emplace_back(token.substr(2, equals - 2), token.substr(equals + 1));
        continue;
      }
      const std::string name = token.substr(2);
      if (index + 1 < tokens.size() && tokens[index + 1].size() >= 2 && tokens[index + 1][0] == '-' &&
          tokens[index + 1][1] == '-') {
        out.flags.emplace_back(name, std::string{});
        continue;
      }
      if (index + 1 < tokens.size()) {
        out.flags.emplace_back(name, tokens[++index]);
        continue;
      }
      out.flags.emplace_back(name, std::string{});
      continue;
    }
    out.positional.push_back(token);
  }
  return true;
}

bool transmit(Context& context, const off::WireRequest& request) {
  if (!context.remote) {
    off::WireResponse response;
    const off::Reason code = off::execute_request(*context.fabric, request, response);
    if (!response.body.empty()) {
      std::printf("%s\n", response.body.c_str());
    } else {
      std::printf("{\"status\":\"%s\"}\n", std::string(off::reason_code_text(code)).c_str());
    }
    return off::reason_is_accept(code);
  }
  off::Frame frame;
  frame.kind = off::WireMessage::Request;
  frame.correlation = context.next_correlation++;
  std::vector<u8> payload;
  const off::Reason encoded = off::encode_request(request, payload, context.max_frame);
  if (encoded != off::Reason::Ok) {
    return false;
  }
  frame.payload = std::move(payload);
  if (!context.channel->send_frame(frame)) {
    return false;
  }
  off::Frame reply;
  off::Reason error = off::Reason::Ok;
  if (!context.channel->recv_frame(reply, error)) {
    return false;
  }
  if (reply.kind != off::WireMessage::Response) {
    return false;
  }
  off::WireResponse response;
  if (off::decode_response(reply.payload.data(), reply.payload.size(), response) != off::Reason::Ok) {
    return false;
  }
  std::printf("%s\n", response.body.c_str());
  return off::reason_is_accept(response.status);
}

off::WireRequest make_request(off::WireRequestKind kind, u64 key) {
  off::WireRequest request;
  request.kind = kind;
  request.key = key;
  return request;
}

bool body_ready(Context& context, off::WireRequest& request) {
  // Bodies larger than the negotiated ceiling are refused before any socket
  // write; the caller sees a stable refusal instead of a partial frame.
  if (request.body.size() > context.max_frame) {
    return false;
  }
  return true;
}

void print_usage() {
  std::fputs(
      "usage: off [--store DIR | --connect HOST:PORT] [--max-frame-bytes N]\n"
      "           [--script FILE | --stdin] COMMAND [ARGS]\n"
      "\n"
      "observation\n"
      "  view | export | stats | recovery | intents | pull | explain SERVICE\n"
      "enrolment and topology\n"
      "  enroll SOURCE ROLE\n"
      "  service NAME SCOPE stateless|stateful [--caps a,b] [--deps s:hard,t:soft]\n"
      "          [--pre fence_advance,verified_effect,...] [--state-gen N]\n"
      "  target NAME HOST DEVICE KIND SCOPE HOST_INC DEVICE_INC\n"
      "  topology GENERATION REPORTER\n"
      "  policy GENERATION SCOPE [--max-attempts N] [--max-plan-steps N] [--candidates N]\n"
      "         [--evidence-max-age N] [--dependency-depth N] [--allow-degraded 0|1]\n"
      "         [--allow-failback 0|1] [--require-operator-ambiguous 0|1]\n"
      "  withdraw SERVICE OPERATOR\n"
      "  place SERVICE TARGET OPERATOR [--key N]\n"
      "evidence\n"
      "  capability TARGET GENERATION CAPS HOST_INC DEVICE_INC SOURCE\n"
      "  fail SERVICE TARGET CLASS SOURCE SEQUENCE [--ambiguous 0|1] [--id N]\n"
      "       [--inc-host N --inc-dev N]\n"
      "  dep SERVICE DEPENDENCY healthy|degraded|failed|unknown SOURCE SEQUENCE\n"
      "recovery\n"
      "  failover SERVICE [--key N] [--evidence N] [--trigger failure_evidence|planned_maintenance|operator_command]\n"
      "           [--requester NAME] [--authorized 0|1]\n"
      "  resolve SERVICE EVIDENCE confirm|deny OPERATOR [--key N]\n"
      "  resume SERVICE OPERATOR [--key N]\n"
      "  failback SERVICE TARGET OPERATOR [--key N]\n"
      "  ack ATTEMPT FENCE GENERATION accept|reject EXECUTOR [--key N]\n"
      "  effect SERVICE ATTEMPT GENERATION FENCE TARGET KIND positive|negative SOURCE\n"
      "         [--state-gen N] [--id N] [--inc-host N --inc-dev N]\n"
      "  cancel KEY\n"
      "  fence SERVICE TARGET TOKEN [--inc-host N --inc-dev N]\n"
      "  shutdown\n",
      stderr);
}

bool run_command(Context& context, const std::string& command, const Args& args) {
  const std::vector<std::string>& p = args.positional;
  bool ok = true;
  const auto need = [&](std::size_t count) { return p.size() >= count; };

  if (command == "view" || command == "export" || command == "stats" || command == "recovery") {
    off::WireRequestKind kind = off::WireRequestKind::QueryView;
    if (command == "export") {
      kind = off::WireRequestKind::Export;
    } else if (command == "stats") {
      kind = off::WireRequestKind::Stats;
    } else if (command == "recovery") {
      kind = off::WireRequestKind::Recovery;
    }
    return transmit(context, make_request(kind, 0));
  }
  if (command == "intents" || command == "pull") {
    return transmit(context, make_request(off::WireRequestKind::PullIntent, 0));
  }
  if (command == "explain") {
    if (!need(1)) {
      return false;
    }
    off::ServiceName service;
    if (!parse_name(p[0], service)) {
      return false;
    }
    off::WireRequest request = make_request(off::WireRequestKind::Explain, 0);
    off::CanonicalWriter writer;
    put_name(writer, service);
    request.body = writer.buffer();
    return transmit(context, request);
  }
  if (command == "enroll") {
    if (!need(2)) {
      return false;
    }
    off::SourceName source;
    off::SourceRole role{};
    if (!parse_name(p[0], source) || !off::source_role_parse(p[1], role)) {
      return false;
    }
    off::WireRequest request = make_request(off::WireRequestKind::EnrollSource, 0);
    off::CanonicalWriter writer;
    put_name(writer, source);
    writer.put_u8(static_cast<u8>(role));
    request.body = writer.buffer();
    return transmit(context, request);
  }
  if (command == "service") {
    if (!need(3)) {
      return false;
    }
    off::ServiceDescriptor descriptor;
    if (!parse_name(p[0], descriptor.name) || !parse_name(p[1], descriptor.scope) ||
        !off::statefulness_parse(p[2], descriptor.statefulness)) {
      return false;
    }
    std::string value;
    if (args.get("caps", value) && !parse_capabilities(value, descriptor.required_capabilities)) {
      return false;
    }
    if (args.get("deps", value) && !parse_dependencies(value, descriptor.dependencies)) {
      return false;
    }
    if (args.get("pre", value) && !parse_preconditions(value, descriptor.preconditions)) {
      return false;
    }
    const u64 state_generation = args.number("state-gen", 0, ok);
    if (!ok) {
      return false;
    }
    descriptor.last_known_state_generation = off::StateGeneration::from_value(state_generation);
    off::SourceName registrar;
    std::string registrar_text = "topo";
    (void)args.get("registrar", registrar_text);
    if (!parse_name(registrar_text, registrar)) {
      return false;
    }
    off::WireRequest request = make_request(off::WireRequestKind::RegisterService, 0);
    off::CanonicalWriter writer;
    put_name(writer, registrar);
    encode(writer, descriptor);
    request.body = writer.buffer();
    return transmit(context, request);
  }
  if (command == "target") {
    if (!need(7)) {
      return false;
    }
    off::TargetRecord record;
    if (!parse_name(p[0], record.name) || !parse_name(p[1], record.host) ||
        !parse_name(p[2], record.device) || !off::device_kind_parse(p[3], record.kind) ||
        !parse_name(p[4], record.scope)) {
      return false;
    }
    u64 host_incarnation = 0;
    u64 device_incarnation = 0;
    if (!parse_u64(p[5], host_incarnation) || !parse_u64(p[6], device_incarnation)) {
      return false;
    }
    record.incarnation.host = off::Incarnation::from_value(host_incarnation);
    record.incarnation.device = off::Incarnation::from_value(device_incarnation);
    off::SourceName registrar;
    std::string registrar_text = "topo";
    (void)args.get("registrar", registrar_text);
    if (!parse_name(registrar_text, registrar)) {
      return false;
    }
    off::WireRequest request = make_request(off::WireRequestKind::RegisterTarget, 0);
    off::CanonicalWriter writer;
    put_name(writer, registrar);
    encode(writer, record);
    request.body = writer.buffer();
    return transmit(context, request);
  }
  if (command == "topology") {
    if (!need(2)) {
      return false;
    }
    u64 generation = 0;
    off::SourceName reporter;
    if (!parse_u64(p[0], generation) || !parse_name(p[1], reporter)) {
      return false;
    }
    off::WireRequest request = make_request(off::WireRequestKind::SetTopologyGeneration, 0);
    off::CanonicalWriter writer;
    writer.put_u64(generation);
    put_name(writer, reporter);
    request.body = writer.buffer();
    return transmit(context, request);
  }
  if (command == "policy") {
    if (!need(2)) {
      return false;
    }
    off::PolicyDescriptor policy;
    u64 generation = 0;
    if (!parse_u64(p[0], generation) || !parse_name(p[1], policy.scope)) {
      return false;
    }
    policy.generation = off::PolicyGeneration::from_value(generation);
    u64 max_attempts = policy.max_attempts_per_generation;
    u64 max_plan_steps = policy.max_plan_steps;
    u64 candidates = policy.max_fallback_candidates;
    u64 evidence_age = policy.evidence_max_age_ticks;
    u64 dependency_depth = policy.dependency_max_depth;
    max_attempts = args.number("max-attempts", max_attempts, ok);
    max_plan_steps = args.number("max-plan-steps", max_plan_steps, ok);
    candidates = args.number("candidates", candidates, ok);
    evidence_age = args.number("evidence-max-age", evidence_age, ok);
    dependency_depth = args.number("dependency-depth", dependency_depth, ok);
    const u64 degraded = args.number("allow-degraded", policy.allow_degraded_continuity ? 1 : 0, ok);
    const u64 failback = args.number("allow-failback", policy.allow_failback ? 1 : 0, ok);
    const u64 ambiguous =
        args.number("require-operator-ambiguous", policy.require_operator_for_ambiguous ? 1 : 0, ok);
    if (!ok || max_attempts > 0xFFFFFFFFULL || max_plan_steps > 0xFFFFFFFFULL ||
        candidates > 0xFFFFFFFFULL || evidence_age > 0xFFFFFFFFULL ||
        dependency_depth > 0xFFFFFFFFULL) {
      return false;
    }
    policy.max_attempts_per_generation = static_cast<off::u32>(max_attempts);
    policy.max_plan_steps = static_cast<off::u32>(max_plan_steps);
    policy.max_fallback_candidates = static_cast<off::u32>(candidates);
    policy.evidence_max_age_ticks = static_cast<off::u32>(evidence_age);
    policy.dependency_max_depth = static_cast<off::u32>(dependency_depth);
    policy.allow_degraded_continuity = degraded != 0;
    policy.allow_failback = failback != 0;
    policy.require_operator_for_ambiguous = ambiguous != 0;
    off::WireRequest request = make_request(off::WireRequestKind::SetPolicy, 0);
    off::CanonicalWriter writer;
    encode(writer, policy);
    request.body = writer.buffer();
    return transmit(context, request);
  }
  if (command == "withdraw") {
    if (!need(2)) {
      return false;
    }
    off::ServiceName service;
    off::SourceName requester;
    if (!parse_name(p[0], service) || !parse_name(p[1], requester)) {
      return false;
    }
    off::WireRequest request = make_request(off::WireRequestKind::WithdrawService, 0);
    off::CanonicalWriter writer;
    put_name(writer, service);
    put_name(writer, requester);
    request.body = writer.buffer();
    return transmit(context, request);
  }
  if (command == "place") {
    if (!need(3)) {
      return false;
    }
    off::PlacementRequest placement;
    placement.key = off::RequestKey::from_value(args.number("key", 0, ok));
    if (!parse_name(p[0], placement.service) || !parse_name(p[1], placement.target) ||
        !parse_name(p[2], placement.requester)) {
      return false;
    }
    if (!ok) {
      return false;
    }
    off::WireRequest request =
        make_request(off::WireRequestKind::EstablishPlacement, placement.key.value());
    off::CanonicalWriter writer;
    put_name(writer, placement.service);
    put_name(writer, placement.target);
    put_name(writer, placement.requester);
    request.body = writer.buffer();
    return transmit(context, request);
  }
  if (command == "capability") {
    if (!need(6)) {
      return false;
    }
    off::CapabilityEvidence evidence;
    u64 generation = 0;
    u64 host_incarnation = 0;
    u64 device_incarnation = 0;
    if (!parse_name(p[0], evidence.target) || !parse_u64(p[1], generation) ||
        !parse_capabilities(p[2], evidence.capabilities) || !parse_u64(p[3], host_incarnation) ||
        !parse_u64(p[4], device_incarnation) || !parse_name(p[5], evidence.source)) {
      return false;
    }
    evidence.generation = off::CapabilityGeneration::from_value(generation);
    evidence.incarnation.host = off::Incarnation::from_value(host_incarnation);
    evidence.incarnation.device = off::Incarnation::from_value(device_incarnation);
    u64 topology = 0;
    std::string value;
    if (!args.get("topology", value)) {
      if (context.remote) {
        std::fputs("off: --topology is required in remote mode\n", stderr);
        return false;
      }
      topology = context.fabric->view().topology_generation.value();
    } else if (!parse_u64(value, topology)) {
      return false;
    }
    if (!ok) {
      return false;
    }
    evidence.topology_generation = off::TopologyGeneration::from_value(topology);
    off::WireRequest request = make_request(off::WireRequestKind::IngestCapability, 0);
    off::CanonicalWriter writer;
    encode(writer, evidence);
    request.body = writer.buffer();
    return transmit(context, request);
  }
  if (command == "fail") {
    if (!need(5)) {
      return false;
    }
    off::FailureEvidence evidence;
    u64 sequence = 0;
    u64 identifier = 0;
    u64 host_incarnation = 0;
    u64 device_incarnation = 0;
    if (!parse_name(p[0], evidence.service) || !parse_name(p[1], evidence.target.target) ||
        !off::failure_class_parse(p[2], evidence.failure_class) ||
        !parse_name(p[3], evidence.provenance.source) || !parse_u64(p[4], sequence)) {
      return false;
    }
    evidence.provenance.sequence = off::EvidenceSeq::from_value(sequence);
    const u64 ambiguous = args.number("ambiguous", 0, ok);
    if (!ok || ambiguous > 2) {
      return false;
    }
    evidence.ambiguity = static_cast<off::AmbiguityState>(ambiguous);
    identifier = args.number("id", 0, ok);
    host_incarnation = args.number("inc-host", 0, ok);
    device_incarnation = args.number("inc-dev", 0, ok);
    if (!ok) {
      return false;
    }
    evidence.id = off::EvidenceId::from_value(identifier);
    evidence.target.incarnation.host = off::Incarnation::from_value(host_incarnation);
    evidence.target.incarnation.device = off::Incarnation::from_value(device_incarnation);
    if (context.remote) {
      evidence.provenance.epoch = off::CoordinatorEpoch::from_value(args.number("epoch", 0, ok));
      evidence.provenance.boot = off::BootId::from_value(args.number("boot", 0, ok));
      evidence.topology_generation =
          off::TopologyGeneration::from_value(args.number("topology", 0, ok));
    } else {
      const off::FabricView current = context.fabric->view();
      evidence.provenance.epoch = current.epoch;
      evidence.provenance.boot = current.boot;
      evidence.topology_generation = current.topology_generation;
    }
    if (!ok) {
      return false;
    }
    off::WireRequest request = make_request(off::WireRequestKind::IngestFailure, 0);
    off::CanonicalWriter writer;
    encode(writer, evidence);
    request.body = writer.buffer();
    return transmit(context, request);
  }
  if (command == "dep") {
    if (!need(5)) {
      return false;
    }
    off::DependencyObservation observation;
    u64 sequence = 0;
    if (!parse_name(p[0], observation.service) || !parse_name(p[1], observation.dependency) ||
        !off::dependency_health_parse(p[2], observation.health) ||
        !parse_name(p[3], observation.provenance.source) || !parse_u64(p[4], sequence)) {
      return false;
    }
    observation.provenance.sequence = off::EvidenceSeq::from_value(sequence);
    if (context.remote) {
      observation.provenance.epoch = off::CoordinatorEpoch::from_value(args.number("epoch", 0, ok));
      observation.provenance.boot = off::BootId::from_value(args.number("boot", 0, ok));
      observation.topology_generation =
          off::TopologyGeneration::from_value(args.number("topology", 0, ok));
    } else {
      const off::FabricView current = context.fabric->view();
      observation.provenance.epoch = current.epoch;
      observation.provenance.boot = current.boot;
      observation.topology_generation = current.topology_generation;
    }
    if (!ok) {
      return false;
    }
    off::WireRequest request = make_request(off::WireRequestKind::IngestDependency, 0);
    off::CanonicalWriter writer;
    encode(writer, observation);
    request.body = writer.buffer();
    return transmit(context, request);
  }
  if (command == "failover" || command == "resume") {
    if (!need(1)) {
      return false;
    }
    off::FailoverRequest failover;
    failover.key = off::RequestKey::from_value(args.number("key", 0, ok));
    if (!parse_name(p[0], failover.service)) {
      return false;
    }
    if (command == "resume") {
      if (!need(2) || !parse_name(p[1], failover.requester)) {
        return false;
      }
      failover.operator_authorized = true;
      failover.trigger = off::RecoveryTrigger::OperatorCommand;
    } else {
      std::string trigger = "failure_evidence";
      (void)args.get("trigger", trigger);
      if (!off::recovery_trigger_parse(trigger, failover.trigger)) {
        return false;
      }
      std::string requester = "operator";
      (void)args.get("requester", requester);
      if (!parse_name(requester, failover.requester)) {
        return false;
      }
      failover.evidence = off::EvidenceId::from_value(args.number("evidence", 0, ok));
      failover.operator_authorized = args.number("authorized", 0, ok) != 0;
    }
    if (!ok) {
      return false;
    }
    off::WireRequest request = make_request(
        command == "resume" ? off::WireRequestKind::ResumeRecovery
                            : off::WireRequestKind::RequestFailover,
        failover.key.value());
    off::CanonicalWriter writer;
    put_name(writer, failover.service);
    writer.put_u8(static_cast<u8>(failover.trigger));
    writer.put_u64(failover.evidence.value());
    put_name(writer, failover.requester);
    writer.put_bool(failover.operator_authorized);
    request.body = writer.buffer();
    return transmit(context, request);
  }
  if (command == "resolve") {
    if (!need(4)) {
      return false;
    }
    off::AmbiguityResolution resolution;
    resolution.key = off::RequestKey::from_value(args.number("key", 0, ok));
    u64 evidence = 0;
    if (!parse_name(p[0], resolution.service) || !parse_u64(p[1], evidence) ||
        !parse_name(p[3], resolution.resolved_by)) {
      return false;
    }
    if (p[2] == "confirm") {
      resolution.confirm_failure = true;
    } else if (p[2] == "deny") {
      resolution.confirm_failure = false;
    } else {
      return false;
    }
    resolution.evidence = off::EvidenceId::from_value(evidence);
    if (!ok) {
      return false;
    }
    off::WireRequest request =
        make_request(off::WireRequestKind::ResolveAmbiguity, resolution.key.value());
    off::CanonicalWriter writer;
    put_name(writer, resolution.service);
    writer.put_u64(resolution.evidence.value());
    writer.put_bool(resolution.confirm_failure);
    put_name(writer, resolution.resolved_by);
    request.body = writer.buffer();
    return transmit(context, request);
  }
  if (command == "failback") {
    if (!need(3)) {
      return false;
    }
    off::FailbackRequest failback;
    failback.key = off::RequestKey::from_value(args.number("key", 0, ok));
    if (!parse_name(p[0], failback.service) || !parse_name(p[1], failback.preferred_target) ||
        !parse_name(p[2], failback.requester)) {
      return false;
    }
    off::FabricView view;
    if (context.remote) {
      view.topology_generation = off::TopologyGeneration::from_value(args.number("topology", 0, ok));
      view.policy_generation = off::PolicyGeneration::from_value(args.number("policy", 0, ok));
    } else {
      view = context.fabric->view();
    }
    failback.observed_topology_generation = view.topology_generation;
    failback.observed_policy_generation = view.policy_generation;
    if (!ok) {
      return false;
    }
    off::WireRequest request =
        make_request(off::WireRequestKind::RequestFailback, failback.key.value());
    off::CanonicalWriter writer;
    put_name(writer, failback.service);
    put_name(writer, failback.preferred_target);
    writer.put_u64(failback.observed_topology_generation.value());
    writer.put_u64(failback.observed_policy_generation.value());
    put_name(writer, failback.requester);
    request.body = writer.buffer();
    return transmit(context, request);
  }
  if (command == "ack") {
    if (!need(5)) {
      return false;
    }
    off::IntentAck ack;
    u64 attempt = 0;
    u64 fence = 0;
    u64 generation = 0;
    if (!parse_u64(p[0], attempt) || !parse_u64(p[1], fence) || !parse_u64(p[2], generation) ||
        !parse_name(p[4], ack.executor)) {
      return false;
    }
    if (p[3] == "accept") {
      ack.accepted = true;
    } else if (p[3] == "reject") {
      ack.accepted = false;
    } else {
      return false;
    }
    ack.key = off::RequestKey::from_value(args.number("key", 0, ok));
    ack.attempt = off::AttemptId::from_value(attempt);
    ack.fence = off::FenceToken::from_value(fence);
    ack.generation = off::FailoverGeneration::from_value(generation);
    if (!ok) {
      return false;
    }
    off::WireRequest request = make_request(off::WireRequestKind::AcknowledgeIntent, ack.key.value());
    off::CanonicalWriter writer;
    writer.put_u64(ack.attempt.value());
    writer.put_u64(ack.fence.value());
    writer.put_u64(ack.generation.value());
    writer.put_bool(ack.accepted);
    put_name(writer, ack.executor);
    request.body = writer.buffer();
    return transmit(context, request);
  }
  if (command == "effect") {
    if (!need(8)) {
      return false;
    }
    off::EffectReport report;
    u64 attempt = 0;
    u64 generation = 0;
    u64 fence = 0;
    u64 identifier = 0;
    off::u64 state_generation = 0;
    u64 host_incarnation = 0;
    u64 device_incarnation = 0;
    if (!parse_name(p[0], report.service) || !parse_u64(p[1], attempt) ||
        !parse_u64(p[2], generation) || !parse_u64(p[3], fence) ||
        !parse_name(p[4], report.target.target) || !off::effect_kind_parse(p[5], report.kind) ||
        !off::effect_result_parse(p[6], report.result) ||
        !parse_name(p[7], report.provenance.source)) {
      return false;
    }
    identifier = args.number("id", 0, ok);
    state_generation = args.number("state-gen", 0, ok);
    host_incarnation = args.number("inc-host", 0, ok);
    device_incarnation = args.number("inc-dev", 0, ok);
    if (!ok) {
      return false;
    }
    report.attempt = off::AttemptId::from_value(attempt);
    report.generation = off::FailoverGeneration::from_value(generation);
    report.fence = off::FenceToken::from_value(fence);
    report.id = off::EffectId::from_value(identifier);
    report.state_generation = off::StateGeneration::from_value(state_generation);
    report.target.incarnation.host = off::Incarnation::from_value(host_incarnation);
    report.target.incarnation.device = off::Incarnation::from_value(device_incarnation);
    if (context.remote) {
      report.provenance.epoch = off::CoordinatorEpoch::from_value(args.number("epoch", 0, ok));
      report.provenance.boot = off::BootId::from_value(args.number("boot", 0, ok));
    } else {
      const off::FabricView current = context.fabric->view();
      report.provenance.epoch = current.epoch;
      report.provenance.boot = current.boot;
    }
    if (!ok) {
      return false;
    }
    off::WireRequest request = make_request(off::WireRequestKind::ReportEffect, 0);
    off::CanonicalWriter writer;
    encode(writer, report);
    request.body = writer.buffer();
    return transmit(context, request);
  }
  if (command == "cancel") {
    if (!need(1)) {
      return false;
    }
    u64 key = 0;
    if (!parse_u64(p[0], key)) {
      return false;
    }
    off::WireRequest request = make_request(off::WireRequestKind::CancelRequest, 0);
    off::CanonicalWriter writer;
    writer.put_u64(key);
    request.body = writer.buffer();
    return transmit(context, request);
  }
  if (command == "fence") {
    if (!need(3)) {
      return false;
    }
    off::FenceCheck check;
    u64 token = 0;
    u64 host_incarnation = 0;
    u64 device_incarnation = 0;
    if (!parse_name(p[0], check.service) || !parse_name(p[1], check.holder.target) ||
        !parse_u64(p[2], token)) {
      return false;
    }
    host_incarnation = args.number("inc-host", 0, ok);
    device_incarnation = args.number("inc-dev", 0, ok);
    if (!ok) {
      return false;
    }
    check.presented = off::FenceToken::from_value(token);
    check.holder.incarnation.host = off::Incarnation::from_value(host_incarnation);
    check.holder.incarnation.device = off::Incarnation::from_value(device_incarnation);
    off::WireRequest request = make_request(off::WireRequestKind::VerifyFence, 0);
    off::CanonicalWriter writer;
    put_name(writer, check.service);
    encode(writer, check.holder);
    writer.put_u64(check.presented.value());
    request.body = writer.buffer();
    return transmit(context, request);
  }
  if (command == "shutdown") {
    return transmit(context, make_request(off::WireRequestKind::Shutdown, 0));
  }
  std::fprintf(stderr, "off: unknown command %s\n", command.c_str());
  return false;
}

}  // namespace

namespace {

std::vector<std::string> tokenize(const std::string& line) {
  std::vector<std::string> out;
  std::string current;
  bool in_token = false;
  for (const char c : line) {
    if (c == ' ' || c == '\t' || c == '\r' || c == '\n') {
      if (in_token) {
        out.push_back(current);
        current.clear();
        in_token = false;
      }
      continue;
    }
    current.push_back(c);
    in_token = true;
  }
  if (in_token) {
    out.push_back(current);
  }
  return out;
}

/// Runs one already-tokenized command line against the opened surface.
int run_tokens(Context& context, const std::vector<std::string>& tokens) {
  if (tokens.empty()) {
    return 0;
  }
  const std::string& command = tokens[0];
  if (command == "help") {
    print_usage();
    return 0;
  }
  const std::vector<std::string> rest(tokens.begin() + 1, tokens.end());
  Args args;
  parse_args(rest, args);
  return run_command(context, command, args) ? 0 : 1;
}

bool open_surface(Context& context) {
  if (context.remote) {
    std::string error;
    std::optional<off::Socket> socket = off::Socket::connect_to(context.host, context.port, error);
    if (!socket.has_value()) {
      return false;
    }
    context.channel.emplace(std::move(*socket), context.max_frame);
    off::Frame hello;
    hello.kind = off::WireMessage::Hello;
    hello.correlation = context.next_correlation++;
    off::CanonicalWriter writer;
    writer.put_u16(off::kProtocolVersion);
    writer.put_text("off");
    hello.payload = writer.buffer();
    if (!context.channel->send_frame(hello)) {
      return false;
    }
    off::Frame welcome;
    off::Reason error_code = off::Reason::Ok;
    if (!context.channel->recv_frame(welcome, error_code) ||
        welcome.kind != off::WireMessage::Welcome) {
      return false;
    }
    return true;
  }
  off::FabricConfig config;
  if (!context.store_directory.empty()) {
    config.store_directory = std::filesystem::path(context.store_directory);
  }
  context.fabric.emplace(config);
  return context.fabric->open() == off::Reason::Ok;
}

int run_script(Context& context, std::FILE* file) {
  int status = 0;
  char buffer[8192];
  while (std::fgets(buffer, static_cast<int>(sizeof(buffer)), file) != nullptr) {
    std::string line(buffer);
    const std::size_t comment = line.find('#');
    if (comment != std::string::npos) {
      line.resize(comment);
    }
    const std::vector<std::string> tokens = tokenize(line);
    if (tokens.empty()) {
      continue;
    }
    std::printf("> %s\n", line.c_str());
    std::fflush(stdout);
    const int code = run_tokens(context, tokens);
    if (code != 0) {
      status = code;
    }
  }
  return status;
}

}  // namespace

int main(int argc, char** argv) {
  Context context;
  std::vector<std::string> tokens;
  for (int index = 1; index < argc; ++index) {
    tokens.emplace_back(argv[index]);
  }

  std::string script_path;
  bool from_stdin = false;
  std::vector<std::string> command_tokens;
  for (std::size_t index = 0; index < tokens.size(); ++index) {
    const std::string& token = tokens[index];
    if (token == "--store" || token == "--connect" || token == "--max-frame-bytes" ||
        token == "--script") {
      if (index + 1 >= tokens.size()) {
        return fail("missing value for " + token);
      }
      const std::string value = tokens[index + 1];
      if (token == "--store") {
        context.store_directory = value;
      } else if (token == "--script") {
        script_path = value;
      } else if (token == "--connect") {
        const std::size_t colon = value.rfind(':');
        u64 port = 0;
        if (colon == std::string::npos || !parse_u64(value.substr(colon + 1), port) ||
            port > 65535) {
          return fail("--connect expects HOST:PORT");
        }
        context.remote = true;
        context.host = value.substr(0, colon);
        context.port = static_cast<u16>(port);
      } else {
        u64 parsed = 0;
        if (!parse_u64(value, parsed) || parsed == 0 || parsed > off::kAbsoluteMaxFramePayload) {
          return fail("--max-frame-bytes expects a bounded size");
        }
        context.max_frame = static_cast<std::size_t>(parsed);
      }
      ++index;
      continue;
    }
    if (token == "--stdin") {
      from_stdin = true;
      continue;
    }
    if (token == "--help" || token == "-h") {
      print_usage();
      return 0;
    }
    command_tokens.assign(tokens.begin() + static_cast<std::ptrdiff_t>(index), tokens.end());
    break;
  }

  if (script_path.empty() && !from_stdin && command_tokens.empty()) {
    print_usage();
    return 2;
  }

  std::optional<off::SocketRuntime> socket_runtime;
  if (context.remote) {
    socket_runtime.emplace();
  }
  if (!open_surface(context)) {
    if (context.fabric.has_value()) {
      return fail("fabric refused to open");
    }
    return fail("could not establish the requested surface");
  }

  int status = 0;
  if (!script_path.empty()) {
    std::FILE* file = std::fopen(script_path.c_str(), "rb");
    if (file == nullptr) {
      return fail("cannot open script " + script_path);
    }
    status = run_script(context, file);
    std::fclose(file);
  } else if (from_stdin) {
    status = run_script(context, stdin);
  } else {
    status = run_tokens(context, command_tokens);
  }

  if (context.fabric.has_value()) {
    (void)context.fabric->close();
  }
  return status;
}
