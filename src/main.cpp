#include "cuda_scan.h"
#include "pool_miner.h"
#include "solo_miner.h"

#include <exception>
#include <cmath>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

namespace {

enum class RunMode {
  kBenchmark,
  kPool,
  kSolo,
};

struct AppConfig {
  RunMode mode = RunMode::kBenchmark;
  miner::BenchmarkConfig benchmark{};
  miner::PoolConfig pool{};
  miner::SoloConfig solo{};
};

void print_usage(const char* exe) {
  std::cout
      << "Usage: " << exe << " [options]\n\n"
      << "Common options:\n"
      << "  --mode <benchmark|pool|solo> Run mode (default: benchmark)\n"
      << "  --device <id,id|all>     CUDA device indices (default: all)\n"
      << "  --threads <value>        CUDA threads per block, multiple of 32 (default: 256)\n"
      << "  --blocks <value>         CUDA blocks per launch (default: auto)\n"
      << "  --chunk-nonces <value>   Nonces processed in one kernel launch (default: auto)\n"
      << "\n"
      << "Benchmark mode options:\n"
      << "  --seconds <value>        Benchmark duration in seconds (default: 10)\n"
      << "\n"
      << "Pool mode options:\n"
      << "  --pool <host:port>       Stratum pool endpoint (default: poolflix.eu:5555)\n"
      << "  --user <username>        Stratum username (required in pool mode)\n"
      << "  --pass <password>        Stratum password (default: x)\n"
      << "  --pool-difficulty <diff>  Request and floor pool difficulty (e.g. 10000)\n"
      << "  --nonce-submit-be        Submit nonce in big-endian hex (default: little-endian)\n"
      << "\n"
      << "Solo mode options:\n"
      << "  --rpc-url <url>          Bitcoin Core RPC endpoint (default: http://127.0.0.1:8332)\n"
      << "  --rpc-user <user>        Bitcoin Core RPC username (optional with cookie auth)\n"
      << "  --rpc-pass <pass>        Bitcoin Core RPC password (optional with cookie auth)\n"
      << "  --rpc-cookie <path>      Bitcoin Core cookie file (default autodetect)\n"
      << "  --address <addr>         Payout address for block reward (optional, defaults to wallet new address)\n"
      << "\n"
      << "  --help                   Show this help\n";
}

bool parse_int(const std::string& value, int& out) {
  try {
    std::size_t consumed = 0;
    const int parsed = std::stoi(value, &consumed, 10);
    if (consumed != value.size()) {
      return false;
    }
    out = parsed;
    return true;
  } catch (const std::exception&) {
    return false;
  }
}

bool parse_u64(const std::string& value, std::uint64_t& out) {
  try {
    std::size_t consumed = 0;
    const std::uint64_t parsed = std::stoull(value, &consumed, 10);
    if (consumed != value.size()) {
      return false;
    }
    out = parsed;
    return true;
  } catch (const std::exception&) {
    return false;
  }
}

bool parse_double(const std::string& value, double& out) {
  try {
    std::size_t consumed = 0;
    const double parsed = std::stod(value, &consumed);
    if (consumed != value.size()) {
      return false;
    }
    out = parsed;
    return true;
  } catch (const std::exception&) {
    return false;
  }
}

int parse_args(int argc, char** argv, AppConfig& config) {
  config.pool.pool = "poolflix.eu:5555";
  config.pool.pass = "x";
  config.solo.rpc_url = "http://127.0.0.1:8332";

  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];

    auto next_value = [&](const char* name, std::string& out) -> bool {
      if (i + 1 >= argc) {
        std::cerr << "Missing value for " << name << "\n";
        return false;
      }
      out = argv[++i];
      return true;
    };

    if (arg == "--help" || arg == "-h") {
      print_usage(argv[0]);
      return 1;
    }

    if (arg == "--mode") {
      std::string value;
      if (!next_value("--mode", value)) {
        return -1;
      }
      if (value == "benchmark") {
        config.mode = RunMode::kBenchmark;
      } else if (value == "pool") {
        config.mode = RunMode::kPool;
      } else if (value == "solo") {
        config.mode = RunMode::kSolo;
      } else {
        std::cerr << "Invalid value for --mode (expected benchmark|pool|solo)\n";
        return -1;
      }
      continue;
    }

    if (arg == "--device") {
      std::string value;
      if (!next_value("--device", value)) {
        return -1;
      }
      config.benchmark.devices.clear();
      config.pool.devices.clear();
      config.solo.devices.clear();
      if (value != "all") {
        std::stringstream ss(value);
        std::string token;
        while (std::getline(ss, token, ',')) {
          int parsed = 0;
          if (!parse_int(token, parsed)) {
            std::cerr << "Invalid value for --device: " << token << "\n";
            return -1;
          }
          config.benchmark.devices.push_back(parsed);
          config.pool.devices.push_back(parsed);
          config.solo.devices.push_back(parsed);
        }
      }
      continue;
    }

