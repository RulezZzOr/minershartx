#include "solo_miner.h"

#include "address_decoder.h"
#include "cuda_scan.h"
#include "nonce_partition.h"
#include "sha256_cpu.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
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
std::string get_env_var(const char* name) {
  char* value = nullptr;
  size_t size = 0;
  if (_dupenv_s(&value, &size, name) != 0 || value == nullptr) {
    return {};
  }
  std::string result(value);
  std::free(value);
  return result;
}
#else
std::string get_env_var(const char* name) {
  if (const char* value = std::getenv(name)) {
    return std::string(value);
  }
  return {};
}
#endif

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

struct GpuTelemetry {
  bool valid = false;
  int temperature_c = 0;
  double power_w = 0.0;
  int fan_percent = -1;
};

struct HttpEndpoint {
  std::string host;
  std::string port;
  std::string path = "/";
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

class TcpSocket {
 public:
  ~TcpSocket() { close(); }

  bool connect_to(const std::string& host,
                  const std::string& port,
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
      const SocketHandle s = static_cast<SocketHandle>(
          ::socket(rp->ai_family, rp->ai_socktype, rp->ai_protocol));
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
  }

  bool send_all(const std::string& payload) {
    if (sock_ == kInvalidSocket) {
      return false;
    }

    std::size_t sent = 0;
    while (sent < payload.size()) {
      const int n = ::send(sock_, payload.data() + sent,
                           static_cast<int>(payload.size() - sent), 0);
      if (n <= 0) {
        return false;
      }
      sent += static_cast<std::size_t>(n);
    }

    return true;
  }

  bool recv_all(std::string& out) {
    if (sock_ == kInvalidSocket) {
      return false;
    }

    out.clear();
    char buffer[8192];
    for (;;) {
      const int n = ::recv(sock_, buffer, static_cast<int>(sizeof(buffer)), 0);
      if (n < 0) {
        return false;
      }
      if (n == 0) {
        break;
      }
      out.append(buffer, static_cast<std::size_t>(n));
    }

    return true;
  }

  bool recv_all(std::string& out, const std::atomic<bool>* cancel_flag) {
    if (sock_ == kInvalidSocket) {
      return false;
    }

    out.clear();
    char buffer[8192];
    for (;;) {
      if (cancel_flag != nullptr &&
          cancel_flag->load(std::memory_order_relaxed)) {
        return false;
      }

      fd_set readfds;
      FD_ZERO(&readfds);
      FD_SET(sock_, &readfds);

      timeval timeout{};
      timeout.tv_sec = 1;
      timeout.tv_usec = 0;

#ifdef _WIN32
      const int ready = select(0, &readfds, nullptr, nullptr, &timeout);
#else
      const int ready = select(static_cast<int>(sock_) + 1, &readfds, nullptr,
                               nullptr, &timeout);
#endif
      if (ready < 0) {
        return false;
      }
      if (ready == 0) {
        continue;
      }

      const int n = ::recv(sock_, buffer, static_cast<int>(sizeof(buffer)), 0);
      if (n < 0) {
        return false;
      }
      if (n == 0) {
        break;
      }
      out.append(buffer, static_cast<std::size_t>(n));
    }

    return true;
  }

