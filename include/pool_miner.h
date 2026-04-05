#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace miner {

struct PoolConfig {
  std::string pool;
  std::string user;
  std::string pass;
  double requested_diff = 0.0;
  bool debug_pool_header = false;
  bool submit_nonce_be = false;
  std::vector<int> devices;
  int blocks = 0;
  int threads = 256;
  std::uint64_t chunk_nonces = 0;
};

int run_pool_miner(const PoolConfig& config);

}  // namespace miner
