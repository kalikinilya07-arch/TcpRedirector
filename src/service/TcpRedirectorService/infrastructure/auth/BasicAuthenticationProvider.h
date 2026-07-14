#pragma once

#include "../../domain/ports/IAuthenticationProvider.h"
#include <string>

namespace tcp_redirector {
namespace infrastructure {

/// Провайдер Basic Authentication.
///
/// Инкапсулирует существующую логику Base64(login:password).
/// Не поддерживает многошаговую аутентификацию (needs_continue всегда false).
/// Используется как fallback при KerberosPreferred или изолированно при BasicOnly.
class BasicAuthenticationProvider : public domain::ports::IAuthenticationProvider {
public:
    BasicAuthenticationProvider(std::string username, std::string password)
        : m_username(std::move(username))
        , m_password(std::move(password)) {}

    domain::ports::CreateContextResult CreateContext(
        std::string_view /*spn*/) override;

    domain::ports::ContinueContextResult ContinueContext(
        uint64_t context_id,
        std::string_view challenge) override;

    void CloseContext(uint64_t context_id) override;
    void Reset() override;

    domain::ports::AuthProviderType GetType() const override {
        return domain::ports::AuthProviderType::Basic;
    }

private:
    std::string m_username;
    std::string m_password;

    static std::string Base64Encode(const std::string& input);
};

} // namespace infrastructure
} // namespace tcp_redirector