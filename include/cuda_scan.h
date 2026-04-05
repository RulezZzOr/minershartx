#pragma once

#include <cstdint>
#include <vector>

namespace miner {

struct BenchmarkConfig {
  std::vector<int> devices;
  int blocks = 0;
  int threads = 256;
  double seconds = 10.0;
  std::uint64_t chunk_nonces = 0;
};

struct ScanWork {
  std::uint64_t nonce_count = 0;
  std::uint32_t start_nonce = 0;
  std::uint32_t midstate[8] = {};
  std::uint32_t tail0 = 0;
  std::uint32_t tail1 = 0;
  std::uint32_t tail2 = 0;
  std::uint32_t target[8] = {};
};

struct ScanResult {
  bool found = false;
  std::uint32_t nonce = 0;
  std::uint32_t hash_le[8] = {};
};

struct ScanEngineConfig {
  int device = 0;
  int blocks = 0;
  int threads = 256;
};

struct ScanEngineInfo {
  int device = 0;
  int blocks = 0;
  int threads = 0;
  int sms = 0;
  char name[256] = {};
};

struct ScanEngine;

int get_cuda_device_count(int& count);
int run_benchmark(const BenchmarkConfig& config);
int create_scan_engine(const ScanEngineConfig& config, ScanEngine*& out_engine);
void destroy_scan_engine(ScanEngine* engine);
int get_scan_engine_info(const ScanEngine* engine, ScanEngineInfo& out_info);
int run_cuda_scan(ScanEngine* engine, const ScanWork& work, ScanResult& result);

}  // namespace miner
