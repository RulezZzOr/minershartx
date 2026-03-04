#pragma once

#include <cstdint>
#include <string>

namespace miner {

struct PoolConfig {
  std::string pool;
  std::string user;
  std::string pass;
  bool submit_nonce_be = false;
  int device = 0;
  int blocks = 0;
  int threads = 256;
  std::uint64_t chunk_nonces = 0;
};

int run_pool_miner(const PoolConfig& config);

}  // namespace miner
