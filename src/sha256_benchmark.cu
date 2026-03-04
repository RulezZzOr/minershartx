#include "cuda_scan.h"
#include "sha256_cpu.h"

#include <cuda_runtime.h>

#include <array>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <iomanip>
#include <iostream>
#include <new>

namespace miner {

struct DeviceScanResult {
  int found;
  std::uint32_t nonce;
  std::uint32_t hash_le[8];
};

struct ScanEngine {
  int device = 0;
  int blocks = 0;
  int threads = 256;
  cudaDeviceProp props{};

  std::uint32_t* d_midstate = nullptr;
  std::uint32_t* d_target = nullptr;
  DeviceScanResult* d_result = nullptr;

  std::uint32_t cached_midstate[8] = {};
  std::uint32_t cached_target[8] = {};
  bool midstate_valid = false;
  bool target_valid = false;
};

namespace {

constexpr std::uint32_t kSha256Init[8] = {
    0x6a09e667U, 0xbb67ae85U, 0x3c6ef372U, 0xa54ff53aU,
    0x510e527fU, 0x9b05688cU, 0x1f83d9abU, 0x5be0cd19U,
};

__constant__ std::uint32_t kSha256KDevice[64] = {
    0x428a2f98U, 0x71374491U, 0xb5c0fbcfU, 0xe9b5dba5U, 0x3956c25bU,
    0x59f111f1U, 0x923f82a4U, 0xab1c5ed5U, 0xd807aa98U, 0x12835b01U,
    0x243185beU, 0x550c7dc3U, 0x72be5d74U, 0x80deb1feU, 0x9bdc06a7U,
    0xc19bf174U, 0xe49b69c1U, 0xefbe4786U, 0x0fc19dc6U, 0x240ca1ccU,
    0x2de92c6fU, 0x4a7484aaU, 0x5cb0a9dcU, 0x76f988daU, 0x983e5152U,
    0xa831c66dU, 0xb00327c8U, 0xbf597fc7U, 0xc6e00bf3U, 0xd5a79147U,
    0x06ca6351U, 0x14292967U, 0x27b70a85U, 0x2e1b2138U, 0x4d2c6dfcU,
    0x53380d13U, 0x650a7354U, 0x766a0abbU, 0x81c2c92eU, 0x92722c85U,
    0xa2bfe8a1U, 0xa81a664bU, 0xc24b8b70U, 0xc76c51a3U, 0xd192e819U,
    0xd6990624U, 0xf40e3585U, 0x106aa070U, 0x19a4c116U, 0x1e376c08U,
    0x2748774cU, 0x34b0bcb5U, 0x391c0cb3U, 0x4ed8aa4aU, 0x5b9cca4fU,
    0x682e6ff3U, 0x748f82eeU, 0x78a5636fU, 0x84c87814U, 0x8cc70208U,
    0x90befffaU, 0xa4506cebU, 0xbef9a3f7U, 0xc67178f2U,
};

#define CUDA_CHECK_GOTO(expr)                                                     \
  do {                                                                             \
    const cudaError_t err = (expr);                                                \
    if (err != cudaSuccess) {                                                      \
      std::cerr << "CUDA error: " << cudaGetErrorString(err)                     \
                << " for " << #expr << " at " << __FILE__ << ":" << __LINE__ \
                << "\n";                                                          \
      status = 1;                                                                   \
      goto cleanup;                                                                 \
    }                                                                                \
  } while (0)

#define CUDA_CHECK_RETURN(expr)                                                     \
  do {                                                                              \
    const cudaError_t err = (expr);                                                 \
    if (err != cudaSuccess) {                                                       \
      std::cerr << "CUDA error: " << cudaGetErrorString(err)                      \
                << " for " << #expr << " at " << __FILE__ << ":" << __LINE__  \
                << "\n";                                                           \
      return 1;                                                                     \
    }                                                                               \
  } while (0)

__device__ __forceinline__ std::uint32_t rotr32(const std::uint32_t x,
                                                 const int n) {
  return (x >> n) | (x << (32 - n));
}

__device__ __forceinline__ std::uint32_t ch(const std::uint32_t x,
                                             const std::uint32_t y,
                                             const std::uint32_t z) {
  return (x & y) ^ (~x & z);
}

__device__ __forceinline__ std::uint32_t maj(const std::uint32_t x,
                                              const std::uint32_t y,
                                              const std::uint32_t z) {
  return (x & y) ^ (x & z) ^ (y & z);
}

__device__ __forceinline__ std::uint32_t big_sigma0(const std::uint32_t x) {
  return rotr32(x, 2) ^ rotr32(x, 13) ^ rotr32(x, 22);
}

__device__ __forceinline__ std::uint32_t big_sigma1(const std::uint32_t x) {
  return rotr32(x, 6) ^ rotr32(x, 11) ^ rotr32(x, 25);
}

__device__ __forceinline__ std::uint32_t small_sigma0(const std::uint32_t x) {
  return rotr32(x, 7) ^ rotr32(x, 18) ^ (x >> 3);
}

__device__ __forceinline__ std::uint32_t small_sigma1(const std::uint32_t x) {
  return rotr32(x, 17) ^ rotr32(x, 19) ^ (x >> 10);
}

__device__ __forceinline__ std::uint32_t bswap32(const std::uint32_t x) {
  return ((x & 0x000000FFU) << 24) | ((x & 0x0000FF00U) << 8) |
         ((x & 0x00FF0000U) >> 8) | ((x & 0xFF000000U) >> 24);
}

__device__ __forceinline__ void sha256_compress_device(std::uint32_t state[8],
                                                        const std::uint32_t block[16]) {
  std::uint32_t w[64];
#pragma unroll
  for (int i = 0; i < 16; ++i) {
    w[i] = block[i];
  }
#pragma unroll
  for (int i = 16; i < 64; ++i) {
    w[i] = small_sigma1(w[i - 2]) + w[i - 7] + small_sigma0(w[i - 15]) +
           w[i - 16];
  }

  std::uint32_t a = state[0];
  std::uint32_t b = state[1];
  std::uint32_t c = state[2];
  std::uint32_t d = state[3];
  std::uint32_t e = state[4];
  std::uint32_t f = state[5];
  std::uint32_t g = state[6];
  std::uint32_t h = state[7];

#pragma unroll
  for (int i = 0; i < 64; ++i) {
    const std::uint32_t t1 = h + big_sigma1(e) + ch(e, f, g) + kSha256KDevice[i] +
                             w[i];
    const std::uint32_t t2 = big_sigma0(a) + maj(a, b, c);
    h = g;
    g = f;
    f = e;
    e = d + t1;
    d = c;
    c = b;
    b = a;
    a = t1 + t2;
  }

  state[0] += a;
  state[1] += b;
  state[2] += c;
  state[3] += d;
  state[4] += e;
  state[5] += f;
  state[6] += g;
  state[7] += h;
}

__device__ __forceinline__ void sha256d_hash_words_le(
    const std::uint32_t midstate[8], const std::uint32_t tail0,
    const std::uint32_t tail1, const std::uint32_t tail2,
    const std::uint32_t nonce_le, std::uint32_t out_hash_le[8]) {
  std::uint32_t state1[8];
#pragma unroll
  for (int i = 0; i < 8; ++i) {
    state1[i] = midstate[i];
  }

  std::uint32_t block1[16];
  block1[0] = tail0;
  block1[1] = tail1;
  block1[2] = tail2;
  block1[3] = bswap32(nonce_le);
  block1[4] = 0x80000000U;
#pragma unroll
  for (int i = 5; i < 15; ++i) {
    block1[i] = 0U;
  }
  block1[15] = 640U;

  sha256_compress_device(state1, block1);

  std::uint32_t state2[8];
#pragma unroll
  for (int i = 0; i < 8; ++i) {
    state2[i] = kSha256Init[i];
  }

  std::uint32_t block2[16];
#pragma unroll
  for (int i = 0; i < 8; ++i) {
    block2[i] = state1[i];
  }
  block2[8] = 0x80000000U;
#pragma unroll
  for (int i = 9; i < 15; ++i) {
    block2[i] = 0U;
  }
  block2[15] = 256U;

  sha256_compress_device(state2, block2);

#pragma unroll
  for (int i = 0; i < 8; ++i) {
    out_hash_le[i] = bswap32(state2[i]);
  }
}

__device__ __forceinline__ bool hash_leq_target(const std::uint32_t hash_le[8],
                                                 const std::uint32_t target_le[8]) {
  for (int i = 7; i >= 0; --i) {
    if (hash_le[i] < target_le[i]) {
      return true;
    }
    if (hash_le[i] > target_le[i]) {
      return false;
    }
  }
  return true;
}

__global__ void sha256d_scan_kernel(const std::uint32_t* __restrict__ midstate,
                                    const std::uint32_t tail0,
                                    const std::uint32_t tail1,
                                    const std::uint32_t tail2,
                                    const std::uint32_t start_nonce,
                                    const std::uint64_t total_nonces,
                                    const std::uint32_t* __restrict__ target,
                                    DeviceScanResult* result) {
  const std::uint64_t tid =
      static_cast<std::uint64_t>(blockIdx.x) * blockDim.x + threadIdx.x;
  const std::uint64_t stride =
      static_cast<std::uint64_t>(gridDim.x) * blockDim.x;

  std::uint32_t ms[8];
  std::uint32_t target_local[8];
#pragma unroll
  for (int i = 0; i < 8; ++i) {
    ms[i] = midstate[i];
    target_local[i] = target[i];
  }

  std::uint32_t poll_counter = 0;
  for (std::uint64_t i = tid; i < total_nonces; i += stride) {
    if ((poll_counter++ & 0x3FU) == 0U && result->found != 0) {
      return;
    }

    const std::uint32_t nonce = start_nonce + static_cast<std::uint32_t>(i);
    std::uint32_t hash_le[8];
    sha256d_hash_words_le(ms, tail0, tail1, tail2, nonce, hash_le);
    if (!hash_leq_target(hash_le, target_local)) {
      continue;
    }

    if (atomicCAS(&result->found, 0, 1) == 0) {
      result->nonce = nonce;
#pragma unroll
      for (int w = 0; w < 8; ++w) {
        result->hash_le[w] = hash_le[w];
      }
    }
    return;
  }
}

__global__ void sha256d_benchmark_kernel(const std::uint32_t* midstate,
                                         const std::uint32_t tail0,
                                         const std::uint32_t tail1,
                                         const std::uint32_t tail2,
                                         const std::uint32_t start_nonce,
                                         const std::uint64_t total_nonces,
                                         unsigned long long* counter,
                                         std::uint32_t* sink) {
  const std::uint64_t tid =
      static_cast<std::uint64_t>(blockIdx.x) * blockDim.x + threadIdx.x;
  const std::uint64_t stride =
      static_cast<std::uint64_t>(gridDim.x) * blockDim.x;

  std::uint32_t ms[8];
#pragma unroll
  for (int i = 0; i < 8; ++i) {
    ms[i] = midstate[i];
  }

  std::uint64_t local_count = 0;
  std::uint32_t local_sink = 0;

  for (std::uint64_t i = tid; i < total_nonces; i += stride) {
    const std::uint32_t nonce = start_nonce + static_cast<std::uint32_t>(i);
    std::uint32_t hash_le[8];
    sha256d_hash_words_le(ms, tail0, tail1, tail2, nonce, hash_le);
    local_sink ^= hash_le[0] ^ hash_le[7];
    ++local_count;
  }

  if (local_count > 0) {
    atomicAdd(counter, static_cast<unsigned long long>(local_count));
    atomicXor(sink, local_sink);
  }
}

std::array<std::uint8_t, 80> make_demo_header() {
  std::array<std::uint8_t, 80> header{};
  header.fill(0);

  // version 0x20000000 in little endian
  header[0] = 0x00;
  header[1] = 0x00;
  header[2] = 0x00;
  header[3] = 0x20;

  for (std::size_t i = 4; i < 76; ++i) {
    header[i] = static_cast<std::uint8_t>((i * 17U + 11U) & 0xFFU);
  }

  header[76] = 0x00;
  header[77] = 0x00;
  header[78] = 0x00;
  header[79] = 0x00;

  return header;
}

int resolve_device_and_launch(int device,
                              int requested_blocks,
                              int threads,
                              int& out_blocks,
                              cudaDeviceProp& out_props) {
  int device_count = 0;
  CUDA_CHECK_RETURN(cudaGetDeviceCount(&device_count));
  if (device_count <= 0) {
    std::cerr << "No CUDA device found.\n";
    return 1;
  }

  if (device < 0 || device >= device_count) {
    std::cerr << "Invalid CUDA device index " << device << " (available: 0.."
              << (device_count - 1) << ")\n";
    return 1;
  }

  CUDA_CHECK_RETURN(cudaSetDevice(device));
  CUDA_CHECK_RETURN(cudaGetDeviceProperties(&out_props, device));

  out_blocks = requested_blocks > 0 ? requested_blocks : out_props.multiProcessorCount * 32;
  if (out_blocks <= 0) {
    std::cerr << "Unable to infer valid CUDA block count.\n";
    return 1;
  }

  if (out_blocks > out_props.maxGridSize[0]) {
    out_blocks = out_props.maxGridSize[0];
  }

  if (threads <= 0 || threads > 1024 || (threads % 32 != 0)) {
    std::cerr << "Invalid threads value " << threads << " (expected 32..1024 and divisible by 32)\n";
    return 1;
  }

  return 0;
}

}  // namespace

int create_scan_engine(const ScanEngineConfig& config, ScanEngine*& out_engine) {
  out_engine = nullptr;

  int blocks = 0;
  cudaDeviceProp props{};
  const int launch_status =
      resolve_device_and_launch(config.device, config.blocks, config.threads, blocks, props);
  if (launch_status != 0) {
    return launch_status;
  }

  int status = 0;
  ScanEngine* engine = new (std::nothrow) ScanEngine();
  if (engine == nullptr) {
    std::cerr << "Failed to allocate ScanEngine\n";
    return 1;
  }

  engine->device = config.device;
  engine->blocks = blocks;
  engine->threads = config.threads;
  engine->props = props;

  CUDA_CHECK_GOTO(cudaSetDevice(engine->device));
  CUDA_CHECK_GOTO(cudaMalloc(reinterpret_cast<void**>(&engine->d_midstate),
                             sizeof(engine->cached_midstate)));
  CUDA_CHECK_GOTO(
      cudaMalloc(reinterpret_cast<void**>(&engine->d_target), sizeof(engine->cached_target)));
  CUDA_CHECK_GOTO(cudaMalloc(reinterpret_cast<void**>(&engine->d_result), sizeof(DeviceScanResult)));
  CUDA_CHECK_GOTO(cudaMemset(engine->d_result, 0, sizeof(DeviceScanResult)));

  out_engine = engine;
  return 0;

cleanup:
  if (engine != nullptr) {
    destroy_scan_engine(engine);
  }
  return status;
}

void destroy_scan_engine(ScanEngine* engine) {
  if (engine == nullptr) {
    return;
  }

  cudaSetDevice(engine->device);
  if (engine->d_midstate != nullptr) {
    cudaFree(engine->d_midstate);
  }
  if (engine->d_target != nullptr) {
    cudaFree(engine->d_target);
  }
  if (engine->d_result != nullptr) {
    cudaFree(engine->d_result);
  }

  delete engine;
}

int get_scan_engine_info(const ScanEngine* engine, ScanEngineInfo& out_info) {
  if (engine == nullptr) {
    std::cerr << "get_scan_engine_info called with null engine\n";
    return 1;
  }

  out_info.device = engine->device;
  out_info.blocks = engine->blocks;
  out_info.threads = engine->threads;
  out_info.sms = engine->props.multiProcessorCount;
  std::snprintf(out_info.name, sizeof(out_info.name), "%s", engine->props.name);
  return 0;
}

int run_cuda_scan(ScanEngine* engine, const ScanWork& work, ScanResult& result) {
  result = ScanResult{};
  if (engine == nullptr) {
    std::cerr << "run_cuda_scan called with null engine\n";
    return 1;
  }
  if (work.nonce_count == 0) {
    std::cerr << "run_cuda_scan called with nonce_count == 0\n";
    return 1;
  }

  int status = 0;
  CUDA_CHECK_GOTO(cudaSetDevice(engine->device));

  if (!engine->midstate_valid ||
      std::memcmp(engine->cached_midstate, work.midstate, sizeof(work.midstate)) != 0) {
    std::memcpy(engine->cached_midstate, work.midstate, sizeof(work.midstate));
    CUDA_CHECK_GOTO(cudaMemcpy(engine->d_midstate, engine->cached_midstate,
                               sizeof(engine->cached_midstate), cudaMemcpyHostToDevice));
    engine->midstate_valid = true;
  }

  if (!engine->target_valid ||
      std::memcmp(engine->cached_target, work.target, sizeof(work.target)) != 0) {
    std::memcpy(engine->cached_target, work.target, sizeof(work.target));
    CUDA_CHECK_GOTO(cudaMemcpy(engine->d_target, engine->cached_target,
                               sizeof(engine->cached_target), cudaMemcpyHostToDevice));
    engine->target_valid = true;
  }

  CUDA_CHECK_GOTO(cudaMemset(engine->d_result, 0, sizeof(DeviceScanResult)));

  sha256d_scan_kernel<<<engine->blocks, engine->threads>>>(
      engine->d_midstate, work.tail0, work.tail1, work.tail2, work.start_nonce,
      work.nonce_count, engine->d_target, engine->d_result);
  CUDA_CHECK_GOTO(cudaGetLastError());

  {
    DeviceScanResult host_result{};
    CUDA_CHECK_GOTO(cudaMemcpy(&host_result, engine->d_result, sizeof(host_result),
                               cudaMemcpyDeviceToHost));

    result.found = host_result.found != 0;
    result.nonce = host_result.nonce;
    for (int i = 0; i < 8; ++i) {
      result.hash_le[i] = host_result.hash_le[i];
    }
  }

cleanup:
  return status;
}

int run_benchmark(const BenchmarkConfig& config) {
  int status = 0;
  int blocks = 0;
  cudaDeviceProp props{};
  status = resolve_device_and_launch(config.device, config.blocks, config.threads, blocks,
                                     props);
  if (status != 0) {
    return status;
  }

  const std::uint64_t min_chunk =
      static_cast<std::uint64_t>(blocks) * static_cast<std::uint64_t>(config.threads);

  std::uint64_t chunk_nonces = config.chunk_nonces;
  if (chunk_nonces == 0) {
    chunk_nonces = min_chunk * 4096ULL;
  }
  if (chunk_nonces < min_chunk) {
    chunk_nonces = min_chunk;
  }

  const auto header = make_demo_header();

  std::uint32_t midstate[8] = {};
  std::uint32_t tail0 = 0;
  std::uint32_t tail1 = 0;
  std::uint32_t tail2 = 0;
  std::uint32_t start_nonce = 0;
  build_midstate_from_header(header, midstate, tail0, tail1, tail2, start_nonce);

  std::uint32_t* d_midstate = nullptr;
  unsigned long long* d_counter = nullptr;
  std::uint32_t* d_sink = nullptr;

  CUDA_CHECK_GOTO(cudaMalloc(reinterpret_cast<void**>(&d_midstate), sizeof(midstate)));
  CUDA_CHECK_GOTO(cudaMalloc(reinterpret_cast<void**>(&d_counter), sizeof(unsigned long long)));
  CUDA_CHECK_GOTO(cudaMalloc(reinterpret_cast<void**>(&d_sink), sizeof(std::uint32_t)));

  {
    const unsigned long long zero_counter = 0ULL;
    const std::uint32_t zero_sink = 0U;
    CUDA_CHECK_GOTO(cudaMemcpy(d_midstate, midstate, sizeof(midstate),
                               cudaMemcpyHostToDevice));
    CUDA_CHECK_GOTO(cudaMemcpy(d_counter, &zero_counter, sizeof(zero_counter),
                               cudaMemcpyHostToDevice));
    CUDA_CHECK_GOTO(cudaMemcpy(d_sink, &zero_sink, sizeof(zero_sink),
                               cudaMemcpyHostToDevice));
  }

  std::cout << "Device: " << props.name << "\n";
  std::cout << "SMs: " << props.multiProcessorCount << ", blocks: " << blocks
            << ", threads/block: " << config.threads << "\n";
  std::cout << "Chunk nonces: " << chunk_nonces << "\n";
  std::cout << "Benchmark target: " << config.seconds << " s\n";

  auto t0 = std::chrono::steady_clock::now();
  double elapsed_s = 0.0;
  std::uint64_t launches = 0;

  do {
    sha256d_benchmark_kernel<<<blocks, config.threads>>>(
        d_midstate, tail0, tail1, tail2, start_nonce, chunk_nonces, d_counter,
        d_sink);
    CUDA_CHECK_GOTO(cudaGetLastError());
    CUDA_CHECK_GOTO(cudaDeviceSynchronize());

    start_nonce += static_cast<std::uint32_t>(chunk_nonces);
    ++launches;

    const auto now = std::chrono::steady_clock::now();
    elapsed_s = std::chrono::duration<double>(now - t0).count();
  } while (elapsed_s < config.seconds);

  {
    unsigned long long total_nonces = 0ULL;
    std::uint32_t sink = 0U;
    CUDA_CHECK_GOTO(
        cudaMemcpy(&total_nonces, d_counter, sizeof(total_nonces), cudaMemcpyDeviceToHost));
    CUDA_CHECK_GOTO(cudaMemcpy(&sink, d_sink, sizeof(sink), cudaMemcpyDeviceToHost));

    const double hashes_per_second =
        static_cast<double>(total_nonces) / (elapsed_s > 0.0 ? elapsed_s : 1.0);
    std::cout << std::fixed << std::setprecision(3);
    std::cout << "Elapsed: " << elapsed_s << " s\n";
    std::cout << "Kernel launches: " << launches << "\n";
    std::cout << "Total hashes: " << total_nonces << "\n";
    std::cout << "Hashrate: " << (hashes_per_second / 1e9) << " GH/s\n";
    std::cout << std::hex << std::showbase;
    std::cout << "Debug sink: " << sink << "\n";
    std::cout << std::dec << std::noshowbase;
  }

cleanup:
  if (d_midstate != nullptr) {
    cudaFree(d_midstate);
  }
  if (d_counter != nullptr) {
    cudaFree(d_counter);
  }
  if (d_sink != nullptr) {
    cudaFree(d_sink);
  }

  return status;
}

}  // namespace miner
