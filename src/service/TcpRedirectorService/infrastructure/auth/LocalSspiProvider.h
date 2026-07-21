#pragma once

/**
 * @file LocalSspiProvider.h
 * @brief Локальная реализация IAuthProvider поверх существующего SSPI.
 *
 * Оборачивает infrastructure::SspiNegotiate БЕЗ изменения семантики: SSPI
 * выполняется в текущем процессе (LocalSystem), т.е. под МАШИННЫМ аккаунтом
 * домена (COMPUTERNAME$). Это в точности прежнее поведение relay до Phase 1 —
 * никакого per-user helper здесь нет.
 *
 * Каждый экземпляр держит свой SspiContext на всё время одного соединения,
 * повторяя прежнее время жизни стекового SspiContext в ConnectionHandler.
 *
 * См. plans/kerberos_per_user_auth_helper_plan.md §4.1.
 * Разработчик: Kalikin Iliya
 */

#include <string>
#include <memory>

#include "../../domain/ports/IAuthProvider.h"
#include "auth_sspi.h"

namespace tcp_redirector {
namespace infrastructure {

/**
 * @brief IAuthProvider на базе локального SSPI (машинный аккаунт).
 *
 * Не потокобезопасен: обслуживает один handshake в одном потоке.
 */
class LocalSspiProvider : public domain::ports::IAuthProvider {
public:
    LocalSspiProvider() = default;
    ~LocalSspiProvider() override = default;

    LocalSspiProvider(const LocalSspiProvider&) = delete;
    LocalSspiProvider& operator=(const LocalSspiProvider&) = delete;

    /**
     * @brief Выполнить один шаг Negotiate через infrastructure::SspiNegotiate.
     *
     * Прямое, поведение-сохраняющее делегирование к прежней функции. Маппинг
     * SspiResult → AuthStepStatus подобран так, чтобы relay-логика отката
     * осталась идентичной прежней (NoCredentials/Error → отключение SSPI на
     * соединении; NeedContinue → повтор CONNECT; Complete/Success → готово).
     */
    domain::ports::AuthStepStatus NextToken(const std::string& spn,
                                            const std::string& serverToken,
                                            std::string& outToken) override {
        const SspiResult r = SspiNegotiate(m_ctx, serverToken, outToken, spn);
        switch (r) {
            case SspiResult::NeedContinue:
                return domain::ports::AuthStepStatus::ContinueNeeded;
            case SspiResult::NoCredentials:
                return domain::ports::AuthStepStatus::NoCredentials;
            case SspiResult::Error:
                return domain::ports::AuthStepStatus::Failed;
            case SspiResult::Complete:
            case SspiResult::Success:
            default:
                return domain::ports::AuthStepStatus::Complete;
        }
    }

private:
    SspiContext m_ctx;  //!< Per-connection SSPI-состояние (credentials + context).
};

/**
 * @brief Фабрика, всегда создающая LocalSspiProvider.
 *
 * Phase 1: единственный источник провайдеров. Идентичность соединения
 * игнорируется (машинный аккаунт не зависит от вызывающего процесса). На
 * Phase 4/6 сюда (или в новую фабрику) добавится выбор BrokeredAuthProvider
 * при per_user_auth_enabled и наличии helper'а для сессии.
 */
class LocalSspiProviderFactory : public domain::ports::IAuthProviderFactory {
public:
    std::unique_ptr<domain::ports::IAuthProvider>
    Create(const domain::ports::ConnectionIdentity& /*identity*/) override {
        return std::make_unique<LocalSspiProvider>();
    }
};

} // namespace infrastructure
} // namespace tcp_redirector
