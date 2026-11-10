// JRP/1 client: what a step uses to leave a message on the relay or pick one up.
#pragma once

#include <chrono>
#include <cstdint>
#include <expected>
#include <istream>
#include <optional>
#include <ostream>
#include <string>

#include <zmq.hpp>
#include <zmq_addon.hpp>

#include "protocol/message.hpp"

namespace jrelay::client {

struct ClientOptions {
  std::string endpoint;                          // e.g. tcp://relay.local:7700
  std::string token;                             // OIDC token of the job (dev:<run> in the prototype)
  std::string job_id;                            // this job's id in the workflow file
  std::optional<std::string> server_public_key;  // Z85; enables CURVE when set
  std::chrono::milliseconds timeout{60'000};     // max silence from the server
  std::uint64_t window_chunks = 4;               // download credit window, in chunks
};

struct ClientError {
  proto::ErrorCode code = proto::ErrorCode::Internal;
  std::string text;
};

struct TransferResult {
  std::uint64_t size = 0;
  proto::Digest digest{};
};

class Client {
 public:
  Client(zmq::context_t& context, ClientOptions options);

  // HELLO / WELCOME. Must succeed before any other call.
  [[nodiscard]] std::expected<void, ClientError> connect();

  // Uploads `in` as message (this job, step, name). `size` lets the relay
  // reject an upload that does not fit before any data is sent (D1).
  [[nodiscard]] std::expected<TransferResult, ClientError> put(const std::string& step,
                                                               const std::string& name, std::istream& in,
                                                               std::optional<std::uint64_t> size);

  // Downloads message (job, step, name) into `out` and verifies its BLAKE3
  // digest. `job` defaults to this client's own job.
  [[nodiscard]] std::expected<TransferResult, ClientError> get(const std::optional<std::string>& job,
                                                               const std::string& step,
                                                               const std::string& name, std::ostream& out);

  // Frees every message of this run on the relay. Returns how many were freed.
  [[nodiscard]] std::expected<std::uint64_t, ClientError> release();

  [[nodiscard]] std::uint64_t chunk_size() const noexcept { return chunk_size_; }

 private:
  struct Reply {
    proto::Command command{};
    proto::Metadata meta;
    zmq::message_t data;  // empty unless DATA
  };

  void send(proto::Command c, const proto::Metadata& m, zmq::message_t* data = nullptr);
  [[nodiscard]] std::expected<Reply, ClientError> receive(std::chrono::milliseconds timeout);

  ClientOptions options_;
  zmq::socket_t socket_;
  std::uint64_t chunk_size_ = 4ull << 20;
  bool connected_ = false;
};

}  // namespace jrelay::client
