#pragma once

/**
 * @file ChildProcessSupervisor.h
 * @brief WP12 — переиспользуемый супервизор дочернего процесса.
 *
 * НАЗНАЧЕНИЕ:
 *   Небольшая утилита, инкапсулирующая типовую задачу «запусти дочерний
 *   бинарник под нашим контролем, пиши его stdout/stderr в наш логгер,
 *   перезапускай при неожиданном падении».  Используется в первую очередь
 *   `Tun2SocksEngineExternal` (§6.12 плана), но проектируется как
 *   универсальный примитив без завязки на tun2socks.
 *
 * КЛЮЧЕВЫЕ ГАРАНТИИ:
 *   1. **Kill-on-close job object.**  Дочерний процесс всегда прикрепляется
 *      к Job Object'у с флагом `JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE`.  Даже
 *      при аварийном завершении родителя (сервиса) ОС форс-килит ребёнка —
 *      никаких «сирот» tun2socks.exe после падения сервиса.
 *   2. **CREATE_NO_WINDOW.**  Ни при каких обстоятельствах у ребёнка нет
 *      видимого консольного окна — это критично при работе под SCM,
 *      где иначе получим EASTER_EGG-конcоль на winsta0\Default.
 *   3. **Piped stdout/stderr → callback → logger.**  Каждая строка
 *      (LF-terminated, `\r` отрезается) отдаётся в пользовательский коллбэк
 *      синхронно; коллбэк ожидается лёгким.  Пайпы — анонимные, read-конец
 *      NON-inheritable, write-конец inheritable и передан через STARTUPINFO.
 *      stdin ребёнка — открытый `NUL`, чтобы ChatGPT-подобные бинарники,
 *      которые пытаются читать stdin, не вешались бы на закрытом канале.
 *   4. **Supervisor thread.**  Отдельный поток, который делает
 *      `WaitForSingleObject(hProcess, INFINITE)` и на неожиданный выход
 *      респавнит ребёнка (если `restart_on_crash=true`), уважая
 *      `restart_backoff_ms` и лимит `max_restarts_per_minute`
 *      (rate-limit — rolling window в 60 секунд).  При превышении лимита
 *      логируется ERROR «restart rate limit reached; giving up» и
 *      супервизор завершается — до следующего внешнего `Start()`.
 *   5. **Graceful stop.**  `Stop()` сначала пытается отправить
 *      `CTRL_BREAK_EVENT` (ребёнок стартует с `CREATE_NEW_PROCESS_GROUP`),
 *      ждёт до `graceful_stop_timeout_ms`, затем закрывает job — и ОС
 *      гарантированно убьёт ребёнка через KILL_ON_JOB_CLOSE.  Все треды
 *      (супервизор + два ридера) джойнятся синхронно.  Идемпотентно.
 */

#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <windows.h>

#include <atomic>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "../../domain/ports/IConnectionMonitor.h"   // ILogSink

namespace tcp_redirector {
namespace infrastructure {
namespace process {

/**
 * @brief Параметры запуска / поведения супервизора.
 */
struct ChildProcessConfig {
    //!< Абсолютный путь к исполняемому файлу.
    std::wstring executable;
    //!< CLI-аргументы (без argv[0]).  Каждый элемент экранируется отдельно
    //!< по CommandLineToArgvW-правилам при сборке строки.
    std::vector<std::wstring> args;
    //!< Рабочая директория ребёнка.  Пусто → наследуется от родителя.
    std::wstring working_directory;

    // --- Supervision --------------------------------------------------------

    //!< Автоперезапуск при неожиданном выходе (кроме kill'а через Stop()).
    bool restart_on_crash = true;
    //!< Пауза перед перезапуском (мс).  Клэмпится в [100; 60000].
    int  restart_backoff_ms = 2000;
    //!< Скольжащий лимит рестартов за последние 60 секунд.  При превышении
    //!< супервизор пишет ERROR и завершается без дальнейших попыток.
    int  max_restarts_per_minute = 6;

    // --- Timing -------------------------------------------------------------

