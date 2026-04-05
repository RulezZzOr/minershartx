#include "address_decoder.h"
#include "sha256_cpu.h"

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

void expect_address_script(const std::string& label,
                           const std::string& address,
                           const std::string& expected_script_hex) {
  std::vector<std::uint8_t> script;
  std::string error;
  const bool ok = miner::decode_bitcoin_address_script(address, script, error);
  expect(ok, label + " decoded (" + error + ")");
  if (!ok) {
    return;
  }

  const std::string actual_hex = miner::bytes_to_hex(script.data(), script.size());
  expect(actual_hex == expected_script_hex,
         label + " expected " + expected_script_hex + " got " + actual_hex);
}

void test_known_addresses() {
  expect_address_script(
      "mainnet P2PKH",
      "1A1zP1eP5QGefi2DMPTfTL5SLmv7DivfNa",
      "76a91462e907b15cbf27d5425399ebf6f0fb50ebb88f1888ac");

  expect_address_script(
      "mainnet P2SH",
      "3J98t1WpEZ73CNmQviecrnyiWrnqRhWNLy",
      "a914b472a266d0bd89c13706a4132ccfb16f7c3b9fcb87");

  expect_address_script(
      "mainnet bech32 v0",
      "bc1qw508d6qejxtdg4y5r3zarvary0c5xw7kv8f3t4",
      "0014751e76e8199196d454941c45d1b3a323f1433bd6");

  expect_address_script(
      "mainnet bech32 v0 uppercase",
      "BC1QW508D6QEJXTDG4Y5R3ZARVARY0C5XW7KV8F3T4",
      "0014751e76e8199196d454941c45d1b3a323f1433bd6");
}

void test_rejections() {
  std::vector<std::uint8_t> script;
  std::string error;

  expect(!miner::decode_bitcoin_address_script("bc1qw508d6qejxtdg4y5r3zarvary0c5xw7kygt08l",
                                                script, error),
         "reject invalid bech32 checksum");
  expect(!miner::decode_bitcoin_address_script("1BoatSLRHtKNngkdXEeobR76b53LETtp90",
                                                script, error),
         "reject invalid base58 character");
}

}  // namespace

int main() {
  test_known_addresses();
  test_rejections();

  if (g_failures != 0) {
    std::cerr << g_failures << " address test failure(s)\n";
    return 1;
  }

  std::cout << "address decoder self-test passed\n";
  return 0;
}
