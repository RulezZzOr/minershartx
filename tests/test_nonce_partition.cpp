#include "nonce_partition.h"

#include <cstdint>
#include <iostream>
#include <vector>

namespace {

int g_failures = 0;

void expect(const bool condition, const char* label) {
  if (!condition) {
    std::cerr << "FAIL: " << label << "\n";
    ++g_failures;
  }
}

void test_partition_coverage() {
  const std::size_t workers = 4;
  std::uint64_t total = 0;
  std::uint64_t previous_end = 0;

  for (std::size_t i = 0; i < workers; ++i) {
    const miner::NoncePartition part = miner::split_nonce_space(i, workers);
    expect(part.size > 0, "partition size positive");
    expect(part.start == previous_end, "partition is contiguous");
    previous_end = part.start + part.size;
    total += part.size;
  }

  expect(total == 0x100000000ULL, "partition covers full nonce space");
  expect(previous_end == 0x100000000ULL, "last partition ends at 2^32");
}

void test_partition_single_worker() {
  const miner::NoncePartition part = miner::split_nonce_space(0, 1);
  expect(part.start == 0, "single worker starts at zero");
  expect(part.size == 0x100000000ULL, "single worker gets full space");
}

}  // namespace

int run_nonce_partition_tests() {
  test_partition_coverage();
  test_partition_single_worker();

  if (g_failures != 0) {
    std::cerr << g_failures << " nonce partition test failure(s)\n";
    return 1;
  }
  return 0;
}
