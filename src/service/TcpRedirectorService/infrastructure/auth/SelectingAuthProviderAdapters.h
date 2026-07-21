#pragma once

/**
 * @file SelectingAuthProviderAdapters.h
 * @brief Продакшн-адаптеры портов SelectingAuthProviderFactory (Variant 4b, Phase 6).
 *
 * Соединяют узкие порты выбирающей фабрики с реальными инфраструктурными
 * компонентами:
 *   - ConnectionTableSessionResolver : IUserSessionResolver
 *       ConnectionIdentity.client_port -> PID (ConnectionTable.GetInfo, иначе
 *       ProcessResolver::ResolvePidBySourcePort) -> WTS sessionId
 *       (ProcessIdToSessionId). Полностью соответствует §1.3. Никогда не бросает;
 *       любой сбой на цепочке => false (fallback на стороне фабрики).
 *   - ManagerParamsSource : IBrokeredParamsSource
 *       Делегирует AuthHelperManager::BuildBrokeredParams.
 *
 * Вынесены в отдельный header, чтобы SelectingAuthProviderFactory.h оставался
 * свободным от зависимостей на ConnectionTable/ProcessResolver/AuthHelperManager
 * и легко тестировался с фейками.
 *
 * См. plans/kerberos_per_user_auth_helper_plan.md §1.3/§5, Phase 6.
 */

#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <windows.h>

#include <cstdint>

#include "../../domain/ports/IConnectionTable.h"
#include "../../domain/ports/IConnectionMonitor.h"   // ILogSink
#include "../process/ProcessResolver.h"
#include "AuthHelperManager.h"
#include "SelectingAuthProviderFactory.h"

namespace tcp_redirector {
namespace infrastructure {
namespace auth {

/**
 * @brief Продакшн-резолвер сессии: client_port -> PID -> WTS sessionId (§1.3).
 *
 * Приоритет PID: ConnectionTable (задан на capture-time), иначе ProcessResolver
 * (скан TCP-таблицы с 30 с кэшем). Затем ProcessIdToSessionId. Потокобезопасен
 * (обе зависимости потокобезопасны; собственного мутабельного состояния нет).
 */
class ConnectionTableSessionResolver : public IUserSessionResolver {
public:
    /**
     * @param connTable  Таблица соединений (для PID по client_port). Не владеющий.
     * @param resolver   ProcessResolver-фолбэк. Не владеющий.
     * @param log        Опциональный лог.
     */
    ConnectionTableSessionResolver(domain::ports::IConnectionTable* connTable,
                                   process::ProcessResolver* resolver,
                                   domain::ports::ILogSink* log = nullptr)
        : m_connTable(connTable), m_resolver(resolver), m_log(log) {}

    bool ResolveSession(const domain::ports::ConnectionIdentity& id,
                        std::uint32_t& sessionIdOut) override {
        sessionIdOut = 0;

        const uint16_t clientPort = id.client_port;
        if (clientPort == 0) {
            Log(domain::LogLevel::Debug,
                "resolve: client_port==0 — cannot resolve session");
            return false;
        }

        // 1. PID: сначала из ConnectionTable (проставлен на capture-time), потом
        //    фолбэк на ProcessResolver (скан TCP-таблицы).
        uint32_t pid = 0;
        if (m_connTable) {
            domain::ports::ConnectionInfo info{};
            if (m_connTable->GetInfo(clientPort, &info) && info.pid != 0) {
                pid = info.pid;
            }
        }
        if (pid == 0 && m_resolver) {
            pid = m_resolver->ResolvePidBySourcePort(clientPort);
        }
        if (pid == 0) {
            Log(domain::LogLevel::Debug,
                "resolve: no PID for client_port=" + std::to_string(clientPort));
            return false;
        }

        // 2. PID -> WTS sessionId.
        DWORD sid = 0;
        if (!ProcessIdToSessionId(static_cast<DWORD>(pid), &sid)) {
            Log(domain::LogLevel::Debug,
                "resolve: ProcessIdToSessionId failed for pid=" +
                    std::to_string(pid) + " GLE=" +
                    std::to_string(GetLastError()));
            return false;
        }

        sessionIdOut = static_cast<std::uint32_t>(sid);
        Log(domain::LogLevel::Debug,
            "resolve: client_port=" + std::to_string(clientPort) + " -> pid=" +
                std::to_string(pid) + " -> session=" +
                std::to_string(sessionIdOut));
        return true;
    }

private:
    void Log(domain::LogLevel level, const std::string& msg) const {
        if (m_log) m_log->Log(level, "selectauth", msg);
    }

    domain::ports::IConnectionTable* m_connTable = nullptr;
    process::ProcessResolver*        m_resolver = nullptr;
    domain::ports::ILogSink*         m_log = nullptr;
};

/**
 * @brief Продакшн-источник BrokeredAuthParams поверх AuthHelperManager.
 *
 * Тонкая обёртка: делегирует BuildBrokeredParams менеджеру. Не владеющий
 * указатель на менеджер (живёт в ServiceMain дольше relay/фабрики).
 */
class ManagerParamsSource : public IBrokeredParamsSource {
public:
    explicit ManagerParamsSource(AuthHelperManager* manager)
        : m_manager(manager) {}

    bool BuildBrokeredParams(std::uint32_t sessionId,
                             infrastructure::BrokeredAuthParams& out) override {
        if (!m_manager) return false;
        return m_manager->BuildBrokeredParams(sessionId, out);
    }

private:
    AuthHelperManager* m_manager = nullptr;
};

} // namespace auth
} // namespace infrastructure
} // namespace tcp_redirector
