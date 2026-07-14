#pragma once

#include <windows.h>
#include <security.h>
#include <string>
#include <string_view>
#include <vector>

namespace tcp_redirector {
namespace auth_agent {

/// Результат шага SSPI-negotiation.
struct SspiStepResult {
    bool success;
    std::string token;          // Base64-encoded SSPI token
    bool needs_continue;        // true если нужен ещё один раунд
    std::string error_message;
};

/// Обёртка над SSPI для Kerberos/Negotiate аутентификации.
///
/// Выполняет AcquireCredentialsHandleW + InitializeSecurityContextW
/// в контексте текущего процесса (пользовательская сессия).
///
/// Не хранит состояние между вызовами — состояние хранится в ContextStore.
class SspiEngine {
public:
    SspiEngine();
    ~SspiEngine();

    // Не копируемый
    SspiEngine(const SspiEngine&) = delete;
    SspiEngine& operator=(const SspiEngine&) = delete;

    /// Создать новый контекст безопасности для заданного SPN.
    /// Возвращает начальный токен для отправки прокси.
    SspiStepResult CreateContext(std::string_view spn,
                                  CredHandle& outCredentials,
                                  CtxtHandle& outContext);

    /// Продолжить контекст с challenge от прокси (ответ 407).
    SspiStepResult ContinueContext(CtxtHandle& context,
                                    std::string_view challenge);

    /// Освободить учётные данные.
    static void FreeCredentials(CredHandle& creds);

    /// Освободить контекст безопасности.
    static void DeleteContext(CtxtHandle& ctx);

private:
    static std::string Base64Encode(const std::vector<uint8_t>& data);
    static std::vector<uint8_t> Base64Decode(const std::string& data);
    static std::string MakeSpn(std::string_view host);
    static std::string SspiErrorText(SECURITY_STATUS sc);
};

} // namespace auth_agent
} // namespace tcp_redirector