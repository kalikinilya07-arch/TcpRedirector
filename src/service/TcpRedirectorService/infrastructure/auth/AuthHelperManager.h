#pragma once

/**
 * @file AuthHelperManager.h
 * @brief Variant 4b, Phase 5 — менеджер per-user auth helper'ов.
 *
 * См. plans/kerberos_per_user_auth_helper_plan.md §1.2/§1.3, §3, §6 и
 * src/service/AuthHelper/README.md (CLI-контракт Phase 3).
 *
 * НАЗНАЧЕНИЕ
 * ----------
 * Компонент LocalSystem-службы, который:
 *   1. Перечисляет интерактивные пользовательские сессии (WTSEnumerateSessionsW),
 *      пропуская session 0 и сессии без залогиненного пользователя.
 *   2. Запускает РОВНО ОДИН TcpRedirectorAuthHelper.exe в каждой подходящей
 *      сессии через WTSQueryUserToken -> DuplicateTokenEx(primary) ->
 *      CreateEnvironmentBlock -> CreateProcessAsUserW.
 *   3. Отслеживает их (session id, user SID, PID, nonce, pipe name, SPN/host,
 *      время запуска) в потокобезопасной таблице.
 *   4. Держит всех детей под Job Object'ом с KILL_ON_JOB_CLOSE, так что helper'ы
 *      умирают вместе со службой (нет сирот).
 *   5. Убирает helper'ов на logoff/крэше; на RDP-disconnect helper остаётся жив
 *      (утверждённое решение — быстрый реконнект).
 *   6. Отдаёт lookup (TryGet) и построение BrokeredAuthParams, чтобы Phase 6 мог
 *      найти "helper для сессии N" и собрать BrokeredAuthProvider.
 *
 * ГРАНИЦЫ PHASE 5
 * ---------------
 * НЕ трогает relay / выбор провайдера (Phase 6), НЕ форсит fallback (Phase 7),
 * НЕ пакетирует installer (Phase 8). Класс полностью юзабелен и самодостаточен;
 * проводка SERVICE_CONTROL_SESSIONCHANGE и per-connection резолв — задача Phase 6
 * (см. doc-комментарии к OnSessionChange / BuildBrokeredParams).
 *
 * ПОТОКОБЕЗОПАСНОСТЬ
 * ------------------
 * Публичные методы Start/Stop/OnSessionChange/TryGet/BuildBrokeredParams
 * потокобезопасны (внутренний mutex). Монитор-тред вызывает те же приватные
 * launch/kill-примитивы под тем же mutex'ом.
 */

#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <windows.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <deque>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "../../domain/entities/ProxyConfig.h"          // domain::ProxyConfig
#include "../../domain/ports/IConnectionMonitor.h"       // ILogSink, LogLevel
#include "BrokeredAuthProvider.h"                        // BrokeredAuthParams