    //!< Сколько ждать выхода ребёнка после Ctrl-Break, прежде чем убивать
    //!< через закрытие job'а.
    int  graceful_stop_timeout_ms = 3000;
};

/**
 * @brief Пользовательские коллбэки.
 *
 * Все коллбэки вызываются из внутренних тредов супервизора (не из потока,
 * который вызывал Start()).  Ожидается, что коллбэки лёгкие и НЕ вызывают
 * Stop() из самих себя (это привело бы к join-самому-себе).
 */
struct ChildProcessCallbacks {
    //!< Одна строка stdout ребёнка (без завершающих `\r\n`).
    std::function<void(const std::string& line)> on_stdout;
    //!< Одна строка stderr ребёнка (без завершающих `\r\n`).
    std::function<void(const std::string& line)> on_stderr;
    //!< Ребёнок вышел; передаётся exit-code от GetExitCodeProcess.
    //!< Вызывается ДО решения о рестарте.
    std::function<void(DWORD exit_code)>         on_exit;
};

/**
 * @brief Супервизор дочернего процесса под Windows Job Object'ом.
 *
 * Потокобезопасность: `Start()`/`Stop()` не должны вызываться конкурентно
 * (обычно вызывающая сторона — единственный владелец).  `IsRunning()`/
 * `ChildPid()` можно вызывать из любого потока.
 */
class ChildProcessSupervisor {
public:
    /**
     * @brief Конструктор.  Ничего не спавнит — только сохраняет параметры.
     * @param cfg  Конфигурация ребёнка.  Мувается внутрь.
     * @param cb   Коллбэки stdout/stderr/exit.  Мувается внутрь.
     * @param log  Опциональный ILogSink.  Может быть nullptr — тогда
     *             супервизор молчит (кроме callback'ов пользователя).
     */
    ChildProcessSupervisor(ChildProcessConfig cfg,
                           ChildProcessCallbacks cb,
                           domain::ports::ILogSink* log = nullptr);

    /**
     * @brief Деструктор.  Синхронно вызывает Stop() — блокируется, пока
     *        супервизор-тред и ридеры не остановятся.
     */
    ~ChildProcessSupervisor();

    ChildProcessSupervisor(const ChildProcessSupervisor&) = delete;
    ChildProcessSupervisor& operator=(const ChildProcessSupervisor&) = delete;
    ChildProcessSupervisor(ChildProcessSupervisor&&) = delete;
    ChildProcessSupervisor& operator=(ChildProcessSupervisor&&) = delete;

    /**
     * @brief Запустить супервизор и первую копию ребёнка.
     *
     * Идемпотентен — повторный Start после успешного Start возвращает true
     * без побочных эффектов.
     *
     * @param outError [out, optional] диагностическое сообщение при провале.
     * @return true при успехе первого спавна; false при ошибке
     *         (job/pipe/CreateProcess).  Треды при провале не оставляются.
     */
    bool Start(std::string* outError);

    /**
     * @brief Остановить ребёнка и супервизор.  Синхронно.
     *
     * Последовательность:
     *   1. Взводим stop-event.
     *   2. Пытаемся graceful — CTRL_BREAK_EVENT на группу процессов ребёнка.
     *   3. Ждём до `graceful_stop_timeout_ms`.
     *   4. Закрываем job → KILL_ON_JOB_CLOSE убивает ребёнка форс-но.
     *   5. Джойним supervisor-тред и ридеры.
     *
     * Идемпотентен: повторные вызовы — no-op.
     */
    void Stop();

    /**
     * @brief true, если сейчас ребёнок жив (мгновенный snapshot).
     */
    bool IsRunning() const { return m_child_alive.load(std::memory_order_acquire); }

    /**
     * @brief PID текущего ребёнка (0, если не запущен).
     */
    DWORD ChildPid() const { return m_child_pid.load(std::memory_order_acquire); }

private:
    // --- Supervisor implementation (см. .cpp) ---
    void SupervisorLoop();
    bool SpawnChild(std::string& err);       // один спавн (может быть вызван N раз)
    void JoinReaders();
    void KillJobAndCloseHandles();

    // reader-thread bodies
    static void ReaderLoop(HANDLE pipe_read,
                           std::function<void(const std::string&)> cb);

    // --- Логирование (nullptr-safe wrappers) ---
    void LogInfo (const std::string& msg) const;
    void LogWarn (const std::string& msg) const;
    void LogError(const std::string& msg) const;
    void LogDebug(const std::string& msg) const;

    // --- Config / callbacks / logger ---
    ChildProcessConfig       m_cfg;
    ChildProcessCallbacks    m_cb;
    domain::ports::ILogSink* m_log = nullptr;

    // --- Управляющее состояние ---
    // Жизненный цикл: nullptr → создан один раз в Start(), закрывается в Stop().
    HANDLE                m_stop_event = nullptr;   // manual-reset
    HANDLE                m_job        = nullptr;

    // Текущая генерация ребёнка (пересоздаётся на каждый спавн).
    HANDLE                m_child_process = nullptr;
    HANDLE                m_child_thread  = nullptr;
    HANDLE                m_stdout_read   = nullptr;
    HANDLE                m_stderr_read   = nullptr;
    std::thread           m_stdout_reader;
    std::thread           m_stderr_reader;

    // Супервизорный тред.
    std::thread           m_supervisor_thread;

    std::atomic<bool>     m_started{false};        // Start() отработал успешно
    std::atomic<bool>     m_stop_requested{false}; // Stop() вошёл
    std::atomic<bool>     m_child_alive{false};
    std::atomic<DWORD>    m_child_pid{0};

    // Rate-limit rolling window (используется только из supervisor-треда).
    std::deque<int64_t>   m_restart_ticks_ms;
};

} // namespace process
} // namespace infrastructure
} // namespace tcp_redirector
