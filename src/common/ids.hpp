// Validation of the identifiers that name a message: run, job, step and name.
#pragma once

#include <compare>
#include <string>
#include <string_view>

namespace jrelay {

inline constexpr std::size_t kMaxIdLength = 255;
inline constexpr std::string_view kDefaultName = "default";

// GitHub job and step ids are made of letters, digits, '-' and '_'. Names also
// allow '.', so a step can use file-like names such as "app.tar".
bool is_valid_job_or_step_id(std::string_view id) noexcept;
bool is_valid_name(std::string_view name) noexcept;
bool is_valid_run_id(std::string_view run) noexcept;  // decimal digits

// Identifies a message inside one run.
struct Key {
  std::string job;
  std::string step;
  std::string name;

  auto operator<=>(const Key&) const = default;
  bool operator==(const Key&) const = default;
};

std::string to_string(const Key& k);  // "job/step/name"

}  // namespace jrelay
