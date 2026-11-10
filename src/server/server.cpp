#include "server/server.hpp"

#include <algorithm>
#include <iostream>
#include <utility>

namespace jrelay::server {

using proto::Command;
using proto::ErrorCode;
using proto::Metadata;

namespace {

constexpr auto kPollInterval = std::chrono::milliseconds(200);
constexpr auto kHousekeepingInterval = std::chrono::seconds(5);

std::string_view view(const zmq::message_t& m) noexcept {
  return {static_cast<const char*>(m.data()), m.size()};
}

}  // namespace

Server::Server(zmq::context_t& context, ServerConfig config, std::shared_ptr<const Authenticator> auth)
    : config_(std::move(config)),
      auth_(std::move(auth)),
      budget_(config_.memory_limit),
      store_(budget_, config_.max_message_size),
      socket_(context, zmq::socket_type::router) {
  socket_.set(zmq::sockopt::router_mandatory, 1);  // report unreachable peers instead of dropping
  socket_.set(zmq::sockopt::linger, 0);
  socket_.set(zmq::sockopt::sndhwm, 0);  // credit already bounds what is queued per client
  socket_.set(zmq::sockopt::rcvhwm, 0);
  if (config_.curve_secret_key) {
    socket_.set(zmq::sockopt::curve_server, 1);
    socket_.set(zmq::sockopt::curve_secretkey, *config_.curve_secret_key);
  }
  socket_.bind(config_.bind);
  endpoint_ = socket_.get(zmq::sockopt::last_endpoint);
}

ServerStats Server::stats() const {
  return {budget_.used(), store_.run_count(), store_.message_count(), sessions_.size()};
}

void Server::run() {
  last_housekeeping_ = Clock::now();
  while (!stop_.load(std::memory_order_relaxed)) {
    zmq::pollitem_t items[] = {{socket_.handle(), 0, ZMQ_POLLIN, 0}};
    try {
      zmq::poll(items, 1, kPollInterval);
    } catch (const zmq::error_t& e) {
      if (e.num() == EINTR) continue;  // a signal (e.g. SIGINT to stop): re-check stop_
      throw;
    }
    if ((items[0].revents & ZMQ_POLLIN) != 0) drain_incoming();

    const auto now = Clock::now();
    if (now - last_housekeeping_ >= kHousekeepingInterval) {
      housekeeping(now);
      last_housekeeping_ = now;
    }
  }
}

void Server::drain_incoming() {
  while (true) {
    std::vector<zmq::message_t> parts;
    auto n = zmq::recv_multipart(socket_, std::back_inserter(parts), zmq::recv_flags::dontwait);
    if (!n) return;  // nothing more for now
    if (parts.size() < 3) continue;  // not JRP: identity + header + metadata at least
    const std::string peer(view(parts[0]));
    handle(peer, parts);
    drop_dead_peers();
  }
}

void Server::drop_dead_peers() {
  for (const auto& peer : dead_peers_) {
    if (auto it = sessions_.find(peer); it != sessions_.end()) {
      store_.abort_upload(std::move(it->second.upload));
      sessions_.erase(it);
    }
  }
  dead_peers_.clear();
}

void Server::handle(const std::string& peer, std::vector<zmq::message_t>& parts) {
  auto cmd = proto::decode_header(view(parts[1]));
  if (!cmd) {
    send_error(peer, ErrorCode::BadRequest, std::string(proto::to_string(cmd.error())));
    return;
  }
  auto meta = proto::decode_metadata(view(parts[2]));
  if (!meta) {
    send_error(peer, ErrorCode::BadRequest, std::string(proto::to_string(meta.error())));
    return;
  }

  Session& s = sessions_[peer];
  s.last_activity = Clock::now();

  if (*cmd != Command::Hello && !s.authenticated) {
    send_error(peer, ErrorCode::Unauthorized, "HELLO first");
    return;
  }

  switch (*cmd) {
    case Command::Hello: on_hello(peer, s, *meta); break;
    case Command::Put: on_put(peer, s, *meta); break;
    case Command::Data:
      if (parts.size() != 4) {
        send_error(peer, ErrorCode::BadRequest, "DATA needs exactly one data frame");
        return;
      }
      on_data(peer, s, *meta, std::move(parts[3]));
      break;
    case Command::Commit: on_commit(peer, s, *meta); break;
    case Command::Get: on_get(peer, s, *meta); break;
    case Command::Credit: on_credit(peer, s, *meta); break;
    case Command::Release: on_release(peer, s); break;
    default:
      send_error(peer, ErrorCode::BadRequest, "unexpected command");
      break;
  }
}

void Server::on_hello(const std::string& peer, Session& s, const Metadata& m) {
  if (!m.token) {
    send_error(peer, ErrorCode::Unauthorized, "missing token");
    return;
  }
  if (!m.job_id || !is_valid_job_or_step_id(*m.job_id)) {
    send_error(peer, ErrorCode::BadRequest, "missing or invalid job_id");
    return;
  }
  auto id = auth_->authenticate(*m.token);
  if (!id) {
    send_error(peer, ErrorCode::Unauthorized, id.error());
    return;
  }
  // A new HELLO resets the session: drop anything left over from before.
  store_.abort_upload(std::move(s.upload));
  s.download.reset();
  s.authenticated = true;
  s.identity = std::move(*id);
  s.job_id = *m.job_id;

  Metadata reply;
  reply.chunk_size = config_.chunk_size;
  reply.total_size = config_.max_message_size;
  send(peer, Command::Welcome, reply);
}

void Server::on_put(const std::string& peer, Session& s, const Metadata& m) {
  if (s.upload) {
    send_error(peer, ErrorCode::BadRequest, "an upload is already in progress on this connection");
    return;
  }
  // A job only writes its own messages; the run comes from the token.
  if (m.job_id && *m.job_id != s.job_id) {
    send_error(peer, ErrorCode::Unauthorized, "a job can only write its own messages");
    return;
  }
  if (!m.step_id || !is_valid_job_or_step_id(*m.step_id)) {
    send_error(peer, ErrorCode::BadRequest, "missing or invalid step_id");
    return;
  }
  const std::string name = m.name.value_or(std::string(kDefaultName));
  if (!is_valid_name(name)) {
    send_error(peer, ErrorCode::BadRequest, "invalid name");
    return;
  }

  Key key{s.job_id, *m.step_id, name};
  auto upload = store_.begin_upload(s.identity.run_id, key, m.total_size,
                                    Producer{s.identity.check_run_id, s.identity.run_attempt},
                                    Clock::now());
  if (!upload) {
    std::string text(proto::to_string(upload.error()));
    if (upload.error() == ErrorCode::NoMemory) {
      text += ": " + std::to_string(budget_.available()) + " bytes available";
    }
    send_error(peer, upload.error(), std::move(text));
    return;
  }
  s.upload = std::move(*upload);

  Metadata reply;
  reply.credit = config_.chunk_size * config_.window_chunks;
  reply.chunk_size = config_.chunk_size;
  send(peer, Command::Ok, reply);
}

void Server::on_data(const std::string& peer, Session& s, const Metadata& m, zmq::message_t&& data) {
  if (!s.upload) return;  // late data after an error: the client already got ERROR
  if (!m.offset) {
    store_.abort_upload(std::move(s.upload));
    send_error(peer, ErrorCode::BadRequest, "DATA without offset");
    return;
  }
  const std::uint64_t size = data.size();
  if (auto ok = s.upload->append(*m.offset, std::move(data)); !ok) {
    store_.abort_upload(std::move(s.upload));
    std::string text(proto::to_string(ok.error()));
    if (ok.error() == ErrorCode::NoMemory) {
      text += ": " + std::to_string(budget_.available()) + " bytes available";
    }
    send_error(peer, ok.error(), std::move(text));
    return;
  }
  Metadata credit;
  credit.credit = size;
  send(peer, Command::Credit, credit);
}

void Server::on_commit(const std::string& peer, Session& s, const Metadata& m) {
  if (!s.upload) {
    send_error(peer, ErrorCode::BadRequest, "COMMIT without upload");
    return;
  }
  if (!m.digest) {
    store_.abort_upload(std::move(s.upload));
    send_error(peer, ErrorCode::BadRequest, "COMMIT without digest");
    return;
  }
  auto blob = store_.commit(std::move(s.upload), *m.digest);
  if (!blob) {
    send_error(peer, blob.error(), std::string(proto::to_string(blob.error())));
    return;
  }
  Metadata reply;
  reply.total_size = (*blob)->size();
  reply.digest = (*blob)->digest();
  send(peer, Command::Ok, reply);
}

void Server::on_get(const std::string& peer, Session& s, const Metadata& m) {
  if (s.download) {
    send_error(peer, ErrorCode::BadRequest, "a download is already in progress on this connection");
    return;
  }
  const std::string job = m.job_id.value_or(s.job_id);  // same job when omitted
  if (!is_valid_job_or_step_id(job) || !m.step_id || !is_valid_job_or_step_id(*m.step_id)) {
    send_error(peer, ErrorCode::BadRequest, "missing or invalid job_id/step_id");
    return;
  }
  const std::string name = m.name.value_or(std::string(kDefaultName));
  if (!is_valid_name(name)) {
    send_error(peer, ErrorCode::BadRequest, "invalid name");
    return;
  }
  auto blob = store_.find(s.identity.run_id, Key{job, *m.step_id, name});
  if (!blob) {
    send_error(peer, blob.error(), std::string(proto::to_string(blob.error())));
    return;
  }
  auto start = (*blob)->chunk_at(m.offset.value_or(0));
  if (!start) {
    send_error(peer, ErrorCode::BadRequest, "offset is not at a chunk boundary");
    return;
  }
  s.download = Download{std::move(*blob), *start,
                        m.credit.value_or(config_.chunk_size * config_.window_chunks)};
  pump(peer, s);
}

void Server::on_credit(const std::string& peer, Session& s, const Metadata& m) {
  if (!s.download || !m.credit) return;
  s.download->credit += *m.credit;
  pump(peer, s);
}

void Server::on_release(const std::string& peer, Session& s) {
  store_.abort_upload(std::move(s.upload));
  s.download.reset();
  const std::size_t freed = store_.release_run(s.identity.run_id, Clock::now());
  Metadata reply;
  reply.total_size = freed;  // number of messages released
  send(peer, Command::Ok, reply);
}

void Server::pump(const std::string& peer, Session& s) {
  if (!s.download) return;
  Download& d = *s.download;
  const Blob& blob = *d.blob;

  // Credit is in bytes (FILEMQ style). A chunk is sent while any credit is left;
  // with a window of several chunks the producer of credit never stalls on that.
  while (d.next_chunk < blob.chunk_count() && d.credit > 0) {
    Metadata meta;
    meta.offset = blob.chunk_offset(d.next_chunk);
    meta.fin = d.next_chunk + 1 == blob.chunk_count();
    zmq::message_t chunk = blob.share_chunk(d.next_chunk);
    const std::uint64_t size = chunk.size();
    if (!send(peer, Command::Data, meta, &chunk)) {
      s.download.reset();  // peer gone
      return;
    }
    d.credit = d.credit > size ? d.credit - size : 0;
    ++d.next_chunk;
  }
  // END goes out as soon as the last chunk is sent; it carries no payload, so
  // it does not wait for credit.
  if (d.next_chunk >= blob.chunk_count()) {
    Metadata end;
    end.total_size = blob.size();
    end.digest = blob.digest();
    send(peer, Command::End, end);
    s.download.reset();
  }
}

void Server::housekeeping(Clock::time_point now) {
  for (auto it = sessions_.begin(); it != sessions_.end();) {
    if (now - it->second.last_activity > config_.session_idle_timeout) {
      store_.abort_upload(std::move(it->second.upload));
      it = sessions_.erase(it);
    } else {
      ++it;
    }
  }
  if (const auto expired = store_.sweep(now, config_.max_run_age); expired > 0) {
    std::clog << "jrelay-server: released " << expired << " run(s) past the maximum age\n";
  }
}

bool Server::send(const std::string& peer, Command c, const Metadata& m, zmq::message_t* data) {
  const auto header = proto::encode_header(c);
  const std::string meta = proto::encode(m);
  try {
    socket_.send(zmq::buffer(peer), zmq::send_flags::sndmore);
    socket_.send(zmq::buffer(header), zmq::send_flags::sndmore);
    if (data != nullptr) {
      socket_.send(zmq::buffer(meta), zmq::send_flags::sndmore);
      socket_.send(*data, zmq::send_flags::none);
    } else {
      socket_.send(zmq::buffer(meta), zmq::send_flags::none);
    }
    return true;
  } catch (const zmq::error_t& e) {
    if (e.num() == EHOSTUNREACH) {
      // Do not erase the session here: callers may still hold a reference to it.
      dead_peers_.push_back(peer);
      return false;
    }
    throw;
  }
}

void Server::send_error(const std::string& peer, ErrorCode code, std::string text) {
  Metadata m;
  m.error_code = code;
  m.error_text = std::move(text);
  send(peer, Command::Error, m);
}

}  // namespace jrelay::server
