// jrelay: command-line client used from workflow steps.
//
//   jrelay put  [--step ID] [--name NAME] [--file PATH|-]
//   jrelay get  [--job ID] --step ID [--name NAME] [--out PATH|-]
//   jrelay release
//   jrelay keygen
//
// Environment:
//   JRELAY_ENDPOINT          relay address, e.g. tcp://relay.local:7700   (required)
//   JRELAY_TOKEN             job token (prototype: dev:<run_id>[:<attempt>]) (required)
//   JRELAY_SERVER_PUBLIC_KEY Z85 public key of the relay; enables CURVE
//   JRELAY_TIMEOUT_MS        max silence from the relay (default 60000)
//   GITHUB_JOB               this job's id (set by GitHub)
//   GITHUB_ACTION            this step's id when the step has `id:` (set by GitHub)
#include <array>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>

#include "client/client.hpp"
#include "common/digest.hpp"
#include "common/ids.hpp"

namespace {

using jrelay::client::ClientError;
using jrelay::proto::ErrorCode;

enum Exit : int {
  kOk = 0,
  kError = 1,
  kUsage = 2,
  kNotFound = 3,
  kRunFinished = 4,
  kNoMemory = 5,
};

std::optional<std::string> env(const char* name) {
  const char* v = std::getenv(name);
  if (v == nullptr || *v == '\0') return std::nullopt;
  return std::string(v);
}

int usage() {
  std::cerr << "Usage:\n"
               "  jrelay put  [--step ID] [--name NAME] [--file PATH|-]\n"
               "  jrelay get  [--job ID] --step ID [--name NAME] [--out PATH|-]\n"
               "  jrelay release\n"
               "  jrelay keygen\n"
               "Environment: JRELAY_ENDPOINT, JRELAY_TOKEN, JRELAY_SERVER_PUBLIC_KEY, GITHUB_JOB, GITHUB_ACTION\n";
  return kUsage;
}

int fail(const ClientError& e) {
  std::cerr << "jrelay: error: " << e.text << "\n";
  switch (e.code) {
    case ErrorCode::NotFound: return kNotFound;
    case ErrorCode::RunFinished:
      std::cerr << "jrelay: the run already finished and its data was released.\n"
                   "jrelay: if you used \"Re-run failed jobs\", use \"Re-run all jobs\" instead.\n";
      return kRunFinished;
    case ErrorCode::NoMemory: return kNoMemory;
    default: return kError;
  }
}

double mib_per_s(std::uint64_t bytes, std::chrono::steady_clock::duration d) {
  const double secs = std::chrono::duration<double>(d).count();
  return secs > 0 ? (static_cast<double>(bytes) / (1 << 20)) / secs : 0.0;
}

// GitHub sets GITHUB_ACTION to the step's `id:`, or to "__run", "__run_2"...
// for steps without one. Those generated names are not usable as message ids.
std::optional<std::string> step_from_env() {
  auto a = env("GITHUB_ACTION");
  if (!a || a->starts_with("__") || !jrelay::is_valid_job_or_step_id(*a)) return std::nullopt;
  return a;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 2) return usage();
  const std::string_view command = argv[1];

  if (command == "keygen") {
    std::array<char, 41> pub{};
    std::array<char, 41> sec{};
    if (zmq_curve_keypair(pub.data(), sec.data()) != 0) {
      std::cerr << "jrelay: this libzmq has no CURVE support\n";
      return kError;
    }
    std::cout << "public " << pub.data() << "\nsecret " << sec.data() << "\n";
    return kOk;
  }

  std::optional<std::string> job, step, name, file, out;
  for (int i = 2; i < argc; ++i) {
    const std::string_view arg = argv[i];
    if (i + 1 >= argc) return usage();
    const std::string value = argv[++i];
    if (arg == "--job") job = value;
    else if (arg == "--step") step = value;
    else if (arg == "--name") name = value;
    else if (arg == "--file") file = value;
    else if (arg == "--out") out = value;
    else return usage();
  }

