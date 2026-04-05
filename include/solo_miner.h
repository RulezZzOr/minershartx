#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace miner {

struct SoloConfig {
  std::string rpc_url;
  std::string rpc_user;
  std::string rpc_pass;
  std::string rpc_cookie_path;
  std::string address;
  std::vector<int> devices;
  int blocks = 0;
  int threads = 256;
  std::uint64_t chunk_nonces = 0;
  int template_poll_seconds = 15;
};

int run_solo_miner(const SoloConfig& config);

}  // namespace miner