namespace tcp_redirector {
namespace infrastructure {
namespace auth {

// ============================================================================
// Чистые (без Win32-launch) хелперы — вынесены в namespace detail для юнит-тестов
// ============================================================================
namespace detail {

/**
 * @brief Сгенерировать криптостойкий nonce (hex-строка) через BCryptGenRandom.
 *
 * @param byteLen  Количество случайных байт (по умолчанию 32 => 64 hex-символа).
 * @return hex-строка длиной byteLen*2; пустая строка при сбое RNG (крайне
 *         маловероятно — вызывающая сторона трактует пустой nonce как fatal и
 *         пропускает запуск helper'а).
 */
std::string GenerateNonceHex(std::size_t byteLen = 32);

/**
 * @brief Вывести SPN, с которым запускать/сверять helper.
 *
 * Правило (§1.2 источника истины): если auth_spn непусто — использовать его;
 * иначе derive "HTTP/<proxyHost>". Оба аргумента — UTF-8.
 *
 * @param authSpn    Явный SPN из конфига (может быть пуст).
 * @param proxyHost  Хост прокси (для деривации).
 * @return Итоговый SPN; пусто, если и SPN, и host пусты (fail-closed на стороне
 *         helper'а — пустой allow-list => все sspi_step отвечаются denied).
 */
std::string DeriveSpn(const std::string& authSpn, const std::string& proxyHost);

/**
 * @brief Построить argv (без argv[0]) для запуска helper'а.
 *
 * Формирует: --session <id> --nonce <nonce> (--spn <spn> | --proxy-host <host>)
 *            --version <ver>
 * Приоритет: непустой spn => "--spn"; иначе непустой host => "--proxy-host".
 * Если оба пусты — ни того ни другого (helper стартует с пустым allow-list =>
 * fail-closed). Значения — wide (для CreateProcessAsUserW).
 *
 * @param sessionId  Целевая WTS-сессия.
 * @param nonceHex   Nonce (hex, ASCII).
 * @param spn        SPN (UTF-8) — если непусто, добавляется --spn.
 * @param proxyHost  Хост (UTF-8) — используется, только если spn пуст.
 * @param version    Строка версии helper'а (ASCII).
 * @return Вектор wide-аргументов (без имени exe).
 */
std::vector<std::wstring> BuildHelperArgs(std::uint32_t sessionId,
                                          const std::string& nonceHex,
                                          const std::string& spn,
                                          const std::string& proxyHost,
                                          const std::string& version);

/**
 * @brief Собрать полную командную строку из exe + argv (CommandLineToArgvW-квотинг).
 *
 * Вынесено отдельно для тестируемости квотинга/сборки.
 */
std::wstring BuildCommandLine(const std::wstring& exe,
                              const std::vector<std::wstring>& args);

}  // namespace detail

// ============================================================================
// HelperRecord — одна запись отслеживаемого helper'а
// ============================================================================

/**
 * @brief Снимок состояния одного запущенного helper'а (для Phase 6 lookup).
 *
 * hProcess здесь НЕ владеющий — это дескриптор, которым владеет менеджер;
 * копия в out-параметре TryGet не должна закрываться вызывающей стороной.
 */
struct HelperRecord {
    std::uint32_t sessionId = 0;    //!< WTS session id.
    std::string   userSid;          //!< SID пользователя сессии (строка S-1-5-...).
    std::string   userName;         //!< DOMAIN\user (диагностика; может быть пуст).
    DWORD         pid = 0;          //!< PID запущенного helper.exe (=> expected_server_pid).
    std::string   nonce;            //!< Одноразовый nonce, выданный при запуске (=> expected_nonce).
    std::string   pipeName;         //!< Полное имя pipe (\\.\pipe\TcpRedirectorAuth_<id>).
    std::string   launchedSpn;      //!< SPN, с которым запущен (для запроса/сверки).
    std::string   proxyHost;        //!< Хост прокси, с которым запущен.
    std::chrono::steady_clock::time_point startTime;  //!< Момент последнего запуска.
    HANDLE        hProcess = nullptr;  //!< Не владеющая копия process-handle (для монитора).
};

// ============================================================================
// AuthHelperManager
// ============================================================================

/**
 * @brief Конфигурация запуска helper'ов (заполняется из domain::ProxyConfig).
 */
struct AuthHelperManagerConfig {
    std::wstring helperExePath;     //!< Полный путь к TcpRedirectorAuthHelper.exe.
    std::string  spn;               //!< Итоговый SPN (уже derived: auth_spn|HTTP/<host>).
    std::string  proxyHost;         //!< Хост прокси (UTF-8) — на случай --proxy-host.
    int          helperTimeoutMs = 5000;  //!< Проброс в BrokeredAuthParams.
    std::string  helperVersion = "1";     //!< Версия helper (--version, попадает в hello).

    // --- Crash-recovery / anti-storm -----------------------------------------
    int restartBackoffMs = 2000;        //!< Пауза перед relaunch после крэша (мс).
    int maxRestartsPerMinute = 5;       //!< Скользящий лимит relaunch на сессию/минуту.
    int monitorPollMs = 1000;           //!< Период опроса монитора (мс).
};

/**
 * @brief Менеджер per-user auth helper'ов (см. заголовок файла).
 */
class AuthHelperManager {
public:
    /**
     * @brief Конструктор. Ничего не запускает — только сохраняет параметры.
     * @param cfg  Конфигурация запуска (путь к exe, SPN/host, timeout, backoff).
     * @param log  Опциональный ILogSink (nullptr => молча).
     */
    AuthHelperManager(AuthHelperManagerConfig cfg,
                      domain::ports::ILogSink* log = nullptr);

    /**
     * @brief Деструктор. Синхронно вызывает Stop().
     */
    ~AuthHelperManager();

    AuthHelperManager(const AuthHelperManager&) = delete;
    AuthHelperManager& operator=(const AuthHelperManager&) = delete;
    AuthHelperManager(AuthHelperManager&&) = delete;
    AuthHelperManager& operator=(AuthHelperManager&&) = delete;

    /**
     * @brief Удобная фабрика конфигурации из domain::ProxyConfig + пути к exe.
     *
     * Выполняет derive SPN (auth_spn иначе HTTP/<host>) и конверсию wide->utf8.
     * helperExePath обычно берётся из AppPaths (рядом с EXE службы). Phase 6:
     * см. doc в attempt_completion — как сконструировать менеджер в ServiceMain.
     */
    static AuthHelperManagerConfig MakeConfig(const domain::ProxyConfig& pc,
                                              const std::wstring& helperExePath,
                                              const std::string& helperVersion = "1");

