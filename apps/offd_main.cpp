// offd - the Offload Failover Fabric service daemon.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// The daemon owns one Fabric instance and exposes it over a framed, bounded,
// integrity-checked protocol on a real socket. Connections are served by a
// fixed worker pool fed from a bounded accept queue, so both concurrency and
// memory are bounded before any work is admitted.

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstdio>
#include <deque>
#include <filesystem>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "off/off.hpp"

namespace {

struct Options {
  std::string host{"127.0.0.1"};
  off::u16 port{0};
  std::string store_directory{};
  std::size_t max_connections{32};
  std::size_t workers{4};
  std::size_t max_frame{off::kDefaultMaxFramePayload};
  bool allow_remote_shutdown{false};
  bool announce_ready{false};
};

void print_usage() {
  std::fputs(
      "usage: offd [--listen HOST:PORT] [--store DIR] [--workers N]\n"
      "            [--max-connections N] [--max-frame-bytes N]\n"
      "            [--allow-remote-shutdown] [--ready]\n",
      stderr);
}

bool next_value(int argc, char** argv, int& index, std::string& value) {
  if (index + 1 >= argc) {
    return false;
  }
  value = argv[++index];
  return true;
}

bool parse_options(int argc, char** argv, Options& options) {
  for (int index = 1; index < argc; ++index) {
    const std::string argument = argv[index];
    std::string value;
    if (argument == "--help" || argument == "-h") {
      print_usage();
      return false;
    }
    if (argument == "--listen") {
      if (!next_value(argc, argv, index, value)) {
        return false;
      }
      const std::size_t colon = value.rfind(':');
      if (colon == std::string::npos) {
        return false;
      }
      off::u64 parsed = 0;
      if (!off::parse_decimal(value.substr(colon + 1), parsed) || parsed > 65535) {
        return false;
      }
      options.host = value.substr(0, colon);
      options.port = static_cast<off::u16>(parsed);
      continue;
    }
    if (argument == "--store") {
      if (!next_value(argc, argv, index, value)) {
        return false;
      }
      options.store_directory = value;
      continue;
    }
    if (argument == "--workers") {
      off::u64 parsed = 0;
      if (!next_value(argc, argv, index, value) || !off::parse_decimal(value, parsed) ||
          parsed == 0 || parsed > 64) {
        return false;
      }
      options.workers = static_cast<std::size_t>(parsed);
      continue;
    }
    if (argument == "--max-connections") {
      off::u64 parsed = 0;
      if (!next_value(argc, argv, index, value) || !off::parse_decimal(value, parsed) ||
          parsed == 0 || parsed > 4096) {
        return false;
      }
      options.max_connections = static_cast<std::size_t>(parsed);
      continue;
    }
    if (argument == "--max-frame-bytes") {
      off::u64 parsed = 0;
      if (!next_value(argc, argv, index, value) || !off::parse_decimal(value, parsed) ||
          parsed == 0 || parsed > off::kAbsoluteMaxFramePayload) {
        return false;
      }
      options.max_frame = static_cast<std::size_t>(parsed);
      continue;
    }
    if (argument == "--allow-remote-shutdown") {
      options.allow_remote_shutdown = true;
      continue;
    }
    if (argument == "--ready") {
      options.announce_ready = true;
      continue;
    }
    std::fprintf(stderr, "offd: unrecognized argument %s\n", argument.c_str());
    return false;
  }
  return true;
}

/// Bounded queue of accepted connections handed to the worker pool.
class AcceptQueue {
 public:
  explicit AcceptQueue(std::size_t capacity) : capacity_(capacity == 0 ? 1 : capacity) {}

  bool push(off::Socket socket) {
    std::lock_guard<std::mutex> guard(mutex_);
    if (closed_ || queue_.size() >= capacity_) {
      return false;
    }
    queue_.push_back(std::move(socket));
    ready_.notify_one();
    return true;
  }

  bool pop(off::Socket& out) {
    std::unique_lock<std::mutex> lock(mutex_);
    ready_.wait(lock, [this] { return closed_ || !queue_.empty(); });
    if (queue_.empty()) {
      return false;
    }
    out = std::move(queue_.front());
    queue_.pop_front();
    return true;
  }

