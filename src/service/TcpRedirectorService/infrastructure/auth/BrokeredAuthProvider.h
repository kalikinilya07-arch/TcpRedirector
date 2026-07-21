#pragma once

/**
 * @file BrokeredAuthProvider.h
 * @brief IAuthProvider поверх per-user helper'а через AuthBrokerClient (Phase 4).
 *
 * Variant 4b, Phase 4. См. plans/kerberos_per_user_auth_helper_plan.md §2/§3/§5,
 * IAuthProvider.h (Phase 1) и AuthBrokerClient.h.
 *
 * Реализует ТОЧНО тот же контракт domain::ports::IAuthProvider, что и
 * LocalSspiProvider, поэтому relay потреблять его может без изменений. В отличие
 * от LocalSspiProvider, токены производит НЕ локальный (машинный) SSPI, а
 * per-user helper: провайдер гоняет sspi_step по named-pipe к helper'у нужной
 * сессии и возвращает USER'ские Negotiate-токены.
 *
 * Жизненный цикл (1:1 с прежним стековым SspiContext в ConnectionHandler):
 *   - Один экземпляр = один handshake одного соединения relay.
 *   - Генерирует ОДИН correlation_id на экземпляр (на соединение), переиспользуя
 *     его между вызовами NextToken (§2.3).
 *   - Ленивое подключение AuthBrokerClient на ПЕРВОМ NextToken; на подключении
 *     сверяет hello: session_id == ожидаемому И nonce == ожидаемому (§3.2).
 *     Любое несовпадение => Failed (жёсткий отказ).
 *   - Деструктор шлёт release(correlation_id) (best-effort) и закрывает клиент.
 *
 * ПОЛИТИКА ОТКАТА (утверждённое решение fallback=drop): провайдер НИКОГДА не
 * откатывается молча на машинную аутентификацию. На любой сбой (нет helper'а /
 * таймаут / denied / parse / hello-mismatch) он возвращает Failed. Превращение
 * Failed в drop соединения — задача Phase 7; на Phase 4 возврат Failed корректен.
 *
 * Идентичность (session_id, ожидаемый nonce, SPN/proxy_host, timeout) передаётся
 * в конструктор извне: реальный резолв PID→session→SID и выбор
 * brokered-vs-local — это Phase 6. Так провайдер тестируется в изоляции.
 *
 * Не потокобезопасен: один экземпляр — один поток ConnectionHandler.
 */

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>

#include "shared/auth_broker/AuthBrokerProtocol.h"
#include "../../domain/ports/IAuthProvider.h"
#include "../../domain/ports/IConnectionMonitor.h"  // ILogSink, LogLevel
#include "AuthBrokerClient.h"

namespace tcp_redirector {
namespace infrastructure {

/**
 * @brief Параметры одного brokered-провайдера (заполняются Phase 5/6).
 *
 * На Phase 4 эти значения приходят из фабрики/теста, а не резолвятся из
 * соединения. Phase 6 заполнит session_id/expected_nonce/expected_server_pid из
 * реального резолва PID→session и записей AuthHelperManager.
 */
struct BrokeredAuthParams {
    std::uint32_t session_id = 0;         //!< Целевая WTS-сессия (имя pipe).
    std::string   expected_nonce;         //!< Ожидаемый nonce из hello (§3.2).
    std::string   spn;                    //!< SPN; если пусто — берётся из NextToken/derive.
    std::string   proxy_host;             //!< Хост прокси (для запроса/деривации SPN).
    int           helper_timeout_ms = 5000; //!< Таймаут pipe-операций (мс).

    // TODO(Phase 5): ожидаемый PID запущенного helper'а для сверки с
    //                GetNamedPipeServerProcessId (сейчас 0 = не сверять).
    DWORD         expected_server_pid = 0;

    // Явный correlation_id (для тестируемости/детерминизма). Пусто => провайдер
    // сгенерирует "conn-<uint64>" сам.
    std::string   correlation_id_override;
};

/**
 * @brief IAuthProvider через per-user helper (service-side brokered).
 */
class BrokeredAuthProvider : public domain::ports::IAuthProvider {
public:
    BrokeredAuthProvider(BrokeredAuthParams params,
                         domain::ports::ILogSink* logSink = nullptr)
        : m_params(std::move(params)), m_logSink(logSink) {
        m_correlationId = m_params.correlation_id_override.empty()
                              ? MakeCorrelationId()
                              : m_params.correlation_id_override;
    }

