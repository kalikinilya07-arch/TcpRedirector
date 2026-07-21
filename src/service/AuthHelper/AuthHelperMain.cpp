/**
 * @file AuthHelperMain.cpp
 * @brief Точка входа TcpRedirectorAuthHelper.exe (Variant 4b, Phase 3).
 *
 * См. plans/kerberos_per_user_auth_helper_plan.md §1.4 (helper process),
 * §2 (IPC), §3 (security), §6 (session-0 isolation).
 *
 * НАЗНАЧЕНИЕ:
 *   Крошечный headless-процесс, запускаемый службой (Phase 5) внутри
 *   логон-сессии пользователя через CreateProcessAsUser. Держит named-pipe
 *   СЕРВЕР \\.\pipe\TcpRedirectorAuth_<sessionId> и выполняет SSPI/Negotiate В
 *   СОБСТВЕННОМ (пользовательском) контексте, отдавая токены службе-клиенту
 *   (LocalSystem). Так Kerberos использует TGT ПОЛЬЗОВАТЕЛЯ, а не машинного
 *   аккаунта.
 *
 * КОНТРАКТ КОМАНДНОЙ СТРОКИ (согласован с планом §1.2 launch и §2.1 nonce):
 *   TcpRedirectorAuthHelper.exe
 *       [--session <N>]            WTS session id. Если опущено — определяем сами
 *                                  через ProcessIdToSessionId(GetCurrentProcessId()).
 *                                  Если задан — ДОЛЖЕН совпасть с собственным
 *                                  (иначе выходим: защита от запуска не в той сессии).
 *       [--pipe <name>]            Полное имя pipe. Если опущено — строим из
 *                                  MakeAuthPipeNameW(sessionId).
 *       --nonce <hex/token>        Одноразовый секрет, который служба записала при
 *                                  запуске; helper вернёт его в hello (§2.1/§3.2).
 *       --spn <SPN>                Разрешённый SPN (можно повторять несколько раз).
 *       --proxy-host <host>        Альтернатива: derive SPN "HTTP/<host>".
 *       [--ttl <sec>]              TTL брошенных SSPI-контекстов (дефолт 30).
 *       [--idle-exit <sec>]        Само-выход при простое (0 = не выходить).
 *       [--console]                Дублировать лог в stderr (интерактивная отладка).
 *       [--version <str>]          Строка версии helper для hello (диагностика).
 *
 * ИСТОЧНИК SPN allow-list:
 *   Аргументы --spn (0..N) + опционально --proxy-host => "HTTP/<host>". В Phase 5
 *   менеджер соберёт это из конфигурации службы (auth.spn / proxy host, план §4.5)
 *   и передаст helper'у на запуск. Пустой список => helper отклоняет ВСЕ sspi_step
 *   (fail-closed) — helper НИКОГДА не работает оракулом для произвольных SPN.
 *
 * ЖИЗНЕННЫЙ ЦИКЛ:
 *   - Отказываемся работать в session 0 (services session; §6): per-user Kerberos
 *     там бессмысленен и небезопасен.
 *   - Служит, пока: не закрыт pipe/не пришёл сигнал остановки, родитель жив.
 *   - Родитель (служба) присоединяет нас к kill-on-close Job Object (Phase 5),
 *     поэтому при падении службы ОС убьёт helper. Дополнительно: если родитель
 *     задан и умер (WaitForSingleObject на его хэндле), выходим сами.
 *   - Ctrl-родитель может закрыть наш stdin: поток-watchdog это замечает и
 *     инициирует остановку (мягкое завершение).
 *
 * СБОРКА: линкует общий auth_sspi.cpp (без копий), secur32/advapi32.
 */

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <atomic>
#include <string>
#include <thread>
#include <vector>

#include "AuthPipeServer.h"
#include "HelperLog.h"
#include "../../shared/auth_broker/AuthBrokerProtocol.h"
#include "../TcpRedirectorService/infrastructure/auth/auth_sspi.h"