    if (arg == "--threads") {
      std::string value;
      int parsed = 0;
      if (!next_value("--threads", value) || !parse_int(value, parsed)) {
        std::cerr << "Invalid value for --threads\n";
        return -1;
      }
      config.benchmark.threads = parsed;
      config.pool.threads = parsed;
      config.solo.threads = parsed;
      continue;
    }

    if (arg == "--blocks") {
      std::string value;
      int parsed = 0;
      if (!next_value("--blocks", value) || !parse_int(value, parsed)) {
        std::cerr << "Invalid value for --blocks\n";
        return -1;
      }
      config.benchmark.blocks = parsed;
      config.pool.blocks = parsed;
      config.solo.blocks = parsed;
      continue;
    }

    if (arg == "--chunk-nonces") {
      std::string value;
      std::uint64_t parsed = 0;
      if (!next_value("--chunk-nonces", value) || !parse_u64(value, parsed)) {
        std::cerr << "Invalid value for --chunk-nonces\n";
        return -1;
      }
      config.benchmark.chunk_nonces = parsed;
      config.pool.chunk_nonces = parsed;
      config.solo.chunk_nonces = parsed;
      continue;
    }

    if (arg == "--seconds") {
      std::string value;
      if (!next_value("--seconds", value) || !parse_double(value, config.benchmark.seconds)) {
        std::cerr << "Invalid value for --seconds\n";
        return -1;
      }
      continue;
    }

    if (arg == "--pool") {
      if (!next_value("--pool", config.pool.pool)) {
        return -1;
      }
      continue;
    }

    if (arg == "--user") {
      if (!next_value("--user", config.pool.user)) {
        return -1;
      }
      continue;
    }

    if (arg == "--pass") {
      if (!next_value("--pass", config.pool.pass)) {
        return -1;
      }
      continue;
    }

    if (arg == "--pool-difficulty") {
      std::string value;
      if (!next_value("--pool-difficulty", value) ||
          !parse_double(value, config.pool.requested_diff) ||
          !std::isfinite(config.pool.requested_diff) ||
          config.pool.requested_diff <= 0.0) {
        std::cerr << "Invalid value for --pool-difficulty\n";
        return -1;
      }
      continue;
    }

    if (arg == "--rpc-url") {
      if (!next_value("--rpc-url", config.solo.rpc_url)) {
        return -1;
      }
      continue;
    }

    if (arg == "--rpc-user") {
      if (!next_value("--rpc-user", config.solo.rpc_user)) {
        return -1;
      }
      continue;
    }

    if (arg == "--rpc-pass") {
      if (!next_value("--rpc-pass", config.solo.rpc_pass)) {
        return -1;
      }
      continue;
    }

    if (arg == "--rpc-cookie") {
      if (!next_value("--rpc-cookie", config.solo.rpc_cookie_path)) {
        return -1;
      }
      continue;
    }

    if (arg == "--address") {
      if (!next_value("--address", config.solo.address)) {
        return -1;
      }
      continue;
    }

    if (arg == "--nonce-submit-be") {
      config.pool.submit_nonce_be = true;
      continue;
    }

    std::cerr << "Unknown argument: " << arg << "\n";
    return -1;
  }

  if (config.benchmark.seconds <= 0.0) {
    std::cerr << "--seconds must be > 0\n";
    return -1;
  }

  if (config.benchmark.threads <= 0 || config.benchmark.threads > 1024 ||
      (config.benchmark.threads % 32 != 0)) {
    std::cerr << "--threads must be in range 32..1024 and divisible by 32\n";
    return -1;
  }

  if (config.benchmark.blocks < 0) {
    std::cerr << "--blocks must be >= 0\n";
    return -1;
  }

  if (config.mode == RunMode::kPool && config.pool.user.empty()) {
    std::cerr << "--user is required in --mode pool\n";
    return -1;
  }

  if (config.benchmark.devices.empty()) {
    int count = 0;
    if (miner::get_cuda_device_count(count) == 0 && count > 0) {
      for (int i = 0; i < count; ++i) {
        config.benchmark.devices.push_back(i);
        config.pool.devices.push_back(i);
        config.solo.devices.push_back(i);
      }
    } else {
      config.benchmark.devices.push_back(0);
      config.pool.devices.push_back(0);
      config.solo.devices.push_back(0);
    }
  }

  return 0;
}

}  // namespace

int main(int argc, char** argv) {
  AppConfig config;
  const int parse_status = parse_args(argc, argv, config);
  if (parse_status > 0) {
    return 0;
  }
  if (parse_status < 0) {
    std::cerr << "Use --help for supported arguments.\n";
    return 1;
  }

  if (config.mode == RunMode::kBenchmark) {
    return miner::run_benchmark(config.benchmark);
  }

  if (config.mode == RunMode::kPool) {
    return miner::run_pool_miner(config.pool);
  }

  return miner::run_solo_miner(config.solo);
}