    ~BrokeredAuthProvider() override {
        // Best-effort release + закрытие клиента (RAII).
        if (m_client && m_client->IsConnected()) {
            m_client->SendRelease(m_correlationId);
        }
        m_client.reset();
    }

    BrokeredAuthProvider(const BrokeredAuthProvider&) = delete;
    BrokeredAuthProvider& operator=(const BrokeredAuthProvider&) = delete;

    /**
     * @brief Один шаг Negotiate через helper.
     *
     * На первом вызове лениво подключается к helper'у и валидирует hello. Затем
     * отправляет sspi_step и маппит статус ответа на AuthStepStatus. Никогда не
     * бросает; на любой сбой => Failed (no-fallback, §5 drop).
     *
     * @param spn          SPN цели; если задан — используется, иначе берётся из
     *                     params.spn, иначе derive из proxy_host ("HTTP/<host>").
     * @param serverToken  base64-challenge (пусто на первом шаге).
     * @param outToken     [out] исходящий Negotiate-токен (base64).
     */
    domain::ports::AuthStepStatus NextToken(const std::string& spn,
                                            const std::string& serverToken,
                                            std::string& outToken) override {
        outToken.clear();

        if (m_hardFailed) {
            // Однажды провалившись (mismatch/denied/io), не пытаемся снова —
            // no-fallback, детерминированный Failed до конца соединения.
            return domain::ports::AuthStepStatus::Failed;
        }

        if (!EnsureConnected()) {
            m_hardFailed = true;
            return domain::ports::AuthStepStatus::Failed;
        }

        shared::auth_broker::SspiStepRequest req;
        req.correlation_id = m_correlationId;
        req.spn = ResolveSpn(spn);
        req.proxy_host = m_params.proxy_host;
        req.server_token = serverToken;
        req.session_id = m_params.session_id;

        shared::auth_broker::SspiStepResponse resp;
        const BrokerCallResult cr = m_client->SendStep(req, resp);
        if (cr != BrokerCallResult::Ok) {
            // Таймаут / io / parse / too_large / not_connected => Failed.
            Log(domain::LogLevel::Warn,
                "sspi_step failed (" + std::string(ToString(cr)) +
                    ") corr=" + m_correlationId);
            m_hardFailed = true;
            return domain::ports::AuthStepStatus::Failed;
        }

        if (resp.status == shared::auth_broker::AuthStatus::Denied) {
            // SPN вне allow-list. Жёсткий отказ + WARN, НИКОГДА не молчаливый
            // откат на машинную аутентификацию (§2.4/§3.3).
            Log(domain::LogLevel::Warn,
                "helper DENIED spn='" + req.spn + "' (not on allow-list) corr=" +
                    m_correlationId +
                    (resp.detail.empty() ? "" : (" detail=" + resp.detail)));
            m_hardFailed = true;
            return domain::ports::AuthStepStatus::Failed;
        }

        outToken = resp.out_token;

        const shared::auth_broker::MappedAuthStepStatus mapped =
            shared::auth_broker::ToAuthStepStatus(resp.status);
        const domain::ports::AuthStepStatus result = Bridge(mapped);

        if (result == domain::ports::AuthStepStatus::Failed) {
            m_hardFailed = true;
            Log(domain::LogLevel::Warn,
                "helper error status corr=" + m_correlationId +
                    (resp.detail.empty() ? "" : (" detail=" + resp.detail)));
        }
        return result;
    }

    /**
     * @brief Phase 7: brokered-провайдер имеет drop-семантику.
     *
     * Любой Failed на per-user-пути обязан приводить к закрытию соединения, а не
     * к откату на машинную аутентификацию (§5 fallback=drop/error). relay читает
     * этот флаг и трактует Failed как терминальный drop.
     */
    bool DropOnFailure() const override { return true; }

private:
    // Мост shared-зеркала на реальный Phase 1 enum (значения совпадают).
    static domain::ports::AuthStepStatus Bridge(
        shared::auth_broker::MappedAuthStepStatus m) {
        return static_cast<domain::ports::AuthStepStatus>(static_cast<int>(m));
    }

