#pragma once

/**
 * @file AuthPipeServer.h
 * @brief Helper-side named-pipe СЕРВЕР брокера токенов SSPI/Negotiate.
 *
 * Variant 4b, Phase 3. См. plans/kerberos_per_user_auth_helper_plan.md:
 *   §1.4 (helper process), §2 (IPC), §3 (security).
 *
 * РОЛЬ: helper — СЕРВЕР pipe, служба (LocalSystem) — КЛИЕНТ (роли обратны
 * GUI-каналу в PipeServer.h). Helper выполняется В КОНТЕКСТЕ ПОЛЬЗОВАТЕЛЯ, поэтому
 * AcquireCredentialsHandleW(NULL, L"Negotiate", ...) в auth_sspi.cpp получает
 * КЕРБЕРОС-БИЛЕТ ЭТОГО ПОЛЬЗОВАТЕЛЯ (его TGT), а не машинного аккаунта.
 *
 * БЕЗОПАСНОСТЬ (§3):
 *   1. Pipe создаётся с SDDL, допускающим ТОЛЬКО:
 *        - владельца-пользователя (SID из собственного токена, TokenUser), и
 *        - LocalSystem (S-1-5-18) — служба.
 *      Больше НИКОГО (в т.ч. НЕ даём BUILTIN\Administrators, чтобы локальный
 *      админ из другой сессии не мог открыть чужой helper-pipe).
 *   2. После подключения клиента helper вызывает ImpersonateNamedPipeClient +
 *      GetTokenInformation(TokenUser) и требует, чтобы вызывающий был LocalSystem.
 *      Иначе — отключаем клиента (defense-in-depth поверх ACL).
 *   3. FILE_FLAG_FIRST_PIPE_INSTANCE на первом инстансе — защита от squatting'а
 *      имени pipe (как в PipeServer.h).
 *   4. SPN allow-list: sspi_step обслуживается ТОЛЬКО для SPN из списка (ожидаемый
 *      SPN прокси). Иначе status="denied" — helper не должен быть генератором
 *      Negotiate-токенов под произвольные сервисы (token-oracle scoping, §3.3).
 *
 * СОСТОЯНИЕ HANDSHAKE:
 *   Многошаговый Negotiate идентифицируется correlation_id. На каждый
 *   correlation_id держим SspiContext (auth_sspi.h). release уничтожает контекст;
 *   TTL-GC подчищает брошенные контексты (если release потерялся).
 *
 * КЭШ CREDENTIAL HANDLE:
 *   AcquireCredentialsHandle — самая дорогая операция. Делаем её ОДИН РАЗ на
 *   (пользователь фиксирован процессом), переиспользуем CredHandle между шагами и
 *   соединениями. Реализовано через "seed"-SspiContext: свежесозданный
 *   per-correlation контекст копирует уже полученный CredHandle из кэша, а
 *   AcquireCredentialsHandle повторно НЕ вызывается.
 *
 * Разработчик: Phase 3 (helper EXE).
 */

#include <windows.h>
#include <sddl.h>
#include <security.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "../TcpRedirectorService/infrastructure/auth/auth_sspi.h"
#include "../../shared/auth_broker/AuthBrokerProtocol.h"
#include "HelperLog.h"

#pragma comment(lib, "advapi32.lib")
#pragma comment(lib, "secur32.lib")

