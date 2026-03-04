#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace miner {

bool hex_to_bytes(const std::string& hex, std::vector<std::uint8_t>& out);
std::string bytes_to_hex(const std::uint8_t* data, std::size_t size);

void sha256(const std::uint8_t* data, std::size_t size,
            std::array<std::uint8_t, 32>& out);
void sha256d(const std::uint8_t* data, std::size_t size,
             std::array<std::uint8_t, 32>& out);

std::uint32_t load_be32(const std::uint8_t* p);
std::uint32_t load_le32(const std::uint8_t* p);
void store_le32(std::uint32_t value, std::uint8_t* p);

void reverse_bytes(std::vector<std::uint8_t>& bytes);
void reverse_bytes_32(std::array<std::uint8_t, 32>& bytes);

void build_midstate_from_header(const std::array<std::uint8_t, 80>& header,
                                std::uint32_t out_midstate[8],
                                std::uint32_t& tail0,
                                std::uint32_t& tail1,
                                std::uint32_t& tail2,
                                std::uint32_t& start_nonce_le);

}  // namespace miner
