#include "pool_miner.h"

#include "cuda_scan.h"
#include "sha256_cpu.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <iomanip>
#include <iostream>
#include <limits>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#pragma comment(lib, "Ws2_32.lib")
#else
#include <arpa/inet.h>
#include <netdb.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace miner {
namespace {

using json = nlohmann::json;

#ifdef _WIN32
using SocketHandle = SOCKET;
constexpr SocketHandle kInvalidSocket = INVALID_SOCKET;
#else
using SocketHandle = int;
constexpr SocketHandle kInvalidSocket = -1;
#endif

struct Uint256 {
  std::uint32_t w[8] = {};
};

constexpr Uint256 kDiff1Target = {
    {0x00000000U, 0x00000000U, 0x00000000U, 0x00000000U,
     0x00000000U, 0x00000000U, 0xFFFF0000U, 0x00000000U}};

struct StratumJob {
  std::string job_id;
  std::vector<std::uint8_t> prevhash_le;
  std::vector<std::uint8_t> coinb1;
  std::vector<std::uint8_t> coinb2;
  std::vector<std::vector<std::uint8_t>> merkle_branch_le;
  std::array<std::uint8_t, 4> version_le{};
  std::array<std::uint8_t, 4> nbits_le{};
  std::array<std::uint8_t, 4> ntime_le{};
  bool clean_jobs = false;
};

struct GpuTelemetry {
  bool valid = false;
  int temperature_c = 0;
  double power_w = 0.0;
  int fan_percent = -1;
};

class SocketSystem {
 public:
  SocketSystem() {
#ifdef _WIN32
    WSADATA wsa{};
    ready_ = (WSAStartup(MAKEWORD(2, 2), &wsa) == 0);
#else
    ready_ = true;
#endif
  }

  ~SocketSystem() {
#ifdef _WIN32
    if (ready_) {
      WSACleanup();
    }
#endif
  }

  bool ready() const { return ready_; }

 private:
  bool ready_ = false;
};

class LineSocket {
 public:
  enum class ReadLineStatus {
    kLine,
    kTimeout,
    kClosed,
  };

  ~LineSocket() { close(); }

  bool connect_to(const std::string& host, const std::string& port,
                  std::string& error) {
    close();

    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;

    addrinfo* result = nullptr;
    const int gai = getaddrinfo(host.c_str(), port.c_str(), &hints, &result);
    if (gai != 0) {
#ifdef _WIN32
      error = "getaddrinfo failed with code " + std::to_string(WSAGetLastError());
#else
      error = gai_strerror(gai);
#endif
      return false;
    }

    for (addrinfo* rp = result; rp != nullptr; rp = rp->ai_next) {
      SocketHandle s =
          static_cast<SocketHandle>(::socket(rp->ai_family, rp->ai_socktype, rp->ai_protocol));
      if (s == kInvalidSocket) {
        continue;
      }

      if (::connect(s, rp->ai_addr, static_cast<int>(rp->ai_addrlen)) == 0) {
        sock_ = s;
        break;
      }

#ifdef _WIN32
      ::closesocket(s);
#else
      ::close(s);
#endif
    }

    freeaddrinfo(result);

    if (sock_ == kInvalidSocket) {
      error = "connection failed";
      return false;
    }

    return true;
  }

  void close() {
    if (sock_ != kInvalidSocket) {
#ifdef _WIN32
      ::closesocket(sock_);
#else
      ::close(sock_);
#endif
      sock_ = kInvalidSocket;
    }
    buffer_.clear();
  }

  bool send_line(const std::string& line) {
    if (sock_ == kInvalidSocket) {
      return false;
    }

    std::string payload = line;
    payload.push_back('\n');

    std::size_t sent = 0;
    while (sent < payload.size()) {
      const int n =
          ::send(sock_, payload.data() + sent, static_cast<int>(payload.size() - sent), 0);
      if (n <= 0) {
        return false;
      }
      sent += static_cast<std::size_t>(n);
    }

    return true;
  }

  ReadLineStatus read_line(int timeout_ms, std::string& out_line) {
    for (;;) {
      const auto pos = buffer_.find('\n');
      if (pos != std::string::npos) {
        out_line = buffer_.substr(0, pos);
        buffer_.erase(0, pos + 1);
        if (!out_line.empty() && out_line.back() == '\r') {
          out_line.pop_back();
        }
        return ReadLineStatus::kLine;
      }

      const int wait_status = wait_readable(timeout_ms);
      if (wait_status == 0) {
        return ReadLineStatus::kTimeout;
      }
      if (wait_status < 0) {
        return ReadLineStatus::kClosed;
      }

      char tmp[4096];
      const int n = ::recv(sock_, tmp, static_cast<int>(sizeof(tmp)), 0);
      if (n <= 0) {
        return ReadLineStatus::kClosed;
      }
      buffer_.append(tmp, static_cast<std::size_t>(n));

      // after the first recv call, keep draining without blocking
      timeout_ms = 0;
    }
  }

 private:
  int wait_readable(const int timeout_ms) {
    if (sock_ == kInvalidSocket) {
      return -1;
    }

    fd_set readfds;
    FD_ZERO(&readfds);
    FD_SET(sock_, &readfds);

    timeval tv{};
    timeval* tv_ptr = nullptr;

    if (timeout_ms >= 0) {
      tv.tv_sec = timeout_ms / 1000;
      tv.tv_usec = (timeout_ms % 1000) * 1000;
      tv_ptr = &tv;
    }

#ifdef _WIN32
    const int rv = ::select(0, &readfds, nullptr, nullptr, tv_ptr);
#else
    const int rv = ::select(sock_ + 1, &readfds, nullptr, nullptr, tv_ptr);
#endif
    if (rv < 0) {
      return -1;
    }
    if (rv == 0) {
      return 0;
    }
    return FD_ISSET(sock_, &readfds) ? 1 : -1;
  }

