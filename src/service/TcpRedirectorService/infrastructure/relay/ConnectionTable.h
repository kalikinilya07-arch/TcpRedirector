#pragma once

//
// ConnectionTable — thread-safe hash table mapping src_port → original destination.
//
// При первом SYN целевого процесса сохраняем (src_port → origDstIP, origDstPort, proxyConfigId).
// Последующие пакеты от того же src_port перенаправляются на relay.
// Ответные пакеты от relay восстанавливают original DST.
//
// Реализация: массив связных списков + SRWLOCK (как в ProxyBridge).
//

#include <windows.h>
#include <cstdint>
#include <cstring>

#include "../../domain/ports/IConnectionTable.h"

#ifndef CONNECTION_HASH_SIZE
#define CONNECTION_HASH_SIZE 4096
#endif

namespace tcp_redirector {
namespace infrastructure {

struct ConnectionEntry {
    uint16_t src_port;
    uint32_t src_ip;
    uint32_t orig_dest_ip;
    uint16_t orig_dest_port;
    uint32_t proxy_config_id;
    bool     is_tracked;
    ConnectionEntry* next;
};

class ConnectionTable : public domain::ports::IConnectionTable {
public:
    ConnectionTable() {
        InitializeSRWLock(&m_lock);
        memset(m_table, 0, sizeof(m_table));
    }

    ~ConnectionTable() {
        Clear();
    }

    // Сохранить соединение: src_port → {orig_dest_ip, orig_dest_port, proxy_config_id}
    void Add(uint16_t src_port, uint32_t src_ip,
             uint32_t orig_dest_ip, uint16_t orig_dest_port,
             uint32_t proxy_config_id) {
        AcquireSRWLockExclusive(&m_lock);

        int hash = src_port % CONNECTION_HASH_SIZE;
        ConnectionEntry* entry = m_table[hash];
        while (entry) {
            if (entry->src_port == src_port) {
                // Update existing
                entry->src_ip = src_ip;
                entry->orig_dest_ip = orig_dest_ip;
                entry->orig_dest_port = orig_dest_port;
                entry->proxy_config_id = proxy_config_id;
                entry->is_tracked = true;
                ReleaseSRWLockExclusive(&m_lock);
                return;
            }
            entry = entry->next;
        }

        // Create new
        auto* conn = new ConnectionEntry();
        conn->src_port = src_port;
        conn->src_ip = src_ip;
        conn->orig_dest_ip = orig_dest_ip;
        conn->orig_dest_port = orig_dest_port;
        conn->proxy_config_id = proxy_config_id;
        conn->is_tracked = true;
        conn->next = m_table[hash];
        m_table[hash] = conn;

        ReleaseSRWLockExclusive(&m_lock);
    }

    // Получить original destination для src_port
    bool Get(uint16_t src_port, uint32_t* out_orig_dest_ip,
             uint16_t* out_orig_dest_port) {
        AcquireSRWLockShared(&m_lock);

        int hash = src_port % CONNECTION_HASH_SIZE;
        ConnectionEntry* entry = m_table[hash];
        while (entry) {
            if (entry->src_port == src_port && entry->is_tracked) {
                if (out_orig_dest_ip) *out_orig_dest_ip = entry->orig_dest_ip;
                if (out_orig_dest_port) *out_orig_dest_port = entry->orig_dest_port;
                ReleaseSRWLockShared(&m_lock);
                return true;
            }
            entry = entry->next;
        }

        ReleaseSRWLockShared(&m_lock);
        return false;
    }

    // Получить proxy_config_id для src_port
    uint32_t GetProxyConfigId(uint16_t src_port) {
        AcquireSRWLockShared(&m_lock);

        int hash = src_port % CONNECTION_HASH_SIZE;
        ConnectionEntry* entry = m_table[hash];
        while (entry) {
            if (entry->src_port == src_port && entry->is_tracked) {
                uint32_t id = entry->proxy_config_id;
                ReleaseSRWLockShared(&m_lock);
                return id;
            }
            entry = entry->next;
        }

        ReleaseSRWLockShared(&m_lock);
        return 0;
    }

    // Проверить, отслеживается ли src_port
    bool IsTracked(uint16_t src_port) {
        AcquireSRWLockShared(&m_lock);

        int hash = src_port % CONNECTION_HASH_SIZE;
        ConnectionEntry* entry = m_table[hash];
        while (entry) {
            if (entry->src_port == src_port && entry->is_tracked) {
                ReleaseSRWLockShared(&m_lock);
                return true;
            }
            entry = entry->next;
        }

        ReleaseSRWLockShared(&m_lock);
        return false;
    }

    // Удалить соединение (при FIN/RST)
    void Remove(uint16_t src_port) {
        AcquireSRWLockExclusive(&m_lock);

        int hash = src_port % CONNECTION_HASH_SIZE;
        ConnectionEntry** pp = &m_table[hash];
        while (*pp) {
            if ((*pp)->src_port == src_port) {
                ConnectionEntry* to_delete = *pp;
                *pp = (*pp)->next;
                delete to_delete;
                ReleaseSRWLockExclusive(&m_lock);
                return;
            }
            pp = &(*pp)->next;
        }

        ReleaseSRWLockExclusive(&m_lock);
    }

    // Очистить все соединения
    void Clear() {
        AcquireSRWLockExclusive(&m_lock);

        for (int i = 0; i < CONNECTION_HASH_SIZE; i++) {
            ConnectionEntry* entry = m_table[i];
            while (entry) {
                ConnectionEntry* next = entry->next;
                delete entry;
                entry = next;
            }
            m_table[i] = nullptr;
        }

        ReleaseSRWLockExclusive(&m_lock);
    }

private:
    ConnectionEntry* m_table[CONNECTION_HASH_SIZE];
    SRWLOCK m_lock;
};

} // namespace infrastructure
} // namespace tcp_redirector