  void close() {
    std::lock_guard<std::mutex> guard(mutex_);
    closed_ = true;
    ready_.notify_all();
  }

 private:
  std::mutex mutex_{};
  std::condition_variable ready_{};
  std::deque<off::Socket> queue_{};
  std::size_t capacity_{1};
  bool closed_{false};
};

struct Server {
  explicit Server(Options configuration) : options(std::move(configuration)), fabric(make_config()) {}

  [[nodiscard]] off::FabricConfig make_config() const {
    off::FabricConfig config;
    if (!options.store_directory.empty()) {
      config.store_directory = std::filesystem::path(options.store_directory);
    }
    return config;
  }

  /// Registers a connection so shutdown can interrupt it. A worker blocked in a
  /// receive would otherwise keep the process alive after the listener closed.
  void register_connection(off::Socket* socket) {
    std::lock_guard<std::mutex> guard(connections_mutex);
    connections.push_back(socket);
  }

  void unregister_connection(off::Socket* socket) {
    std::lock_guard<std::mutex> guard(connections_mutex);
    connections.erase(std::remove(connections.begin(), connections.end(), socket),
                      connections.end());
  }

  /// Closes the listening socket and interrupts every in-flight connection, so
  /// both the acceptor and the workers observe the shutdown.
  void begin_shutdown() {
    stopping.store(true);
    listener.interrupt();
    std::lock_guard<std::mutex> guard(connections_mutex);
    for (off::Socket* socket : connections) {
      socket->interrupt();
    }
  }

  Options options;
  off::Fabric fabric;
  off::Socket listener{};
  std::atomic<bool> stopping{false};
  std::atomic<std::size_t> live_connections{0};
  std::atomic<std::size_t> accepted_connections{0};
  std::atomic<std::size_t> rejected_connections{0};
  std::mutex connections_mutex{};
  std::vector<off::Socket*> connections{};
};

bool send_error(off::FrameChannel& channel, off::u64 correlation, off::Reason code) {
  off::CanonicalWriter writer;
  writer.put_u16(static_cast<off::u16>(code));
  off::Frame reply;
  reply.kind = off::WireMessage::Error;
  reply.correlation = correlation;
  reply.payload = writer.buffer();
  return channel.send_frame(reply);
}

void serve_connection(Server& server, off::Socket socket) {
  off::FrameChannel channel(std::move(socket), server.options.max_frame);
  server.register_connection(&channel.socket());
  // The guard guarantees the registry never keeps a dangling pointer, whatever
  // path the connection takes out of this function.
  struct Unregister {
    Server& server;
    off::Socket* socket;
    ~Unregister() {
      server.unregister_connection(socket);
      server.live_connections.fetch_sub(1);
    }
  } unregister{server, &channel.socket()};

  off::Reason error = off::Reason::Ok;
  off::Frame frame;
  if (!channel.recv_frame(frame, error)) {
    return;
  }
  if (frame.kind != off::WireMessage::Hello) {
    (void)send_error(channel, frame.correlation, off::Reason::ProtocolViolation);
    return;
  }
  {
    off::CanonicalReader reader(frame.payload.data(), frame.payload.size());
    off::u16 version = 0;
    std::string client;
    if (!reader.get_u16(version) || !reader.get_text(client)) {
      return;
    }
    if (version != off::kProtocolVersion) {
      (void)send_error(channel, frame.correlation, off::Reason::ProtocolVersionMismatch);
      return;
    }
  }
  {
    off::Frame welcome;
    welcome.kind = off::WireMessage::Welcome;
    welcome.correlation = frame.correlation;
    off::CanonicalWriter writer;
    writer.put_u16(off::kProtocolVersion);
    writer.put_u64(server.fabric.view().epoch.value());
    writer.put_u64(server.fabric.view().boot.value());
    welcome.payload = writer.buffer();
    if (!channel.send_frame(welcome)) {
      return;
    }
  }

  for (;;) {
    if (!channel.recv_frame(frame, error)) {
      break;
    }
    if (frame.kind == off::WireMessage::Cancel) {
      off::CanonicalReader reader(frame.payload.data(), frame.payload.size());
      off::u64 raw_key = 0;
      if (reader.get_u64(raw_key)) {
        (void)server.fabric.cancel(off::RequestKey::from_value(raw_key), nullptr);
      }
      continue;
    }
    if (frame.kind != off::WireMessage::Request) {
      break;
    }
    off::WireRequest request;
    const off::Reason decoded =
        off::decode_request(frame.payload.data(), frame.payload.size(), request);
    off::WireResponse response;
    if (decoded != off::Reason::Ok) {
      response.status = decoded;
      response.accepted = false;
      response.body = "{\"accepted\":false,\"status\":{\"code\":\"" +
                      std::string(off::reason_code_text(decoded)) + "\"}}";
    } else {
      const off::Reason executed = off::execute_request(server.fabric, request, response);
      if (response.body.empty()) {
        response.status = executed;
        response.body = "{\"accepted\":false,\"status\":{\"code\":\"" +
                        std::string(off::reason_code_text(executed)) + "\"}}";
      }
    }
    std::vector<off::u8> payload;
    if (off::encode_response(response, payload, server.options.max_frame) != off::Reason::Ok) {
      break;
    }
    off::Frame reply;
    reply.kind = off::WireMessage::Response;
    reply.correlation = frame.correlation;
    reply.payload = std::move(payload);
    if (!channel.send_frame(reply)) {
      break;
    }
    if (request.kind == off::WireRequestKind::Shutdown && server.options.allow_remote_shutdown) {
      server.begin_shutdown();
      break;
    }
  }
}

}  // namespace

