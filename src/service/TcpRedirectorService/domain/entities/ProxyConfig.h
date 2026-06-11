#pragma once

#include <string>
#include <vector>
#include <chrono>
#include <optional>
#include <functional>
#include <memory>

namespace tcp_redirector {
namespace domain {

struct ProxyConfig {
    std::wstring host;
    uint16_t port = 3128;
    bool auth_required = false;
    std::wstring login;
    // Password is stored encrypted, never in plaintext in logs/config dumps
    std::vector<uint8_t> encrypted_password;
    bool has_password = false;
};

enum class RuleType {
    ProcessName,
    ProcessPath,
    Global
};

enum class RuleAction {
    Proxy,
    Direct,
    Block
};

struct Rule {
    std::string id;
    RuleType type = RuleType::ProcessName;
    RuleAction action = RuleAction::Proxy;
    std::wstring pattern;
    std::wstring description;
    int priority = 0;
    bool enabled = true;
    std::chrono::system_clock::time_point created;
    std::chrono::system_clock::time_point modified;
};

enum class ConnectionState {
    Redirecting,
    ConnectingToProxy,
    TunnelEstablished,
    Closing,
    Closed,
    Error
};

struct ConnectionRecord {
    uint64_t id = 0;
    uint32_t pid = 0;
    std::wstring process_name;
    std::wstring process_path;
    std::wstring destination_host;
    std::string destination_ip;
    uint16_t destination_port = 0;
    std::chrono::steady_clock::time_point start_time;
    std::chrono::milliseconds duration{0};
    uint64_t rx_bytes = 0;
    uint64_t tx_bytes = 0;
    bool proxy_enabled = true;
    ConnectionState state = ConnectionState::Redirecting;
};

struct RedirectEvent {
    uint64_t redirect_id = 0;
    uint32_t pid = 0;
    std::wstring process_path;
    uint32_t original_address_v4 = 0;
    uint16_t original_port = 0;
    uint16_t redirect_local_port = 0;
    bool is_ipv6 = false;
};

enum class LogLevel {
    Trace = 0,
    Debug = 1,
    Info = 2,
    Warn = 3,
    Error = 4,
    Off = 5
};

struct LogEntry {
    std::chrono::system_clock::time_point timestamp;
    LogLevel level = LogLevel::Info;
    std::string logger;
    std::string message;
    std::string file;      // __FILE__ (опционально)
    int line = 0;          // __LINE__ (опционально)
    std::string function;  // __FUNCTION__ (опционально)
};

struct ServiceStats {
    uint64_t total_connections = 0;
    uint64_t active_connections = 0;
    uint64_t total_rx_bytes = 0;
    uint64_t total_tx_bytes = 0;
    uint64_t proxy_errors = 0;
    double avg_latency_ms = 0.0;
};

struct DriverStats {
    uint32_t pending_redirects = 0;
    int64_t total_redirects = 0;
    int64_t failed_redirects = 0;
    uint32_t rules_count = 0;
    uint32_t queue_max_size = 4096;
};

} // namespace domain
} // namespace tcp_redirector