namespace {

using namespace tcp_redirector::auth_helper;
namespace protocol = tcp_redirector::shared::auth_broker;

// Глобальный указатель на сервер для сигнала остановки из watchdog-потоков.
std::atomic<AuthPipeServer*> g_server{nullptr};

std::string WideToUtf8(const std::wstring& w) {
    if (w.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(),
                                nullptr, 0, nullptr, nullptr);
    std::string s(n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(),
                        s.data(), n, nullptr, nullptr);
    return s;
}

struct CmdArgs {
    bool hasSession = false;
    std::uint32_t session = 0;
    std::string pipe;
    std::string nonce;
    std::vector<std::string> spns;
    std::string proxyHost;
    std::uint32_t ttl = 30;
    std::uint32_t idleExit = 0;
    bool console = false;
    std::string version = "phase3";
};

bool ParseArgs(int argc, wchar_t** argv, CmdArgs& out) {
    for (int i = 1; i < argc; ++i) {
        std::wstring a = argv[i];
        auto next = [&](std::string& dst) -> bool {
            if (i + 1 >= argc) return false;
            dst = WideToUtf8(argv[++i]);
            return true;
        };
        if (a == L"--session") {
            std::string v;
            if (!next(v)) return false;
            out.session = static_cast<std::uint32_t>(strtoul(v.c_str(), nullptr, 10));
            out.hasSession = true;
        } else if (a == L"--pipe") {
            if (!next(out.pipe)) return false;
        } else if (a == L"--nonce") {
            if (!next(out.nonce)) return false;
        } else if (a == L"--spn") {
            std::string v;
            if (!next(v)) return false;
            if (!v.empty()) out.spns.push_back(v);
        } else if (a == L"--proxy-host") {
            if (!next(out.proxyHost)) return false;
        } else if (a == L"--ttl") {
            std::string v;
            if (!next(v)) return false;
            out.ttl = static_cast<std::uint32_t>(strtoul(v.c_str(), nullptr, 10));
        } else if (a == L"--idle-exit") {
            std::string v;
            if (!next(v)) return false;
            out.idleExit = static_cast<std::uint32_t>(strtoul(v.c_str(), nullptr, 10));
        } else if (a == L"--console") {
            out.console = true;
        } else if (a == L"--version") {
            if (!next(out.version)) return false;
        } else {
            // Неизвестный аргумент — не фатально, но логируем позже.
        }
    }
    return true;
}

// Watchdog: закрытие stdin (служба закрыла свой конец) => мягкая остановка.
void StdinWatchdog(std::atomic<bool>* stop) {
    HANDLE hIn = GetStdHandle(STD_INPUT_HANDLE);
    if (hIn == nullptr || hIn == INVALID_HANDLE_VALUE) return;
    char buf[64];
    DWORD read = 0;
    while (!stop->load(std::memory_order_relaxed)) {
        BOOL ok = ReadFile(hIn, buf, sizeof(buf), &read, nullptr);
        if (!ok || read == 0) {
            // EOF/ошибка — родитель закрыл stdin.
            LogInfo("stdin closed by parent; initiating shutdown");
            AuthPipeServer* srv = g_server.load();
            if (srv) srv->Stop();
            break;
        }
    }
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
    CmdArgs args;
    if (!ParseArgs(argc, argv, args)) {
        // Логгер ещё не инициализирован (не знаем session) — пишем по дефолту.
        HelperLog::Instance().Init(0, true);
        LogError("Invalid command line arguments");
        return 2;
    }

    // Определяем собственную сессию.
    DWORD ownSession = 0;
    if (!ProcessIdToSessionId(GetCurrentProcessId(), &ownSession)) {
        HelperLog::Instance().Init(0, args.console);
        LogError("ProcessIdToSessionId failed err=" +
                 std::to_string(GetLastError()));
        return 3;
    }

    std::uint32_t sessionId = args.hasSession ? args.session
                                              : static_cast<std::uint32_t>(ownSession);

    // Инициализируем логгер (user-writable, per-session файл).
    HelperLog::Instance().Init(sessionId, args.console);
    LogInfo("TcpRedirectorAuthHelper starting (version=" + args.version +
            ", own_session=" + std::to_string(ownSession) + ")");
    LogInfo("Log file: " + WideToUtf8(HelperLog::Instance().Path()));

    // §6: отказываемся работать в session 0.
    if (ownSession == 0) {
        LogError("Refusing to run in session 0 (services session); "
                 "per-user Kerberos is only meaningful in an interactive session");
        return 4;
    }

    // Если --session задан явно и не совпал с нашей сессией — выходим (нас
    // запустили не в той сессии; per-user контекст был бы неверным).
    if (args.hasSession && args.session != static_cast<std::uint32_t>(ownSession)) {
        LogError("Session mismatch: requested=" + std::to_string(args.session) +
                 " own=" + std::to_string(ownSession) + "; exiting");
        return 5;
    }

    // Имя pipe: из аргумента или строим по сессии.
    std::string pipeName = args.pipe.empty()
        ? protocol::MakeAuthPipeName(sessionId)
        : args.pipe;

    // Собираем SPN allow-list.
    std::vector<std::string> allowedSpns = args.spns;
    if (!args.proxyHost.empty()) {
        allowedSpns.push_back(tcp_redirector::infrastructure::MakeSpn(args.proxyHost));
    }
    if (allowedSpns.empty()) {
        LogWarn("No SPN allow-list provided (--spn/--proxy-host); helper will DENY "
                "all sspi_step requests (fail-closed)");
    } else {
        for (const auto& s : allowedSpns) {
            LogInfo("Allowed SPN: " + s);
        }
    }
    if (args.nonce.empty()) {
        LogWarn("No --nonce provided; hello will carry an empty nonce "
                "(service-side nonce validation in Phase 4 will fail)");
    }

    // Конфигурируем и запускаем сервер.
    AuthPipeServerConfig cfg;
    cfg.sessionId = sessionId;
    cfg.pipeName = pipeName;
    cfg.nonce = args.nonce;
    cfg.helperVersion = args.version;
    cfg.allowedSpns = std::move(allowedSpns);
    cfg.contextTtlSeconds = args.ttl;
    cfg.idleExitSeconds = args.idleExit;

    AuthPipeServer server(cfg);
    g_server.store(&server);

    // Watchdog stdin (мягкая остановка при закрытии родителем).
    std::atomic<bool> stopWatch{false};
    std::thread watchdog(StdinWatchdog, &stopWatch);

    // Основной цикл (блокирующий).
    server.Run();

    // Останов watchdog.
    stopWatch = true;
    HANDLE hIn = GetStdHandle(STD_INPUT_HANDLE);
    if (hIn && hIn != INVALID_HANDLE_VALUE) CancelIoEx(hIn, nullptr);
    if (watchdog.joinable()) watchdog.join();

    g_server.store(nullptr);
    LogInfo("TcpRedirectorAuthHelper exiting");
    return 0;
}
