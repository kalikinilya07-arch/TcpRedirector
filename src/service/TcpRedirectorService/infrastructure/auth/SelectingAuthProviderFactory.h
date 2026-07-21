#pragma once

/**
 * @file SelectingAuthProviderFactory.h
 * @brief Выбирающая фабрика IAuthProvider (Variant 4b, Phase 6/7).
 *
 * Центральная точка принятия решения «какой провайдер аутентификации отдать на
 * данное соединение». Заменяет дефолтную LocalSspiProviderFactory в relay, когда
 * включён per-user Kerberos helper. Инкапсулирует ВСЮ политику §5:
 *
 *   ┌────────────────────────────┬──────────────────────────────────────────────┐
 *   │ Условие                     │ Возвращаемый провайдер                        │
 *   ├────────────────────────────┼──────────────────────────────────────────────┤
 *   │ per_user_auth_enabled=false │ LocalSspiProvider (legacy, машинный аккаунт)  │
 *   │ резолв session OK +         │ BrokeredAuthProvider (per-user helper)        │
 *   │   BuildBrokeredParams OK    │                                               │
 *   │ резолв FAIL / нет helper'а: │                                               │
 *   │   fallback_policy=Drop      │ DropAuthProvider (Failed => relay drop)       │
 *   │   fallback_policy=Error     │ DropAuthProvider (Failed => drop, + ERROR лог)│
 *   │   fallback_policy=System    │ LocalSspiProvider (legacy машинный аккаунт)   │
 *   └────────────────────────────┴──────────────────────────────────────────────┘
 *
 * Дизайн ради тестируемости: фабрика зависит НЕ от конкретных
 * ProcessResolver/ConnectionTable/AuthHelperManager, а от двух узких портов:
 *   - IUserSessionResolver     — ConnectionIdentity -> WTS sessionId;
 *   - IBrokeredParamsSource    — sessionId -> BrokeredAuthParams (есть ли helper).
 * Продакшн-адаптеры к реальным инфраструктурным компонентам объявлены ниже и
 * реализованы в SelectingAuthProviderFactory.cpp. Юнит-тесты подставляют фейки и
 * проверяют таблицу решений без Win32/сети.
 *
 * Потокобезопасность: Create() вызывается из потоков ConnectionHandler
 * параллельно. Сама фабрика не хранит мутабельного состояния (только константные
 * параметры + указатели на потокобезопасные зависимости), поэтому Create()
 * реентерабелен. Возвращаемые провайдеры — по одному на соединение (не шарятся).
 *
 * См. plans/kerberos_per_user_auth_helper_plan.md §1.3/§5, Phase 6/7.
 */

#include <cstdint>
#include <memory>
#include <string>

#include "../../domain/entities/ProxyConfig.h"        // domain::AuthFallbackPolicy
#include "../../domain/ports/IAuthProvider.h"
#include "../../domain/ports/IConnectionMonitor.h"     // ILogSink, LogLevel
#include "BrokeredAuthProvider.h"                       // BrokeredAuthParams
#include "DropAuthProvider.h"
#include "LocalSspiProvider.h"

namespace tcp_redirector {
namespace infrastructure {
namespace auth {

/**
 * @brief Порт: резолв идентичности соединения в WTS-сессию пользователя.
 *
 * Продакшн-реализация (ConnectionTableSessionResolver, см. ниже) идёт по цепочке
 * §1.3: PID из ConnectionTable по client_port, иначе
 * ProcessResolver::ResolvePidBySourcePort, затем ProcessIdToSessionId(pid,&sid).
 * Любой сбой на этой цепочке => возврат false (устойчиво к гонкам ProcessResolver,
 * никогда не бросает).
 */
class IUserSessionResolver {
public:
    virtual ~IUserSessionResolver() = default;

    /**
     * @brief Резолвить сессию для соединения.
     * @param id          Идентичность соединения (используется client_port).
     * @param sessionIdOut [out] WTS session id при успехе.
     * @return true, если удалось получить sessionId; false при любом сбое.
     */
    virtual bool ResolveSession(const domain::ports::ConnectionIdentity& id,
                                std::uint32_t& sessionIdOut) = 0;
};

/**
 * @brief Порт: построение BrokeredAuthParams для сессии (наличие helper'а).
 *
 * Продакшн-реализация (ManagerParamsSource) делегирует
 * AuthHelperManager::BuildBrokeredParams. Возврат false означает «нет helper'а
 * для этой сессии» => фабрика применит fallback_policy.
 */
class IBrokeredParamsSource {
public:
    virtual ~IBrokeredParamsSource() = default;

