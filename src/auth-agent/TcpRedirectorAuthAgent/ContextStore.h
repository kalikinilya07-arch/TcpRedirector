#pragma once

#include <windows.h>
#include <security.h>
#include <string>
#include <unordered_map>
#include <chrono>
#include <mutex>
#include <cstdint>

namespace tcp_redirector {
namespace auth_agent {

/// Запись в кэше контекстов.
/// Содержит полный набор SSPI-хендлов для одного CONNECT.
struct ContextEntry {
    CredHandle credentials;   // хендл учётных данных
    CtxtHandle context;       // хендл контекста безопасности
    std::string spn;          // Service Principal Name
    std::chrono::steady_clock::time_point created_at;
    std::chrono::seconds ttl; // время жизни (по умолчанию 30 минут)
    bool in_use;              // контекст активен (не закрыт)

    bool IsExpired() const {
        auto now = std::chrono::steady_clock::now();
        return (now - created_at) > ttl;
    }
};

/// Потокобезопасное хранилище SSPI-контекстов.
///
/// Каждый ContextId живёт ровно один HTTP CONNECT.
/// После CloseContext() или истечения TTL — контекст уничтожается.
/// Повторное использование ContextId запрещено.
class ContextStore {
public:
    static constexpr auto kDefaultTtl = std::chrono::seconds(1800); // 30 минут

    ContextStore() = default;
    ~ContextStore();

    // Не копируемый
    ContextStore(const ContextStore&) = delete;
    ContextStore& operator=(const ContextStore&) = delete;

    /// Создать новый контекст. Возвращает уникальный ID.
    uint64_t Create(CredHandle credentials, CtxtHandle context,
                    std::string spn,
                    std::chrono::seconds ttl = kDefaultTtl);

    /// Получить контекст по ID. Возвращает nullptr если не найден или истёк.
    ContextEntry* Get(uint64_t id);

    /// Закрыть и удалить контекст.
    void Close(uint64_t id);

    /// Удалить все просроченные контексты.
    void CleanupExpired();

    /// Удалить все контексты (при завершении процесса).
    void Clear();

    /// Количество активных контекстов.
    size_t Size() const;

private:
    mutable std::mutex m_mutex;
    std::unordered_map<uint64_t, ContextEntry> m_contexts;
    uint64_t m_nextId = 1;

    void DestroyContext(ContextEntry& entry);
};

} // namespace auth_agent
} // namespace tcp_redirector