namespace tcp_redirector {
namespace auth_helper {

namespace protocol = tcp_redirector::shared::auth_broker;
namespace sspi = tcp_redirector::infrastructure;

/**
 * @brief RAII-деструктор SECURITY_DESCRIPTOR из
 *        ConvertStringSecurityDescriptorToSecurityDescriptorW (LocalAlloc).
 * Зеркалит LocalFreeDeleter из PipeServer.h.
 */
struct LocalFreeDeleter {
    void operator()(PSECURITY_DESCRIPTOR p) const noexcept {
        if (p) ::LocalFree(p);
    }
};
using SdPtr = std::unique_ptr<std::remove_pointer_t<PSECURITY_DESCRIPTOR>, LocalFreeDeleter>;

/**
 * @brief Конфигурация helper-сервера, заполняется из командной строки в main().
 */
struct AuthPipeServerConfig {
    std::uint32_t sessionId = 0;    //!< Сессия, которую обслуживаем; в имени pipe.
    std::string   pipeName;         //!< Полное имя pipe (MakeAuthPipeNameW).
    std::string   nonce;            //!< Одноразовый секрет от менеджера (hello).
    std::string   helperVersion;    //!< Версия helper.exe (диагностика в hello).
    // SPN allow-list. Источник: аргументы --spn (может повторяться) ЛИБО derive из
    // --proxy-host (HTTP/<host>). Пустой список => helper отклоняет ВСЕ sspi_step
    // (fail-closed): без явного ожидаемого SPN мы не работаем оракулом.
    std::vector<std::string> allowedSpns;
    std::uint32_t contextTtlSeconds = 30;  //!< TTL брошенных SspiContext (§2.3).
    std::uint32_t idleExitSeconds = 0;     //!< 0 = не выходить по простою.
};

/**
 * @brief Named-pipe сервер helper'а: один клиент (служба) за раз, с переоткрытием.
 */
class AuthPipeServer {
public:
    explicit AuthPipeServer(AuthPipeServerConfig cfg)
        : m_cfg(std::move(cfg)) {}

    ~AuthPipeServer() {
        Stop();
        ClearAllContexts();
        FreeCachedCredentials();
    }

    /**
     * @brief Основной блокирующий цикл сервера. Возвращается при m_running=false
     *        (например, по сигналу stdin/родителя) или фатальной ошибке создания pipe.
     */
    void Run() {
        m_running = true;
        std::wstring wPipe = Widen(m_cfg.pipeName);
        bool firstInstance = true;

        LogInfo("AuthPipeServer starting: pipe=" + m_cfg.pipeName +
                " session=" + std::to_string(m_cfg.sessionId) +
                " allowed_spns=" + std::to_string(m_cfg.allowedSpns.size()));

        while (m_running.load(std::memory_order_relaxed)) {
            auto [sa, sdOwner] = MakeOwnerAndSystemOnlySA();
            if (!sa.lpSecurityDescriptor) {
                LogError("Failed to build pipe security descriptor; retrying in 1s");
                std::this_thread::sleep_for(std::chrono::seconds(1));
                continue;
            }

            DWORD pipeFlags = PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE | PIPE_WAIT;
            if (firstInstance) pipeFlags |= FILE_FLAG_FIRST_PIPE_INSTANCE;

            HANDLE pipe = CreateNamedPipeW(
                wPipe.c_str(),
                PIPE_ACCESS_DUPLEX,
                pipeFlags,
                1,  // одна инстанция: один клиент-служба на сессию за раз.
                static_cast<DWORD>(protocol::kMaxMessageSize),
                static_cast<DWORD>(protocol::kMaxMessageSize),
                0,
                &sa);

            if (pipe == INVALID_HANDLE_VALUE) {
                DWORD err = GetLastError();
                // ERROR_ACCESS_DENIED/ERROR_PIPE_BUSY при FIRST_PIPE_INSTANCE =>
                // кто-то уже занял имя (возможный squat). Логируем и ждём.
                LogError("CreateNamedPipeW failed err=" + std::to_string(err) +
                         (firstInstance ? " (first instance; possible squat)" : ""));
                std::this_thread::sleep_for(std::chrono::seconds(1));
                continue;
            }
            firstInstance = false;

            {
                std::lock_guard<std::mutex> lk(m_pipeMutex);
                m_hPipe = pipe;
            }

            BOOL connected = ConnectNamedPipe(pipe, nullptr);
            if (!connected && GetLastError() == ERROR_PIPE_CONNECTED) {
                connected = TRUE;
            }
            if (!m_running.load(std::memory_order_relaxed)) {
                ClosePipe(pipe);
                break;
            }
            if (!connected) {
                LogDebug("ConnectNamedPipe failed err=" + std::to_string(GetLastError()));
                ClosePipe(pipe);
                continue;
            }

            HandleClient(pipe);
            ClosePipe(pipe);

            // GC брошенных контекстов между клиентами.
            GcContexts();
        }

        LogInfo("AuthPipeServer stopped");
    }

