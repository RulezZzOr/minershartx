#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace miner {

bool decode_bitcoin_address_script(const std::string& address,
                                   std::vector<std::uint8_t>& script_pubkey,
                                   std::string& error);

}  // namespace miner
