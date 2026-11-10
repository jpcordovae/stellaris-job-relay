// jrelay-server: command-line entry point for the relay.
#include <csignal>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>

#include "server/server.hpp"

namespace {

jrelay::server::Server* g_server = nullptr;

extern "C" void on_signal(int) {
  if (g_server != nullptr) g_server->stop();
}

std::optional<std::uint64_t> parse_size(std::string_view s) {
  if (s.empty()) return std::nullopt;
  std::uint64_t mult = 1;
  switch (s.back()) {
    case 'K': case 'k': mult = 1ull << 10; s.remove_suffix(1); break;
    case 'M': case 'm': mult = 1ull << 20; s.remove_suffix(1); break;
    case 'G': case 'g': mult = 1ull << 30; s.remove_suffix(1); break;
    default: break;
  }
  if (s.empty()) return std::nullopt;
  std::uint64_t v = 0;
  for (char c : s) {
    if (c < '0' || c > '9') return std::nullopt;
    v = v * 10 + static_cast<std::uint64_t>(c - '0');
  }
  return v * mult;
}

void usage() {
  std::cerr <<
      "Usage: jrelay-server [options]\n"
      "  --bind ENDPOINT          default tcp://0.0.0.0:7700\n"
      "  --memory-limit SIZE      payload memory budget (default 1G); uploads that do not fit are rejected\n"
      "  --max-message SIZE       largest single message (default: same as --memory-limit)\n"
      "  --chunk-size SIZE        chunk size suggested to clients (default 4M)\n"
      "  --curve-secret-file PATH enable CURVE encryption with the server secret key (Z85) in PATH\n"
      "  --dev-auth               accept dev:<run_id> tokens. INSECURE, for local testing only.\n"
      "                           (GitHub OIDC verification is not implemented in this prototype.)\n"
      "SIZE accepts K, M or G suffixes.\n";
}

}  // namespace

int main(int argc, char** argv) {
  jrelay::server::ServerConfig config;
  bool dev_auth = false;
  bool max_message_set = false;

  for (int i = 1; i < argc; ++i) {
    std::string_view arg = argv[i];
    auto value = [&]() -> std::optional<std::string_view> {
      if (i + 1 >= argc) return std::nullopt;
      return std::string_view(argv[++i]);
    };
    auto size_value = [&]() -> std::optional<std::uint64_t> {
      auto v = value();
      return v ? parse_size(*v) : std::nullopt;
    };

    if (arg == "--bind") {
      auto v = value();
      if (!v) return usage(), 2;
      config.bind = std::string(*v);
    } else if (arg == "--memory-limit") {
      auto v = size_value();
      if (!v) return usage(), 2;
      config.memory_limit = *v;
    } else if (arg == "--max-message") {
      auto v = size_value();
      if (!v) return usage(), 2;
      config.max_message_size = *v;
      max_message_set = true;
    } else if (arg == "--chunk-size") {
      auto v = size_value();
      if (!v || *v == 0) return usage(), 2;
      config.chunk_size = *v;
    } else if (arg == "--curve-secret-file") {
      auto v = value();
      if (!v) return usage(), 2;
      std::ifstream f{std::string(*v)};
      std::string key;
      if (!(f >> key) || key.size() != 40) {
        std::cerr << "jrelay-server: " << *v << " must contain a 40-character Z85 secret key\n";
        return 2;
      }
      config.curve_secret_key = key;
    } else if (arg == "--dev-auth") {
      dev_auth = true;
    } else if (arg == "-h" || arg == "--help") {
      usage();
      return 0;
    } else {
      std::cerr << "jrelay-server: unknown option " << arg << "\n";
      usage();
      return 2;
    }
  }
  if (!max_message_set) config.max_message_size = config.memory_limit;

  if (!dev_auth) {
    std::cerr << "jrelay-server: GitHub OIDC verification is not implemented yet in this prototype.\n"
                 "Start with --dev-auth to test locally (INSECURE).\n";
    return 2;
  }
  std::cerr << "jrelay-server: WARNING --dev-auth accepts any dev:<run_id> token. Do not expose this server.\n";

  try {
    zmq::context_t context(1);
    jrelay::server::Server server(context, config, std::make_shared<jrelay::server::DevAuthenticator>());
    g_server = &server;
    std::signal(SIGINT, on_signal);
    std::signal(SIGTERM, on_signal);
    std::cerr << "jrelay-server: listening on " << server.endpoint()
              << (config.curve_secret_key ? " (CURVE)" : " (no encryption)")
              << ", memory limit " << config.memory_limit << " bytes\n";
    server.run();
    g_server = nullptr;
    std::cerr << "jrelay-server: stopped\n";
  } catch (const std::exception& e) {
    std::cerr << "jrelay-server: " << e.what() << "\n";
    return 1;
  }
  return 0;
}