  const auto endpoint = env("JRELAY_ENDPOINT");
  const auto token = env("JRELAY_TOKEN");
  const auto my_job = env("GITHUB_JOB");
  if (!endpoint || !token || !my_job) {
    std::cerr << "jrelay: JRELAY_ENDPOINT, JRELAY_TOKEN and GITHUB_JOB must be set\n";
    return kUsage;
  }
  if (!jrelay::is_valid_job_or_step_id(*my_job)) {
    std::cerr << "jrelay: invalid GITHUB_JOB\n";
    return kUsage;
  }
  const std::string msg_name = name.value_or(std::string(jrelay::kDefaultName));

  jrelay::client::ClientOptions options;
  options.endpoint = *endpoint;
  options.token = *token;
  options.job_id = *my_job;
  options.server_public_key = env("JRELAY_SERVER_PUBLIC_KEY");
  if (auto t = env("JRELAY_TIMEOUT_MS")) {
    std::uint64_t ms = 0;
    for (char c : *t) {
      if (c < '0' || c > '9' || ms > 86'400'000) {
        std::cerr << "jrelay: JRELAY_TIMEOUT_MS must be a number of milliseconds (max one day)\n";
        return kUsage;
      }
      ms = ms * 10 + static_cast<std::uint64_t>(c - '0');
    }
    options.timeout = std::chrono::milliseconds(ms);
  }

  try {
    zmq::context_t context(1);
    jrelay::client::Client client(context, options);
    if (auto c = client.connect(); !c) return fail(c.error());
    const auto start = std::chrono::steady_clock::now();

    if (command == "put") {
      if (!step) step = step_from_env();
      if (!step) {
        std::cerr << "jrelay: give --step, or run inside a step that has an `id:`\n";
        return kUsage;
      }
      std::optional<std::uint64_t> size;
      std::ifstream fin;
      std::istream* in = &std::cin;
      if (file && *file != "-") {
        fin.open(*file, std::ios::binary);
        if (!fin) {
          std::cerr << "jrelay: cannot open " << *file << "\n";
          return kError;
        }
        std::error_code ec;
        if (auto s = std::filesystem::file_size(*file, ec); !ec) size = s;
        in = &fin;
      }
      auto r = client.put(*step, msg_name, *in, size);
      if (!r) return fail(r.error());
      std::cerr << "jrelay: put " << *my_job << "/" << *step << "/" << msg_name << " " << r->size
                << " bytes, " << mib_per_s(r->size, std::chrono::steady_clock::now() - start)
                << " MiB/s, blake3 " << jrelay::to_hex(r->digest) << "\n";
      return kOk;
    }

    if (command == "get") {
      if (!step) {
        std::cerr << "jrelay: get needs --step\n";
        return kUsage;
      }
      std::ofstream fout;
      std::ostream* os = &std::cout;
      if (out && *out != "-") {
        fout.open(*out, std::ios::binary | std::ios::trunc);
        if (!fout) {
          std::cerr << "jrelay: cannot create " << *out << "\n";
          return kError;
        }
        os = &fout;
      }
      auto r = client.get(job, *step, msg_name, *os);
      if (!r) return fail(r.error());
      std::cerr << "jrelay: got " << job.value_or(*my_job) << "/" << *step << "/" << msg_name << " "
                << r->size << " bytes, " << mib_per_s(r->size, std::chrono::steady_clock::now() - start)
                << " MiB/s, blake3 " << jrelay::to_hex(r->digest) << " verified\n";
      return kOk;
    }

    if (command == "release") {
      auto r = client.release();
      if (!r) return fail(r.error());
      std::cerr << "jrelay: released " << *r << " message(s) of this run\n";
      return kOk;
    }
  } catch (const std::exception& e) {
    std::cerr << "jrelay: " << e.what() << "\n";
    return kError;
  }
  return usage();
}
