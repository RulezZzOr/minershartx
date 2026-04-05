#include "address_decoder.h"

#include "sha256_cpu.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace miner {
namespace {

constexpr char kBase58Alphabet[] =
    "123456789ABCDEFGHJKLMNPQRSTUVWXYZabcdefghijkmnopqrstuvwxyz";
constexpr char kBech32Alphabet[] = "qpzry9x8gf2tvdw0s3jn54khce6mua7l";
constexpr std::uint32_t kBech32Constant = 1U;
constexpr std::uint32_t kBech32mConstant = 0x2bc830a3U;

int base58_index(const char c) {
  const char* pos = std::strchr(kBase58Alphabet, c);
  if (pos == nullptr) {
    return -1;
  }
  return static_cast<int>(pos - kBase58Alphabet);
}

std::uint32_t bech32_polymod(const std::vector<std::uint8_t>& values) {
  std::uint32_t chk = 1U;
  for (const std::uint8_t value : values) {
    const std::uint32_t top = chk >> 25U;
    chk = ((chk & 0x1ffffffU) << 5U) ^ static_cast<std::uint32_t>(value);
    if ((top & 1U) != 0U) chk ^= 0x3b6a57b2U;
    if ((top & 2U) != 0U) chk ^= 0x26508e6dU;
    if ((top & 4U) != 0U) chk ^= 0x1ea119faU;
    if ((top & 8U) != 0U) chk ^= 0x3d4233ddU;
    if ((top & 16U) != 0U) chk ^= 0x2a1462b3U;
  }
  return chk;
}

std::vector<std::uint8_t> bech32_hrp_expand(const std::string& hrp) {
  std::vector<std::uint8_t> ret;
  ret.reserve(hrp.size() * 2U + 1U);
  for (const unsigned char c : hrp) {
    ret.push_back(static_cast<std::uint8_t>(c >> 5U));
  }
  ret.push_back(0U);
  for (const unsigned char c : hrp) {
    ret.push_back(static_cast<std::uint8_t>(c & 0x1FU));
  }
  return ret;
}

bool convert_bits(const std::vector<std::uint8_t>& input,
                  const int from_bits,
                  const int to_bits,
                  const bool pad,
                  std::vector<std::uint8_t>& out) {
  std::uint32_t acc = 0U;
  int bits = 0;
  const std::uint32_t maxv = (1U << to_bits) - 1U;
  const std::uint32_t max_acc = (1U << (from_bits + to_bits - 1)) - 1U;

  for (const std::uint8_t value : input) {
    if ((value >> from_bits) != 0U) {
      return false;
    }
    acc = ((acc << from_bits) | value) & max_acc;
    bits += from_bits;
    while (bits >= to_bits) {
      bits -= to_bits;
      out.push_back(static_cast<std::uint8_t>((acc >> bits) & maxv));
    }
  }

  if (pad) {
    if (bits != 0) {
      out.push_back(static_cast<std::uint8_t>((acc << (to_bits - bits)) & maxv));
    }
  } else {
    if (bits >= from_bits) {
      return false;
    }
    if (((acc << (to_bits - bits)) & maxv) != 0U) {
      return false;
    }
  }

  return true;
}

bool decode_base58check(const std::string& text,
                        std::vector<std::uint8_t>& decoded,
                        std::string& error) {
  if (text.empty()) {
    error = "empty address";
    return false;
  }

  std::size_t leading_zeroes = 0;
  while (leading_zeroes < text.size() && text[leading_zeroes] == '1') {
    ++leading_zeroes;
  }

  std::vector<std::uint8_t> digits(((text.size() - leading_zeroes) * 733U) / 1000U + 1U, 0U);
  std::size_t length = 0;

  for (const char c : text) {
    const int value = base58_index(c);
    if (value < 0) {
      error = "invalid base58 character in address";
      return false;
    }

    int carry = value;
    std::size_t i = 0;
    for (auto it = digits.rbegin(); (carry != 0 || i < length) && it != digits.rend();
         ++it, ++i) {
      carry += 58 * (*it);
      *it = static_cast<std::uint8_t>(carry % 256);
      carry /= 256;
    }
    length = i;
    if (carry != 0) {
      error = "base58 decode overflow";
      return false;
    }
  }

  decoded.clear();
  decoded.insert(decoded.end(), leading_zeroes, 0U);
  if (length != 0) {
    decoded.insert(decoded.end(), digits.end() - static_cast<std::ptrdiff_t>(length),
                   digits.end());
  }

  if (decoded.size() < 5U) {
    error = "base58check address too short";
    return false;
  }

  const std::size_t payload_size = decoded.size() - 4U;
  std::array<std::uint8_t, 32> checksum{};
  sha256d(decoded.data(), payload_size, checksum);
  if (!std::equal(checksum.begin(), checksum.begin() + 4,
                  decoded.begin() + static_cast<std::ptrdiff_t>(payload_size))) {
    error = "base58check checksum mismatch";
    return false;
  }

  decoded.resize(payload_size);
  return true;
}

bool decode_bech32(const std::string& address,
                   std::vector<std::uint8_t>& script_pubkey,
                   std::string& error) {
  if (address.empty()) {
    error = "empty address";
    return false;
  }

  bool has_lower = false;
  bool has_upper = false;
  for (const unsigned char c : address) {
    if (c < 33U || c > 126U) {
      error = "invalid bech32 character";
      return false;
    }
    if (std::islower(c) != 0) {
      has_lower = true;
    }
    if (std::isupper(c) != 0) {
      has_upper = true;
    }
  }
  if (has_lower && has_upper) {
    error = "mixed-case bech32 address";
    return false;
  }

  std::string lower = address;
  std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) {
    return static_cast<char>(std::tolower(c));
  });

  const std::size_t sep = lower.rfind('1');
  if (sep == std::string::npos || sep < 1U || sep + 7U > lower.size()) {
    error = "invalid bech32 separator";
    return false;
  }

  const std::string hrp = lower.substr(0, sep);
  std::vector<std::uint8_t> values;
  values.reserve(lower.size() - sep - 1U);
  for (std::size_t i = sep + 1U; i < lower.size(); ++i) {
    const char* pos = std::strchr(kBech32Alphabet, lower[i]);
    if (pos == nullptr) {
      error = "invalid bech32 character";
      return false;
    }
    values.push_back(static_cast<std::uint8_t>(pos - kBech32Alphabet));
  }

  std::vector<std::uint8_t> polymod_input = bech32_hrp_expand(hrp);
  polymod_input.insert(polymod_input.end(), values.begin(), values.end());
  const std::uint32_t polymod = bech32_polymod(polymod_input);
  const bool is_bech32 = polymod == kBech32Constant;
  const bool is_bech32m = polymod == kBech32mConstant;
  if (!is_bech32 && !is_bech32m) {
    error = "bech32 checksum mismatch";
    return false;
  }

  const std::uint8_t version = values.front();
  if (version > 16U) {
    error = "unsupported witness version";
    return false;
  }

  std::vector<std::uint8_t> program_data;
  program_data.reserve(values.size() - 7U);
  for (std::size_t i = 1U; i + 6U < values.size(); ++i) {
    program_data.push_back(values[i]);
  }

  std::vector<std::uint8_t> program;
  if (!convert_bits(program_data, 5, 8, false, program)) {
    error = "invalid witness program encoding";
    return false;
  }

  if (version == 0U) {
    if (!is_bech32) {
      error = "witness v0 address must use bech32 checksum";
      return false;
    }
    if (program.size() != 20U && program.size() != 32U) {
      error = "witness v0 program must be 20 or 32 bytes";
      return false;
    }
  } else {
    if (!is_bech32m) {
      error = "witness v1+ address must use bech32m checksum";
      return false;
    }
    if (program.size() < 2U || program.size() > 40U) {
      error = "witness program length out of range";
      return false;
    }
  }

  script_pubkey.clear();
  script_pubkey.reserve(program.size() + 2U);
  script_pubkey.push_back(version == 0U ? 0x00U : static_cast<std::uint8_t>(0x50U + version));
  script_pubkey.push_back(static_cast<std::uint8_t>(program.size()));
  script_pubkey.insert(script_pubkey.end(), program.begin(), program.end());
  return true;
}