    /// Сигнализировать циклу об остановке и разблокировать pending I/O.
    void Stop() {
        m_running = false;
        std::lock_guard<std::mutex> lk(m_pipeMutex);
        if (m_hPipe != INVALID_HANDLE_VALUE) {
            CancelIoEx(m_hPipe, nullptr);
        }
    }

private:
    // ======================================================================
    // Обслуживание одного подключённого клиента
    // ======================================================================
    void HandleClient(HANDLE pipe) {
        // --- §3.2: клиент ОБЯЗАН быть LocalSystem ---
        if (!VerifyCallerIsLocalSystem(pipe)) {
            LogWarn("Rejecting non-LocalSystem client on auth pipe");
            DisconnectNamedPipe(pipe);
            return;
        }

        LogDebug("LocalSystem client connected");

        // --- §2.1/§3.2: сразу отправляем hello с nonce ---
        {
            protocol::HelloMessage hello;
            hello.session_id = m_cfg.sessionId;
            hello.nonce = m_cfg.nonce;
            hello.helper_version = m_cfg.helperVersion;
            std::string framed;
            if (protocol::FrameMessage(protocol::Serialize(hello), framed)) {
                if (!WriteMessage(pipe, framed)) {
                    LogDebug("Failed to write hello; client gone");
                    return;
                }
            }
        }

        // --- Цикл запрос→ответ ---
        std::string raw;
        while (m_running.load(std::memory_order_relaxed)) {
            if (!ReadMessage(pipe, raw)) {
                LogDebug("Client disconnected or read error");
                break;
            }

            if (!protocol::IsFrameWithinLimit(raw)) {
                LogWarn("Received oversized frame; replying error");
                SendStepError(pipe, "frame_too_large");
                continue;
            }

            std::string op;
            protocol::ParseError pe = protocol::PeekOp(raw, op);
            if (pe != protocol::ParseError::Ok) {
                LogWarn(std::string("PeekOp failed: ") + protocol::ToString(pe));
                SendStepError(pipe, std::string("peek_op:") + protocol::ToString(pe));
                continue;
            }

            if (op == protocol::kOpSspiStep) {
                HandleSspiStep(pipe, raw);
            } else if (op == protocol::kOpRelease) {
                HandleRelease(raw);
                // release не требует ответа по протоколу; продолжаем цикл.
            } else if (op == protocol::kOpPing) {
                HandlePing(pipe);
            } else {
                LogWarn("Unknown op: " + op);
                SendStepError(pipe, "unknown_op");
            }
        }
    }

    // ----------------------------------------------------------------------
    // sspi_step
    // ----------------------------------------------------------------------
    void HandleSspiStep(HANDLE pipe, const std::string& raw) {
        protocol::SspiStepRequest req;
        protocol::ParseError pe = protocol::ParseSspiStepRequest(raw, req);
        if (pe != protocol::ParseError::Ok) {
            LogWarn(std::string("Parse sspi_step failed: ") + protocol::ToString(pe));
            SendStepError(pipe, std::string("parse:") + protocol::ToString(pe));
            return;
        }

        // §3.3: SPN allow-list — жёсткий отказ вне списка (denied, не error).
        if (!IsSpnAllowed(req.spn)) {
            LogWarn("SPN not in allow-list, denying: '" + req.spn + "'");
            SendStepStatus(pipe, protocol::AuthStatus::Denied, "",
                           "spn_not_allowed");
            return;
        }

        // Найти/создать per-correlation SspiContext.
        std::shared_ptr<ContextEntry> entry = GetOrCreateContext(req.correlation_id);
        if (!entry) {
            SendStepError(pipe, "context_alloc_failed");
            return;
        }

        std::string outToken;
        sspi::SspiResult r;
        {
            std::lock_guard<std::mutex> lk(entry->mutex);
            entry->lastActivity = std::chrono::steady_clock::now();

            // Кэш credential handle: при первом шаге контекста подсеваем уже
            // полученный CredHandle, чтобы AcquireCredentialsHandle не вызывался
            // повторно (см. §риски "per-connection latency").
            SeedCredentialsIfNeeded(entry->ctx, req.spn);

            r = sspi::SspiNegotiate(entry->ctx, req.server_token, outToken, req.spn);

            // После первого успешного шага сохраняем полученный CredHandle в кэш
            // (если ещё не кэширован), чтобы переиспользовать в будущих контекстах.
            CaptureCredentialsToCache(entry->ctx, req.spn);
        }

        protocol::AuthStatus status = MapSspiResult(r);
        std::string detail;
        if (status == protocol::AuthStatus::Error) detail = "sspi_error";
        else if (status == protocol::AuthStatus::NoCredentials) detail = "no_credentials";

        LogDebug("sspi_step corr=" + req.correlation_id +
                 " spn='" + req.spn + "' -> " + protocol::ToWire(status) +
                 " out_token_len=" + std::to_string(outToken.size()));

        SendStepStatus(pipe, status, outToken, detail);

        // На терминальном исходе можно освободить контекст сразу (relay также
        // пришлёт release; двойное освобождение безопасно).
        if (status == protocol::AuthStatus::Complete ||
            status == protocol::AuthStatus::Error ||
            status == protocol::AuthStatus::NoCredentials) {
            EraseContext(req.correlation_id);
        }
    }

