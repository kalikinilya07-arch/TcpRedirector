#pragma once

/**
 * @file ConnectionTable.h
 * @brief Реализация потокобезопасной хеш-таблицы соединений.
 *
 * Отображает локальный порт (src_port) на оригинальный адрес назначения,
 * PID процесса, путь к процессу и счётчики переданных байт.
 * Реализация: массив связных списков + SRWLOCK (как в ProxyBridge).
 *
 * При первом SYN целевого процесса сохраняем (src_port → origDstIP, origDstPort, proxyConfigId).
 * Последующие пакеты от того же src_port перенаправляются на relay.
 * Ответные пакеты от relay восстанавливают original DST.
 */

#include <windows.h>
#include <cstdint>
#include <cstring>
#include <atomic>

#include "../../domain/ports/IConnectionTable.h"

#ifndef CONNECTION_HASH_SIZE
//! Размер хеш-таблицы (количество бакетов). Должен быть степенью двойки.
#define CONNECTION_HASH_SIZE 4096
#endif

namespace tcp_redirector {
namespace infrastructure {

/**
 * @brief Элемент связного списка хеш-таблицы соединений.
 *
 * Хранит полную информацию об отслеживаемом TCP-соединении:
 * оригинальный адрес назначения, идентификатор процесса, путь,
 * счётчики переданных байт в обоих направлениях.
 */
struct ConnectionEntry {
    uint16_t src_port;              //!< Исходный порт на локальной машине (ключ)
    uint32_t src_ip;                //!< IP-адрес источника
    uint32_t orig_dest_ip;          //!< Оригинальный IP-адрес назначения
    uint16_t orig_dest_port;        //!< Оригинальный порт назначения
    uint32_t proxy_config_id;       //!< ID конфигурации прокси
    bool     is_tracked;            //!< Флаг: соединение под наблюдением
    uint32_t pid;                   //!< PID процесса-владельца соединения
    wchar_t  proc_path[260];        //!< Полный путь к исполняемому файлу процесса
    uint64_t bytes_up;              //!< Байт, переданных от клиента к цели
    uint64_t bytes_down;            //!< Байт, переданных от цели к клиенту
    ConnectionEntry* next;          //!< Указатель на следующий элемент в цепочке коллизий
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

    /// Number of currently tracked connections.
    uint32_t GetTrackedCount() const { return m_trackedCount.load(std::memory_order_relaxed); }

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
        m_trackedCount.fetch_add(1, std::memory_order_relaxed);

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
                if (m_trackedCount.load(std::memory_order_relaxed) > 0)
                    m_trackedCount.fetch_sub(1, std::memory_order_relaxed);
                ReleaseSRWLockExclusive(&m_lock);
                return;
            }
            pp = &(*pp)->next;
        }

        ReleaseSRWLockExclusive(&m_lock);
    }

    // Set process info for a connection (PID + path)
    void SetProcessInfo(uint16_t src_port, uint32_t pid, const wchar_t* proc_path) override {
        AcquireSRWLockExclusive(&m_lock);

        int hash = src_port % CONNECTION_HASH_SIZE;
        ConnectionEntry* entry = m_table[hash];
        while (entry) {
            if (entry->src_port == src_port && entry->is_tracked) {
                entry->pid = pid;
                if (proc_path) {
                    wcscpy_s(entry->proc_path, proc_path);
                }
                ReleaseSRWLockExclusive(&m_lock);
                return;
            }
            entry = entry->next;
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
        m_trackedCount.store(0, std::memory_order_relaxed);

        ReleaseSRWLockExclusive(&m_lock);
    }

    // Добавить байты к существующему соединению
    void AddBytes(uint16_t src_port, uint64_t up, uint64_t down) override {
        AcquireSRWLockExclusive(&m_lock);

        int hash = src_port % CONNECTION_HASH_SIZE;
        ConnectionEntry* entry = m_table[hash];
        while (entry) {
            if (entry->src_port == src_port && entry->is_tracked) {
                entry->bytes_up += up;
                entry->bytes_down += down;
                ReleaseSRWLockExclusive(&m_lock);
                return;
            }
            entry = entry->next;
        }

        ReleaseSRWLockExclusive(&m_lock);
    }

    // Получить полную информацию о соединении
    bool GetInfo(uint16_t src_port, domain::ports::ConnectionInfo* info) override {
        AcquireSRWLockShared(&m_lock);

        int hash = src_port % CONNECTION_HASH_SIZE;
        ConnectionEntry* entry = m_table[hash];
        while (entry) {
            if (entry->src_port == src_port && entry->is_tracked) {
                info->pid = entry->pid;
                wcscpy_s(info->proc_path, entry->proc_path);
                info->bytes_up = entry->bytes_up;
                info->bytes_down = entry->bytes_down;
                ReleaseSRWLockShared(&m_lock);
                return true;
            }
            entry = entry->next;
        }

        ReleaseSRWLockShared(&m_lock);
        return false;
    }

private:
    ConnectionEntry* m_table[CONNECTION_HASH_SIZE];
    SRWLOCK m_lock;
    std::atomic<uint32_t> m_trackedCount{0};
};

} // namespace infrastructure
} // namespace tcp_redirector