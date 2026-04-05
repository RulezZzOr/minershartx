#include "sha256_cpu.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <string>
#include <vector>

namespace {

int g_failures = 0;

void expect(const bool condition, const std::string& label) {
  if (!condition) {
    std::cerr << "FAIL: " << label << "\n";
    ++g_failures;
  }
}

void expect_hex_digest(const std::string& label,
                       const std::uint8_t* data,
                       const std::size_t size,
                       const char* expected_hex,
                       const bool double_sha256) {
  std::array<std::uint8_t, 32> digest{};
  if (double_sha256) {
    miner::sha256d(data, size, digest);
  } else {
    miner::sha256(data, size, digest);
  }

  const std::string actual_hex = miner::bytes_to_hex(digest.data(), digest.size());
  expect(actual_hex == expected_hex, label + " expected " + expected_hex + " got " + actual_hex);
}

void test_hex_helpers() {
  std::vector<std::uint8_t> bytes;
  expect(miner::hex_to_bytes("00ff10", bytes), "hex_to_bytes accepts valid input");
  expect(bytes.size() == 3 && bytes[0] == 0x00U && bytes[1] == 0xFFU && bytes[2] == 0x10U,
         "hex_to_bytes decodes valid input");
  expect(miner::bytes_to_hex(bytes.data(), bytes.size()) == "00ff10",
         "bytes_to_hex roundtrip");
  expect(!miner::hex_to_bytes("0", bytes), "hex_to_bytes rejects odd length");
  expect(!miner::hex_to_bytes("zz", bytes), "hex_to_bytes rejects invalid characters");
}

void test_sha256_vectors() {
  const std::string empty;
  expect_hex_digest("sha256(\"\")",
                    reinterpret_cast<const std::uint8_t*>(empty.data()),
                    empty.size(),
                    "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855",
                    false);

  const std::string abc = "abc";
  expect_hex_digest("sha256(\"abc\")",
                    reinterpret_cast<const std::uint8_t*>(abc.data()),
                    abc.size(),
                    "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad",
                    false);
}

void test_sha256d_vectors() {
  const std::string empty;
  expect_hex_digest("sha256d(\"\")",
                    reinterpret_cast<const std::uint8_t*>(empty.data()),
                    empty.size(),
                    "5df6e0e2761359d30a8275058e299fcc0381534545f55cf43e41983f5d4c9456",
                    true);

  const std::string abc = "abc";
  expect_hex_digest("sha256d(\"abc\")",
                    reinterpret_cast<const std::uint8_t*>(abc.data()),
                    abc.size(),
                    "4f8b42c22dd3729b519ba6f68d2da7cc5b2d606d05daed5ad5128cc03e6c6358",
                    true);

  std::array<std::uint8_t, 80> zero_header{};
  zero_header.fill(0U);
  expect_hex_digest("sha256d(80 zero bytes)",
                    zero_header.data(),
                    zero_header.size(),
                    "4be7570e8f70eb093640c8468274ba759745a7aa2b7d25ab1e0421b259845014",
                    true);
}

void test_midstate_split() {
  std::array<std::uint8_t, 80> header{};
  for (std::size_t i = 0; i < header.size(); ++i) {
    header[i] = static_cast<std::uint8_t>(i);
  }

  std::uint32_t midstate[8] = {};
  std::uint32_t tail0 = 0;
  std::uint32_t tail1 = 0;
  std::uint32_t tail2 = 0;
  std::uint32_t start_nonce_le = 0;
  miner::build_midstate_from_header(header, midstate, tail0, tail1, tail2, start_nonce_le);

  expect(tail0 == 0x40414243U, "midstate tail0 matches big-endian load");
  expect(tail1 == 0x44454647U, "midstate tail1 matches big-endian load");
  expect(tail2 == 0x48494A4BU, "midstate tail2 matches big-endian load");
  expect(start_nonce_le == 0x4F4E4D4CU, "midstate nonce matches little-endian load");

  bool any_midstate_nonzero = false;
  for (std::uint32_t word : midstate) {
    any_midstate_nonzero = any_midstate_nonzero || (word != 0U);
  }
  expect(any_midstate_nonzero, "midstate is populated");
}

void test_stratum_header_layout() {
  const char* version_hex = "00000020";
  const char* prevhash_hex =
      "6a2be50e9bfb43f4973d1b2a84fc5fa27a11730b000209b00000000000000000";
  const char* merkle_hex =
      "bc65e13b5f7c69e9ce91adbb063d46b9be2445805e6a2498e13863708d57392c";
  const char* ntime_hex = "b8852666";
  const char* nbits_hex = "19420317";
  const char* nonce_hex = "00000000";

  std::vector<std::uint8_t> version_bytes;
  std::vector<std::uint8_t> prevhash_bytes;
  std::vector<std::uint8_t> merkle_bytes;
  std::vector<std::uint8_t> ntime_bytes;
  std::vector<std::uint8_t> nbits_bytes;
  std::vector<std::uint8_t> nonce_bytes;

  expect(miner::hex_to_bytes(version_hex, version_bytes) && version_bytes.size() == 4,
         "stratum header version decodes");
  expect(miner::hex_to_bytes(prevhash_hex, prevhash_bytes) && prevhash_bytes.size() == 32,
         "stratum header prevhash decodes");
  expect(miner::hex_to_bytes(merkle_hex, merkle_bytes) && merkle_bytes.size() == 32,
         "stratum header merkle decodes");
  expect(miner::hex_to_bytes(ntime_hex, ntime_bytes) && ntime_bytes.size() == 4,
         "stratum header ntime decodes");
  expect(miner::hex_to_bytes(nbits_hex, nbits_bytes) && nbits_bytes.size() == 4,
         "stratum header nbits decodes");
  expect(miner::hex_to_bytes(nonce_hex, nonce_bytes) && nonce_bytes.size() == 4,
         "stratum header nonce decodes");

  std::array<std::uint8_t, 80> header{};
  std::array<std::uint8_t, 32> merkle_header{};
  std::copy(merkle_bytes.begin(), merkle_bytes.end(), merkle_header.begin());
  miner::reverse_bytes_32(merkle_header);

  std::memcpy(header.data() + 0, version_bytes.data(), 4);
  std::memcpy(header.data() + 4, prevhash_bytes.data(), 32);
  std::memcpy(header.data() + 36, merkle_header.data(), 32);
  std::memcpy(header.data() + 68, ntime_bytes.data(), 4);
  std::memcpy(header.data() + 72, nbits_bytes.data(), 4);
  std::memcpy(header.data() + 76, nonce_bytes.data(), 4);

  expect(miner::bytes_to_hex(header.data(), header.size()) ==
             "000000206a2be50e9bfb43f4973d1b2a84fc5fa27a11730b000209b000000000000000002c39578d706338e198246a5e804524beb9463d06bbad91cee9697c5f3be165bcb88526661942031700000000",
         "stratum header layout matches known example");
}

}  // namespace

int main() {
  test_hex_helpers();
  test_sha256_vectors();
  test_sha256d_vectors();
  test_midstate_split();
  test_stratum_header_layout();

  if (g_failures != 0) {
    std::cerr << g_failures << " self-test failure(s)\n";
    return 1;
  }

  std::cout << "minershartx self-test passed\n";
  return 0;
}
