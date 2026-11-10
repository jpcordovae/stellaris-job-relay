#include "common/ids.hpp"

#include <algorithm>

namespace jrelay {

namespace {

bool is_alnum(char c) noexcept {
  return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9');
}

}  // namespace

bool is_valid_job_or_step_id(std::string_view id) noexcept {
  if (id.empty() || id.size() > kMaxIdLength) return false;
  return std::ranges::all_of(id, [](char c) { return is_alnum(c) || c == '-' || c == '_'; });
}

bool is_valid_name(std::string_view name) noexcept {
  if (name.empty() || name.size() > kMaxIdLength) return false;
  if (name == "." || name == "..") return false;
  return std::ranges::all_of(name, [](char c) { return is_alnum(c) || c == '-' || c == '_' || c == '.'; });
}

bool is_valid_run_id(std::string_view run) noexcept {
  if (run.empty() || run.size() > 20) return false;
  return std::ranges::all_of(run, [](char c) { return c >= '0' && c <= '9'; });
}

std::string to_string(const Key& k) { return k.job + "/" + k.step + "/" + k.name; }

}  // namespace jrelay