    // ----------------------------------------------------------------------
    // release
    // ----------------------------------------------------------------------
    void HandleRelease(const std::string& raw) {
        protocol::ReleaseMessage rel;
        protocol::ParseError pe = protocol::ParseReleaseMessage(raw, rel);
        if (pe != protocol::ParseError::Ok) {
            LogDebug(std::string("Parse release failed: ") + protocol::ToString(pe));
            return;
        }
        LogDebug("release corr=" + rel.correlation_id);
        EraseContext(rel.correlation_id);
    }

    // ----------------------------------------------------------------------
    // ping (health)
    // ----------------------------------------------------------------------
    void HandlePing(HANDLE pipe) {
        // Отвечаем минимальным status=complete без токена как liveness-ACK.
        SendStepStatus(pipe, protocol::AuthStatus::Complete, "", "pong");
    }

    // ======================================================================
    // Per-correlation context store + TTL GC
    // ======================================================================
    struct ContextEntry {
        std::mutex mutex;
        sspi::SspiContext ctx;
        std::chrono::steady_clock::time_point lastActivity =
            std::chrono::steady_clock::now();
    };

    std::shared_ptr<ContextEntry> GetOrCreateContext(const std::string& corr) {
        std::lock_guard<std::mutex> lk(m_ctxMutex);
        auto it = m_contexts.find(corr);
        if (it != m_contexts.end()) return it->second;
        auto entry = std::make_shared<ContextEntry>();
        m_contexts.emplace(corr, entry);
        return entry;
    }

    void EraseContext(const std::string& corr) {
        std::shared_ptr<ContextEntry> victim;
        {
            std::lock_guard<std::mutex> lk(m_ctxMutex);
            auto it = m_contexts.find(corr);
            if (it == m_contexts.end()) return;
            victim = it->second;
            m_contexts.erase(it);
        }
        // Освобождаем SspiContext вне m_ctxMutex, но НЕ трогаем кэшированный
        // CredHandle: он общий и живёт отдельно (см. FreeCachedCredentials).
        if (victim) {
            std::lock_guard<std::mutex> lk(victim->mutex);
            ReleaseContextKeepCachedCreds(victim->ctx);
        }
    }

    void GcContexts() {
        auto now = std::chrono::steady_clock::now();
        std::vector<std::shared_ptr<ContextEntry>> expired;
        {
            std::lock_guard<std::mutex> lk(m_ctxMutex);
            for (auto it = m_contexts.begin(); it != m_contexts.end();) {
                auto age = std::chrono::duration_cast<std::chrono::seconds>(
                    now - it->second->lastActivity).count();
                if (age >= static_cast<long long>(m_cfg.contextTtlSeconds)) {
                    expired.push_back(it->second);
                    it = m_contexts.erase(it);
                } else {
                    ++it;
                }
            }
        }
        for (auto& e : expired) {
            std::lock_guard<std::mutex> lk(e->mutex);
            ReleaseContextKeepCachedCreds(e->ctx);
        }
        if (!expired.empty()) {
            LogDebug("GC released " + std::to_string(expired.size()) +
                     " stale SSPI contexts");
        }
    }

    void ClearAllContexts() {
        std::vector<std::shared_ptr<ContextEntry>> all;
        {
            std::lock_guard<std::mutex> lk(m_ctxMutex);
            for (auto& kv : m_contexts) all.push_back(kv.second);
            m_contexts.clear();
        }
        for (auto& e : all) {
            std::lock_guard<std::mutex> lk(e->mutex);
            ReleaseContextKeepCachedCreds(e->ctx);
        }
    }

