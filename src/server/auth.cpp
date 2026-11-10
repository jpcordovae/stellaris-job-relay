#include "server/auth.hpp"

#include "common/ids.hpp"

namespace jrelay::server {

std::expected<Identity, std::string> DevAuthenticator::authenticate(std::string_view token) const {
  constexpr std::string_view kPrefix = "dev:";
  if (!token.starts_with(kPrefix)) return std::unexpected("expected a dev:<run_id> token");
  token.remove_prefix(kPrefix.size());

  std::string_view run = token;
  std::string_view attempt = "1";
  if (auto colon = token.find(':'); colon != std::string_view::npos) {
    run = token.substr(0, colon);
    attempt = token.substr(colon + 1);
  }
  if (!is_valid_run_id(run)) return std::unexpected("invalid run id in token");
  if (!is_valid_run_id(attempt)) return std::unexpected("invalid run attempt in token");
  return Identity{std::string(run), std::string(attempt), "dev"};
}

}  // namespace jrelay::server
