#include "cuda_scan.h"
#include "pool_miner.h"
#include "solo_miner.h"

#include <exception>
#include <cmath>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace {

using json = nlohmann::json;

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
      << "  --config <path>          Load JSON config before CLI overrides\n"
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
      << "  --debug-pool-header      Print 80-byte header and component fields\n"
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

bool parse_mode_value(const std::string& value, RunMode& out) {
  if (value == "benchmark") {
    out = RunMode::kBenchmark;
    return true;
  }
  if (value == "pool") {
    out = RunMode::kPool;
    return true;
  }
  if (value == "solo") {
    out = RunMode::kSolo;
    return true;
  }
  return false;
}

bool parse_device_json(const json& value, std::vector<int>& out_devices) {
  out_devices.clear();

  if (value.is_string()) {
    const std::string s = value.get<std::string>();
    if (s == "all") {
      return true;
    }

    std::stringstream ss(s);
    std::string token;
    while (std::getline(ss, token, ',')) {
      int parsed = 0;
      if (!parse_int(token, parsed)) {
        return false;
      }
      out_devices.push_back(parsed);
    }
    return true;
  }

  if (value.is_array()) {
    for (const auto& item : value) {
      if (!item.is_number_integer()) {
        return false;
      }
      out_devices.push_back(item.get<int>());
    }
    return true;
  }

  if (value.is_number_integer()) {
    out_devices.push_back(value.get<int>());
    return true;
  }

  return false;
}

bool apply_json_config(const json& root, AppConfig& config, std::string& error) {
  if (!root.is_object()) {
    error = "config root must be a JSON object";
    return false;
  }

  if (root.contains("mode")) {
    if (!root["mode"].is_string() ||
        !parse_mode_value(root["mode"].get<std::string>(), config.mode)) {
      error = "invalid config value for mode";
      return false;
    }
  }

  if (root.contains("device")) {
    if (!parse_device_json(root["device"], config.benchmark.devices)) {
      error = "invalid config value for device";
      return false;
    }
    config.pool.devices = config.benchmark.devices;
    config.solo.devices = config.benchmark.devices;
  }

  if (root.contains("devices")) {
    if (!parse_device_json(root["devices"], config.benchmark.devices)) {
      error = "invalid config value for devices";
      return false;
    }
    config.pool.devices = config.benchmark.devices;
    config.solo.devices = config.benchmark.devices;
  }

  if (root.contains("threads")) {
    if (!root["threads"].is_number_integer()) {
      error = "invalid config value for threads";
      return false;
    }
    config.benchmark.threads = root["threads"].get<int>();
    config.pool.threads = config.benchmark.threads;
    config.solo.threads = config.benchmark.threads;
  }

  if (root.contains("blocks")) {
    if (!root["blocks"].is_number_integer()) {
      error = "invalid config value for blocks";
      return false;
    }
    config.benchmark.blocks = root["blocks"].get<int>();
    config.pool.blocks = config.benchmark.blocks;
    config.solo.blocks = config.benchmark.blocks;
  }

  if (root.contains("chunk_nonces")) {
    if (!root["chunk_nonces"].is_number_unsigned() &&
        !root["chunk_nonces"].is_number_integer()) {
      error = "invalid config value for chunk_nonces";
      return false;
    }
    if (root["chunk_nonces"].is_number_integer()) {
      const auto signed_value = root["chunk_nonces"].get<long long>();
      if (signed_value < 0) {
        error = "invalid config value for chunk_nonces";
        return false;
      }
      config.benchmark.chunk_nonces = static_cast<std::uint64_t>(signed_value);
    } else {
      config.benchmark.chunk_nonces = root["chunk_nonces"].get<std::uint64_t>();
    }
    config.pool.chunk_nonces = config.benchmark.chunk_nonces;
    config.solo.chunk_nonces = config.benchmark.chunk_nonces;
  }

  if (root.contains("seconds")) {
    if (!root["seconds"].is_number()) {
      error = "invalid config value for seconds";
      return false;
    }
    config.benchmark.seconds = root["seconds"].get<double>();
  }

  if (root.contains("pool")) {
    if (!root["pool"].is_string()) {
      error = "invalid config value for pool";
      return false;
    }
    config.pool.pool = root["pool"].get<std::string>();
  }

  if (root.contains("user")) {
    if (!root["user"].is_string()) {
      error = "invalid config value for user";
      return false;
    }
    config.pool.user = root["user"].get<std::string>();
  }

  if (root.contains("pass")) {
    if (!root["pass"].is_string()) {
      error = "invalid config value for pass";
      return false;
    }
    config.pool.pass = root["pass"].get<std::string>();
  }

  if (root.contains("pool_difficulty")) {
    if (!root["pool_difficulty"].is_number()) {
      error = "invalid config value for pool_difficulty";
      return false;
    }
    config.pool.requested_diff = root["pool_difficulty"].get<double>();
  }

  if (root.contains("debug_pool_header")) {
    if (!root["debug_pool_header"].is_boolean()) {
      error = "invalid config value for debug_pool_header";
      return false;
    }
    config.pool.debug_pool_header = root["debug_pool_header"].get<bool>();
  }

  if (root.contains("nonce_submit_be")) {
    if (!root["nonce_submit_be"].is_boolean()) {
      error = "invalid config value for nonce_submit_be";
      return false;
    }
    config.pool.submit_nonce_be = root["nonce_submit_be"].get<bool>();
  }

  if (root.contains("rpc_url")) {
    if (!root["rpc_url"].is_string()) {
      error = "invalid config value for rpc_url";
      return false;
    }
    config.solo.rpc_url = root["rpc_url"].get<std::string>();
  }

  if (root.contains("rpc_user")) {
    if (!root["rpc_user"].is_string()) {
      error = "invalid config value for rpc_user";
      return false;
    }
    config.solo.rpc_user = root["rpc_user"].get<std::string>();
  }

  if (root.contains("rpc_pass")) {
    if (!root["rpc_pass"].is_string()) {
      error = "invalid config value for rpc_pass";
      return false;
    }
    config.solo.rpc_pass = root["rpc_pass"].get<std::string>();
  }

  if (root.contains("rpc_cookie")) {
    if (!root["rpc_cookie"].is_string()) {
      error = "invalid config value for rpc_cookie";
      return false;
    }
    config.solo.rpc_cookie_path = root["rpc_cookie"].get<std::string>();
  }

  if (root.contains("address")) {
    if (!root["address"].is_string()) {
      error = "invalid config value for address";
      return false;
    }
    config.solo.address = root["address"].get<std::string>();
  }

  if (root.contains("template_poll_seconds")) {
    if (!root["template_poll_seconds"].is_number_integer()) {
      error = "invalid config value for template_poll_seconds";
      return false;
    }
    config.solo.template_poll_seconds = root["template_poll_seconds"].get<int>();
  }

  return true;
}