  SocketHandle sock_ = kInvalidSocket;
  std::string buffer_;
};

bool split_host_port(const std::string& endpoint,
                     std::string& host,
                     std::string& port) {
  const std::size_t pos = endpoint.rfind(':');
  if (pos == std::string::npos || pos == 0 || pos + 1 >= endpoint.size()) {
    return false;
  }

  host = endpoint.substr(0, pos);
  port = endpoint.substr(pos + 1);

  while(!port.empty() && (port.back() == '\r' || port.back() == '\n' || port.back() == ' ' || port.back() == '\t')) {
    port.pop_back();
  }
  while(!host.empty() && (host.back() == '\r' || host.back() == '\n' || host.back() == ' ' || host.back() == '\t')) {
    host.pop_back();
  }

  return true;
}

bool parse_u32_hex_bytes(const std::string& hex, std::array<std::uint8_t, 4>& out) {
  std::vector<std::uint8_t> tmp;
  if (!hex_to_bytes(hex, tmp) || tmp.size() != 4) {
    return false;
  }
  for (int i = 0; i < 4; ++i) {
    out[i] = tmp[static_cast<std::size_t>(i)];
  }
  return true;
}

bool parse_prevhash_stratum_bytes(const std::string& hex,
                                  std::vector<std::uint8_t>& out) {
  // The pools we target already emit prevhash in the byte order expected by
  // the block header, so keep the hex bytes as-is.
  return hex_to_bytes(hex, out) && out.size() == 32;
}

bool parse_notify(const json& msg, StratumJob& out_job) {
  if (!msg.contains("params") || !msg["params"].is_array()) {
    return false;
  }

  const auto& p = msg["params"];
  if (p.size() < 9) {
    return false;
  }

  if (!p[0].is_string() || !p[1].is_string() || !p[2].is_string() ||
      !p[3].is_string() || !p[4].is_array() || !p[5].is_string() ||
      !p[6].is_string() || !p[7].is_string() || !p[8].is_boolean()) {
    return false;
  }

  StratumJob job{};
  job.job_id = p[0].get<std::string>();

  if (!parse_prevhash_stratum_bytes(p[1].get<std::string>(), job.prevhash_le)) {
    return false;
  }

  if (!hex_to_bytes(p[2].get<std::string>(), job.coinb1) ||
      !hex_to_bytes(p[3].get<std::string>(), job.coinb2)) {
    return false;
  }

  for (const auto& branch : p[4]) {
    if (!branch.is_string()) {
      return false;
    }

    std::vector<std::uint8_t> branch_bytes;
    if (!hex_to_bytes(branch.get<std::string>(), branch_bytes) ||
        branch_bytes.size() != 32) {
      return false;
    }

    job.merkle_branch_le.push_back(std::move(branch_bytes));
  }

  // Stratum v1 provides these fields in Bitcoin header byte order already.
  if (!parse_u32_hex_bytes(p[5].get<std::string>(), job.version_le) ||
      !parse_u32_hex_bytes(p[6].get<std::string>(), job.nbits_le) ||
      !parse_u32_hex_bytes(p[7].get<std::string>(), job.ntime_le)) {
    return false;
  }

  job.clean_jobs = p[8].get<bool>();
  out_job = std::move(job);
  return true;
}

Uint256 target_from_difficulty(const double diff) {
  double bounded = diff;
  if (!std::isfinite(bounded) || bounded <= 0.0) {
    bounded = 1.0;
  }

  std::uint64_t integer_diff = static_cast<std::uint64_t>(std::floor(bounded));
  if (integer_diff == 0) {
    integer_diff = 1;
  }

  Uint256 q{};
  std::uint64_t rem = 0;
  for (int i = 7; i >= 0; --i) {
    const std::uint64_t cur = (rem << 32) | static_cast<std::uint64_t>(kDiff1Target.w[i]);
    q.w[i] = static_cast<std::uint32_t>(cur / integer_diff);
    rem = cur % integer_diff;
  }
  return q;
}

std::string format_nonce_submit_hex(const std::uint32_t nonce, const bool submit_be) {
  std::array<std::uint8_t, 4> nonce_bytes{};
  if (submit_be) {
    nonce_bytes[0] = static_cast<std::uint8_t>((nonce >> 24) & 0xFFU);
    nonce_bytes[1] = static_cast<std::uint8_t>((nonce >> 16) & 0xFFU);
    nonce_bytes[2] = static_cast<std::uint8_t>((nonce >> 8) & 0xFFU);
    nonce_bytes[3] = static_cast<std::uint8_t>(nonce & 0xFFU);
  } else {
    store_le32(nonce, nonce_bytes.data());
  }
  return bytes_to_hex(nonce_bytes.data(), nonce_bytes.size());
}

bool hash_be_meets_target(const std::array<std::uint8_t, 32>& hash_be,
                          const Uint256& target_le) {
  // Compare as a big-endian 256-bit integer, matching the hash byte order
  // produced by sha256d() and the target word order used in the kernel.
  for (int i = 0; i < 8; ++i) {
    const std::uint32_t hash_word_be =
        load_be32(hash_be.data() + static_cast<std::size_t>(i) * 4U);
    const std::uint32_t target_word_be = target_le.w[7 - i];
    if (hash_word_be < target_word_be) {
      return true;
    }
    if (hash_word_be > target_word_be) {
      return false;
    }
  }
  return true;
}

std::vector<std::uint8_t> make_extranonce2_bytes(const int size,
                                                 const std::uint64_t counter) {
  std::vector<std::uint8_t> out(static_cast<std::size_t>(size), 0U);
  std::uint64_t x = counter;
  for (int i = size - 1; i >= 0; --i) {
    out[static_cast<std::size_t>(i)] = static_cast<std::uint8_t>(x & 0xFFU);
    x >>= 8;
  }
  return out;
}

bool build_work_header(const StratumJob& job,
                       const std::vector<std::uint8_t>& extranonce1,
                       const int extranonce2_size,
                       const std::uint64_t extranonce2_counter,
                       std::array<std::uint8_t, 32>& merkle_header_out,
                       std::array<std::uint8_t, 80>& header,
                       std::string& extranonce2_hex,
                       std::string& ntime_submit_hex,
                       std::string& nonce_submit_hex) {
  const auto extranonce2 = make_extranonce2_bytes(extranonce2_size, extranonce2_counter);
  extranonce2_hex = bytes_to_hex(extranonce2.data(), extranonce2.size());

  std::vector<std::uint8_t> coinbase;
  coinbase.reserve(job.coinb1.size() + extranonce1.size() + extranonce2.size() +
                   job.coinb2.size());
  coinbase.insert(coinbase.end(), job.coinb1.begin(), job.coinb1.end());
  coinbase.insert(coinbase.end(), extranonce1.begin(), extranonce1.end());
  coinbase.insert(coinbase.end(), extranonce2.begin(), extranonce2.end());
  coinbase.insert(coinbase.end(), job.coinb2.begin(), job.coinb2.end());

  std::array<std::uint8_t, 32> merkle{};
  sha256d(coinbase.data(), coinbase.size(), merkle);

  // Stratum v1 delivers header bytes in the order expected for block-header
  // serialization. The merkle tree uses raw hash bytes during construction,
  // and only the final root is reversed for inclusion in the header.
  for (const auto& branch_le : job.merkle_branch_le) {
    std::array<std::uint8_t, 64> concat{};
    std::memcpy(concat.data(), merkle.data(), 32);
    std::memcpy(concat.data() + 32, branch_le.data(), 32);
    sha256d(concat.data(), concat.size(), merkle);
  }

  std::array<std::uint8_t, 32> merkle_header = merkle;
  reverse_bytes_32(merkle_header);
  merkle_header_out = merkle_header;

  header.fill(0);
  std::memcpy(header.data() + 0, job.version_le.data(), 4);
  std::memcpy(header.data() + 4, job.prevhash_le.data(), 32);
  std::memcpy(header.data() + 36, merkle_header.data(), 32);
  std::memcpy(header.data() + 68, job.ntime_le.data(), 4);
  std::memcpy(header.data() + 72, job.nbits_le.data(), 4);
  std::memset(header.data() + 76, 0, 4);

  ntime_submit_hex = bytes_to_hex(job.ntime_le.data(), job.ntime_le.size());
  nonce_submit_hex = "00000000";
  return true;
}

std::string make_json_request(const int id,
                              const std::string& method,
                              const json& params) {
  json req;
  req["id"] = id;
  req["method"] = method;
  req["params"] = params;
  return req.dump();
}

std::string trim_ascii(const std::string& value) {
  std::size_t begin = 0;
  while (begin < value.size() &&
         (value[begin] == ' ' || value[begin] == '\t' ||
          value[begin] == '\r' || value[begin] == '\n')) {
    ++begin;
  }

  std::size_t end = value.size();
  while (end > begin &&
         (value[end - 1] == ' ' || value[end - 1] == '\t' ||
          value[end - 1] == '\r' || value[end - 1] == '\n')) {
    --end;
  }

  return value.substr(begin, end - begin);
}

bool parse_int_value(const std::string& value, int& out) {
  try {
    std::size_t consumed = 0;
    const int parsed = std::stoi(value, &consumed, 10);
    if (consumed != value.size()) {
      return false;
    }
    out = parsed;
    return true;
  } catch (...) {
    return false;
  }
}

bool parse_double_value(const std::string& value, double& out) {
  try {
    std::size_t consumed = 0;
    const double parsed = std::stod(value, &consumed);
    if (consumed != value.size()) {
      return false;
    }
    out = parsed;
    return true;
  } catch (...) {
    return false;
  }
}

bool is_target_reject_error(const nlohmann::json& error_value,
                            const std::string& error_text) {
  if (error_value.is_array() && !error_value.empty() && error_value[0].is_number_integer()) {
    if (error_value[0].get<int>() == 23) {
      return true;
    }
  }
  if (error_value.is_object() && error_value.contains("code") &&
      error_value["code"].is_number_integer() && error_value["code"].get<int>() == 23) {
    return true;
  }

  return error_text.find("Above target") != std::string::npos ||
         error_text.find("low difficulty share") != std::string::npos ||
         error_text.find("Difficulty too low") != std::string::npos;
}

bool read_gpu_telemetry(const int device, GpuTelemetry& out) {
  out = GpuTelemetry{};

  std::ostringstream cmd;
  cmd << "nvidia-smi --query-gpu=temperature.gpu,power.draw,fan.speed "
         "--format=csv,noheader,nounits -i "
      << device;
#ifdef _WIN32
  cmd << " 2>nul";
  FILE* pipe = _popen(cmd.str().c_str(), "r");
#else
  cmd << " 2>/dev/null";
  FILE* pipe = popen(cmd.str().c_str(), "r");
#endif
  if (pipe == nullptr) {
    return false;
  }

  char buffer[256];
  std::string line;
  if (std::fgets(buffer, static_cast<int>(sizeof(buffer)), pipe) != nullptr) {
    line = buffer;
  }

#ifdef _WIN32
  const int close_status = _pclose(pipe);
#else
  const int close_status = pclose(pipe);
#endif
  if (close_status != 0 || line.empty()) {
    return false;
  }

  std::array<std::string, 3> fields{};
  int field_count = 0;
  {
    std::stringstream ss(line);
    std::string field;
    while (std::getline(ss, field, ',') && field_count < 3) {
      fields[static_cast<std::size_t>(field_count)] = trim_ascii(field);
      ++field_count;
    }
  }

  if (field_count < 2) {
    return false;
  }

  int temp_c = 0;
  double power_w = 0.0;
  if (!parse_int_value(fields[0], temp_c) ||
      !parse_double_value(fields[1], power_w)) {
    return false;
  }

  out.valid = true;
  out.temperature_c = temp_c;
  out.power_w = power_w;

  int fan_pct = 0;
  if (field_count >= 3 && parse_int_value(fields[2], fan_pct)) {
    out.fan_percent = fan_pct;
  }

  return true;
}

std::string format_uptime_hms(const std::uint64_t seconds_total) {
  const std::uint64_t hours = seconds_total / 3600ULL;
  const std::uint64_t minutes = (seconds_total % 3600ULL) / 60ULL;
  const std::uint64_t seconds = seconds_total % 60ULL;

  std::ostringstream oss;
  oss << std::setfill('0') << std::setw(2) << hours << ":" << std::setw(2)
      << minutes << ":" << std::setw(2) << seconds;
  return oss.str();
}

std::string format_compact_diff(double diff) {
  if (!std::isfinite(diff) || diff <= 0.0) {
    return "-";
  }

  static constexpr struct {
    double scale;
    const char* suffix;
  } kUnits[] = {
      {1e12, "T"},
      {1e9, "G"},
      {1e6, "M"},
      {1e3, "K"},
  };

  double value = diff;
  const char* suffix = "";
  for (const auto& unit : kUnits) {
    if (value >= unit.scale) {
      value /= unit.scale;
      suffix = unit.suffix;
      break;
    }
  }

  std::ostringstream oss;
  if (value >= 100.0) {
    oss << std::fixed << std::setprecision(0);
  } else if (value >= 10.0) {
    oss << std::fixed << std::setprecision(1);
  } else {
    oss << std::fixed << std::setprecision(2);
  }
  oss << value << suffix;
  return oss.str();
}

std::string right_trim_to_width(const std::string& value, const std::size_t width) {
  if (value.size() <= width) {
    return value;
  }
  return value.substr(value.size() - width);
}

void print_pool_header_debug(const char* phase,
                             const int device,
                             const StratumJob& job,
                             const std::array<std::uint8_t, 80>& header,
                             const std::array<std::uint8_t, 32>& merkle_header,
                             const std::string& ex2_hex,
                             const std::string& ntime_hex,
                             const std::string& nonce_submit_hex,
                             const double diff,
                             const bool nonce_submit_be) {
  std::cout << "[pool-debug] " << phase
            << " GPU " << device
            << " job=" << job.job_id
            << " diff=" << format_compact_diff(diff)
            << " submit_mode=" << (nonce_submit_be ? "BE" : "LE")
            << " clean=" << (job.clean_jobs ? "true" : "false")
            << " branches=" << job.merkle_branch_le.size()
            << " ex2=" << ex2_hex
            << " ntime=" << ntime_hex
            << " nonce_submit=" << nonce_submit_hex
            << "\n";
  std::cout << "[pool-debug] job_fields prevhash="
            << bytes_to_hex(job.prevhash_le.data(), job.prevhash_le.size())
            << " version=" << bytes_to_hex(job.version_le.data(), job.version_le.size())
            << " nbits=" << bytes_to_hex(job.nbits_le.data(), job.nbits_le.size())
            << " ntime=" << bytes_to_hex(job.ntime_le.data(), job.ntime_le.size())
            << " coinb1_len=" << job.coinb1.size()
            << " coinb2_len=" << job.coinb2.size()
            << "\n";
  if (!job.merkle_branch_le.empty()) {
    std::cout << "[pool-debug] first_branch="
              << bytes_to_hex(job.merkle_branch_le.front().data(),
                              job.merkle_branch_le.front().size())
              << "\n";
  }
  std::cout << "[pool-debug] fields version="
            << bytes_to_hex(header.data(), 4)
            << " prevhash=" << bytes_to_hex(header.data() + 4, 32)
            << " merkle=" << bytes_to_hex(merkle_header.data(), 32)
            << " ntime=" << bytes_to_hex(header.data() + 68, 4)
            << " nbits=" << bytes_to_hex(header.data() + 72, 4)
            << " nonce=" << bytes_to_hex(header.data() + 76, 4)
            << "\n";
  std::cout << "[pool-debug] header="
            << bytes_to_hex(header.data(), header.size())
            << "\n";
}

void print_pool_stats_header() {
  std::cout
      << "------------------------------------------------------------------------------------------------------------------------\n"
      << "  Uptime  |  5s GH/s | Avg GH/s | TempC | PowerW | J/GH |   kWh  | Fan% | Shares A/R/D | Reject |   Diff   |    Ex2     | Job\n"
      << "------------------------------------------------------------------------------------------------------------------------\n";
}

}  // namespace