    /**
     * @brief Первичная энумерация + запуск helper'ов + старт монитор-треда.
     *
     * Идемпотентен. Сбой запуска отдельной сессии НЕ фатален (лог + пропуск).
     * @return true, если менеджер поднялся (job создан, монитор запущен).
     */
    bool Start();

    /**
     * @brief Остановить всё: убить helper'ов (закрытие job'а), джойн монитора.
     * Идемпотентен.
     */
    void Stop();

    /**
     * @brief Обработчик session-change (Phase 6 вызовет из HandlerEx).
     *
     * @param event      WTS_SESSION_LOGON / WTS_CONSOLE_CONNECT / WTS_REMOTE_CONNECT
     *                   / WTS_SESSION_LOGOFF / WTS_CONSOLE_DISCONNECT /
     *                   WTS_REMOTE_DISCONNECT / WTS_SESSION_LOCK / WTS_SESSION_UNLOCK.
     * @param sessionId  Сессия из dwEventData.
     *
     * Политика (§6, утверждённые решения):
     *   - LOGON / CONSOLE_CONNECT / REMOTE_CONNECT / UNLOCK => EnsureHelper (запуск,
     *     если ещё нет и сессия подходящая).
     *   - LOGOFF => KillHelper (убрать helper этой сессии).
     *   - CONSOLE_DISCONNECT / REMOTE_DISCONNECT / (session)DISCONNECT / LOCK =>
     *     НИЧЕГО (keep-alive — сессия ещё залогинена).
     */
    void OnSessionChange(DWORD event, DWORD sessionId);

    /**
     * @brief Найти запись helper'а для сессии.
     * @return true + копия записи, если helper для сессии есть.
     */
    bool TryGet(std::uint32_t sessionId, HelperRecord& out) const;

    /**
     * @brief Построить BrokeredAuthParams для сессии (для Phase 6).
     *
     * Заполняет session_id / expected_nonce / expected_server_pid / spn /
     * proxy_host / helper_timeout_ms из записи. correlation_id_override оставляет
     * пустым (провайдер сгенерирует сам).
     *
     * @return true, если helper для сессии есть (out заполнен); иначе false.
     */
    bool BuildBrokeredParams(std::uint32_t sessionId,
                             infrastructure::BrokeredAuthParams& out) const;

    /**
     * @brief Число живых отслеживаемых helper'ов (диагностика/тесты).
     */
    std::size_t HelperCount() const;

private:
    // --- Внутреннее состояние одной записи (владеющее дескрипторами) ----------
    struct Entry {
        HelperRecord rec;                 //!< Публичный снимок.
        HANDLE       hProcess = nullptr;  //!< Владеющий process-handle.
        HANDLE       hThread  = nullptr;  //!< Владеющий thread-handle.
        HANDLE       hStdinWrite = nullptr;  //!< Наш конец stdin-pipe (закрытие = graceful).
        std::deque<int64_t> restartTicksMs;  //!< Скользящее окно relaunch (rate-limit).
        bool         givingUp = false;    //!< Превышен лимит relaunch — больше не пытаться.
    };

    // --- Приватные примитивы (все под m_mutex, если не указано иное) ----------
    bool EnsureJob();                     // ленивое создание kill-on-close job.
    std::vector<DWORD> EnumerateEligibleSessions() const;  // активные интерактивные, !=0, с юзером.
    bool EnsureHelperLocked(DWORD sessionId, bool isRelaunch);  // запуск, если нет/relaunch.
    bool LaunchHelperLocked(DWORD sessionId, Entry& outEntry);  // сырой launch (WTS+CreateProcessAsUser).
    void KillHelperLocked(DWORD sessionId);  // убить + удалить запись.
    void KillAllLocked();
    void MonitorLoop();                   // фоновой мониторинг крэшей + relaunch.

    static bool ResolveTokenUser(HANDLE token, std::string& sidOut, std::string& nameOut);
    static bool IsSessionEligible(DWORD sessionId);  // != 0.

    // --- Логирование (nullptr-safe) ------------------------------------------
    void LogInfo (const std::string& msg) const;
    void LogWarn (const std::string& msg) const;
    void LogError(const std::string& msg) const;
    void LogDebug(const std::string& msg) const;

    AuthHelperManagerConfig  m_cfg;
    domain::ports::ILogSink* m_log = nullptr;

    mutable std::mutex       m_mutex;         // защищает m_entries + job + started.
    std::map<DWORD, Entry>   m_entries;       // sessionId -> запись.
    HANDLE                   m_job = nullptr; // kill-on-close job для всех детей.

    std::thread              m_monitor;
    HANDLE                   m_stopEvent = nullptr;  // manual-reset; будит монитор.
    std::atomic<bool>        m_started{false};
};

}  // namespace auth
}  // namespace infrastructure
}  // namespace tcp_redirector
