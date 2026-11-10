#include "client/client.hpp"

#include <array>
#include <cstring>
#include <utility>
#include <vector>

#include "common/digest.hpp"

namespace jrelay::client {

using proto::Command;
using proto::ErrorCode;
using proto::Metadata;

namespace {

std::string_view view(const zmq::message_t& m) noexcept {
  return {static_cast<const char*>(m.data()), m.size()};
}

ClientError error_from(const Metadata& m) {
  return {m.error_code.value_or(ErrorCode::Internal), m.error_text.value_or("relay error")};
}

ClientError unexpected_reply(Command got) {
  return {ErrorCode::BadRequest, "unexpected reply " + std::string(proto::to_string(got))};
}

}  // namespace

Client::Client(zmq::context_t& context, ClientOptions options)
    : options_(std::move(options)), socket_(context, zmq::socket_type::dealer) {
  socket_.set(zmq::sockopt::linger, 0);
  if (options_.server_public_key) {
    std::array<char, 41> pub{};
    std::array<char, 41> sec{};
    if (zmq_curve_keypair(pub.data(), sec.data()) != 0) throw zmq::error_t();
    socket_.set(zmq::sockopt::curve_serverkey, *options_.server_public_key);
    socket_.set(zmq::sockopt::curve_publickey, std::string(pub.data(), 40));
    socket_.set(zmq::sockopt::curve_secretkey, std::string(sec.data(), 40));
  }
  socket_.connect(options_.endpoint);
}

void Client::send(Command c, const Metadata& m, zmq::message_t* data) {
  const auto header = proto::encode_header(c);
  const std::string meta = proto::encode(m);
  socket_.send(zmq::buffer(header), zmq::send_flags::sndmore);
  if (data != nullptr) {
    socket_.send(zmq::buffer(meta), zmq::send_flags::sndmore);
    socket_.send(*data, zmq::send_flags::none);
  } else {
    socket_.send(zmq::buffer(meta), zmq::send_flags::none);
  }
}

std::expected<Client::Reply, ClientError> Client::receive(std::chrono::milliseconds timeout) {
  zmq::pollitem_t items[] = {{socket_.handle(), 0, ZMQ_POLLIN, 0}};
  zmq::poll(items, 1, timeout);
  if ((items[0].revents & ZMQ_POLLIN) == 0) {
    return std::unexpected(ClientError{ErrorCode::Timeout, "no answer from the relay"});
  }
  std::vector<zmq::message_t> parts;
  if (!zmq::recv_multipart(socket_, std::back_inserter(parts)) || parts.size() < 2) {
    return std::unexpected(ClientError{ErrorCode::BadRequest, "malformed reply"});
  }
  auto cmd = proto::decode_header(view(parts[0]));
  if (!cmd) return std::unexpected(ClientError{ErrorCode::BadRequest, std::string(proto::to_string(cmd.error()))});
  auto meta = proto::decode_metadata(view(parts[1]));
  if (!meta) return std::unexpected(ClientError{ErrorCode::BadRequest, std::string(proto::to_string(meta.error()))});

  Reply r{*cmd, std::move(*meta), {}};
  if (parts.size() >= 3) r.data = std::move(parts[2]);
  if (r.command == Command::Error) return std::unexpected(error_from(r.meta));
  return r;
}

std::expected<void, ClientError> Client::connect() {
  Metadata hello;
  hello.token = options_.token;
  hello.job_id = options_.job_id;
  send(Command::Hello, hello);
  auto r = receive(options_.timeout);
  if (!r) return std::unexpected(r.error());
  if (r->command != Command::Welcome) return std::unexpected(unexpected_reply(r->command));
  if (r->meta.chunk_size && *r->meta.chunk_size > 0) chunk_size_ = *r->meta.chunk_size;
  connected_ = true;
  return {};
}

std::expected<TransferResult, ClientError> Client::put(const std::string& step, const std::string& name,
                                                       std::istream& in, std::optional<std::uint64_t> size) {
  if (!connected_) return std::unexpected(ClientError{ErrorCode::Unauthorized, "not connected"});

  Metadata put;
  put.step_id = step;
  put.name = name;
  put.total_size = size;
  send(Command::Put, put);
  auto ok = receive(options_.timeout);
  if (!ok) return std::unexpected(ok.error());
  if (ok->command != Command::Ok) return std::unexpected(unexpected_reply(ok->command));
  std::uint64_t credit = ok->meta.credit.value_or(chunk_size_);
  if (ok->meta.chunk_size && *ok->meta.chunk_size > 0) chunk_size_ = *ok->meta.chunk_size;

  // Applies a CREDIT; anything else while uploading is an error.
  auto absorb = [&](const Reply& r) -> std::expected<void, ClientError> {
    if (r.command != Command::Credit) return std::unexpected(unexpected_reply(r.command));
    credit += r.meta.credit.value_or(0);
    return {};
  };

  Hasher hasher;
  std::uint64_t offset = 0;
  const auto chunk = static_cast<std::size_t>(chunk_size_);
  while (true) {
    // Pick up credit (or an early ERROR such as NoMemory) without blocking.
    while (true) {
      auto r = receive(std::chrono::milliseconds(0));
      if (!r) {
        if (r.error().code == ErrorCode::Timeout) break;
        return std::unexpected(r.error());
      }
      if (auto a = absorb(*r); !a) return std::unexpected(a.error());
    }
    if (credit == 0) {
      auto r = receive(options_.timeout);
      if (!r) return std::unexpected(r.error());
      if (auto a = absorb(*r); !a) return std::unexpected(a.error());
      continue;
    }

    // Read straight into the message buffer: no intermediate copy.
    zmq::message_t data(chunk);
    in.read(static_cast<char*>(data.data()), static_cast<std::streamsize>(chunk));
    const auto n = static_cast<std::size_t>(in.gcount());
    if (in.bad()) return std::unexpected(ClientError{ErrorCode::Internal, "read error on input"});
    if (n == 0) break;
    if (n < chunk) data = zmq::message_t(data.data(), n);  // last, shorter chunk: one small copy

    hasher.update(data.data(), n);
    Metadata dm;
    dm.offset = offset;
    send(Command::Data, dm, &data);
    offset += n;
    credit = credit > n ? credit - n : 0;
    if (n < chunk) break;
  }

  Metadata commit;
  commit.digest = hasher.finish();
  commit.total_size = offset;
  send(Command::Commit, commit);
  while (true) {
    auto r = receive(options_.timeout);
    if (!r) return std::unexpected(r.error());
    if (r->command == Command::Credit) continue;  // credit for the last chunks
    if (r->command != Command::Ok) return std::unexpected(unexpected_reply(r->command));
    return TransferResult{offset, *commit.digest};
  }
}

std::expected<TransferResult, ClientError> Client::get(const std::optional<std::string>& job,
                                                       const std::string& step, const std::string& name,
                                                       std::ostream& out) {
  if (!connected_) return std::unexpected(ClientError{ErrorCode::Unauthorized, "not connected"});

  Metadata get;
  get.job_id = job;
  get.step_id = step;
  get.name = name;
  get.credit = chunk_size_ * options_.window_chunks;
  send(Command::Get, get);

  Hasher hasher;
  std::uint64_t received = 0;
  while (true) {
    auto r = receive(options_.timeout);
    if (!r) return std::unexpected(r.error());
    if (r->command == Command::Data) {
      if (r->meta.offset.value_or(~0ull) != received) {
        return std::unexpected(ClientError{ErrorCode::BadRequest, "out-of-order data"});
      }
      const std::size_t n = r->data.size();
      out.write(static_cast<const char*>(r->data.data()), static_cast<std::streamsize>(n));
      if (!out) return std::unexpected(ClientError{ErrorCode::Internal, "write error on output"});
      hasher.update(r->data.data(), n);
      received += n;
      Metadata credit;
      credit.credit = n;  // that much buffer is free again
      send(Command::Credit, credit);
      continue;
    }
    if (r->command != Command::End) return std::unexpected(unexpected_reply(r->command));
    const proto::Digest digest = hasher.finish();
    if (r->meta.total_size.value_or(~0ull) != received || !r->meta.digest || *r->meta.digest != digest) {
      return std::unexpected(ClientError{ErrorCode::IntegrityMismatch, "received data does not match its digest"});
    }
    out.flush();
    return TransferResult{received, digest};
  }
}

std::expected<std::uint64_t, ClientError> Client::release() {
  if (!connected_) return std::unexpected(ClientError{ErrorCode::Unauthorized, "not connected"});
  send(Command::Release, Metadata{});
  auto r = receive(options_.timeout);
  if (!r) return std::unexpected(r.error());
  if (r->command != Command::Ok) return std::unexpected(unexpected_reply(r->command));
  return r->meta.total_size.value_or(0);
}

}  // namespace jrelay::client