    // ======================================================================
    // Credential-handle caching (per user/SPN; пользователь фиксирован процессом)
    // ======================================================================
    //
    // Кэшируем СЫРОЙ CredHandle. SspiContext::Release() освобождает и context, и
    // credentials, поэтому для per-correlation контекстов мы:
    //   - при старте контекста КОПИРУЕМ кэшированный CredHandle в ctx.credentials
    //     и ставим ctx.initialized=true, чтобы SspiNegotiate НЕ вызывал
    //     AcquireCredentialsHandle заново;
    //   - при уничтожении контекста освобождаем ТОЛЬКО context (DeleteSecurityContext),
    //     а credentials обнуляем в копии (не освобождаем — владелец кэш).
    //
    void SeedCredentialsIfNeeded(sspi::SspiContext& ctx, const std::string& spn) {
        std::lock_guard<std::mutex> lk(m_credMutex);
        if (ctx.initialized) return;  // контекст уже прогрет.
        if (!m_credValid) return;     // кэша ещё нет — SspiNegotiate получит сам.
        ctx.credentials = m_cachedCred;
        ctx.initialized = true;
        ctx.targetSpn = spn;
    }

    void CaptureCredentialsToCache(sspi::SspiContext& ctx, const std::string& /*spn*/) {
        std::lock_guard<std::mutex> lk(m_credMutex);
        if (m_credValid) return;
        if (ctx.initialized &&
            (ctx.credentials.dwLower != 0 || ctx.credentials.dwUpper != 0)) {
            m_cachedCred = ctx.credentials;
            m_credValid = true;
            LogDebug("Cached Negotiate credential handle for reuse");
        }
    }

    /**
     * @brief Освободить контекст, НЕ трогая кэшированный общий CredHandle.
     *
     * Если ctx.credentials совпадает с кэшированным, обнуляем его в ctx до
     * вызова Release(), чтобы FreeCredentialsHandle не убил общий дескриптор.
     */
    void ReleaseContextKeepCachedCreds(sspi::SspiContext& ctx) {
        {
            std::lock_guard<std::mutex> lk(m_credMutex);
            if (m_credValid &&
                ctx.credentials.dwLower == m_cachedCred.dwLower &&
                ctx.credentials.dwUpper == m_cachedCred.dwUpper) {
                // Отвязываем общий дескриптор; сам контекст (context) освободим ниже.
                ctx.credentials = {0, 0};
            }
        }
        ctx.Release();  // освободит только SecurityContext (credentials уже 0).
    }

    void FreeCachedCredentials() {
        std::lock_guard<std::mutex> lk(m_credMutex);
        if (m_credValid) {
            FreeCredentialsHandle(&m_cachedCred);
            m_cachedCred = {0, 0};
            m_credValid = false;
        }
    }

    // ======================================================================
    // SPN allow-list (§3.3)
    // ======================================================================
    bool IsSpnAllowed(const std::string& spn) const {
        if (m_cfg.allowedSpns.empty()) return false;  // fail-closed.
        for (const auto& a : m_cfg.allowedSpns) {
            if (EqualsIgnoreCase(a, spn)) return true;
        }
        return false;
    }

    static bool EqualsIgnoreCase(const std::string& a, const std::string& b) {
        if (a.size() != b.size()) return false;
        for (size_t i = 0; i < a.size(); ++i) {
            char ca = a[i], cb = b[i];
            if (ca >= 'A' && ca <= 'Z') ca = static_cast<char>(ca - 'A' + 'a');
            if (cb >= 'A' && cb <= 'Z') cb = static_cast<char>(cb - 'A' + 'a');
            if (ca != cb) return false;
        }
        return true;
    }

    // ======================================================================
    // Отображение SspiResult -> AuthStatus (протокол §2.2)
    // ======================================================================
    static protocol::AuthStatus MapSspiResult(sspi::SspiResult r) {
        switch (r) {
            case sspi::SspiResult::NeedContinue: return protocol::AuthStatus::Continue;
            case sspi::SspiResult::Success:      return protocol::AuthStatus::Continue;
            case sspi::SspiResult::Complete:     return protocol::AuthStatus::Complete;
            case sspi::SspiResult::NoCredentials:return protocol::AuthStatus::NoCredentials;
            case sspi::SspiResult::Error:        return protocol::AuthStatus::Error;
        }
        return protocol::AuthStatus::Error;
    }