int run_pool_miner(const PoolConfig& config) {
  if (config.devices.empty()) {
    std::cerr << "No devices specified for pool mining\n";
    return 1;
  }

  SocketSystem socket_system;
  if (!socket_system.ready()) {
    std::cerr << "Failed to initialize socket system.\n";
    return 1;
  }

  std::string host;
  std::string port;
  if (!split_host_port(config.pool, host, port)) {
    std::cerr << "Invalid --pool endpoint format. Expected host:port\n";
    return 1;
  }

  LineSocket sock;
  std::string connect_error;
  if (!sock.connect_to(host, port, connect_error)) {
    std::cerr << "Unable to connect to " << config.pool << ": " << connect_error
              << "\n";
    return 1;
  }

  std::cout << "Connected to " << config.pool << "\n";

  struct SharedJob {
    StratumJob job;
    std::vector<std::uint8_t> extranonce1;
    int extranonce2_size = 0;
    double diff = 1.0;
    Uint256 share_target{};
    std::uint64_t generation = 0;
    bool valid = false;
  };

  std::mutex job_mutex;
  std::condition_variable job_cv;
  SharedJob shared_job;
  if (config.requested_diff > 0.0) {
    shared_job.diff = config.requested_diff;
  }
  shared_job.share_target = target_from_difficulty(shared_job.diff);
  std::atomic<bool> stop_flag{false};

  std::mutex socket_mutex;
  std::uint64_t submit_id = 1000;
  struct PendingSubmit {
    std::string job_id;
    std::string ex2_hex;
    std::string ntime_hex;
    std::uint32_t nonce = 0;
    bool nonce_be = false;
    int attempt = 1;
    std::uint64_t generation = 0;
  };
  std::unordered_map<int, PendingSubmit> pending_submits;
  std::uint64_t share_accepts = 0;
  std::uint64_t share_rejects = 0;
  std::uint64_t share_discarded = 0;
  bool active_nonce_submit_be = config.submit_nonce_be;
  bool autodetect_nonce_mode_reported = false;
  bool warned_nonce_endian = false;
  std::atomic<bool> socket_needs_reconnect{false};

  std::atomic<std::uint64_t> global_hashes{0};

  std::vector<std::thread> gpu_threads;
  std::atomic<int> engine_init_errors{0};

  for (std::size_t i = 0; i < config.devices.size(); ++i) {
    const int device = config.devices[i];
    const std::uint32_t ex2_prefix = static_cast<std::uint32_t>(i);

    gpu_threads.emplace_back([&, device, ex2_prefix]() {
      ScanEngineConfig engine_cfg{};
      engine_cfg.device = device;
      engine_cfg.blocks = config.blocks;
      engine_cfg.threads = config.threads;

      ScanEngine* raw_engine = nullptr;
      if (create_scan_engine(engine_cfg, raw_engine) != 0) {
        engine_init_errors++;
        return;
      }
      std::unique_ptr<ScanEngine, decltype(&destroy_scan_engine)> engine(
          raw_engine, &destroy_scan_engine);

      ScanEngineInfo info{};
      if (get_scan_engine_info(engine.get(), info) == 0) {
        std::lock_guard<std::mutex> lock(socket_mutex);
        std::cout << "CUDA: device=" << info.device << " name=" << info.name
                  << " sms=" << info.sms << " blocks=" << info.blocks
                  << " threads=" << info.threads << "\n";
      }

      const bool chunk_from_user = config.chunk_nonces != 0;
      std::uint64_t tuned_chunk = config.chunk_nonces;
      if (!chunk_from_user && tuned_chunk == 0) {
        tuned_chunk = static_cast<std::uint64_t>(info.blocks) *
                      static_cast<std::uint64_t>(info.threads) * 1024ULL;
      }
      if (!chunk_from_user && tuned_chunk == 0) {
        tuned_chunk = static_cast<std::uint64_t>(config.threads) * 1024ULL * 64ULL;
      }
      if (!chunk_from_user) {
        if (tuned_chunk < 16ULL * 1024ULL * 1024ULL) tuned_chunk = 16ULL * 1024ULL * 1024ULL;
        if (tuned_chunk > (1ULL << 31)) tuned_chunk = (1ULL << 31);
      } else if (tuned_chunk > (1ULL << 32)) {
        tuned_chunk = (1ULL << 32);
      }

      {
        std::lock_guard<std::mutex> lock(socket_mutex);
        std::cout << "Device " << device << " chunk nonces: " << tuned_chunk
                  << (chunk_from_user ? " (manual)\n" : " (auto)\n");
      }

      std::string current_job_id;
      std::array<std::uint8_t, 80> current_header{};
      std::uint32_t current_midstate[8] = {};
      std::uint32_t current_tail0 = 0;
      std::uint32_t current_tail1 = 0;
      std::uint32_t current_tail2 = 0;
      std::string current_ex2_hex;
      std::string current_ntime_hex;
      std::string current_nonce_hex;
      std::array<std::uint8_t, 32> current_merkle_header{};
      Uint256 current_target{};
      double current_diff = 0.0;
      std::uint64_t current_generation = 0;

      std::uint32_t scanned_in_space = 0;
      std::uint32_t start_nonce = 0;
      std::uint32_t base_ex2 = 0;

      while (!stop_flag.load(std::memory_order_relaxed)) {
        SharedJob local_job;
        {
          std::unique_lock<std::mutex> lock(job_mutex);
          job_cv.wait_for(lock, std::chrono::milliseconds(100), [&]() { return shared_job.valid; });
          if (!shared_job.valid || stop_flag.load(std::memory_order_relaxed)) {
            continue;
          }
          local_job = shared_job;
        }

        if (local_job.job.job_id != current_job_id) {
          current_job_id = local_job.job.job_id;
          scanned_in_space = 0;
          start_nonce = 0;
          base_ex2 = 0;
        }
        current_generation = local_job.generation;

        if (scanned_in_space == 0) {
          // to avoid collisions, we shift the prefix into the upper bits of ex2
          // assume extranonce2_size is at least 4. If it's smaller, we just rely on base_ex2.
          std::uint64_t full_ex2 = base_ex2;
          if (local_job.extranonce2_size >= 4) {
             full_ex2 = (static_cast<std::uint64_t>(ex2_prefix) << 24) | base_ex2;
          } else {
             full_ex2 = (static_cast<std::uint64_t>(ex2_prefix) << 16) | base_ex2;
          }

          if (!build_work_header(local_job.job, local_job.extranonce1, local_job.extranonce2_size,
                                 full_ex2, current_merkle_header, current_header, current_ex2_hex,
                                 current_ntime_hex, current_nonce_hex)) {
            continue;
          }
          std::uint32_t dummy_nonce = 0;
          build_midstate_from_header(current_header, current_midstate, current_tail0,
                                     current_tail1, current_tail2, dummy_nonce);
          if (config.debug_pool_header) {
            std::lock_guard<std::mutex> lock(socket_mutex);
            print_pool_header_debug("base", device, local_job.job, current_header,
                                    current_merkle_header, current_ex2_hex,
                                    current_ntime_hex, current_nonce_hex,
                                    local_job.diff, active_nonce_submit_be);
          }
        }

        current_target = local_job.share_target;
        current_diff = local_job.diff;

        const std::uint64_t remaining = 4294967296ULL - scanned_in_space;
        const std::uint64_t to_scan = std::min(tuned_chunk, remaining);

        ScanWork scan{};
        scan.nonce_count = to_scan;
        scan.start_nonce = start_nonce;
        scan.tail0 = current_tail0;
        scan.tail1 = current_tail1;
        scan.tail2 = current_tail2;
        for (int i = 0; i < 8; ++i) {
          scan.midstate[i] = current_midstate[i];
          scan.target[i] = current_target.w[i];
        }

        ScanResult scan_result{};
        const int scan_status = run_cuda_scan(engine.get(), scan, scan_result);
        if (scan_status != 0) {
          std::lock_guard<std::mutex> lock(socket_mutex);
          std::cerr << "GPU " << device << " scan failed!\n";
          break;
        }

        global_hashes.fetch_add(to_scan, std::memory_order_relaxed);
        scanned_in_space += static_cast<std::uint32_t>(to_scan);
        start_nonce += static_cast<std::uint32_t>(to_scan);

        if (scan_result.found) {
          std::array<std::uint8_t, 80> verify_header = current_header;
          store_le32(scan_result.nonce, verify_header.data() + 76U);
          const std::string verify_nonce_hex =
              format_nonce_submit_hex(scan_result.nonce, active_nonce_submit_be);

          if (config.debug_pool_header) {
            std::lock_guard<std::mutex> lock(socket_mutex);
            print_pool_header_debug("submit", device, local_job.job, verify_header,
                                    current_merkle_header, current_ex2_hex,
                                    current_ntime_hex, verify_nonce_hex,
                                    current_diff, active_nonce_submit_be);
          }

          std::array<std::uint8_t, 32> verify_hash_be{};
          sha256d(verify_header.data(), verify_header.size(), verify_hash_be);
          const bool cpu_valid = hash_be_meets_target(verify_hash_be, current_target);
          const std::string verify_hash_hex = bytes_to_hex(verify_hash_be.data(), verify_hash_be.size());
          bool stale_share = false;
          {
            std::lock_guard<std::mutex> job_lock(job_mutex);
            stale_share = !shared_job.valid || shared_job.generation != current_generation;
          }

          std::lock_guard<std::mutex> lock(socket_mutex);
          if (!cpu_valid) {
            ++share_discarded;
            std::cout << "GPU " << device << " false-positive dropped nonce=0x" << std::hex
                      << std::nouppercase << scan_result.nonce << std::dec
                      << " cpu_hash=" << verify_hash_hex << "\n";
          } else if (stale_share) {
            ++share_discarded;
            std::cout << "GPU " << device << " stale share discarded nonce=0x" << std::hex
                      << std::nouppercase << scan_result.nonce << std::dec
                      << " cpu_hash=" << verify_hash_hex << "\n";
          } else {
            const bool nonce_submit_be = active_nonce_submit_be;
            current_nonce_hex = verify_nonce_hex;

            const int request_id = static_cast<int>(submit_id++);
            json params = json::array({config.user, current_job_id, current_ex2_hex,
                                       current_ntime_hex, current_nonce_hex});
            if (!sock.send_line(make_json_request(request_id, "mining.submit", params))) {
              socket_needs_reconnect.store(true, std::memory_order_relaxed);
              continue;
            }

            pending_submits.emplace(
                request_id,
                PendingSubmit{current_job_id, current_ex2_hex, current_ntime_hex,
                              scan_result.nonce, nonce_submit_be, 1, current_generation});

            std::cout << "GPU " << device << " Share found nonce=" << current_nonce_hex
                      << " hash=" << verify_hash_hex
                      << " submit_id=" << request_id << "\n";
          }
        }

        if (scanned_in_space == 0) {
          ++base_ex2;
        }
      }
    });
  }

  if (engine_init_errors > 0) {
    stop_flag.store(true);
    for (auto& t : gpu_threads) t.join();
    return 1;
  }

  {
    std::lock_guard<std::mutex> lock(socket_mutex);
    if (!sock.send_line(make_json_request(1, "mining.subscribe", json::array({"minershartx/0.2"})))) {
      std::cerr << "Failed to send mining.subscribe\n";
      stop_flag.store(true);
      for (auto& t : gpu_threads) t.join();
      return 1;
    }
  }

  bool got_subscribe = false;
  bool got_authorize = false;
  bool suggested_difficulty_sent = false;

  auto hashrate_t0 = std::chrono::steady_clock::now();
  auto hashrate_last = hashrate_t0;
  std::uint64_t last_report_global_hashes = 0;
  int stats_rows_printed = 0;

  std::vector<GpuTelemetry> telemetries(config.devices.size());
  auto telemetry_last = hashrate_t0;
  double energy_kwh = 0.0;
  std::uint32_t reconnect_backoff_s = 1;

  auto invalidate_shared_job = [&]() {
    std::lock_guard<std::mutex> lock(job_mutex);
    shared_job.valid = false;
    ++shared_job.generation;
    job_cv.notify_all();
  };

  auto clear_pending_submits = [&]() {
    std::lock_guard<std::mutex> lock(socket_mutex);
    pending_submits.clear();
  };

  auto reconnect_session = [&]() -> bool {
    invalidate_shared_job();
    clear_pending_submits();
    sock.close();

    std::this_thread::sleep_for(std::chrono::seconds(reconnect_backoff_s));

    std::string reconnect_error;
    if (!sock.connect_to(host, port, reconnect_error)) {
      std::cerr << "Reconnect to " << config.pool << " failed: " << reconnect_error
                << " (retrying in " << reconnect_backoff_s << " s)\n";
      reconnect_backoff_s = std::min<std::uint32_t>(reconnect_backoff_s * 2U, 60U);
      return false;
    }

    {
      std::lock_guard<std::mutex> slock(socket_mutex);
      std::cout << "Reconnected to " << config.pool << "\n";
    }

    got_subscribe = false;
    got_authorize = false;
    suggested_difficulty_sent = false;
    reconnect_backoff_s = 1;
    socket_needs_reconnect.store(false, std::memory_order_relaxed);

    if (!sock.send_line(make_json_request(1, "mining.subscribe",
                                          json::array({"minershartx/0.2"})))) {
      std::cerr << "Failed to send mining.subscribe after reconnect\n";
      socket_needs_reconnect.store(true, std::memory_order_relaxed);
      reconnect_backoff_s = std::min<std::uint32_t>(reconnect_backoff_s * 2U, 60U);
      return false;
    }

    return true;
  };

  for (;;) {
    if (socket_needs_reconnect.load(std::memory_order_relaxed)) {
      (void)reconnect_session();
      continue;
    }

    std::string line;
    bool has_job = false;
    {
       std::lock_guard<std::mutex> lock(job_mutex);
       has_job = shared_job.valid;
    }

    const int read_timeout_ms = (!got_subscribe || !got_authorize || !has_job) ? 1000 : 50;
    const auto read_status = sock.read_line(read_timeout_ms, line);
    if (read_status == LineSocket::ReadLineStatus::kLine) {
      json msg = json::parse(line, nullptr, false);
      if (msg.is_discarded() || !msg.is_object()) {
        continue;
      }

      if (msg.contains("id") && !msg["id"].is_null()) {
        const int id = msg["id"].is_number_integer() ? msg["id"].get<int>() : -1;

        if (id == 1) {
          if (!msg.contains("result") || !msg["result"].is_array() ||
              msg["result"].size() < 3 || !msg["result"][1].is_string() ||
              !msg["result"][2].is_number_integer()) {
            std::cerr << "Invalid mining.subscribe response\n";
            break;
          }

          std::vector<std::uint8_t> ex1;
          if (!hex_to_bytes(msg["result"][1].get<std::string>(), ex1)) {
            std::cerr << "Invalid extranonce1 in subscribe response\n";
            break;
          }
          int ex2_size = msg["result"][2].get<int>();
          if (ex2_size <= 0 || ex2_size > 16) {
            std::cerr << "Unsupported extranonce2_size=" << ex2_size << "\n";
            break;
          }

          {
              std::lock_guard<std::mutex> lock(job_mutex);
              shared_job.extranonce1 = ex1;
              shared_job.extranonce2_size = ex2_size;
          }

          got_subscribe = true;
          std::lock_guard<std::mutex> slock(socket_mutex);
          std::cout << "Subscribed. extranonce1="
                    << bytes_to_hex(ex1.data(), ex1.size())
                    << " extranonce2_size=" << ex2_size << "\n";

          if (!sock.send_line(
                  make_json_request(2, "mining.authorize", json::array({config.user, config.pass})))) {
            std::cerr << "Failed to send mining.authorize\n";
            socket_needs_reconnect.store(true, std::memory_order_relaxed);
            break;
          }
          continue;
        }

        if (id == 2) {
          const bool ok = msg.contains("result") && msg["result"].is_boolean() && msg["result"].get<bool>();
          if (!ok) {
            std::cerr << "Authorization rejected by pool\n";
            break;
          }
          got_authorize = true;
          std::lock_guard<std::mutex> slock(socket_mutex);
          std::cout << "Authorized as " << config.user << "\n";

          if (config.requested_diff > 0.0 && !suggested_difficulty_sent) {
            const int suggest_id = 3;
            if (!sock.send_line(make_json_request(
                    suggest_id, "mining.suggest_difficulty",
                    json::array({config.requested_diff})))) {
              std::cerr << "Failed to send mining.suggest_difficulty\n";
              socket_needs_reconnect.store(true, std::memory_order_relaxed);
              break;
            }
            suggested_difficulty_sent = true;
            std::cout << "Requested pool difficulty: " << config.requested_diff << "\n";
          }
          continue;
        }

        if (id >= 1000) {
          std::string reject_error_text;
          nlohmann::json reject_error_value;
          bool have_reject_error_value = false;
          if (msg.contains("error") && !msg["error"].is_null()) {
            reject_error_value = msg["error"];
            have_reject_error_value = true;
            reject_error_text = reject_error_value.dump();
          }

          PendingSubmit pending{};
          bool have_pending = false;
          std::uint64_t current_generation = 0;

          {
            std::lock_guard<std::mutex> job_lock(job_mutex);
            current_generation = shared_job.generation;
          }

          std::lock_guard<std::mutex> slock(socket_mutex);
          auto pending_it = pending_submits.find(id);
          if (pending_it != pending_submits.end()) {
            pending = pending_it->second;
            pending_submits.erase(pending_it);
            have_pending = true;
          }

          const bool accepted = msg.contains("result") && msg["result"].is_boolean() &&
                                msg["result"].get<bool>();
          if (accepted) {
            ++share_accepts;
          } else {
            const bool likely_discard =
                reject_error_text.find("stale") != std::string::npos ||
                reject_error_text.find("job not found") != std::string::npos ||
                reject_error_text.find("duplicate") != std::string::npos ||
                (have_pending && pending.generation != current_generation);
            if (likely_discard) {
              ++share_discarded;
            } else {
              ++share_rejects;
            }
          }

          std::cout << "Share submit id=" << id << " => "
                    << (accepted ? "ACCEPTED" : "REJECTED");

          if (!accepted && !reject_error_text.empty()) {
            std::cout << " error=" << reject_error_text;
          }

          std::cout << " (accepted=" << share_accepts
                    << " rejected=" << share_rejects
                    << " discarded=" << share_discarded << ")\n";

          if (accepted && have_pending) {
            const bool changed_mode = (active_nonce_submit_be != pending.nonce_be);
            active_nonce_submit_be = pending.nonce_be;
            if (changed_mode || !autodetect_nonce_mode_reported) {
              std::cout << "Auto-detected nonce submit mode: "
                        << (active_nonce_submit_be ? "BE" : "LE") << "\n";
              autodetect_nonce_mode_reported = true;
            }
          }

          if (!accepted && have_pending && pending.attempt == 1 &&
              share_accepts == 0) {
            const bool likely_target_reject =
                (have_reject_error_value &&
                 is_target_reject_error(reject_error_value, reject_error_text)) ||
                reject_error_text.empty();
            if (likely_target_reject) {
              const bool retry_nonce_be = !pending.nonce_be;
              const std::string retry_nonce_hex =
                  format_nonce_submit_hex(pending.nonce, retry_nonce_be);
              const int retry_id = static_cast<int>(submit_id++);
              const json retry_params =
                  json::array({config.user, pending.job_id, pending.ex2_hex,
                               pending.ntime_hex, retry_nonce_hex});
              if (!sock.send_line(
                      make_json_request(retry_id, "mining.submit", retry_params))) {
                std::cerr << "Failed to submit share retry\n";
                socket_needs_reconnect.store(true, std::memory_order_relaxed);
                break;
              }

              PendingSubmit retry = pending;
              retry.nonce_be = retry_nonce_be;
              retry.attempt = 2;
              pending_submits.emplace(retry_id, std::move(retry));

              std::cout << "Auto nonce-endian retry submit_id=" << retry_id
                        << " nonce=" << retry_nonce_hex
                        << " mode=" << (retry_nonce_be ? "BE" : "LE") << "\n";
            }
          }

          if (!accepted && !warned_nonce_endian && share_accepts == 0 &&
              share_rejects >= 2) {
            warned_nonce_endian = true;
            if (active_nonce_submit_be) {
              std::cout << "Hint: remove --nonce-submit-be (this pool likely expects nonce in little-endian text)\n";
            } else {
              std::cout << "Hint: try --nonce-submit-be (some pools expect nonce in big-endian text)\n";
            }
          }
          continue;
        }
      }

      if (msg.contains("method") && msg["method"].is_string()) {
        const std::string method = msg["method"].get<std::string>();

        if (method == "mining.set_difficulty") {
          if (msg.contains("params") && msg["params"].is_array() &&
              !msg["params"].empty() && msg["params"][0].is_number()) {
            const double pool_diff = msg["params"][0].get<double>();
            const double effective_diff =
                (config.requested_diff > 0.0 && pool_diff < config.requested_diff)
                    ? config.requested_diff
                    : pool_diff;

            std::lock_guard<std::mutex> lock(job_mutex);
            shared_job.diff = effective_diff;
            shared_job.share_target = target_from_difficulty(effective_diff);
            job_cv.notify_all();

            std::lock_guard<std::mutex> slock(socket_mutex);
            std::cout << "Difficulty update: pool=" << pool_diff;
            if (effective_diff != pool_diff) {
              std::cout << " effective=" << effective_diff
                        << " (floor=" << config.requested_diff << ")";
            }
            std::cout << "\n";
          }
          continue;
        }

        if (method == "mining.notify") {
          StratumJob next_job;
          if (parse_notify(msg, next_job)) {
            std::lock_guard<std::mutex> slock(socket_mutex);
            std::cout << "New job: id=" << next_job.job_id
                      << " clean=" << (next_job.clean_jobs ? "true" : "false")
                      << " branches=" << next_job.merkle_branch_le.size() << "\n";

            std::lock_guard<std::mutex> lock(job_mutex);
            shared_job.job = std::move(next_job);
            ++shared_job.generation;
            shared_job.valid = true;
            job_cv.notify_all();
          }
          continue;
        }
      }
    }

    if (read_status == LineSocket::ReadLineStatus::kClosed) {
      socket_needs_reconnect.store(true, std::memory_order_relaxed);
    }

    if (socket_needs_reconnect.load(std::memory_order_relaxed)) {
      (void)reconnect_session();
      continue;
    }

    if (!got_subscribe || !got_authorize || !has_job) {
      continue;
    }

    const auto now = std::chrono::steady_clock::now();
    const double report_s = std::chrono::duration<double>(now - hashrate_last).count();

    if (report_s >= 5.0) {
      const double total_elapsed = std::chrono::duration<double>(now - hashrate_t0).count();
      const std::uint64_t g_hashes = global_hashes.load(std::memory_order_relaxed);
      const double gh_avg = static_cast<double>(g_hashes) / (total_elapsed > 0.0 ? total_elapsed : 1.0) / 1e9;

      const std::uint64_t interval_hashes = g_hashes - last_report_global_hashes;
      const double gh_5s = static_cast<double>(interval_hashes) / (report_s > 0.0 ? report_s : 1.0) / 1e9;
      last_report_global_hashes = g_hashes;

      std::lock_guard<std::mutex> slock(socket_mutex);

      const std::uint64_t shares_total = share_accepts + share_rejects + share_discarded;
      const double reject_pct = shares_total > 0 ? (100.0 * static_cast<double>(share_rejects) / static_cast<double>(shares_total)) : 0.0;

      const std::string uptime = format_uptime_hms(static_cast<std::uint64_t>(total_elapsed));
      const std::string shares_ard = std::to_string(share_accepts) + "/" +
                                     std::to_string(share_rejects) + "/" +
                                     std::to_string(share_discarded);

      const std::string reject_s = [&]() {
        std::ostringstream oss;
        oss << std::fixed << std::setprecision(1) << reject_pct << "%";
        return oss.str();
      }();

      const double telemetry_age_s = std::chrono::duration<double>(now - telemetry_last).count();
      if (telemetry_age_s >= 15.0) {
        for (std::size_t i = 0; i < config.devices.size(); ++i) {
          read_gpu_telemetry(config.devices[i], telemetries[i]);
        }
        telemetry_last = now;
      }

      double agg_power = 0.0;
      int max_temp = 0;
      int max_fan = 0;
      bool any_valid = false;
      for (const auto& t : telemetries) {
          if (t.valid) {
              any_valid = true;
              agg_power += t.power_w;
              if (t.temperature_c > max_temp) max_temp = t.temperature_c;
              if (t.fan_percent > max_fan) max_fan = t.fan_percent;
          }
      }

      const std::string temp_s = any_valid ? std::to_string(max_temp) : "-";
      const std::string power_s = [&]() {
        if (!any_valid) return std::string("-");
        std::ostringstream oss;
        oss << std::fixed << std::setprecision(1) << agg_power;
        return oss.str();
      }();
      const std::string fan_s = (any_valid && max_fan >= 0) ? std::to_string(max_fan) : std::string("-");

      if (any_valid) {
        energy_kwh += (agg_power * report_s) / 3600000.0;
      }

      const std::string eff_s = [&]() {
        if (!any_valid || gh_5s <= 0.0) return std::string("-");
        std::ostringstream oss;
        oss << std::fixed << std::setprecision(1) << (agg_power / gh_5s);
        return oss.str();
      }();
      const std::string energy_s = [&]() {
        std::ostringstream oss;
        oss << std::fixed << std::setprecision(3) << energy_kwh;
        return oss.str();
      }();

      if (stats_rows_printed == 0 || (stats_rows_printed % 12) == 0) {
        print_pool_stats_header();
      }

      double current_diff_print;
      std::string current_job_id;
      {
         std::lock_guard<std::mutex> lock(job_mutex);
         current_diff_print = shared_job.diff;
         current_job_id = shared_job.valid ? shared_job.job.job_id : "-";
      }

      std::cout << std::right << std::setw(8) << uptime << " | "
                << std::setw(8) << std::fixed << std::setprecision(3) << gh_5s
                << " | " << std::setw(8) << std::fixed << std::setprecision(3)
                << gh_avg << " | " << std::setw(5) << temp_s << " | "
                << std::setw(6) << power_s << " | " << std::setw(4) << eff_s
                << " | " << std::setw(6) << energy_s << " | " << std::setw(4)
                << fan_s << " | " << std::setw(12) << shares_ard << " | "
                << std::setw(6) << reject_s << " | " << std::setw(8)
                << format_compact_diff(current_diff_print) << " | " << std::setw(10)
                << "MULTIGPU"
                << " | "
                << right_trim_to_width(current_job_id, 8)
                << "\n";

      ++stats_rows_printed;
      hashrate_last = now;
    }
  }

  stop_flag.store(true);
  job_cv.notify_all();
  for (auto& t : gpu_threads) t.join();

  return 1;
}

}  // namespace miner
