// Authentication of jobs (decision D6: GitHub OIDC token of each job).
//
// PROTOTYPE STATUS: verification of real GitHub OIDC tokens (JWT RS256 signed
// with GitHub's published keys) is not implemented yet. The prototype only
// ships DevAuthenticator, which accepts "dev:<run_id>" tokens and must be
// enabled explicitly with --dev-auth. It provides NO security and exists only
// to test the protocol and the memory model.
#pragma once

#include <expected>
#include <memory>
#include <string>
#include <string_view>

namespace jrelay::server {

// What the server trusts about a connected job. The run is taken from here,
// never from the client's own claims.
struct Identity {
  std::string run_id;
  std::string run_attempt;
  std::string check_run_id;
};

class Authenticator {
 public:
  virtual ~Authenticator() = default;
  [[nodiscard]] virtual std::expected<Identity, std::string> authenticate(std::string_view token) const = 0;
};

// Accepts "dev:<run_id>" or "dev:<run_id>:<attempt>". For local tests only.
class DevAuthenticator final : public Authenticator {
 public:
  [[nodiscard]] std::expected<Identity, std::string> authenticate(std::string_view token) const override;
};

}  // namespace jrelay::server
