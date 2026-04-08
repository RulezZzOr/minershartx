#pragma once

#include <cstddef>
#include <cstdint>

namespace miner {

struct NoncePartition {
  std::uint64_t start = 0;
  std::uint64_t size = 0;
};

inline NoncePartition split_nonce_space(const std::size_t worker_index,
                                        const std::size_t worker_count) {
  constexpr std::uint64_t kNonceSpace = 0x100000000ULL;
  if (worker_count == 0) {
    return {0, kNonceSpace};
  }

  const std::size_t clamped_index = (worker_index < worker_count) ? worker_index : worker_count - 1;
  const std::uint64_t start = (kNonceSpace * static_cast<std::uint64_t>(clamped_index)) /
                              static_cast<std::uint64_t>(worker_count);
  const std::uint64_t end = (kNonceSpace * static_cast<std::uint64_t>(clamped_index + 1U)) /
                            static_cast<std::uint64_t>(worker_count);
  return {start, end - start};
}

}  // namespace miner