    /**
     * @brief Собрать параметры брокера для сессии.
     * @param sessionId  Целевая WTS-сессия.
     * @param out        [out] Заполненные BrokeredAuthParams при успехе.
     * @return true, если helper для сессии есть и out заполнен; иначе false.
     */
    virtual bool BuildBrokeredParams(std::uint32_t sessionId,
                                     infrastructure::BrokeredAuthParams& out) = 0;
};

/**
 * @brief Конфигурация выбирающей фабрики (снимок из ProxyConfig на старте).
 */
struct SelectingAuthProviderConfig {
    bool                       perUserEnabled = false;   //!< per_user_auth_enabled.
    domain::AuthFallbackPolicy fallbackPolicy =
        domain::AuthFallbackPolicy::Drop;                //!< Политика §5 (default Drop).
    std::string                spn;                       //!< Явный SPN (может быть пуст).
    std::string                proxyHost;                 //!< Хост прокси (для derive SPN).
    int                        helperTimeoutMs = 5000;    //!< Таймаут helper'а (мс).
};

/**
 * @brief Выбирающая фабрика провайдеров аутентификации (см. заголовок файла).
 */
class SelectingAuthProviderFactory : public domain::ports::IAuthProviderFactory {
public:
    /**
     * @brief Конструктор.
     * @param cfg           Конфиг (feature flag, fallback_policy, SPN/host, timeout).
     * @param resolver      Резолвер сессии (может быть nullptr => резолв всегда FAIL).
     * @param paramsSource  Источник BrokeredAuthParams (может быть nullptr => нет helper'ов).
     * @param log           Опциональный лог (nullptr => молча).
     *
     * resolver/paramsSource — не владеющие указатели; их время жизни должно
     * покрывать время жизни фабрики (в ServiceMain оба живут в самой фабрике или
     * в AuthHelperManager, который живёт дольше relay).
     */
    SelectingAuthProviderFactory(SelectingAuthProviderConfig cfg,
                                 IUserSessionResolver* resolver,
                                 IBrokeredParamsSource* paramsSource,
                                 domain::ports::ILogSink* log = nullptr)
        : m_cfg(std::move(cfg)),
          m_resolver(resolver),
          m_paramsSource(paramsSource),
          m_log(log) {}

    /**
     * @brief Создать провайдер для одного соединения (реализация таблицы §5).
     */
    std::unique_ptr<domain::ports::IAuthProvider>
    Create(const domain::ports::ConnectionIdentity& identity) override {
        // 1. Feature off => строго legacy-поведение (машинный аккаунт).
        if (!m_cfg.perUserEnabled) {
            return std::make_unique<LocalSspiProvider>();
        }

        // 2. Резолв PID->session (устойчив к сбоям — при FAIL уходим в fallback).
        std::uint32_t sessionId = 0;
        const bool resolved =
            m_resolver && m_resolver->ResolveSession(identity, sessionId);

        if (resolved) {
            // 3. Есть ли helper для этой сессии? Собираем параметры брокера.
            infrastructure::BrokeredAuthParams params;
            if (m_paramsSource &&
                m_paramsSource->BuildBrokeredParams(sessionId, params)) {
                // Подмешиваем SPN/host/timeout из конфига, если менеджер их не
                // задал (менеджер уже кладёт свои значения; конфиг — запасной).
                if (params.spn.empty())        params.spn = m_cfg.spn;
                if (params.proxy_host.empty()) params.proxy_host = m_cfg.proxyHost;
                if (params.helper_timeout_ms <= 0)
                    params.helper_timeout_ms = m_cfg.helperTimeoutMs;

                Log(domain::LogLevel::Debug,
                    "per-user auth: brokered provider for session " +
                        std::to_string(sessionId) + " (client_port=" +
                        std::to_string(identity.client_port) + ")");
                return std::make_unique<BrokeredAuthProvider>(std::move(params), m_log);
            }

            // Сессия резолвлена, но helper'а нет => fallback.
            Log(domain::LogLevel::Warn,
                "per-user auth: no helper for session " +
                    std::to_string(sessionId) + " (client_port=" +
                    std::to_string(identity.client_port) + ") — applying fallback");
        } else {
            // Не смогли резолвить сессию (гонка ProcessResolver / нет PID / внешний
            // tun2socks и т.п.) => fallback.
            Log(domain::LogLevel::Warn,
                "per-user auth: could not resolve user session (client_port=" +
                    std::to_string(identity.client_port) +
                    ") — applying fallback");
        }

        // 4. Применяем fallback_policy (§5).
        return MakeFallbackProvider(identity);
    }

private:
    // Реализация ветки fallback по политике (§5). Drop/Error => DropAuthProvider
    // (Failed => relay закроет соединение); System => LocalSspiProvider (legacy).
    std::unique_ptr<domain::ports::IAuthProvider>
    MakeFallbackProvider(const domain::ports::ConnectionIdentity& identity) const {
        switch (m_cfg.fallbackPolicy) {
            case domain::AuthFallbackPolicy::System:
                Log(domain::LogLevel::Warn,
                    "per-user auth fallback_policy=system: using machine-account "
                    "SSPI (LocalSystem) for client_port=" +
                        std::to_string(identity.client_port));
                return std::make_unique<LocalSspiProvider>();

            case domain::AuthFallbackPolicy::Error:
                Log(domain::LogLevel::Error,
                    "per-user auth fallback_policy=error: dropping connection "
                    "(client_port=" + std::to_string(identity.client_port) + ")");
                return std::make_unique<DropAuthProvider>();

            case domain::AuthFallbackPolicy::Drop:
            default:
                Log(domain::LogLevel::Warn,
                    "per-user auth fallback_policy=drop: dropping connection "
                    "(client_port=" + std::to_string(identity.client_port) + ")");
                return std::make_unique<DropAuthProvider>();
        }
    }

    void Log(domain::LogLevel level, const std::string& msg) const {
        if (m_log) m_log->Log(level, "selectauth", msg);
    }

    SelectingAuthProviderConfig m_cfg;
    IUserSessionResolver*       m_resolver = nullptr;
    IBrokeredParamsSource*      m_paramsSource = nullptr;
    domain::ports::ILogSink*    m_log = nullptr;
};

} // namespace auth
} // namespace infrastructure
} // namespace tcp_redirector