 private:
  SocketHandle sock_ = kInvalidSocket;
};

struct TemplateTx {
  std::vector<std::uint8_t> data;
  std::array<std::uint8_t, 32> txid_le{};
  std::array<std::uint8_t, 32> wtxid_le{};
};

struct SoloTemplate {
  std::string identity;
  std::string longpollid;
  std::string longpolluri;
  std::string tip_hash;
  std::uint32_t version = 0;
  std::array<std::uint8_t, 32> prevhash_le{};
  std::array<std::uint8_t, 4> bits_le{};
  Uint256 target{};
  std::uint32_t height = 0;
  std::uint32_t curtime = 0;
  std::uint32_t mintime = 0;
  std::uint64_t coinbase_value = 0;
  std::vector<std::vector<std::uint8_t>> coinbaseaux;
  std::vector<TemplateTx> txs;
  std::vector<std::uint8_t> payout_script;
  bool segwit = false;
  bool valid = false;
};

struct SoloWork {
  std::array<std::uint8_t, 80> header{};
  std::uint32_t midstate[8] = {};
  std::uint32_t tail0 = 0;
  std::uint32_t tail1 = 0;
  std::uint32_t tail2 = 0;
  std::vector<std::uint8_t> coinbase_full;
  std::vector<std::uint8_t> coinbase_no_witness;
  std::string tip_hash;
  std::string identity;
  Uint256 target{};
  std::uint32_t height = 0;
  std::uint64_t generation = 0;
  std::uint64_t base_ex2 = 0;
  std::uint32_t last_ntime = 0;
};

struct SoloState {
  SoloTemplate template_data;
  std::uint64_t generation = 0;
  bool valid = false;
};

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

std::string base64_encode(const std::string& value) {
  static constexpr char kTable[] =
      "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  std::string out;
  out.reserve(((value.size() + 2) / 3) * 4);

  std::uint32_t buffer = 0;
  int bits = -6;
  for (const unsigned char c : value) {
    buffer = (buffer << 8U) | c;
    bits += 8;
    while (bits >= 0) {
      out.push_back(kTable[(buffer >> bits) & 0x3FU]);
      bits -= 6;
    }
  }
  if (bits > -6) {
    out.push_back(kTable[((buffer << 8U) >> (bits + 8)) & 0x3FU]);
  }
  while (out.size() % 4 != 0) {
    out.push_back('=');
  }
  return out;
}

std::string read_text_file(const std::string& path) {
  std::ifstream file(path, std::ios::binary);
  if (!file) {
    return {};
  }

  std::ostringstream oss;
  oss << file.rdbuf();
  return oss.str();
}

std::string default_cookie_path() {
#ifdef _WIN32
  if (const std::string appdata = get_env_var("APPDATA"); !appdata.empty()) {
    return appdata + "\\Bitcoin\\.cookie";
  }
  if (const std::string userprofile = get_env_var("USERPROFILE");
      !userprofile.empty()) {
    return userprofile + "\\AppData\\Roaming\\Bitcoin\\.cookie";
  }
  return {};
#else
  if (const std::string home = get_env_var("HOME"); !home.empty()) {
    return home + "/.bitcoin/.cookie";
  }
  return {};
#endif
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

void append_le16(const std::uint16_t value, std::vector<std::uint8_t>& out) {
  out.push_back(static_cast<std::uint8_t>(value & 0xFFU));
  out.push_back(static_cast<std::uint8_t>((value >> 8) & 0xFFU));
}

void append_le32(const std::uint32_t value, std::vector<std::uint8_t>& out) {
  out.push_back(static_cast<std::uint8_t>(value & 0xFFU));
  out.push_back(static_cast<std::uint8_t>((value >> 8) & 0xFFU));
  out.push_back(static_cast<std::uint8_t>((value >> 16) & 0xFFU));
  out.push_back(static_cast<std::uint8_t>((value >> 24) & 0xFFU));
}

void append_le64(const std::uint64_t value, std::vector<std::uint8_t>& out) {
  for (int i = 0; i < 8; ++i) {
    out.push_back(static_cast<std::uint8_t>((value >> (8 * i)) & 0xFFU));
  }
}

void append_varint(const std::uint64_t value, std::vector<std::uint8_t>& out) {
  if (value < 0xFDU) {
    out.push_back(static_cast<std::uint8_t>(value));
  } else if (value <= 0xFFFFU) {
    out.push_back(0xFDU);
    append_le16(static_cast<std::uint16_t>(value), out);
  } else if (value <= 0xFFFFFFFFULL) {
    out.push_back(0xFEU);
    append_le32(static_cast<std::uint32_t>(value), out);
  } else {
    out.push_back(0xFFU);
    append_le64(value, out);
  }
}

std::vector<std::uint8_t> encode_script_num(std::uint32_t value) {
  std::vector<std::uint8_t> out;
  while (value > 0) {
    out.push_back(static_cast<std::uint8_t>(value & 0xFFU));
    value >>= 8;
  }
  if (out.empty()) {
    out.push_back(0U);
  }
  if ((out.back() & 0x80U) != 0U) {
    out.push_back(0U);
  }
  return out;
}

void append_pushdata(const std::vector<std::uint8_t>& data,
                     std::vector<std::uint8_t>& script) {
  if (data.size() < 0x4cU) {
    script.push_back(static_cast<std::uint8_t>(data.size()));
  } else if (data.size() <= 0xFFU) {
    script.push_back(0x4cU);
    script.push_back(static_cast<std::uint8_t>(data.size()));
  } else if (data.size() <= 0xFFFFU) {
    script.push_back(0x4dU);
    append_le16(static_cast<std::uint16_t>(data.size()), script);
  } else {
    script.push_back(0x4eU);
    append_le32(static_cast<std::uint32_t>(data.size()), script);
  }
  script.insert(script.end(), data.begin(), data.end());
}

std::vector<std::uint8_t> encode_extra_nonce_bytes(const std::uint32_t gpu_prefix,
                                                   const std::uint32_t base_ex2) {
  std::vector<std::uint8_t> out(8U);
  std::uint64_t combined =
      (static_cast<std::uint64_t>(gpu_prefix) << 32U) |
      static_cast<std::uint64_t>(base_ex2);
  for (int i = 7; i >= 0; --i) {
    out[static_cast<std::size_t>(i)] = static_cast<std::uint8_t>(combined & 0xFFU);
    combined >>= 8U;
  }
  return out;
}

bool parse_hex_bytes32_reversed(const std::string& hex, std::array<std::uint8_t, 32>& out) {
  std::vector<std::uint8_t> bytes;
  if (!hex_to_bytes(hex, bytes) || bytes.size() != 32U) {
    return false;
  }
  for (int i = 0; i < 32; ++i) {
    out[static_cast<std::size_t>(i)] = bytes[static_cast<std::size_t>(31 - i)];
  }
  return true;
}

bool parse_u32_hex_bytes(const std::string& hex, std::array<std::uint8_t, 4>& out) {
  std::vector<std::uint8_t> bytes;
  if (!hex_to_bytes(hex, bytes) || bytes.size() != 4U) {
    return false;
  }
  for (int i = 0; i < 4; ++i) {
    out[static_cast<std::size_t>(i)] = bytes[static_cast<std::size_t>(i)];
  }
  return true;
}

bool parse_uint256_hex_be(const std::string& hex, Uint256& out) {
  std::vector<std::uint8_t> bytes;
  if (!hex_to_bytes(hex, bytes) || bytes.size() != 32U) {
    return false;
  }
  for (int i = 0; i < 8; ++i) {
    out.w[static_cast<std::size_t>(i)] =
        load_be32(bytes.data() + static_cast<std::size_t>((7 - i) * 4U));
  }
  return true;
}

bool hash_be_meets_target(const std::array<std::uint8_t, 32>& hash_be,
                          const Uint256& target_le) {
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

std::array<std::uint8_t, 32> compute_merkle_root_le(
    std::vector<std::array<std::uint8_t, 32>> hashes) {
  if (hashes.empty()) {
    return {};
  }

  while (hashes.size() > 1) {
    if ((hashes.size() & 1U) != 0U) {
      hashes.push_back(hashes.back());
    }

    std::vector<std::array<std::uint8_t, 32>> next;
    next.reserve(hashes.size() / 2U);
    for (std::size_t i = 0; i < hashes.size(); i += 2U) {
      std::array<std::uint8_t, 64> concat{};
      std::memcpy(concat.data(), hashes[i].data(), 32U);
      std::memcpy(concat.data() + 32U, hashes[i + 1].data(), 32U);
      std::array<std::uint8_t, 32> digest{};
      sha256d(concat.data(), concat.size(), digest);
      reverse_bytes_32(digest);
      next.push_back(digest);
    }
    hashes.swap(next);
  }

  return hashes.front();
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

std::string right_trim_to_width(const std::string& value, const std::size_t width) {
  if (value.size() <= width) {
    return value;
  }
  return value.substr(value.size() - width);
}

bool parse_http_endpoint(const std::string& input,
                         const HttpEndpoint* base,
                         HttpEndpoint& out,
                         std::string& error) {
  std::string endpoint = trim_ascii(input);
  if (endpoint.empty()) {
    error = "empty HTTP endpoint";
    return false;
  }

  if (endpoint.rfind("http://", 0) == 0) {
    endpoint.erase(0, std::strlen("http://"));
  } else if (endpoint.rfind("https://", 0) == 0) {
    error = "https RPC endpoints are not supported";
    return false;
  }

  if (endpoint.find("://") != std::string::npos) {
    error = "unsupported URI scheme";
    return false;
  }

  if (endpoint.front() == '/') {
    if (base == nullptr || base->host.empty() || base->port.empty()) {
      error = "relative URI requires a base endpoint";
      return false;
    }
    out = *base;
    out.path = endpoint;
    return true;
  }

  std::string host_port = endpoint;
  std::string path = "/";
  const std::size_t slash_pos = endpoint.find('/');
  if (slash_pos != std::string::npos) {
    host_port = endpoint.substr(0, slash_pos);
    path = endpoint.substr(slash_pos);
  }

  const std::size_t colon_pos = host_port.rfind(':');
  if (colon_pos == std::string::npos || colon_pos == 0 ||
      colon_pos + 1 >= host_port.size()) {
    if (base != nullptr && endpoint.find(':') == std::string::npos) {
      out = *base;
      out.path = endpoint.front() == '/' ? endpoint : "/" + endpoint;
      return true;
    }
    error = "expected host:port in HTTP endpoint";
    return false;
  }

  out.host = host_port.substr(0, colon_pos);
  out.port = host_port.substr(colon_pos + 1);
  out.path = path.empty() ? "/" : path;
  if (out.path.front() != '/') {
    out.path.insert(out.path.begin(), '/');
  }

  return true;
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

std::string default_rpc_endpoint() {
  return "127.0.0.1:8332";
}

bool resolve_auth_header(const SoloConfig& config,
                        std::string& auth_header,
                        std::string& auth_source,
                        std::string& error) {
  if (!config.rpc_user.empty() || !config.rpc_pass.empty()) {
    if (config.rpc_user.empty() || config.rpc_pass.empty()) {
      error = "both --rpc-user and --rpc-pass are required when using explicit credentials";
      return false;
    }
    auth_header = "Authorization: Basic " + base64_encode(config.rpc_user + ":" + config.rpc_pass);
    auth_source = "explicit credentials";
    return true;
  }

  std::string cookie_path = config.rpc_cookie_path;
  if (cookie_path.empty()) {
    cookie_path = default_cookie_path();
  }
  if (cookie_path.empty()) {
    error = "unable to determine Bitcoin Core cookie path; pass --rpc-cookie or --rpc-user/--rpc-pass";
    return false;
  }

  const std::string cookie = trim_ascii(read_text_file(cookie_path));
  if (cookie.empty()) {
    error = "RPC cookie not found or empty at " + cookie_path +
            "; pass --rpc-user/--rpc-pass or --rpc-cookie";
    return false;
  }

  auth_header = "Authorization: Basic " + base64_encode(cookie);
  auth_source = "cookie " + cookie_path;
  return true;
}

class RpcClient {
 public:
  RpcClient(std::string host, std::string port, std::string auth_header,
            std::string path = "/")
      : host_(std::move(host)), port_(std::move(port)), path_(std::move(path)),
        auth_header_(std::move(auth_header)) {}

  bool call(const std::string& method,
            const json& params,
            json& response,
            std::string& error,
            const std::atomic<bool>* cancel_flag = nullptr) const {
    json req;
    req["jsonrpc"] = "2.0";
    req["id"] = 1;
    req["method"] = method;
    req["params"] = params;

    std::ostringstream http;
    const std::string body = req.dump();
    const std::string request_path = path_.empty() ? "/" : path_;
    http << "POST " << request_path << " HTTP/1.1\r\n"
         << "Host: " << host_ << ":" << port_ << "\r\n"
         << "User-Agent: minershartx/0.3\r\n"
         << "Content-Type: application/json\r\n";
    if (!auth_header_.empty()) {
      http << auth_header_ << "\r\n";
    }
    http << "Content-Length: " << body.size() << "\r\n"
         << "Connection: close\r\n\r\n"
         << body;

    TcpSocket sock;
    std::string connect_error;
    if (!sock.connect_to(host_, port_, connect_error)) {
      error = connect_error;
      return false;
    }

    if (!sock.send_all(http.str())) {
      error = "failed to send RPC request";
      return false;
    }

    std::string raw_response;
    if (!sock.recv_all(raw_response, cancel_flag)) {
      if (cancel_flag != nullptr &&
          cancel_flag->load(std::memory_order_relaxed)) {
        error = "RPC request canceled";
      } else {
        error = "failed to read RPC response";
      }
      return false;
    }

    if (raw_response.rfind("HTTP/", 0) != 0) {
      error = "RPC endpoint did not return HTTP data; are you pointing at the Bitcoin "
              "P2P port 8333 instead of the RPC port 8332?";
      return false;
    }

    const std::size_t header_end = raw_response.find("\r\n\r\n");
    if (header_end == std::string::npos) {
      error = "RPC endpoint returned an invalid HTTP response";
      return false;
    }

    const std::string body_text = raw_response.substr(header_end + 4);
    response = json::parse(body_text, nullptr, false);
    if (response.is_discarded()) {
      error = "RPC response is not valid JSON";
      return false;
    }

    return true;
  }

private:
  std::string host_;
  std::string port_;
  std::string path_ = "/";
  std::string auth_header_;
};

bool rpc_call_checked(const RpcClient& client,
                      const std::string& method,
                      const json& params,
                      json& result,
                      std::string& error_text,
                      const std::atomic<bool>* cancel_flag = nullptr);

bool verify_rpc_connection(const RpcClient& client, std::string& error) {
  json result;
  if (!rpc_call_checked(client, "getblockchaininfo", json::array(), result, error)) {
    return false;
  }

  if (!result.is_object()) {
    error = "getblockchaininfo returned a non-object result";
    return false;
  }

  return true;
}

bool rpc_call_checked(const RpcClient& client,
                      const std::string& method,
                      const json& params,
                      json& result,
                      std::string& error_text,
                      const std::atomic<bool>* cancel_flag) {
  json response;
  if (!client.call(method, params, response, error_text, cancel_flag)) {
    return false;
  }

  if (response.contains("error") && !response["error"].is_null()) {
    error_text = response["error"].dump();
    return false;
  }

  if (!response.contains("result")) {
    error_text = "RPC response missing result";
    return false;
  }

  result = response["result"];
  return true;
}

bool resolve_payout_script(const RpcClient& client,
                           const std::string& requested_address,
                           std::vector<std::uint8_t>& payout_script,
                           std::string& payout_address,
                           std::string& error) {
  std::string address = requested_address;
  if (address.empty()) {
    json result;
    if (!rpc_call_checked(client, "getnewaddress", json::array(), result, error)) {
      error = "wallet RPC getnewaddress failed: " + error;
      return false;
    }
    if (!result.is_string()) {
      error = "getnewaddress returned a non-string result";
      return false;
    }
    address = result.get<std::string>();
  }

  if (decode_bitcoin_address_script(address, payout_script, error)) {
    payout_address = address;
    return true;
  }

  json address_info;
  if (!rpc_call_checked(client, "getaddressinfo", json::array({address}), address_info, error)) {
    return false;
  }
  if (!address_info.is_object() ||
      !address_info.contains("scriptPubKey") ||
      !address_info["scriptPubKey"].is_string()) {
    error = "address validation failed for " + address;
    return false;
  }

  if (!hex_to_bytes(address_info["scriptPubKey"].get<std::string>(), payout_script)) {
    error = "failed to decode scriptPubKey for " + address;
    return false;
  }

  payout_address = address;
  return true;
}

bool parse_template_tx(const json& tx_json, TemplateTx& out, std::string& error) {
  if (!tx_json.is_object() || !tx_json.contains("data") || !tx_json["data"].is_string() ||
      !tx_json.contains("txid") || !tx_json["txid"].is_string()) {
    error = "transaction entry missing data/txid";
    return false;
  }

  if (!hex_to_bytes(tx_json["data"].get<std::string>(), out.data)) {
    error = "failed to decode transaction hex";
    return false;
  }

  std::array<std::uint8_t, 32> txid_be{};
  if (!parse_hex_bytes32_reversed(tx_json["txid"].get<std::string>(), txid_be)) {
    error = "failed to decode txid";
    return false;
  }
  out.txid_le = txid_be;

  if (tx_json.contains("hash") && tx_json["hash"].is_string()) {
    if (!parse_hex_bytes32_reversed(tx_json["hash"].get<std::string>(), out.wtxid_le)) {
      error = "failed to decode witness hash";
      return false;
    }
  } else {
    out.wtxid_le = out.txid_le;
  }

  return true;
}

bool fetch_block_template(const RpcClient& client,
                          const std::vector<std::uint8_t>& payout_script,
                          SoloTemplate& out,
                          std::string& error,
                          const std::string* longpollid = nullptr,
                          const std::atomic<bool>* cancel_flag = nullptr) {
  json request = json::object();
  request["capabilities"] = json::array({"coinbasevalue", "longpoll", "workid"});
  request["rules"] = json::array({"segwit"});
  if (longpollid != nullptr && !longpollid->empty()) {
    request["longpollid"] = *longpollid;
  }

  json result;
  if (!rpc_call_checked(client, "getblocktemplate", json::array({request}), result, error,
                        cancel_flag)) {
    return false;
  }
  if (!result.is_object()) {
    error = "getblocktemplate returned a non-object result";
    return false;
  }

  SoloTemplate tpl{};
  tpl.payout_script = payout_script;

  if (!result.contains("version") || !result["version"].is_number_integer() ||
      !result.contains("previousblockhash") || !result["previousblockhash"].is_string() ||
      !result.contains("transactions") || !result["transactions"].is_array() ||
      !result.contains("coinbasevalue") || !result["coinbasevalue"].is_number_unsigned() ||
      !result.contains("target") || !result["target"].is_string() ||
      !result.contains("height") || !result["height"].is_number_unsigned() ||
      !result.contains("curtime") || !result["curtime"].is_number_unsigned() ||
      !result.contains("bits") || !result["bits"].is_string()) {
    error = "getblocktemplate returned incomplete template";
    return false;
  }

  tpl.version = static_cast<std::uint32_t>(result["version"].get<std::int64_t>());
  if (!parse_hex_bytes32_reversed(result["previousblockhash"].get<std::string>(), tpl.prevhash_le)) {
    error = "failed to decode previousblockhash";
    return false;
  }
  if (!parse_u32_hex_bytes(result["bits"].get<std::string>(), tpl.bits_le)) {
    error = "failed to decode bits";
    return false;
  }
  std::reverse(tpl.bits_le.begin(), tpl.bits_le.end());
  if (!parse_uint256_hex_be(result["target"].get<std::string>(), tpl.target)) {
    error = "failed to decode target";
    return false;
  }

  tpl.height = static_cast<std::uint32_t>(result["height"].get<std::uint64_t>());
  tpl.curtime = static_cast<std::uint32_t>(result["curtime"].get<std::uint64_t>());
  if (result.contains("mintime") && result["mintime"].is_number_unsigned()) {
    tpl.mintime = static_cast<std::uint32_t>(result["mintime"].get<std::uint64_t>());
  } else {
    tpl.mintime = tpl.curtime;
  }
  tpl.coinbase_value = result["coinbasevalue"].get<std::uint64_t>();
  tpl.longpollid = result.contains("longpollid") && result["longpollid"].is_string()
                       ? result["longpollid"].get<std::string>()
                       : std::string{};
  tpl.longpolluri = result.contains("longpolluri") && result["longpolluri"].is_string()
                        ? result["longpolluri"].get<std::string>()
                        : std::string{};
  tpl.tip_hash = result["previousblockhash"].get<std::string>();
  tpl.identity = !tpl.longpollid.empty()
                     ? tpl.longpollid
                     : (tpl.tip_hash + ":" + std::to_string(tpl.height) + ":" +
                        std::to_string(tpl.coinbase_value));
  tpl.segwit = false;
  if (result.contains("rules") && result["rules"].is_array()) {
    for (const auto& rule : result["rules"]) {
      if (rule.is_string() && rule.get<std::string>() == "segwit") {
        tpl.segwit = true;
        break;
      }
    }
  }

  if (result.contains("coinbaseaux") && result["coinbaseaux"].is_object()) {
    std::vector<std::string> keys;
    keys.reserve(result["coinbaseaux"].size());
    for (const auto& item : result["coinbaseaux"].items()) {
      keys.push_back(item.key());
    }
    std::sort(keys.begin(), keys.end());
    for (const auto& key : keys) {
      const auto& value = result["coinbaseaux"][key];
      if (!value.is_string()) {
        continue;
      }
      std::vector<std::uint8_t> bytes;
      if (hex_to_bytes(value.get<std::string>(), bytes)) {
        tpl.coinbaseaux.push_back(std::move(bytes));
      }
    }
  }

  for (const auto& tx_json : result["transactions"]) {
    TemplateTx tx{};
    if (!parse_template_tx(tx_json, tx, error)) {
      return false;
    }
    tpl.txs.push_back(std::move(tx));
  }

  out = std::move(tpl);
  out.valid = true;
  return true;
}

bool build_coinbase_for_template(const SoloTemplate& tpl,
                                 const std::uint32_t gpu_prefix,
                                 const std::uint32_t base_ex2,
                                 std::vector<std::uint8_t>& coinbase_no_witness,
                                 std::vector<std::uint8_t>& coinbase_full,
                                 std::array<std::uint8_t, 32>& coinbase_txid_le,
                                 std::array<std::uint8_t, 32>& merkle_root_le,
                                 std::uint32_t& ntime_out) {
  std::vector<std::uint8_t> script_sig;
  append_pushdata(encode_script_num(tpl.height), script_sig);
  for (const auto& aux : tpl.coinbaseaux) {
    append_pushdata(aux, script_sig);
  }
  const std::vector<std::uint8_t> extra_nonce =
      encode_extra_nonce_bytes(gpu_prefix, base_ex2);
  append_pushdata(extra_nonce, script_sig);

  if (script_sig.size() > 100U) {
    return false;
  }

  const std::uint32_t now = static_cast<std::uint32_t>(std::time(nullptr));
  ntime_out = std::max({tpl.mintime, tpl.curtime, now});

  std::vector<std::uint8_t> tx_no_witness;
  std::vector<std::uint8_t> tx_full;
  tx_no_witness.reserve(200U + script_sig.size() + tpl.payout_script.size());
  tx_full.reserve(220U + script_sig.size() + tpl.payout_script.size());

  auto append_common = [&](std::vector<std::uint8_t>& tx, bool include_witness) {
    append_le32(1U, tx);
    if (include_witness) {
      tx.push_back(0x00U);
      tx.push_back(0x01U);
    }

    append_varint(1U, tx);
    tx.insert(tx.end(), 32U, 0U);
    append_le32(0xFFFFFFFFU, tx);
    append_varint(script_sig.size(), tx);
    tx.insert(tx.end(), script_sig.begin(), script_sig.end());
    append_le32(0xFFFFFFFFU, tx);

    const std::size_t output_count = tpl.segwit ? 2U : 1U;
    append_varint(output_count, tx);

    append_le64(tpl.coinbase_value, tx);
    append_varint(tpl.payout_script.size(), tx);
    tx.insert(tx.end(), tpl.payout_script.begin(), tpl.payout_script.end());

    if (tpl.segwit) {
      std::array<std::uint8_t, 32> witness_root{};
      std::vector<std::array<std::uint8_t, 32>> witness_hashes;
      witness_hashes.reserve(tpl.txs.size() + 1U);
      witness_hashes.push_back(std::array<std::uint8_t, 32>{});
      for (const auto& tx_ref : tpl.txs) {
        witness_hashes.push_back(tx_ref.wtxid_le);
      }
      witness_root = compute_merkle_root_le(std::move(witness_hashes));

      std::array<std::uint8_t, 32> witness_commitment{};
      std::array<std::uint8_t, 64> commitment_input{};
      std::memcpy(commitment_input.data(), witness_root.data(), 32U);
      // 32-byte witness reserved value is all-zeroes.
      sha256d(commitment_input.data(), commitment_input.size(), witness_commitment);

      std::vector<std::uint8_t> commitment_script;
      commitment_script.reserve(38U);
      commitment_script.push_back(0x6aU);
      commitment_script.push_back(0x24U);
      commitment_script.push_back(0xaaU);
      commitment_script.push_back(0x21U);
      commitment_script.push_back(0xa9U);
      commitment_script.push_back(0xedU);
      commitment_script.insert(commitment_script.end(), witness_commitment.begin(),
                               witness_commitment.end());

      append_le64(0U, tx);
      append_varint(commitment_script.size(), tx);
      tx.insert(tx.end(), commitment_script.begin(), commitment_script.end());

      if (include_witness) {
        append_varint(1U, tx);
        append_varint(32U, tx);
        tx.insert(tx.end(), 32U, 0U);
      }
    }

    append_le32(0U, tx);
  };

  append_common(tx_no_witness, false);
  if (tpl.segwit) {
    append_common(tx_full, true);
  } else {
    tx_full = tx_no_witness;
  }

  std::array<std::uint8_t, 32> txid_be{};
  sha256d(tx_no_witness.data(), tx_no_witness.size(), txid_be);
  coinbase_txid_le = txid_be;
  reverse_bytes_32(coinbase_txid_le);

  std::vector<std::array<std::uint8_t, 32>> merkle_hashes;
  merkle_hashes.reserve(tpl.txs.size() + 1U);
  merkle_hashes.push_back(coinbase_txid_le);
  for (const auto& tx_ref : tpl.txs) {
    merkle_hashes.push_back(tx_ref.txid_le);
  }
  merkle_root_le = compute_merkle_root_le(std::move(merkle_hashes));

  coinbase_no_witness = std::move(tx_no_witness);
  coinbase_full = std::move(tx_full);
  return true;
}

bool build_work_header(const SoloTemplate& tpl,
                       const std::uint32_t gpu_prefix,
                       const std::uint32_t base_ex2,
                       SoloWork& work) {
  work = SoloWork{};
  work.generation = 0;
  work.height = tpl.height;
  work.identity = tpl.identity;
  work.tip_hash = tpl.tip_hash;
  work.target = tpl.target;
  work.base_ex2 = base_ex2;

  [[maybe_unused]] std::array<std::uint8_t, 32> coinbase_txid_le{};
  std::array<std::uint8_t, 32> merkle_root_le{};
  if (!build_coinbase_for_template(tpl, gpu_prefix, base_ex2, work.coinbase_no_witness,
                                   work.coinbase_full, coinbase_txid_le, merkle_root_le,
                                   work.last_ntime)) {
    return false;
  }

  work.header.fill(0U);
  store_le32(tpl.version, work.header.data() + 0U);
  std::memcpy(work.header.data() + 4U, tpl.prevhash_le.data(), 32U);
  std::memcpy(work.header.data() + 36U, merkle_root_le.data(), 32U);
  store_le32(work.last_ntime, work.header.data() + 68U);
  std::memcpy(work.header.data() + 72U, tpl.bits_le.data(), 4U);
  std::memset(work.header.data() + 76U, 0, 4U);

  std::uint32_t dummy_nonce = 0;
  build_midstate_from_header(work.header, work.midstate, work.tail0, work.tail1,
                             work.tail2, dummy_nonce);
  return true;
}

std::string build_block_hex(const SoloTemplate& tpl,
                            const std::vector<std::uint8_t>& coinbase_full,
                            const std::array<std::uint8_t, 80>& header) {
  std::vector<std::uint8_t> block;
  block.reserve(80U + 8U + coinbase_full.size() + 1500U);
  block.insert(block.end(), header.begin(), header.end());
  append_varint(static_cast<std::uint64_t>(tpl.txs.size() + 1U), block);
  block.insert(block.end(), coinbase_full.begin(), coinbase_full.end());
  for (const auto& tx : tpl.txs) {
    block.insert(block.end(), tx.data.begin(), tx.data.end());
  }
  return bytes_to_hex(block.data(), block.size());
}

void print_solo_stats_header() {
  std::cout
      << "------------------------------------------------------------------------------------------------------------------------\n"
      << "  Uptime  |  5s GH/s | Avg GH/s | TempC | PowerW | J/GH |   kWh  | Fan% | Blocks A/R/D | Reject | Height |    Tip     | Job\n"
      << "------------------------------------------------------------------------------------------------------------------------\n";
}

}  // namespace

int run_solo_miner(const SoloConfig& config) {
  if (config.devices.empty()) {
    std::cerr << "No devices specified for solo mining\n";
    return 1;
  }

  SocketSystem socket_system;
  if (!socket_system.ready()) {
    std::cerr << "Failed to initialize socket system.\n";
    return 1;
  }

  const std::string rpc_url = config.rpc_url.empty() ? default_rpc_endpoint() : config.rpc_url;
  HttpEndpoint rpc_endpoint;
  std::string rpc_endpoint_error;
  if (!parse_http_endpoint(rpc_url, nullptr, rpc_endpoint, rpc_endpoint_error)) {
    std::cerr << "Invalid --rpc-url format: " << rpc_endpoint_error << "\n";
    return 1;
  }

  std::string auth_header;
  std::string auth_source;
  std::string auth_error;
  if (!resolve_auth_header(config, auth_header, auth_source, auth_error)) {
    std::cerr << auth_error << "\n";
    return 1;
  }

  RpcClient rpc(rpc_endpoint.host, rpc_endpoint.port, auth_header, rpc_endpoint.path);

  std::string rpc_probe_error;
  if (!verify_rpc_connection(rpc, rpc_probe_error)) {
    std::cerr << "Failed to connect to Bitcoin Core RPC at " << rpc_url << ": "
              << rpc_probe_error << "\n";
    return 1;
  }

  std::vector<std::uint8_t> payout_script;
  std::string payout_address;
  std::string rpc_error;
  if (!resolve_payout_script(rpc, config.address, payout_script, payout_address, rpc_error)) {
    std::cerr << "Failed to resolve payout address: " << rpc_error << "\n";
    return 1;
  }

  std::cout << "Connected to Bitcoin Core RPC at " << rpc_url << "\n";
  std::cout << "Payout address: " << payout_address << "\n";
  std::cout << "Auth source: " << auth_source << "\n";

  SoloTemplate initial_template;
  if (!fetch_block_template(rpc, payout_script, initial_template, rpc_error)) {
    std::cerr << "Failed to fetch block template: " << rpc_error << "\n";
    return 1;
  }

  if (!initial_template.longpollid.empty()) {
    std::cout << "Solo longpoll: enabled";
    if (!initial_template.longpolluri.empty()) {
      std::cout << " uri=" << initial_template.longpolluri;
    }
    std::cout << "\n";
  } else {
    std::cout << "Solo longpoll: disabled, using periodic refresh\n";
  }

  std::mutex template_mutex;
  std::condition_variable template_cv;
  SoloState shared_state;
  shared_state.template_data = std::move(initial_template);
  shared_state.valid = true;
  shared_state.generation = 1;

  std::mutex output_mutex;
  std::atomic<bool> stop_flag{false};
  std::atomic<bool> refresh_now{false};
  std::atomic<std::uint64_t> global_hashes{0};
  std::atomic<int> engine_init_errors{0};

  std::atomic<std::uint64_t> block_accepts{0};
  std::atomic<std::uint64_t> block_rejects{0};
  std::atomic<std::uint64_t> block_discarded{0};

  std::vector<std::thread> gpu_threads;

  for (std::size_t i = 0; i < config.devices.size(); ++i) {
    const int device = config.devices[i];
    const std::uint32_t ex2_prefix = static_cast<std::uint32_t>(i);
    const NoncePartition nonce_partition = split_nonce_space(i, config.devices.size());

    gpu_threads.emplace_back([&, device, ex2_prefix, nonce_partition]() {
      ScanEngineConfig engine_cfg{};
      engine_cfg.device = device;
      engine_cfg.blocks = config.blocks;
      engine_cfg.threads = config.threads;

      ScanEngine* raw_engine = nullptr;
      if (create_scan_engine(engine_cfg, raw_engine) != 0) {
        engine_init_errors.fetch_add(1, std::memory_order_relaxed);
        return;
      }
      std::unique_ptr<ScanEngine, decltype(&destroy_scan_engine)> engine(
          raw_engine, &destroy_scan_engine);

      ScanEngineInfo info{};
      if (get_scan_engine_info(engine.get(), info) == 0) {
        std::lock_guard<std::mutex> lock(output_mutex);
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
        std::lock_guard<std::mutex> lock(output_mutex);
        std::cout << "Device " << device << " chunk nonces: " << tuned_chunk
                  << (chunk_from_user ? " (manual)\n" : " (auto)\n");
      }

      SoloWork current_work{};
      std::string current_job_id;
      std::uint64_t current_generation = 0;
      std::uint32_t base_ex2 = 0;
      std::uint64_t partition_scanned = 0;

      while (!stop_flag.load(std::memory_order_relaxed)) {
        SoloTemplate local_template;
        {
          std::unique_lock<std::mutex> lock(template_mutex);
          template_cv.wait_for(lock, std::chrono::milliseconds(100), [&]() {
            return shared_state.valid || stop_flag.load(std::memory_order_relaxed);
          });
          if (!shared_state.valid || stop_flag.load(std::memory_order_relaxed)) {
            continue;
          }
          local_template = shared_state.template_data;
          current_generation = shared_state.generation;
        }

        if (local_template.identity != current_job_id) {
          current_job_id = local_template.identity;
          partition_scanned = 0;
          base_ex2 = 0;
        }

        if (partition_scanned == 0) {
          const std::uint64_t full_ex2 =
              (static_cast<std::uint64_t>(ex2_prefix) << 32U) |
              static_cast<std::uint64_t>(base_ex2);
          if (!build_work_header(local_template, ex2_prefix, base_ex2, current_work)) {
            std::lock_guard<std::mutex> lock(output_mutex);
            std::cerr << "Failed to build solo work header for GPU " << device << "\n";
            break;
          }
          current_work.generation = current_generation;
          current_work.base_ex2 = full_ex2;
        }

        current_work.generation = current_generation;
        if (nonce_partition.size == 0) {
          continue;
        }

        const std::uint64_t start_nonce64 = nonce_partition.start + partition_scanned;
        const std::uint64_t remaining = nonce_partition.size - partition_scanned;
        const std::uint64_t to_scan = std::min(tuned_chunk, remaining);
        if (to_scan == 0) {
          partition_scanned = 0;
          ++base_ex2;
          continue;
        }

        ScanWork scan{};
        scan.nonce_count = to_scan;
        scan.start_nonce = static_cast<std::uint32_t>(start_nonce64);
        scan.tail0 = current_work.tail0;
        scan.tail1 = current_work.tail1;
        scan.tail2 = current_work.tail2;
        for (int j = 0; j < 8; ++j) {
          scan.midstate[j] = current_work.midstate[j];
          scan.target[j] = current_work.target.w[j];
        }

        ScanResult scan_result{};
        const int scan_status = run_cuda_scan(engine.get(), scan, scan_result);
        if (scan_status != 0) {
          std::lock_guard<std::mutex> lock(output_mutex);
          std::cerr << "GPU " << device << " scan failed!\n";
          break;
        }

        global_hashes.fetch_add(to_scan, std::memory_order_relaxed);
        partition_scanned += to_scan;

        if (partition_scanned >= nonce_partition.size) {
          partition_scanned = 0;
          ++base_ex2;
        }

        if (scan_result.found) {
          std::array<std::uint8_t, 80> verify_header = current_work.header;
          store_le32(scan_result.nonce, verify_header.data() + 76U);

          std::array<std::uint8_t, 32> verify_hash_be{};
          sha256d(verify_header.data(), verify_header.size(), verify_hash_be);
          const bool cpu_valid = hash_be_meets_target(verify_hash_be, current_work.target);
          const std::string verify_hash_hex =
              bytes_to_hex(verify_hash_be.data(), verify_hash_be.size());

          bool stale_work = false;
          {
            std::lock_guard<std::mutex> lock(template_mutex);
            stale_work = !shared_state.valid || shared_state.generation != current_generation;
          }

          if (!cpu_valid) {
            std::lock_guard<std::mutex> lock(output_mutex);
            block_discarded.fetch_add(1, std::memory_order_relaxed);
            std::cout << "GPU " << device << " false-positive dropped nonce=0x"
                      << std::hex << std::nouppercase << scan_result.nonce << std::dec
                      << " cpu_hash=" << verify_hash_hex << "\n";
          } else if (stale_work) {
            std::lock_guard<std::mutex> lock(output_mutex);
            block_discarded.fetch_add(1, std::memory_order_relaxed);
            std::cout << "GPU " << device << " stale block discarded nonce=0x"
                      << std::hex << std::nouppercase << scan_result.nonce << std::dec
                      << " cpu_hash=" << verify_hash_hex << "\n";
          } else {
            const std::string block_hex =
                build_block_hex(local_template, current_work.coinbase_full, verify_header);

            json submit_result;
            std::string submit_error;
            const bool rpc_ok =
                rpc_call_checked(rpc, "submitblock", json::array({block_hex}),
                                 submit_result, submit_error);

            std::lock_guard<std::mutex> output_lock(output_mutex);
            std::cout << "GPU " << device << " Block found nonce=0x" << std::hex
                      << std::nouppercase << scan_result.nonce << std::dec
                      << " hash=" << verify_hash_hex << "\n";

            if (!rpc_ok) {
              block_rejects.fetch_add(1, std::memory_order_relaxed);
              std::cout << "Block submit => REJECTED error=" << submit_error
                        << " (accepted=" << block_accepts.load(std::memory_order_relaxed)
                        << " rejected=" << block_rejects.load(std::memory_order_relaxed)
                        << " discarded=" << block_discarded.load(std::memory_order_relaxed)
                        << ")\n";
            } else if (submit_result.is_null()) {
              block_accepts.fetch_add(1, std::memory_order_relaxed);
              std::cout << "Block submit => ACCEPTED"
                        << " (accepted=" << block_accepts.load(std::memory_order_relaxed)
                        << " rejected=" << block_rejects.load(std::memory_order_relaxed)
                        << " discarded=" << block_discarded.load(std::memory_order_relaxed)
                        << ")\n";
              refresh_now.store(true, std::memory_order_relaxed);
              {
                std::lock_guard<std::mutex> lock(template_mutex);
                shared_state.valid = false;
                ++shared_state.generation;
                template_cv.notify_all();
              }
            } else {
              const std::string result_text = submit_result.dump();
              const bool likely_discard =
                  result_text.find("stale") != std::string::npos ||
                  result_text.find("duplicate") != std::string::npos ||
                  result_text.find("already") != std::string::npos;
              if (likely_discard) {
                block_discarded.fetch_add(1, std::memory_order_relaxed);
              } else {
                block_rejects.fetch_add(1, std::memory_order_relaxed);
              }
              std::lock_guard<std::mutex> output_lock(output_mutex);
              std::cout << "Block submit => REJECTED error=" << result_text
                        << " (accepted=" << block_accepts.load(std::memory_order_relaxed)
                        << " rejected=" << block_rejects.load(std::memory_order_relaxed)
                        << " discarded=" << block_discarded.load(std::memory_order_relaxed)
                        << ")\n";
            }
          }
        }

      }
    });
  }

  if (engine_init_errors.load(std::memory_order_relaxed) > 0) {
    stop_flag.store(true, std::memory_order_relaxed);
    template_cv.notify_all();
    for (auto& t : gpu_threads) {
      if (t.joinable()) {
        t.join();
      }
    }
    return 1;
  }

  std::thread template_updater([&]() {
    bool longpoll_supported = !shared_state.template_data.longpollid.empty();
    while (!stop_flag.load(std::memory_order_relaxed)) {
      SoloTemplate next_template;
      std::string update_error;
      bool fetched = false;

      SoloTemplate current_template;
      {
        std::lock_guard<std::mutex> lock(template_mutex);
        current_template = shared_state.template_data;
      }

      if (longpoll_supported && !current_template.longpollid.empty()) {
        HttpEndpoint longpoll_endpoint = rpc_endpoint;
        if (!current_template.longpolluri.empty()) {
          if (!parse_http_endpoint(current_template.longpolluri, &rpc_endpoint,
                                   longpoll_endpoint, update_error)) {
            std::lock_guard<std::mutex> lock(output_mutex);
            std::cerr << "Longpoll endpoint parse failed: " << update_error << "\n";
            longpoll_supported = false;
            continue;
          }
        }

        RpcClient longpoll_client(longpoll_endpoint.host, longpoll_endpoint.port,
                                  auth_header, longpoll_endpoint.path);
        fetched = fetch_block_template(longpoll_client, payout_script, next_template,
                                       update_error, &current_template.longpollid,
                                       &stop_flag);
        if (!fetched) {
          if (stop_flag.load(std::memory_order_relaxed)) {
            break;
          }
          std::lock_guard<std::mutex> lock(output_mutex);
          std::cerr << "Longpoll refresh failed: " << update_error << "\n";
          longpoll_supported = false;
          continue;
        }
      } else {
        const int wait_seconds = std::max(1, config.template_poll_seconds);
        if (!refresh_now.exchange(false, std::memory_order_relaxed)) {
          bool should_refresh = false;
          for (int i = 0; i < wait_seconds && !stop_flag.load(std::memory_order_relaxed); ++i) {
            if (refresh_now.exchange(false, std::memory_order_relaxed)) {
              should_refresh = true;
              break;
            }
            std::this_thread::sleep_for(std::chrono::seconds(1));
          }
          if (!should_refresh && stop_flag.load(std::memory_order_relaxed)) {
            break;
          }
        }

        if (!fetch_block_template(rpc, payout_script, next_template, update_error)) {
          std::lock_guard<std::mutex> lock(output_mutex);
          std::cerr << "Template refresh failed: " << update_error << "\n";
          continue;
        }
      }

      bool changed = false;
      {
        std::lock_guard<std::mutex> lock(template_mutex);
        if (!shared_state.valid ||
            shared_state.template_data.identity != next_template.identity) {
          shared_state.template_data = std::move(next_template);
          shared_state.valid = true;
          ++shared_state.generation;
          changed = true;
          template_cv.notify_all();
        }
      }

      longpoll_supported = !shared_state.template_data.longpollid.empty();

      if (changed) {
        std::lock_guard<std::mutex> lock(output_mutex);
        std::cout << "New template: height=" << shared_state.template_data.height
                  << " txs=" << shared_state.template_data.txs.size()
                  << " tip=" << right_trim_to_width(shared_state.template_data.tip_hash, 8)
                  << "\n";
      }
    }
  });

  auto hashrate_t0 = std::chrono::steady_clock::now();
  auto hashrate_last = hashrate_t0;
  std::uint64_t last_report_global_hashes = 0;
  int stats_rows_printed = 0;
  std::vector<GpuTelemetry> telemetries(config.devices.size());
  auto telemetry_last = hashrate_t0;
  double energy_kwh = 0.0;

  while (!stop_flag.load(std::memory_order_relaxed)) {
    std::this_thread::sleep_for(std::chrono::seconds(5));

    const auto now = std::chrono::steady_clock::now();
    const double report_s = std::chrono::duration<double>(now - hashrate_last).count();
    const double total_elapsed = std::chrono::duration<double>(now - hashrate_t0).count();
    const std::uint64_t g_hashes = global_hashes.load(std::memory_order_relaxed);
    const double gh_avg = static_cast<double>(g_hashes) /
                          (total_elapsed > 0.0 ? total_elapsed : 1.0) / 1e9;
    const std::uint64_t interval_hashes = g_hashes - last_report_global_hashes;
    const double gh_5s = static_cast<double>(interval_hashes) /
                         (report_s > 0.0 ? report_s : 1.0) / 1e9;
    last_report_global_hashes = g_hashes;

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

    if (any_valid) {
      energy_kwh += (agg_power * report_s) / 3600000.0;
    }

    const std::string temp_s = any_valid ? std::to_string(max_temp) : "-";
    const std::string power_s = [&]() {
      if (!any_valid) return std::string("-");
      std::ostringstream oss;
      oss << std::fixed << std::setprecision(1) << agg_power;
      return oss.str();
    }();
    const std::string fan_s = (any_valid && max_fan >= 0) ? std::to_string(max_fan)
                                                           : std::string("-");
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
      print_solo_stats_header();
    }

    SoloTemplate current_template;
    {
      std::lock_guard<std::mutex> lock(template_mutex);
      current_template = shared_state.template_data;
    }

    const std::string uptime =
        format_uptime_hms(static_cast<std::uint64_t>(total_elapsed));
    const std::uint64_t accepted_count = block_accepts.load(std::memory_order_relaxed);
    const std::uint64_t rejected_count = block_rejects.load(std::memory_order_relaxed);
    const std::uint64_t discarded_count = block_discarded.load(std::memory_order_relaxed);
    const std::string blocks_ard = std::to_string(accepted_count) + "/" +
                                   std::to_string(rejected_count) + "/" +
                                   std::to_string(discarded_count);
    const std::uint64_t total_events = accepted_count + rejected_count + discarded_count;
    const double reject_pct = total_events > 0
                                  ? (100.0 * static_cast<double>(rejected_count) /
                                     static_cast<double>(total_events))
                                  : 0.0;
    const std::string reject_s = [&]() {
      std::ostringstream oss;
      oss << std::fixed << std::setprecision(1) << reject_pct << "%";
      return oss.str();
    }();

    std::cout << std::right << std::setw(8) << uptime << " | "
              << std::setw(8) << std::fixed << std::setprecision(3) << gh_5s
              << " | " << std::setw(8) << std::fixed << std::setprecision(3)
              << gh_avg << " | " << std::setw(5) << temp_s << " | "
              << std::setw(6) << power_s << " | " << std::setw(4) << eff_s
              << " | " << std::setw(6) << energy_s << " | " << std::setw(4)
              << fan_s << " | " << std::setw(12) << blocks_ard << " | "
              << std::setw(6) << reject_s << " | " << std::setw(6)
              << current_template.height << " | " << std::setw(10)
              << right_trim_to_width(current_template.tip_hash, 10)
              << " | "
              << right_trim_to_width(current_template.identity, 8)
              << "\n";

    ++stats_rows_printed;
    hashrate_last = now;
  }

  stop_flag.store(true, std::memory_order_relaxed);
  template_cv.notify_all();
  if (template_updater.joinable()) {
    template_updater.join();
  }
  for (auto& t : gpu_threads) {
    if (t.joinable()) {
      t.join();
    }
  }

  return 0;
}

}  // namespace miner