int main(int argc, char** argv) {
  Options options;
  if (!parse_options(argc, argv, options)) {
    print_usage();
    return 2;
  }

  off::SocketRuntime socket_runtime;
  Server server(options);

  const off::Reason opened = server.fabric.open();
  if (opened != off::Reason::Ok) {
    std::fprintf(stderr, "offd: fabric refused to open: %s\n",
                 std::string(off::reason_code_text(opened)).c_str());
    return 3;
  }
  const off::RestartSummary& restart = server.fabric.restart_summary();
  std::fprintf(stderr, "offd: opened epoch=%llu boot=%llu recovery=%s reason=%s\n",
               static_cast<unsigned long long>(restart.epoch.value()),
               static_cast<unsigned long long>(restart.boot.value()),
               std::string(off::recovery_class_text(restart.store.classification)).c_str(),
               std::string(off::reason_code_text(restart.store.reason)).c_str());

  std::string listen_error;
  off::u16 bound_port = 0;
  std::optional<off::Socket> listener =
      off::Socket::listen_on(options.host, options.port, bound_port, listen_error);
  if (!listener.has_value()) {
    std::fprintf(stderr, "offd: %s\n", listen_error.c_str());
    return 4;
  }
  server.listener = std::move(*listener);

  if (options.announce_ready) {
    std::printf("READY %s:%u\n", options.host.c_str(), static_cast<unsigned>(bound_port));
    std::fflush(stdout);
  } else {
    std::fprintf(stderr, "offd: listening on %s:%u\n", options.host.c_str(),
                 static_cast<unsigned>(bound_port));
  }

  AcceptQueue queue(options.max_connections);
  std::vector<std::thread> workers;
  workers.reserve(options.workers);
  for (std::size_t index = 0; index < options.workers; ++index) {
    workers.emplace_back([&queue, &server]() {
      for (;;) {
        off::Socket socket;
        if (!queue.pop(socket)) {
          return;
        }
        serve_connection(server, std::move(socket));
      }
    });
  }

  while (!server.stopping.load()) {
    std::string accept_error;
    std::optional<off::Socket> accepted = server.listener.accept(accept_error);
    if (!accepted.has_value()) {
      break;
    }
    server.accepted_connections.fetch_add(1);
    server.live_connections.fetch_add(1);
    if (!queue.push(std::move(*accepted))) {
      server.rejected_connections.fetch_add(1);
      server.live_connections.fetch_sub(1);
    }
    if (server.stopping.load()) {
      break;
    }
  }

  server.listener.interrupt();
  queue.close();
  for (std::thread& worker : workers) {
    worker.join();
  }
  (void)server.fabric.close();
  return 0;
}
