#pragma once

/**
 * @file Tun2SocksEngineExternal.h
 * @brief WP12 — реализация ITunEngine поверх внешнего `tun2socks.exe`.
 *
 * ПРИНЦИП РАБОТЫ (см. §6.12 плана):
 *   Сервис (WintunCapture) уже:
 *     • создал Wintun-адаптер по имени `adapter_name`,
 *     • сконфигурировал IPv4-адрес адаптера,
 *     • поставил split-tunnel маршруты 0.0.0.0/1 + 128.0.0.0/1,
 *     • поднял на relay-стороне SOCKS5-listener (WP12a) на loopback:port.
 *
 *   Этот класс запускает `tun2socks.exe` как СУПЕРВИЗИРУЕМЫЙ дочерний
 *   процесс, аргументы которого говорят ему:
 *     -device wintun://<adapter_name>   ← прицепиться к НАШЕМУ адаптеру
 *     -proxy  socks5://<socks5_listen>  ← куда форвардить каждое соединение
 *   И, опционально, любые `extra_args` из конфига.  Ребёнок при этом
 *   открывает СВОЮ Wintun-сессию по имени адаптера — сессия на стороне
 *   сервиса не создаётся (см. §6 ownership table в плане).
 *
 * ЖИЗНЕННЫЙ ЦИКЛ:
 *   engine.Start() → supervisor запускает tun2socks.exe под Job Object'ом
 *                    с KILL_ON_JOB_CLOSE.  Логи ребёнка пайпаются в наш
 *                    логгер под тегом "tun2socks".
 *   engine.Stop()  → supervisor killит ребёнка (Ctrl-Break, затем job close).
 *
 * СЧЁТЧИКИ:
 *   RxBytes/TxBytes/ActiveFlows остаются 0 в v1 — best-effort парсинг
 *   stdout по стабильному формату (если он появится) описан в TODO ниже,
 *   но по умолчанию значения не заполняются, так как формат вывода
 *   xjasonlyu/tun2socks не документирован как стабильный.
 */

#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <windows.h>

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>

#include "ITunEngine.h"
#include "../../config/Config.h"                 // ExternalEngineSettings
#include "../../../domain/ports/IConnectionMonitor.h"  // ILogSink

namespace tcp_redirector {
namespace infrastructure {
namespace process { class ChildProcessSupervisor; }
}
}

namespace tcp_redirector {
namespace infrastructure {
namespace capture {
namespace wintun {

/**
 * @brief External-вариант tun2socks-движка.
 *
 * Композит: `ChildProcessSupervisor` + метаданные Wintun-адаптера, к
 * которому дочерний процесс прицепится (сервис создал этот адаптер сам).
 *
 * Потокобезопасность: как и остальные ITunEngine'ы — Start/Stop не
 * должны вызываться конкурентно; счётчики читаемы из любого потока.
 */
class Tun2SocksEngineExternal : public ITunEngine {
public:
    /**
     * @brief Конструктор.
     *
     * @param adapter_name  Имя Wintun-адаптера (то, что сервис уже создал).
     *                      Передаётся ребёнку в форме `-device wintun://<name>`.
     * @param ext           Секция `wintun.external_engine` из конфига
     *                      (executable, extra_args, socks5_listen,
     *                      restart_on_crash, restart_backoff_ms).
     * @param log           Опциональный ILogSink.  nullptr → лишь коллбэки.
     */
    Tun2SocksEngineExternal(std::wstring adapter_name,
                            infrastructure::ExternalEngineSettings ext,
                            domain::ports::ILogSink* log = nullptr);

    ~Tun2SocksEngineExternal() override;

    Tun2SocksEngineExternal(const Tun2SocksEngineExternal&) = delete;
    Tun2SocksEngineExternal& operator=(const Tun2SocksEngineExternal&) = delete;
    Tun2SocksEngineExternal(Tun2SocksEngineExternal&&) = delete;
    Tun2SocksEngineExternal& operator=(Tun2SocksEngineExternal&&) = delete;

    // --- ITunEngine ---
    bool Start(std::string* outError) override;
    void Stop() override;
    uint64_t RxBytes()   const override { return m_rxBytes.load(std::memory_order_relaxed); }
    uint64_t TxBytes()   const override { return m_txBytes.load(std::memory_order_relaxed); }
    uint32_t ActiveFlows() const override { return m_activeFlows.load(std::memory_order_relaxed); }

private:
    // Логирование stdout/stderr дочернего процесса — best-effort клэмп
    // уровней (INFO/DEBUG/WARN), плюс скан известных паттернов для
    // счётчиков.  Вызывается из reader-тредов супервизора.
    void OnChildStdout(const std::string& line);
    void OnChildStderr(const std::string& line);
    void OnChildExit(DWORD exit_code);

    // Логгер-nullptr-safe wrappers (тег "tun2socks").
    void LogInfo (const std::string& msg) const;
    void LogWarn (const std::string& msg) const;
    void LogError(const std::string& msg) const;
    void LogDebug(const std::string& msg) const;

    // --- members ---
    std::wstring                                m_adapterName;
    infrastructure::ExternalEngineSettings      m_ext;
    domain::ports::ILogSink*                    m_log = nullptr;

    std::unique_ptr<infrastructure::process::ChildProcessSupervisor> m_supervisor;

    std::atomic<uint64_t> m_rxBytes{0};
    std::atomic<uint64_t> m_txBytes{0};
    std::atomic<uint32_t> m_activeFlows{0};
    std::atomic<bool>     m_running{false};
};

} // namespace wintun
} // namespace capture
} // namespace infrastructure
} // namespace tcp_redirector