    // ======================================================================
    // Ответы
    // ======================================================================
    void SendStepStatus(HANDLE pipe, protocol::AuthStatus status,
                        const std::string& outToken, const std::string& detail) {
        protocol::SspiStepResponse resp;
        resp.status = status;
        resp.out_token = outToken;
        resp.detail = detail;
        std::string framed;
        if (!protocol::FrameMessage(protocol::Serialize(resp), framed)) {
            LogWarn("Response exceeds max message size; sending bare error");
            protocol::SspiStepResponse err;
            err.status = protocol::AuthStatus::Error;
            err.detail = "response_too_large";
            protocol::FrameMessage(protocol::Serialize(err), framed);
        }
        WriteMessage(pipe, framed);
    }

    void SendStepError(HANDLE pipe, const std::string& detail) {
        SendStepStatus(pipe, protocol::AuthStatus::Error, "", detail);
    }

    // ======================================================================
    // Проверка вызывающего = LocalSystem (§3.2)
    // ======================================================================
    static bool VerifyCallerIsLocalSystem(HANDLE pipe) {
        if (!ImpersonateNamedPipeClient(pipe)) {
            HelperLog::Instance().Warn(
                "ImpersonateNamedPipeClient failed err=" +
                std::to_string(GetLastError()));
            return false;
        }

        bool isSystem = false;
        HANDLE hToken = nullptr;
        if (OpenThreadToken(GetCurrentThread(), TOKEN_QUERY, TRUE, &hToken)) {
            isSystem = TokenUserIsLocalSystem(hToken);
            CloseHandle(hToken);
        } else {
            HelperLog::Instance().Warn(
                "OpenThreadToken failed err=" + std::to_string(GetLastError()));
        }

        RevertToSelf();
        return isSystem;
    }

    static bool TokenUserIsLocalSystem(HANDLE hToken) {
        DWORD len = 0;
        GetTokenInformation(hToken, TokenUser, nullptr, 0, &len);
        if (len == 0) return false;
        std::vector<BYTE> buf(len);
        if (!GetTokenInformation(hToken, TokenUser, buf.data(), len, &len)) {
            return false;
        }
        TOKEN_USER* tu = reinterpret_cast<TOKEN_USER*>(buf.data());

        PSID systemSid = nullptr;
        SID_IDENTIFIER_AUTHORITY ntAuth = SECURITY_NT_AUTHORITY;
        if (!AllocateAndInitializeSid(&ntAuth, 1, SECURITY_LOCAL_SYSTEM_RID,
                                      0, 0, 0, 0, 0, 0, 0, &systemSid)) {
            return false;
        }
        bool equal = EqualSid(tu->User.Sid, systemSid) != FALSE;
        FreeSid(systemSid);
        return equal;
    }

    // ======================================================================
    // SDDL: владелец-пользователь + LocalSystem (§3.1)
    // ======================================================================
    //
    // Строим SDDL строку вида:
    //   O:SYG:SYD:(A;;GA;;;SY)(A;;GA;;;<user-SID>)
    // где <user-SID> — строковый SID из собственного токена (TokenUser).
    // BUILTIN\Administrators (BA) НАМЕРЕННО не включаем (§3.1).
    //
    std::pair<SECURITY_ATTRIBUTES, SdPtr> MakeOwnerAndSystemOnlySA() {
        SECURITY_ATTRIBUTES sa = {};
        sa.nLength = sizeof(sa);
        sa.bInheritHandle = FALSE;

        std::string userSidStr;
        if (!GetOwnUserSidString(userSidStr)) {
            LogError("GetOwnUserSidString failed; cannot secure pipe");
            return {sa, nullptr};
        }

        std::string sddl =
            "O:SYG:SYD:(A;;GA;;;SY)(A;;GA;;;" + userSidStr + ")";
        std::wstring wSddl = Widen(sddl);

        PSECURITY_DESCRIPTOR raw = nullptr;
        if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(
                wSddl.c_str(), SDDL_REVISION_1, &raw, nullptr)) {
            LogError("ConvertStringSecurityDescriptor failed err=" +
                     std::to_string(GetLastError()) + " sddl=" + sddl);
            return {sa, nullptr};
        }
        SdPtr sd(raw);
        sa.lpSecurityDescriptor = sd.get();
        return {sa, std::move(sd)};
    }

