#pragma once

#include <unordered_map>
#include <shared_mutex>
#include <vector>
#include <algorithm>
#include <regex>
#include "../entities/ProxyConfig.h"
#include "../ports/IConnectionMonitor.h"

namespace tcp_redirector {
namespace domain {
namespace services {

class ConnectionTracker : public ports::IConnectionMonitor {
public:
    ConnectionTracker() = default;

    void AddConnection(const ConnectionRecord& record) override {
        std::unique_lock lock(m_mutex);
        m_connections[record.id] = record;
        m_stats.active_connections = static_cast<uint32_t>(m_connections.size());
        m_stats.total_connections++;
        NotifyChanged();
    }

    void UpdateConnection(uint64_t id, const ConnectionRecord& updates) override {
        std::unique_lock lock(m_mutex);
        auto it = m_connections.find(id);
        if (it != m_connections.end()) {
            // Preserve fields that shouldn't be overwritten
            if (updates.rx_bytes > 0 || updates.tx_bytes > 0) {
                m_stats.total_rx_bytes += (updates.rx_bytes - it->second.rx_bytes);
                m_stats.total_tx_bytes += (updates.tx_bytes - it->second.tx_bytes);
            }
            it->second = updates;
            it->second.duration = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - it->second.start_time);

            if (updates.state == ConnectionState::Closed ||
                updates.state == ConnectionState::Error) {
                m_stats.active_connections = static_cast<uint32_t>(m_connections.size()) - 1;
            }
            NotifyChanged();
        }
    }

    void RemoveConnection(uint64_t id) override {
        std::unique_lock lock(m_mutex);
        m_connections.erase(id);
        m_stats.active_connections = static_cast<uint32_t>(m_connections.size());
        NotifyChanged();
    }

    std::vector<ConnectionRecord> GetActiveConnections() const override {
        std::shared_lock lock(m_mutex);
        std::vector<ConnectionRecord> result;
        result.reserve(m_connections.size());
        for (const auto& [id, conn] : m_connections) {
            auto c = conn;
            c.duration = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - c.start_time);
            result.push_back(c);
        }
        return result;
    }

    std::vector<ConnectionRecord> Search(const std::wstring& query) const override {
        std::shared_lock lock(m_mutex);
        std::vector<ConnectionRecord> result;
        if (query.empty()) {
            result.reserve(m_connections.size());
            for (const auto& [id, conn] : m_connections) {
                result.push_back(conn);
            }
            return result;
        }

        std::wstring lower_query = query;
        std::transform(lower_query.begin(), lower_query.end(),
                      lower_query.begin(), ::towlower);

        for (const auto& [id, conn] : m_connections) {
            auto match_field = [&](const std::wstring& field) {
                std::wstring lower = field;
                std::transform(lower.begin(), lower.end(), lower.begin(), ::towlower);
                return lower.find(lower_query) != std::wstring::npos;
            };

            if (match_field(conn.process_name) ||
                match_field(conn.destination_host) ||
                match_field(std::to_wstring(conn.pid))) {
                result.push_back(conn);
            }
        }
        return result;
    }

    std::vector<ConnectionRecord> FilterByProcess(uint32_t pid) const override {
        std::shared_lock lock(m_mutex);
        std::vector<ConnectionRecord> result;
        for (const auto& [id, conn] : m_connections) {
            if (conn.pid == pid) {
                result.push_back(conn);
            }
        }
        return result;
    }

    ServiceStats GetAggregatedStats() const override {
        std::shared_lock lock(m_mutex);
        return m_stats;
    }

    void SetOnConnectionsChanged(ConnectionsCallback callback) override {
        std::unique_lock lock(m_mutex);
        m_callback = callback;
    }

private:
    mutable std::shared_mutex m_mutex;
    std::unordered_map<uint64_t, ConnectionRecord> m_connections;
    ServiceStats m_stats;
    uint64_t m_next_id = 1;
    ConnectionsCallback m_callback;

    void NotifyChanged() {
        if (m_callback) {
            auto connections = GetActiveConnections();
            m_callback(connections);
        }
    }

public:
    uint64_t GenerateId() {
        return m_next_id++;
    }
};

} // namespace services
} // namespace domain
} // namespace tcp_redirector