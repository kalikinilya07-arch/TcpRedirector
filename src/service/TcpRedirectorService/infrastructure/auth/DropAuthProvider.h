#pragma once

/**
 * @file DropAuthProvider.h
 * @brief Тривиальный IAuthProvider, всегда возвращающий Failed (Variant 4b, Phase 6/7).
 *
 * Используется SelectingAuthProviderFactory, когда per-user-аутентификация НЕ
 * может быть выполнена (не удалось резолвить PID→session, нет helper'а для
 * сессии, и т.п.), а действующая политика отката — Drop (утверждённый дефолт)
 * или Error. В этом случае фабрика возвращает DropAuthProvider, чей первый же
 * NextToken отдаёт AuthStepStatus::Failed. relay (Phase 7), увидев Failed от
 * провайдера с DropOnFailure()==true, ЖЁСТКО закрывает соединение и НИКОГДА не
 * откатывается на машинную (LocalSystem) SSPI-аутентификацию.
 *
 * См. plans/kerberos_per_user_auth_helper_plan.md §5.
 */

#include <string>

#include "../../domain/ports/IAuthProvider.h"

namespace tcp_redirector {
namespace infrastructure {

/**
 * @brief Провайдер-«заглушка», всегда возвращающий Failed (drop-семантика).
 *
 * Не потокобезопасен (как и остальные провайдеры) — один экземпляр на одно
 * соединение. Не производит никаких токенов и не имеет побочных эффектов.
 */
class DropAuthProvider : public domain::ports::IAuthProvider {
public:
    DropAuthProvider() = default;
    ~DropAuthProvider() override = default;

    DropAuthProvider(const DropAuthProvider&) = delete;
    DropAuthProvider& operator=(const DropAuthProvider&) = delete;

    /**
     * @brief Всегда возвращает Failed, очищая outToken.
     *
     * Аргументы игнорируются: провайдер не выполняет никакого handshake.
     */
    domain::ports::AuthStepStatus NextToken(const std::string& /*spn*/,
                                            const std::string& /*serverToken*/,
                                            std::string& outToken) override {
        outToken.clear();
        return domain::ports::AuthStepStatus::Failed;
    }

    /**
     * @brief Drop-семантика: Failed => закрыть соединение (§5 drop/error).
     */
    bool DropOnFailure() const override { return true; }
};

} // namespace infrastructure
} // namespace tcp_redirector
