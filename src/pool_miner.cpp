#include "pool_miner.h"

#include "cuda_scan.h"
#include "sha256_cpu.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <iomanip>
#include <iostream>
#include <limits>
#include <memory>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#ifdef _WIN32
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
  std::vector<std::uint8_t> prevhash_be;
  std::vector<std::uint8_t> coinb1;
  std::vector<std::uint8_t> coinb2;
  std::vector<std::vector<std::uint8_t>> merkle_branch_be;
  std::array<std::uint8_t, 4> version_be{};
  std::array<std::uint8_t, 4> nbits_be{};
  std::array<std::uint8_t, 4> ntime_be{};
  bool clean_jobs = false;
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
      error = "getaddrinfo failed";
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

  bool read_line(int timeout_ms, std::string& out_line) {
    for (;;) {
      const auto pos = buffer_.find('\n');
      if (pos != std::string::npos) {
        out_line = buffer_.substr(0, pos);
        buffer_.erase(0, pos + 1);
        if (!out_line.empty() && out_line.back() == '\r') {
          out_line.pop_back();
        }
        return true;
      }

      if (!wait_readable(timeout_ms)) {
        return false;
      }

      char tmp[4096];
      const int n = ::recv(sock_, tmp, static_cast<int>(sizeof(tmp)), 0);
      if (n <= 0) {
        return false;
      }
      buffer_.append(tmp, static_cast<std::size_t>(n));

      // after the first recv call, keep draining without blocking
      timeout_ms = 0;
    }
  }

 private:
  bool wait_readable(const int timeout_ms) {
    if (sock_ == kInvalidSocket) {
      return false;
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
    if (rv <= 0) {
      return false;
    }
    return FD_ISSET(sock_, &readfds) != 0;
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
  return true;
}

void reverse_4(std::array<std::uint8_t, 4>& b) {
  std::swap(b[0], b[3]);
  std::swap(b[1], b[2]);
}

bool parse_u32_be_hex(const std::string& hex, std::array<std::uint8_t, 4>& out) {
  std::vector<std::uint8_t> tmp;
  if (!hex_to_bytes(hex, tmp) || tmp.size() != 4) {
    return false;
  }
  for (int i = 0; i < 4; ++i) {
    out[i] = tmp[static_cast<std::size_t>(i)];
  }
  return true;
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

  if (!hex_to_bytes(p[1].get<std::string>(), job.prevhash_be) ||
      job.prevhash_be.size() != 32) {
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

    job.merkle_branch_be.push_back(std::move(branch_bytes));
  }

  if (!parse_u32_be_hex(p[5].get<std::string>(), job.version_be) ||
      !parse_u32_be_hex(p[6].get<std::string>(), job.nbits_be) ||
      !parse_u32_be_hex(p[7].get<std::string>(), job.ntime_be)) {
    return false;
  }

  job.clean_jobs = p[8].get<bool>();
  out_job = std::move(job);
  return true;
}

Uint256 div_u256_u64(const Uint256& n, const std::uint64_t d) {
  Uint256 q{};
  if (d == 0) {
    return q;
  }

  std::uint64_t rem = 0;
  for (int i = 7; i >= 0; --i) {
    const std::uint64_t cur = (rem << 32) | static_cast<std::uint64_t>(n.w[i]);
    q.w[i] = static_cast<std::uint32_t>(cur / d);
    rem = cur % d;
  }
  return q;
}

Uint256 target_from_difficulty(const double diff) {
  double bounded = diff;
  if (bounded < 1.0) {
    bounded = 1.0;
  }
  if (!std::isfinite(bounded)) {
    bounded = 1.0;
  }
  if (bounded > static_cast<double>(std::numeric_limits<std::uint64_t>::max())) {
    bounded = static_cast<double>(std::numeric_limits<std::uint64_t>::max());
  }

  std::uint64_t integer_diff = static_cast<std::uint64_t>(std::floor(bounded));
  if (integer_diff == 0) {
    integer_diff = 1;
  }

  return div_u256_u64(kDiff1Target, integer_diff);
}

std::string fmt_hash_le_hex(const std::uint32_t hash_le[8]) {
  std::array<std::uint8_t, 32> bytes{};
  for (int i = 0; i < 8; ++i) {
    store_le32(hash_le[i], bytes.data() + static_cast<std::size_t>(i) * 4U);
  }

  std::vector<std::uint8_t> be(bytes.begin(), bytes.end());
  reverse_bytes(be);
  return bytes_to_hex(be.data(), be.size());
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

  for (const auto& branch_be : job.merkle_branch_be) {
    std::vector<std::uint8_t> branch = branch_be;
    reverse_bytes(branch);

    std::array<std::uint8_t, 64> concat{};
    std::memcpy(concat.data(), merkle.data(), 32);
    std::memcpy(concat.data() + 32, branch.data(), 32);
    sha256d(concat.data(), concat.size(), merkle);
  }

  std::array<std::uint8_t, 4> version = job.version_be;
  std::array<std::uint8_t, 4> nbits = job.nbits_be;
  std::array<std::uint8_t, 4> ntime = job.ntime_be;
  reverse_4(version);
  reverse_4(nbits);
  reverse_4(ntime);

  std::vector<std::uint8_t> prevhash = job.prevhash_be;
  reverse_bytes(prevhash);

  header.fill(0);
  std::memcpy(header.data() + 0, version.data(), 4);
  std::memcpy(header.data() + 4, prevhash.data(), 32);
  std::memcpy(header.data() + 36, merkle.data(), 32);
  std::memcpy(header.data() + 68, ntime.data(), 4);
  std::memcpy(header.data() + 72, nbits.data(), 4);
  std::memset(header.data() + 76, 0, 4);

  ntime_submit_hex = bytes_to_hex(job.ntime_be.data(), job.ntime_be.size());
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

}  // namespace

int run_pool_miner(const PoolConfig& config) {
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

  std::unique_ptr<ScanEngine, decltype(&destroy_scan_engine)> engine(
      nullptr, &destroy_scan_engine);
  std::uint64_t tuned_chunk_nonces = config.chunk_nonces;
  {
    ScanEngineConfig engine_cfg{};
    engine_cfg.device = config.device;
    engine_cfg.blocks = config.blocks;
    engine_cfg.threads = config.threads;

    ScanEngine* raw_engine = nullptr;
    if (create_scan_engine(engine_cfg, raw_engine) != 0) {
      return 1;
    }
    engine.reset(raw_engine);

    ScanEngineInfo info{};
    if (get_scan_engine_info(engine.get(), info) == 0) {
      std::cout << "CUDA: device=" << info.device << " name=" << info.name
                << " sms=" << info.sms << " blocks=" << info.blocks
                << " threads=" << info.threads << "\n";

      if (tuned_chunk_nonces == 0) {
        const std::uint64_t base = static_cast<std::uint64_t>(info.blocks) *
                                   static_cast<std::uint64_t>(info.threads);
        tuned_chunk_nonces = base * 64ULL;
      }
    }

    if (tuned_chunk_nonces == 0) {
      tuned_chunk_nonces = static_cast<std::uint64_t>(config.threads) * 1024ULL * 64ULL;
    }

    if (tuned_chunk_nonces < 4ULL * 1024ULL * 1024ULL) {
      tuned_chunk_nonces = 4ULL * 1024ULL * 1024ULL;
    }
    if (tuned_chunk_nonces > 128ULL * 1024ULL * 1024ULL) {
      tuned_chunk_nonces = 128ULL * 1024ULL * 1024ULL;
    }

    std::cout << "Chunk nonces: " << tuned_chunk_nonces << "\n";
  }

  if (!sock.send_line(make_json_request(1, "mining.subscribe", json::array({"minershartx/0.2"})))) {
    std::cerr << "Failed to send mining.subscribe\n";
    return 1;
  }

  bool got_subscribe = false;
  bool got_authorize = false;

  std::vector<std::uint8_t> extranonce1;
  int extranonce2_size = 0;

  StratumJob current_job{};
  bool has_job = false;
  bool reset_nonce_state = true;

  double current_diff = 1.0;
  Uint256 share_target = target_from_difficulty(current_diff);

  std::uint64_t total_hashes = 0;
  auto hashrate_t0 = std::chrono::steady_clock::now();
  auto hashrate_last = hashrate_t0;

  std::uint64_t extranonce2_counter = 0;
  std::uint64_t scanned_in_space = 0;
  std::uint32_t start_nonce = 0;

  std::array<std::uint8_t, 80> current_header{};
  std::uint32_t current_midstate[8] = {};
  std::uint32_t current_tail0 = 0;
  std::uint32_t current_tail1 = 0;
  std::uint32_t current_tail2 = 0;
  std::string current_ex2_hex;
  std::string current_ntime_hex;
  std::string current_nonce_hex;

  std::uint64_t submit_id = 1000;

  for (;;) {
    std::string line;
    const int read_timeout_ms =
        (!got_subscribe || !got_authorize || !has_job) ? 30000 : 0;
    const bool has_line = sock.read_line(read_timeout_ms, line);
    if (has_line) {
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
            return 1;
          }

          if (!hex_to_bytes(msg["result"][1].get<std::string>(), extranonce1)) {
            std::cerr << "Invalid extranonce1 in subscribe response\n";
            return 1;
          }
          extranonce2_size = msg["result"][2].get<int>();
          if (extranonce2_size <= 0 || extranonce2_size > 16) {
            std::cerr << "Unsupported extranonce2_size=" << extranonce2_size << "\n";
            return 1;
          }

          got_subscribe = true;
          std::cout << "Subscribed. extranonce1="
                    << bytes_to_hex(extranonce1.data(), extranonce1.size())
                    << " extranonce2_size=" << extranonce2_size << "\n";

          if (!sock.send_line(
                  make_json_request(2, "mining.authorize", json::array({config.user, config.pass})))) {
            std::cerr << "Failed to send mining.authorize\n";
            return 1;
          }
          continue;
        }

        if (id == 2) {
          const bool ok = msg.contains("result") && msg["result"].is_boolean() &&
                          msg["result"].get<bool>();
          if (!ok) {
            std::cerr << "Authorization rejected by pool\n";
            return 1;
          }
          got_authorize = true;
          std::cout << "Authorized as " << config.user << "\n";
          continue;
        }

        if (id >= 1000) {
          const bool accepted = msg.contains("result") && msg["result"].is_boolean() &&
                                msg["result"].get<bool>();
          std::cout << "Share submit id=" << id << " => "
                    << (accepted ? "ACCEPTED" : "REJECTED") << "\n";
          continue;
        }
      }

      if (msg.contains("method") && msg["method"].is_string()) {
        const std::string method = msg["method"].get<std::string>();

        if (method == "mining.set_difficulty") {
          if (msg.contains("params") && msg["params"].is_array() &&
              !msg["params"].empty() && msg["params"][0].is_number()) {
            current_diff = msg["params"][0].get<double>();
            share_target = target_from_difficulty(current_diff);
            std::cout << "Difficulty update: " << current_diff
                      << " (rounded for target calc)\n";
          }
          continue;
        }

        if (method == "mining.notify") {
          StratumJob next_job;
          if (parse_notify(msg, next_job)) {
            current_job = std::move(next_job);
            has_job = true;
            reset_nonce_state = true;
            std::cout << "New job: id=" << current_job.job_id
                      << " clean=" << (current_job.clean_jobs ? "true" : "false")
                      << " branches=" << current_job.merkle_branch_be.size() << "\n";
          }
          continue;
        }
      }
    }

    if (!got_subscribe || !got_authorize || !has_job) {
      if (!has_line) {
        std::cerr << "Pool connection timeout/disconnect before first job\n";
        return 1;
      }
      continue;
    }

    if (reset_nonce_state) {
      extranonce2_counter = 0;
      scanned_in_space = 0;
      start_nonce = 0;
      reset_nonce_state = false;

      if (!build_work_header(current_job, extranonce1, extranonce2_size,
                             extranonce2_counter, current_header, current_ex2_hex,
                             current_ntime_hex, current_nonce_hex)) {
        std::cerr << "Failed to build work header\n";
        return 1;
      }

      std::uint32_t dummy_nonce = 0;
      build_midstate_from_header(current_header, current_midstate, current_tail0,
                                 current_tail1, current_tail2, dummy_nonce);
    }

    const std::uint64_t chunk_nonces = tuned_chunk_nonces;

    const std::uint64_t nonce_space = (1ULL << 32);
    const std::uint64_t remaining = nonce_space - scanned_in_space;
    const std::uint64_t to_scan = std::min(chunk_nonces, remaining);

    ScanWork scan{};
    scan.nonce_count = to_scan;
    scan.start_nonce = start_nonce;
    scan.tail0 = current_tail0;
    scan.tail1 = current_tail1;
    scan.tail2 = current_tail2;
    for (int i = 0; i < 8; ++i) {
      scan.midstate[i] = current_midstate[i];
      scan.target[i] = share_target.w[i];
    }

    ScanResult scan_result{};
    const int scan_status = run_cuda_scan(engine.get(), scan, scan_result);
    if (scan_status != 0) {
      return scan_status;
    }

    total_hashes += to_scan;
    scanned_in_space += to_scan;
    start_nonce += static_cast<std::uint32_t>(to_scan);

    if (scan_result.found) {
      if (config.submit_nonce_be) {
        std::ostringstream oss;
        oss << std::hex << std::setfill('0') << std::nouppercase << std::setw(8)
            << scan_result.nonce;
        current_nonce_hex = oss.str();
      } else {
        std::array<std::uint8_t, 4> nonce_le{};
        store_le32(scan_result.nonce, nonce_le.data());
        current_nonce_hex = bytes_to_hex(nonce_le.data(), nonce_le.size());
      }

      const int request_id = static_cast<int>(submit_id++);
      json params =
          json::array({config.user, current_job.job_id, current_ex2_hex,
                       current_ntime_hex, current_nonce_hex});
      if (!sock.send_line(make_json_request(request_id, "mining.submit", params))) {
        std::cerr << "Failed to submit share\n";
        return 1;
      }

      std::cout << "Share found nonce=" << current_nonce_hex
                << " hash=" << fmt_hash_le_hex(scan_result.hash_le)
                << " submit_id=" << request_id << "\n";
    }

    if (scanned_in_space >= nonce_space) {
      ++extranonce2_counter;
      scanned_in_space = 0;
      start_nonce = 0;

      if (!build_work_header(current_job, extranonce1, extranonce2_size,
                             extranonce2_counter, current_header, current_ex2_hex,
                             current_ntime_hex, current_nonce_hex)) {
        std::cerr << "Failed to rebuild work header with next extranonce2\n";
        return 1;
      }

      std::uint32_t dummy_nonce = 0;
      build_midstate_from_header(current_header, current_midstate, current_tail0,
                                 current_tail1, current_tail2, dummy_nonce);
    }

    const auto now = std::chrono::steady_clock::now();
    const double report_s =
        std::chrono::duration<double>(now - hashrate_last).count();
    if (report_s >= 5.0) {
      const double total_elapsed =
          std::chrono::duration<double>(now - hashrate_t0).count();
      const double gh =
          static_cast<double>(total_hashes) / (total_elapsed > 0.0 ? total_elapsed : 1.0) /
          1e9;
      std::cout << std::fixed << std::setprecision(3)
                << "Hashrate(avg): " << gh << " GH/s"
                << " total_hashes=" << total_hashes
                << " ex2=" << current_ex2_hex << "\n";
      hashrate_last = now;
    }
  }
}

}  // namespace miner