bool build_script_from_version(const std::vector<std::uint8_t>& payload,
                               const std::uint8_t version,
                               std::vector<std::uint8_t>& script_pubkey,
                               std::string& error) {
  if (payload.size() != 20U) {
    error = "unsupported base58 address payload size";
    return false;
  }

  script_pubkey.clear();
  script_pubkey.reserve(25U);

  if (version == 0x00U || version == 0x6fU) {
    script_pubkey.push_back(0x76U);
    script_pubkey.push_back(0xa9U);
    script_pubkey.push_back(0x14U);
    script_pubkey.insert(script_pubkey.end(), payload.begin(), payload.end());
    script_pubkey.push_back(0x88U);
    script_pubkey.push_back(0xACU);
    return true;
  }

  if (version == 0x05U || version == 0xc4U) {
    script_pubkey.push_back(0xa9U);
    script_pubkey.push_back(0x14U);
    script_pubkey.insert(script_pubkey.end(), payload.begin(), payload.end());
    script_pubkey.push_back(0x87U);
    return true;
  }

  error = "unsupported base58 address version";
  return false;
}

}  // namespace

bool decode_bitcoin_address_script(const std::string& address,
                                   std::vector<std::uint8_t>& script_pubkey,
                                   std::string& error) {
  std::vector<std::uint8_t> decoded;
  std::string normalized = address;
  std::transform(normalized.begin(), normalized.end(), normalized.begin(),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });

  if (normalized.rfind("bc1", 0) == 0 || normalized.rfind("tb1", 0) == 0 ||
      normalized.rfind("bcrt1", 0) == 0) {
    return decode_bech32(address, script_pubkey, error);
  }

  if (!decode_base58check(address, decoded, error)) {
    return false;
  }

  if (decoded.empty()) {
    error = "base58check address missing version byte";
    return false;
  }

  const std::uint8_t version = decoded.front();
  const std::vector<std::uint8_t> payload(decoded.begin() + 1, decoded.end());
  if (build_script_from_version(payload, version, script_pubkey, error)) {
    return true;
  }

  // Support exotic-but-valid bech32-style scriptPubKeys via RPC fallback later.
  return false;
}

}  // namespace miner
