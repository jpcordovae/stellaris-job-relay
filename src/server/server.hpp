// JRP/1 relay server: one ROUTER socket and a single-threaded event loop.
// ZeroMQ's own I/O threads move the bytes; this loop only routes and stores.
#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <string>

#include <zmq.hpp>
#include <zmq_addon.hpp>

#include "server/auth.hpp"
#include "server/memory.hpp"
#include "server/store.hpp"

namespace jrelay::server {

struct ServerConfig {
  std::string bind = "tcp://0.0.0.0:7700";
  std::uint64_t memory_limit = 1ull << 30;           // 1 GiB of payload
  std::uint64_t max_message_size = 1ull << 30;       // per message
  std::uint64_t chunk_size = 4ull << 20;             // D4: 4 MiB suggested to clients
  std::uint64_t window_chunks = 4;                   // credit window, in chunks
  std::chrono::seconds session_idle_timeout{600};    // drop silent clients
  std::chrono::seconds max_run_age{35 * 24 * 3600};  // GitHub's limit for a run
  std::optional<std::string> curve_secret_key;       // Z85; enables CURVE when set
};

struct ServerStats {
  std::uint64_t memory_used = 0;
  std::size_t runs = 0;
  std::size_t messages = 0;
  std::size_t sessions = 0;
};

class Server {
 public:
  Server(zmq::context_t& context, ServerConfig config, std::shared_ptr<const Authenticator> auth);

  // Endpoint actually bound (useful with "tcp://127.0.0.1:*").
  [[nodiscard]] const std::string& endpoint() const noexcept { return endpoint_; }

  // Runs the event loop until stop() is called. Call from one thread only.
  void run();

  // Thread-safe and async-signal-safe.
  void stop() noexcept { stop_.store(true, std::memory_order_relaxed); }

  // Snapshot for tests and logs. Only call from the run() thread or after it returns.
  [[nodiscard]] ServerStats stats() const;

 private:
  struct Download {
    std::shared_ptr<const Blob> blob;
    std::size_t next_chunk = 0;
    std::uint64_t credit = 0;
  };

  struct Session {
    bool authenticated = false;
    Identity identity;
    std::string job_id;
    Clock::time_point last_activity{};
    std::unique_ptr<BlobBuilder> upload;
    std::optional<Download> download;
  };

  void drain_incoming();
  void handle(const std::string& peer, std::vector<zmq::message_t>& parts);
  void on_hello(const std::string& peer, Session& s, const proto::Metadata& m);
  void on_put(const std::string& peer, Session& s, const proto::Metadata& m);
  void on_data(const std::string& peer, Session& s, const proto::Metadata& m, zmq::message_t&& data);
  void on_commit(const std::string& peer, Session& s, const proto::Metadata& m);
  void on_get(const std::string& peer, Session& s, const proto::Metadata& m);
  void on_credit(const std::string& peer, Session& s, const proto::Metadata& m);
  void on_release(const std::string& peer, Session& s);

  void pump(const std::string& peer, Session& s);
  void housekeeping(Clock::time_point now);
  void drop_dead_peers();

  bool send(const std::string& peer, proto::Command c, const proto::Metadata& m,
            zmq::message_t* data = nullptr);
  void send_error(const std::string& peer, proto::ErrorCode code, std::string text);

  ServerConfig config_;
  std::shared_ptr<const Authenticator> auth_;
  MemoryBudget budget_;  // declared before store_: blobs return memory to it on destruction
  Store store_;
  std::map<std::string, Session> sessions_;  // keyed by ZeroMQ routing id
  std::vector<std::string> dead_peers_;      // unreachable peers, removed after handling
  zmq::socket_t socket_;
  std::string endpoint_;
  std::atomic<bool> stop_{false};
  Clock::time_point last_housekeeping_{};
};

}  // namespace jrelay::server