    static bool GetOwnUserSidString(std::string& out) {
        HANDLE hToken = nullptr;
        if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &hToken)) {
            return false;
        }
        DWORD len = 0;
        GetTokenInformation(hToken, TokenUser, nullptr, 0, &len);
        if (len == 0) { CloseHandle(hToken); return false; }
        std::vector<BYTE> buf(len);
        bool ok = GetTokenInformation(hToken, TokenUser, buf.data(), len, &len) != FALSE;
        CloseHandle(hToken);
        if (!ok) return false;

        TOKEN_USER* tu = reinterpret_cast<TOKEN_USER*>(buf.data());
        LPWSTR sidStrW = nullptr;
        if (!ConvertSidToStringSidW(tu->User.Sid, &sidStrW)) {
            return false;
        }
        out = Narrow(sidStrW);
        LocalFree(sidStrW);
        return true;
    }

    // ======================================================================
    // Ввод/вывод message-mode pipe
    // ======================================================================
    bool WriteMessage(HANDLE pipe, const std::string& msg) {
        DWORD written = 0;
        BOOL ok = WriteFile(pipe, msg.data(),
                            static_cast<DWORD>(msg.size()), &written, nullptr);
        if (ok) FlushFileBuffers(pipe);
        return ok && written == msg.size();
    }

    bool ReadMessage(HANDLE pipe, std::string& out) {
        out.clear();
        std::vector<char> buf(protocol::kMaxMessageSize);
        DWORD read = 0;
        BOOL ok = ReadFile(pipe, buf.data(),
                           static_cast<DWORD>(buf.size()), &read, nullptr);
        if (!ok) {
            DWORD err = GetLastError();
            if (err == ERROR_MORE_DATA) {
                // Сообщение больше буфера — по протоколу это нарушение лимита.
                // Дочитываем и отбрасываем, вернём "oversized" наверх.
                out.assign(buf.data(), read);
                char sink[4096];
                DWORD extra = 0;
                while (ReadFile(pipe, sink, sizeof(sink), &extra, nullptr) ||
                       GetLastError() == ERROR_MORE_DATA) {
                    if (extra == 0) break;
                }
                out.resize(protocol::kMaxMessageSize + 1, '\0');  // сигнал oversized.
                return true;
            }
            return false;
        }
        if (read == 0) return false;
        out.assign(buf.data(), read);
        return true;
    }

    void ClosePipe(HANDLE pipe) {
        {
            std::lock_guard<std::mutex> lk(m_pipeMutex);
            if (m_hPipe == pipe) m_hPipe = INVALID_HANDLE_VALUE;
        }
        DisconnectNamedPipe(pipe);
        CloseHandle(pipe);
    }

    // ======================================================================
    // Утилиты кодировки
    // ======================================================================
    static std::wstring Widen(const std::string& s) {
        if (s.empty()) return std::wstring();
        int n = MultiByteToWideChar(CP_UTF8, 0, s.data(),
                                    static_cast<int>(s.size()), nullptr, 0);
        std::wstring w(n, L'\0');
        MultiByteToWideChar(CP_UTF8, 0, s.data(),
                            static_cast<int>(s.size()), w.data(), n);
        return w;
    }

    static std::string Narrow(const std::wstring& w) {
        if (w.empty()) return std::string();
        int n = WideCharToMultiByte(CP_UTF8, 0, w.data(),
                                    static_cast<int>(w.size()),
                                    nullptr, 0, nullptr, nullptr);
        std::string s(n, '\0');
        WideCharToMultiByte(CP_UTF8, 0, w.data(),
                            static_cast<int>(w.size()),
                            s.data(), n, nullptr, nullptr);
        return s;
    }

    // ======================================================================
    // Члены
    // ======================================================================
    AuthPipeServerConfig m_cfg;
    std::atomic<bool> m_running{false};

    HANDLE m_hPipe = INVALID_HANDLE_VALUE;
    std::mutex m_pipeMutex;

    std::map<std::string, std::shared_ptr<ContextEntry>> m_contexts;
    std::mutex m_ctxMutex;

    // Кэшированный CredHandle (общий для всех correlation'ов процесса).
    CredHandle m_cachedCred{0, 0};
    bool m_credValid = false;
    std::mutex m_credMutex;
};

}  // namespace auth_helper
}  // namespace tcp_redirector