bool load_config_file(const std::string& path, AppConfig& config, std::string& error) {
  std::ifstream file(path);
  if (!file) {
    error = "unable to open config file: " + path;
    return false;
  }

  std::ostringstream buffer;
  buffer << file.rdbuf();
  const json root = json::parse(buffer.str(), nullptr, false);
  if (root.is_discarded()) {
    error = "invalid JSON in config file: " + path;
    return false;
  }

  return apply_json_config(root, config, error);
}

int parse_args(int argc, char** argv, AppConfig& config) {
  config.pool.pool = "poolflix.eu:5555";
  config.pool.pass = "x";
  config.solo.rpc_url = "http://127.0.0.1:8332";

  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    if (arg == "--help" || arg == "-h") {
      print_usage(argv[0]);
      return 1;
    }
  }

  std::vector<std::string> config_paths;
  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    if (arg == "--config") {
      if (i + 1 >= argc) {
        std::cerr << "Missing value for --config\n";
        return -1;
      }
      config_paths.push_back(argv[++i]);
    }
  }

  for (const auto& path : config_paths) {
    std::string config_error;
    if (!load_config_file(path, config, config_error)) {
      std::cerr << "Failed to load config file \"" << path << "\": "
                << config_error << "\n";
      return -1;
    }
  }

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

    if (arg == "--mode") {
      std::string value;
      if (!next_value("--mode", value)) {
        return -1;
      }
      if (!parse_mode_value(value, config.mode)) {
        std::cerr << "Invalid value for --mode (expected benchmark|pool|solo)\n";
        return -1;
      }
      continue;
    }

    if (arg == "--config") {
      ++i;
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

    if (arg == "--debug-pool-header") {
      config.pool.debug_pool_header = true;
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
