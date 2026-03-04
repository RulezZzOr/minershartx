#include "sha256_cpu.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <iomanip>
#include <sstream>

namespace miner {
namespace {

constexpr std::uint32_t kInit[8] = {
    0x6a09e667U, 0xbb67ae85U, 0x3c6ef372U, 0xa54ff53aU,
    0x510e527fU, 0x9b05688cU, 0x1f83d9abU, 0x5be0cd19U,
};

constexpr std::uint32_t kRoundK[64] = {
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

inline std::uint32_t rotr32(const std::uint32_t v, const int n) {
  return (v >> n) | (v << (32 - n));
}

inline std::uint32_t ch(const std::uint32_t x,
                        const std::uint32_t y,
                        const std::uint32_t z) {
  return (x & y) ^ (~x & z);
}

inline std::uint32_t maj(const std::uint32_t x,
                         const std::uint32_t y,
                         const std::uint32_t z) {
  return (x & y) ^ (x & z) ^ (y & z);
}

inline std::uint32_t big_sigma0(const std::uint32_t x) {
  return rotr32(x, 2) ^ rotr32(x, 13) ^ rotr32(x, 22);
}

inline std::uint32_t big_sigma1(const std::uint32_t x) {
  return rotr32(x, 6) ^ rotr32(x, 11) ^ rotr32(x, 25);
}

inline std::uint32_t small_sigma0(const std::uint32_t x) {
  return rotr32(x, 7) ^ rotr32(x, 18) ^ (x >> 3);
}

inline std::uint32_t small_sigma1(const std::uint32_t x) {
  return rotr32(x, 17) ^ rotr32(x, 19) ^ (x >> 10);
}

inline int hex_nibble(const char c) {
  if (c >= '0' && c <= '9') {
    return c - '0';
  }
  if (c >= 'a' && c <= 'f') {
    return c - 'a' + 10;
  }
  if (c >= 'A' && c <= 'F') {
    return c - 'A' + 10;
  }
  return -1;
}

void sha256_compress(std::uint32_t state[8], const std::uint32_t block[16]) {
  std::uint32_t w[64];
  for (int i = 0; i < 16; ++i) {
    w[i] = block[i];
  }
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

  for (int i = 0; i < 64; ++i) {
    const std::uint32_t t1 = h + big_sigma1(e) + ch(e, f, g) + kRoundK[i] + w[i];
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

void write_digest(const std::uint32_t state[8], std::array<std::uint8_t, 32>& out) {
  for (int i = 0; i < 8; ++i) {
    out[static_cast<std::size_t>(4 * i)] =
        static_cast<std::uint8_t>((state[i] >> 24) & 0xFFU);
    out[static_cast<std::size_t>(4 * i + 1)] =
        static_cast<std::uint8_t>((state[i] >> 16) & 0xFFU);
    out[static_cast<std::size_t>(4 * i + 2)] =
        static_cast<std::uint8_t>((state[i] >> 8) & 0xFFU);
    out[static_cast<std::size_t>(4 * i + 3)] =
        static_cast<std::uint8_t>(state[i] & 0xFFU);
  }
}

}  // namespace

bool hex_to_bytes(const std::string& hex, std::vector<std::uint8_t>& out) {
  if (hex.size() % 2 != 0) {
    return false;
  }

  out.clear();
  out.reserve(hex.size() / 2);

  for (std::size_t i = 0; i < hex.size(); i += 2) {
    const int hi = hex_nibble(hex[i]);
    const int lo = hex_nibble(hex[i + 1]);
    if (hi < 0 || lo < 0) {
      out.clear();
      return false;
    }
    out.push_back(static_cast<std::uint8_t>((hi << 4) | lo));
  }

  return true;
}

std::string bytes_to_hex(const std::uint8_t* data, const std::size_t size) {
  static constexpr char kHex[] = "0123456789abcdef";
  std::string out;
  out.resize(size * 2);
  for (std::size_t i = 0; i < size; ++i) {
    out[2 * i] = kHex[(data[i] >> 4) & 0x0F];
    out[2 * i + 1] = kHex[data[i] & 0x0F];
  }
  return out;
}

std::uint32_t load_be32(const std::uint8_t* p) {
  return (static_cast<std::uint32_t>(p[0]) << 24) |
         (static_cast<std::uint32_t>(p[1]) << 16) |
         (static_cast<std::uint32_t>(p[2]) << 8) |
         static_cast<std::uint32_t>(p[3]);
}

std::uint32_t load_le32(const std::uint8_t* p) {
  return (static_cast<std::uint32_t>(p[0])) |
         (static_cast<std::uint32_t>(p[1]) << 8) |
         (static_cast<std::uint32_t>(p[2]) << 16) |
         (static_cast<std::uint32_t>(p[3]) << 24);
}

void store_le32(const std::uint32_t value, std::uint8_t* p) {
  p[0] = static_cast<std::uint8_t>(value & 0xFFU);
  p[1] = static_cast<std::uint8_t>((value >> 8) & 0xFFU);
  p[2] = static_cast<std::uint8_t>((value >> 16) & 0xFFU);
  p[3] = static_cast<std::uint8_t>((value >> 24) & 0xFFU);
}

void reverse_bytes(std::vector<std::uint8_t>& bytes) {
  std::reverse(bytes.begin(), bytes.end());
}

void reverse_bytes_32(std::array<std::uint8_t, 32>& bytes) {
  std::reverse(bytes.begin(), bytes.end());
}

void sha256(const std::uint8_t* data, const std::size_t size,
            std::array<std::uint8_t, 32>& out) {
  std::uint32_t state[8];
  std::memcpy(state, kInit, sizeof(state));

  std::size_t offset = 0;
  while (size - offset >= 64) {
    std::uint32_t block[16];
    for (int i = 0; i < 16; ++i) {
      block[i] = load_be32(data + offset + static_cast<std::size_t>(i) * 4U);
    }
    sha256_compress(state, block);
    offset += 64;
  }

  std::uint8_t tail[128] = {0};
  const std::size_t rem = size - offset;
  if (rem > 0) {
    std::memcpy(tail, data + offset, rem);
  }
  tail[rem] = 0x80;

  const std::uint64_t bit_len = static_cast<std::uint64_t>(size) * 8ULL;
  const bool two_blocks = rem >= 56;
  const std::size_t tail_size = two_blocks ? 128 : 64;

  tail[tail_size - 8] = static_cast<std::uint8_t>((bit_len >> 56) & 0xFFU);
  tail[tail_size - 7] = static_cast<std::uint8_t>((bit_len >> 48) & 0xFFU);
  tail[tail_size - 6] = static_cast<std::uint8_t>((bit_len >> 40) & 0xFFU);
  tail[tail_size - 5] = static_cast<std::uint8_t>((bit_len >> 32) & 0xFFU);
  tail[tail_size - 4] = static_cast<std::uint8_t>((bit_len >> 24) & 0xFFU);
  tail[tail_size - 3] = static_cast<std::uint8_t>((bit_len >> 16) & 0xFFU);
  tail[tail_size - 2] = static_cast<std::uint8_t>((bit_len >> 8) & 0xFFU);
  tail[tail_size - 1] = static_cast<std::uint8_t>(bit_len & 0xFFU);

  for (std::size_t b = 0; b < tail_size; b += 64) {
    std::uint32_t block[16];
    for (int i = 0; i < 16; ++i) {
      block[i] = load_be32(tail + b + static_cast<std::size_t>(i) * 4U);
    }
    sha256_compress(state, block);
  }

  write_digest(state, out);
}

void sha256d(const std::uint8_t* data, const std::size_t size,
             std::array<std::uint8_t, 32>& out) {
  std::array<std::uint8_t, 32> first{};
  sha256(data, size, first);
  sha256(first.data(), first.size(), out);
}

void build_midstate_from_header(const std::array<std::uint8_t, 80>& header,
                                std::uint32_t out_midstate[8],
                                std::uint32_t& tail0,
                                std::uint32_t& tail1,
                                std::uint32_t& tail2,
                                std::uint32_t& start_nonce_le) {
  std::uint32_t state[8];
  std::memcpy(state, kInit, sizeof(state));

  std::uint32_t block0[16];
  for (int i = 0; i < 16; ++i) {
    block0[i] = load_be32(&header[static_cast<std::size_t>(i) * 4U]);
  }

  sha256_compress(state, block0);

  for (int i = 0; i < 8; ++i) {
    out_midstate[i] = state[i];
  }

  tail0 = load_be32(&header[64]);
  tail1 = load_be32(&header[68]);
  tail2 = load_be32(&header[72]);
  start_nonce_le = load_le32(&header[76]);
}

}  // namespace miner