    // Ленивое подключение + валидация hello (session_id + nonce).
    bool EnsureConnected() {
        if (m_client && m_client->IsConnected()) return true;
        if (m_connectAttempted) return false;  // одна попытка на соединение.
        m_connectAttempted = true;

        m_client = std::make_unique<AuthBrokerClient>(
            m_params.session_id, m_params.helper_timeout_ms, m_logSink);

        HelloResult hr;
        if (!m_client->Connect(hr)) {
            Log(domain::LogLevel::Warn,
                "connect to helper session=" + std::to_string(m_params.session_id) +
                    " failed: " + ToString(hr.result));
            m_client.reset();
            return false;
        }

        // §3.2: session_id из hello ДОЛЖЕН совпасть с целевой сессией.
        if (hr.hello.session_id != m_params.session_id) {
            Log(domain::LogLevel::Warn,
                "hello session_id mismatch: got " +
                    std::to_string(hr.hello.session_id) + " expected " +
                    std::to_string(m_params.session_id));
            m_client.reset();
            return false;
        }

        // §3.2: nonce из hello ДОЛЖЕН совпасть с выданным при запуске.
        if (hr.hello.nonce != m_params.expected_nonce) {
            Log(domain::LogLevel::Warn,
                "hello nonce mismatch (possible pipe squat) session=" +
                    std::to_string(m_params.session_id));
            m_client.reset();
            return false;
        }

        // TODO(Phase 5): при m_params.expected_server_pid != 0 сверить его с
        //                hr.server_pid и отклонить при несовпадении.
        if (m_params.expected_server_pid != 0 &&
            hr.server_pid != 0 &&
            hr.server_pid != m_params.expected_server_pid) {
            Log(domain::LogLevel::Warn,
                "hello server_pid mismatch: got " + std::to_string(hr.server_pid) +
                    " expected " + std::to_string(m_params.expected_server_pid));
            m_client.reset();
            return false;
        }

        Log(domain::LogLevel::Debug,
            "helper hello OK session=" + std::to_string(m_params.session_id) +
                " corr=" + m_correlationId);
        return true;
    }

    // SPN приоритет: аргумент NextToken -> params.spn -> derive из proxy_host.
    std::string ResolveSpn(const std::string& spnArg) const {
        if (!spnArg.empty()) return spnArg;
        if (!m_params.spn.empty()) return m_params.spn;
        if (!m_params.proxy_host.empty()) return "HTTP/" + m_params.proxy_host;
        return std::string();
    }

    static std::string MakeCorrelationId() {
        // Монотонно растущий счётчик процесса => уникальность в пределах жизни
        // службы; формат "conn-<uint64>" (§2.2).
        static std::atomic<std::uint64_t> s_counter{0};
        const std::uint64_t n = s_counter.fetch_add(1, std::memory_order_relaxed) + 1;
        return "conn-" + std::to_string(n);
    }

    void Log(domain::LogLevel level, const std::string& msg) const {
        if (m_logSink) {
            m_logSink->Log(level, "brokeredauth", msg);
        }
    }

    BrokeredAuthParams              m_params;
    domain::ports::ILogSink*        m_logSink = nullptr;
    std::string                     m_correlationId;
    std::unique_ptr<AuthBrokerClient> m_client;
    bool                            m_connectAttempted = false;
    bool                            m_hardFailed = false;
};

/**
 * @brief Фабрика brokered-провайдеров (Phase 4: доступна, но НЕ дефолт в relay).
 *
 * Реализует domain::ports::IAuthProviderFactory. Конструируется с параметрами
 * (session/nonce/SPN/timeout), общими для соединений; ConnectionIdentity в
 * Create() на Phase 4 игнорируется. Реальный per-connection резолв
 * session/SID + выбор local-vs-brokered — это Phase 6.
 *
 * Phase 6 должен: (1) при per_user_auth_enabled и успешном резолве
 * PID→session→SID вернуть BrokeredAuthProvider для нужной сессии; (2) заполнить
 * expected_nonce/expected_server_pid из записи AuthHelperManager для этой сессии;
 * (3) иначе (feature off / нет helper'а) вернуть LocalSspiProvider или дропнуть
 * по fallback_policy (drop по умолчанию).
 */
class BrokeredAuthProviderFactory : public domain::ports::IAuthProviderFactory {
public:
    explicit BrokeredAuthProviderFactory(BrokeredAuthParams params,
                                         domain::ports::ILogSink* logSink = nullptr)
        : m_params(std::move(params)), m_logSink(logSink) {}

    std::unique_ptr<domain::ports::IAuthProvider>
    Create(const domain::ports::ConnectionIdentity& /*identity*/) override {
        // Phase 4: identity игнорируется — параметры сессии заданы конструктором.
        // Phase 6 заменит это на per-connection резолв (см. doc-комментарий).
        return std::make_unique<BrokeredAuthProvider>(m_params, m_logSink);
    }

private:
    BrokeredAuthParams        m_params;
    domain::ports::ILogSink*  m_logSink = nullptr;
};

} // namespace infrastructure
} // namespace tcp_redirector
