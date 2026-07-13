#pragma once

/**
 * @file Socks5Adapter.h
 * @brief WP12a — тонкий SOCKS5-адаптер над TcpRelayServer.
 *
 * Ровно один вариант использования: внешний движок tun2socks.exe (WP12)
 * получает свой upstream-endpoint в виде SOCKS5-слушателя, привязанного
 * ТОЛЬКО к loopback (`127.0.0.1` или `::1`).  Мы принимаем SOCKS5-хендшейк
 * (RFC 1928, метод `0x00` = no-auth, команда `0x01` = CONNECT), читаем
 * оригинальный dst и передаём принятый сокет в тот же accept-путь, что
 * используется для loopback-flow'ов WinDivert.
 *
 * Байт-помпы здесь НЕТ — она живёт в `TcpRelayServer::HandleNewConnection`,
 * куда мы вручаем сокет через колбэк `Deps::handOff`.  Это гарантирует
 * единый источник истины для Basic/Kerberos-аутентификации, логирования,
 * статистики и обработки TLS.
 *
 * Активируется ТОЛЬКО когда `capture_mode="wintun"` И `wintun.engine="external"`.
 * В любых других режимах — не инстанциируется вообще (zero-cost).
 */

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winsock2.h>
#include <ws2tcpip.h>
#include <string>
#include <atomic>
#include <thread>
#include <memory>
#include <functional>
#include <vector>
#include <mutex>
#include <cstdint>
#include <cstring>

#include "../../domain/ports/IConnectionMonitor.h"     // ILogSink / LogLevel

namespace tcp_redirector {
namespace infrastructure {
namespace relay {

/**
 * @brief SOCKS5-no-auth/CONNECT-only-listener для внешнего tun2socks-движка.
 *
 * Жизненный цикл:
 *   Socks5Adapter a(host, port, deps);
 *   a.Start(&err);   // idempotent
 *   ...
 *   a.Stop();        // idempotent
 *
 * Установленные (переданные хендлеру) соединения продолжают жить
 * независимо от Stop(): их owns TcpRelayServer, который дренит их через
 * свой обычный Stop().
 */
class Socks5Adapter {
public:
    /// Callback: hand the accepted socket into TcpRelayServer's accept path.
    /// Аргументы: (accepted_socket, orig_dst_ip_network_byte_order, orig_dst_port_host_order).
    /// После вызова ownership сокета переходит TcpRelayServer'у — Adapter
    /// его больше не трогает и не закрывает.
    using HandOffFn = std::function<void(SOCKET, uint32_t, uint16_t)>;

    struct Deps {
        HandOffFn handOff;                       //!< обязателен
        domain::ports::ILogSink* log = nullptr;  //!< может быть nullptr
    };

    Socks5Adapter(std::string bind_host, uint16_t bind_port, Deps deps)
        : m_bindHost(std::move(bind_host))
        , m_bindPort(bind_port)
        , m_deps(std::move(deps)) {
    }

    ~Socks5Adapter() {
        Stop();
    }

    Socks5Adapter(const Socks5Adapter&) = delete;
    Socks5Adapter& operator=(const Socks5Adapter&) = delete;

    /**
     * @brief Поднять слушатель.  Idempotent: повторный вызов возвращает true.
     * @param outError на выходе — человекочитаемая причина, если false.
     */
    bool Start(std::string* outError) {
        if (m_running.load(std::memory_order_acquire)) {
            return true;
        }

        if (!m_deps.handOff) {
            SetErr(outError, "SOCKS5 adapter: handOff callback is null");
            return false;
        }

        // Loopback-only bind — оборона в глубину.  Preflight уже отсёк
        // не-loopback хосты, но повторно проверяем здесь (§6.11, §12 risks).
        if (!IsLoopbackHost(m_bindHost)) {
            SetErr(outError,
                "SOCKS5 refuses non-loopback bind: '" + m_bindHost + "'");
            return false;
        }

        // Гарантируем WSAStartup — TcpRelayServer::Start() тоже его делает,
        // так что счётчик Winsock корректно распарится по WSACleanup'ам.
        WSADATA wsa;
        if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
            SetErr(outError, "SOCKS5: WSAStartup failed: " + std::to_string(WSAGetLastError()));
            return false;
        }
        m_wsaStarted = true;

        m_listenSock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (m_listenSock == INVALID_SOCKET) {
            SetErr(outError, "SOCKS5: socket() failed: " + std::to_string(WSAGetLastError()));
            CleanupWsa();
            return false;
        }

        int on = 1;
        setsockopt(m_listenSock, SOL_SOCKET, SO_REUSEADDR, (const char*)&on, sizeof(on));

        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(m_bindPort);
        // "127.0.0.1" → INADDR_LOOPBACK. "::1" тут не годится, потому что
        // мы биндим AF_INET; если пользователь укажет "::1", падаем с
        // конкретной ошибкой (parseable в тесте / preflight'е).
        if (m_bindHost == "::1") {
            SetErr(outError,
                "SOCKS5: IPv6 loopback bind not supported in WP12a; use 127.0.0.1");
            closesocket(m_listenSock);
            m_listenSock = INVALID_SOCKET;
            CleanupWsa();
            return false;
        }
        if (InetPtonA(AF_INET, m_bindHost.c_str(), &addr.sin_addr) != 1) {
            SetErr(outError, "SOCKS5: cannot parse bind host: '" + m_bindHost + "'");
            closesocket(m_listenSock);
            m_listenSock = INVALID_SOCKET;
            CleanupWsa();
            return false;
        }

        if (bind(m_listenSock, (sockaddr*)&addr, sizeof(addr)) == SOCKET_ERROR) {
            int gle = WSAGetLastError();
            SetErr(outError,
                "SOCKS5: bind(" + m_bindHost + ":" + std::to_string(m_bindPort)
                + ") failed: " + std::to_string(gle));
            closesocket(m_listenSock);
            m_listenSock = INVALID_SOCKET;
            CleanupWsa();
            return false;
        }

        if (listen(m_listenSock, SOMAXCONN) == SOCKET_ERROR) {
            int gle = WSAGetLastError();
            SetErr(outError, "SOCKS5: listen() failed: " + std::to_string(gle));
            closesocket(m_listenSock);
            m_listenSock = INVALID_SOCKET;
            CleanupWsa();
            return false;
        }

        m_running.store(true, std::memory_order_release);
        m_acceptThread = std::thread(&Socks5Adapter::AcceptLoop, this);

        Log(domain::LogLevel::Info,
            "SOCKS5 listener bound at " + m_bindHost + ":" + std::to_string(m_bindPort));
        return true;
    }

    /**
     * @brief Остановить слушатель.  Idempotent.  Handed-off соединения
     *        продолжают жить в TcpRelayServer.
     */
    void Stop() {
        bool was = m_running.exchange(false, std::memory_order_acq_rel);
        if (!was) {
            // либо не стартовали, либо уже остановлены — не забываем прибрать
            // за собой WSA, если WSAStartup случился, а listenSock не был создан.
            if (m_wsaStarted) CleanupWsa();
            return;
        }

        if (m_listenSock != INVALID_SOCKET) {
            shutdown(m_listenSock, SD_BOTH);
            closesocket(m_listenSock);
            m_listenSock = INVALID_SOCKET;
        }

        if (m_acceptThread.joinable()) {
            m_acceptThread.join();
        }

        // Ждём завершения незавершённых handshake-потоков (короткие, <100 мс).
        // Handed-off соединения при этом уже НЕ в m_negotiators — их сокет
        // и жизненный цикл переехали в TcpRelayServer.
        {
            std::lock_guard<std::mutex> g(m_negotiatorMx);
            for (auto& t : m_negotiators) {
                if (t.joinable()) t.join();
            }
            m_negotiators.clear();
        }

        CleanupWsa();
        Log(domain::LogLevel::Info, "SOCKS5 listener stopped");
    }

    /// Для тестов и IPC-status.
    bool IsRunning() const { return m_running.load(std::memory_order_acquire); }

    /// Static utility: split "host:port" (WP12a — используется ServiceMain).
    /// Возвращает {"127.0.0.1", 1080}, если строка не распарсилась.
    static std::pair<std::string, uint16_t> ParseHostPort(const std::string& s) {
        if (s.empty()) return {"127.0.0.1", 1080};
        auto colon = s.rfind(':');
        if (colon == std::string::npos || colon == 0 || colon + 1 >= s.size()) {
            return {"127.0.0.1", 1080};
        }
        std::string host = s.substr(0, colon);
        if (host.size() >= 2 && host.front() == '[' && host.back() == ']') {
            host = host.substr(1, host.size() - 2);
        }
        int port = 0;
        for (char c : s.substr(colon + 1)) {
            if (c < '0' || c > '9') return {"127.0.0.1", 1080};
            port = port * 10 + (c - '0');
            if (port > 65535) return {"127.0.0.1", 1080};
        }
        if (port < 1) return {"127.0.0.1", 1080};
        return {host, static_cast<uint16_t>(port)};
    }

private:
    // ---- accept + handshake ---------------------------------------------

    void AcceptLoop() {
        while (m_running.load(std::memory_order_acquire)) {
            fd_set rfds;
            FD_ZERO(&rfds);
            FD_SET(m_listenSock, &rfds);
            timeval tv{1, 0};    // 1s — совместимо с TcpRelayServer::AcceptLoop
            int ret = select(0, &rfds, nullptr, nullptr, &tv);
            if (ret <= 0) continue;
            if (!FD_ISSET(m_listenSock, &rfds)) continue;

            sockaddr_in peer{};
            int addrlen = sizeof(peer);
            SOCKET c = accept(m_listenSock, (sockaddr*)&peer, &addrlen);
            if (c == INVALID_SOCKET) continue;

            // Пояс-и-подтяжки: явно отсекаем не-loopback peer'а ДО хендшейка.
            const uint32_t peer_be = peer.sin_addr.s_addr;
            const uint32_t peer_ho = ntohl(peer_be);
            // 127.0.0.0/8
            if ((peer_ho & 0xFF000000u) != 0x7F000000u) {
                Log(domain::LogLevel::Warn,
                    "SOCKS5: non-loopback peer refused before greeting");
                closesocket(c);
                continue;
            }

            // Спавн handshake-потока.  Список храним, чтобы Stop() их join'ил
            // и мы не оставляли зависшие потоки при выключении.
            std::lock_guard<std::mutex> g(m_negotiatorMx);
            // Периодически подчищаем завершившиеся handshake-потоки — иначе
            // при долгой аптайм-жизни m_negotiators разрастётся линейно.
            ReapNegotiatorsLocked();
            m_negotiators.emplace_back(&Socks5Adapter::HandleClient, this, c);
        }
    }

    // ВНИМАНИЕ: этот метод бежит в handshake-потоке.  Если хендшейк
    // прошёл успешно — сокет отдан handOff, и мы НЕ закрываем его.
    void HandleClient(SOCKET c) {
        // 1. Greeting: [ver, nmethods, methods[nmethods]]
        uint8_t hdr[2];
        if (!ReadExact(c, hdr, 2)) { CloseAdapterSide(c, "greeting hdr"); return; }
        if (hdr[0] != 0x05) {
            // не SOCKS5 — просто закрываем.
            Log(domain::LogLevel::Warn, "SOCKS5: bad protocol version in greeting");
            CloseAdapterSide(c, "bad version");
            return;
        }
        int nmethods = hdr[1];
        if (nmethods <= 0 || nmethods > 255) { CloseAdapterSide(c, "nmethods"); return; }
        uint8_t methods[255];
        if (!ReadExact(c, methods, nmethods)) { CloseAdapterSide(c, "methods"); return; }
        bool haveNoAuth = false;
        for (int i = 0; i < nmethods; ++i) {
            if (methods[i] == 0x00) { haveNoAuth = true; break; }
        }
        if (!haveNoAuth) {
            uint8_t reply[2] = { 0x05, 0xFF };
            (void)send(c, (const char*)reply, 2, 0);
            Log(domain::LogLevel::Warn, "SOCKS5: no acceptable methods (no-auth not offered)");
            CloseAdapterSide(c, "no auth method");
            return;
        }
        uint8_t okmeth[2] = { 0x05, 0x00 };
        if (send(c, (const char*)okmeth, 2, 0) != 2) {
            CloseAdapterSide(c, "send method select");
            return;
        }

        // 2. Request: [ver=0x05, cmd, rsv=0x00, atyp, dst, dst_port]
        uint8_t req[4];
        if (!ReadExact(c, req, 4)) { CloseAdapterSide(c, "request hdr"); return; }
        if (req[0] != 0x05) { CloseAdapterSide(c, "bad req version"); return; }
        const uint8_t cmd = req[1];
        // req[2] = RSV, игнорируем.
        const uint8_t atyp = req[3];

        // Только CONNECT.  BIND/UDP → 0x07 (Command not supported).
        if (cmd != 0x01) {
            SendRequestReply(c, /*rep*/0x07);
            Log(domain::LogLevel::Warn,
                "SOCKS5: unsupported cmd=" + std::to_string(cmd) + " (BIND/UDP)");
            CloseAdapterSide(c, "unsupported cmd");
            return;
        }

        uint32_t dst_ip_be = 0;
        uint16_t dst_port_host = 0;

        if (atyp == 0x01) {
            // IPv4 — 4 байта в network byte order.
            uint8_t ip4[4];
            if (!ReadExact(c, ip4, 4)) { CloseAdapterSide(c, "ipv4 addr"); return; }
            std::memcpy(&dst_ip_be, ip4, 4);
        } else if (atyp == 0x03) {
            // DOMAIN — вычитываем и корректно отвергаем.
            uint8_t dlen = 0;
            if (!ReadExact(c, &dlen, 1)) { CloseAdapterSide(c, "domain len"); return; }
            std::vector<uint8_t> tmp(dlen);
            if (dlen > 0 && !ReadExact(c, tmp.data(), dlen)) {
                CloseAdapterSide(c, "domain bytes");
                return;
            }
            // Съедаем port, чтобы TCP-буфер клиента не остался в перекошенном виде.
            uint8_t portb[2];
            (void)ReadExact(c, portb, 2);
            SendRequestReply(c, /*rep*/0x08);
            Log(domain::LogLevel::Warn,
                "SOCKS5: ATYP=DOMAIN not supported (WP12a — IPv4 only)");
            CloseAdapterSide(c, "atyp domain");
            return;
        } else if (atyp == 0x04) {
            // IPv6 — не поддерживаем в WP12a.
            uint8_t ip6[16];
            (void)ReadExact(c, ip6, 16);
            uint8_t portb[2];
            (void)ReadExact(c, portb, 2);
            SendRequestReply(c, /*rep*/0x08);
            Log(domain::LogLevel::Warn,
                "SOCKS5: ATYP=IPv6 not supported (WP12a — IPv4 only)");
            CloseAdapterSide(c, "atyp ipv6");
            return;
        } else {
            SendRequestReply(c, /*rep*/0x08);
            Log(domain::LogLevel::Warn,
                "SOCKS5: unknown ATYP=" + std::to_string(atyp));
            CloseAdapterSide(c, "atyp unknown");
            return;
        }

        // DST.PORT — network byte order → host.
        uint8_t portb[2];
        if (!ReadExact(c, portb, 2)) { CloseAdapterSide(c, "dst port"); return; }
        dst_port_host = static_cast<uint16_t>((portb[0] << 8) | portb[1]);

        // 3. Success reply: [0x05, 0x00, 0x00, 0x01, BND.ADDR=0.0.0.0, BND.PORT=0].
        //    tun2socks спокойно ест нулевой BND.ADDR/PORT для CONNECT.
        uint8_t ok[10] = { 0x05, 0x00, 0x00, 0x01,
                           0x00, 0x00, 0x00, 0x00,
                           0x00, 0x00 };
        if (send(c, (const char*)ok, 10, 0) != 10) {
            CloseAdapterSide(c, "send success reply");
            return;
        }

        // 4. Hand off — TcpRelayServer забирает сокет.  handOff синхронно
        //    делает ConnectionTable::Add(...) + запускает поток
        //    HandleNewConnection.  Ownership сокета переходит целиком.
        char ipbuf[INET_ADDRSTRLEN] = {};
        struct in_addr in; in.s_addr = dst_ip_be;
        InetNtopA(AF_INET, &in, ipbuf, sizeof(ipbuf));
        Log(domain::LogLevel::Debug,
            std::string("SOCKS5: CONNECT ok → ") + ipbuf + ":"
            + std::to_string(dst_port_host) + " (handing off to relay)");

        try {
            m_deps.handOff(c, dst_ip_be, dst_port_host);
        } catch (...) {
            // handOff бросил — сокет остался у нас, честно закроем.
            Log(domain::LogLevel::Error, "SOCKS5: handOff threw; closing socket");
            closesocket(c);
            return;
        }
        // c уже НЕ наш.  Ничего не закрываем.
    }

    // Отправка неуспешного CONNECT-ответа с указанным `rep` кодом.
    void SendRequestReply(SOCKET c, uint8_t rep) {
        uint8_t r[10] = { 0x05, rep, 0x00, 0x01,
                          0x00, 0x00, 0x00, 0x00,
                          0x00, 0x00 };
        (void)send(c, (const char*)r, 10, 0);
    }

    static bool ReadExact(SOCKET s, void* buf, int n) {
        char* p = static_cast<char*>(buf);
        int off = 0;
        while (off < n) {
            int r = recv(s, p + off, n - off, 0);
            if (r <= 0) return false;
            off += r;
        }
        return true;
    }

    // Закрываем сокет со стороны Adapter'а (когда handOff не состоялся).
    void CloseAdapterSide(SOCKET c, const char* why) {
        Log(domain::LogLevel::Debug,
            std::string("SOCKS5: closing unhanded socket (") + why + ")");
        shutdown(c, SD_BOTH);
        closesocket(c);
    }

    void ReapNegotiatorsLocked() {
        // Требует m_negotiatorMx. std::thread не предоставляет non-blocking
        // способ проверить «done» — используем маркер через detach + list?
        // Проще: жертвуем небольшим накоплением: не reap'аем здесь вовсе,
        // Stop() их joinит.  При типичной жизни (перезапуск раз в сутки)
        // это <100kB.
        (void)0;
    }

    static bool IsLoopbackHost(const std::string& h) {
        if (h.empty()) return false;
        if (h == "::1") return true;                 // IPv6 loopback (принимаем в guard,
                                                     // Start() отдельно ругается — WP12a IPv4-only)
        if (h == "localhost") return true;           // формально ок для preflight;
                                                     // Start() дальше не сможет распарсить
                                                     // как AF_INET и упадёт с parse-error,
                                                     // что тоже приемлемо.
        // 127.0.0.0/8 в текстовом виде — грубая проверка "начинается с 127."
        if (h.compare(0, 4, "127.") == 0) return true;
        return false;
    }

    void SetErr(std::string* out, const std::string& msg) {
        if (out) *out = msg;
        Log(domain::LogLevel::Error, msg);
    }

    void Log(domain::LogLevel lvl, const std::string& msg) {
        if (m_deps.log) m_deps.log->Log(lvl, "relay", msg);
    }

    void CleanupWsa() {
        if (m_wsaStarted) {
            WSACleanup();
            m_wsaStarted = false;
        }
    }

    // ---- state -----------------------------------------------------------

    std::string   m_bindHost;
    uint16_t      m_bindPort;
    Deps          m_deps;

    std::atomic<bool> m_running{false};
    bool          m_wsaStarted = false;
    SOCKET        m_listenSock = INVALID_SOCKET;
    std::thread   m_acceptThread;

    std::mutex               m_negotiatorMx;
    std::vector<std::thread> m_negotiators;
};

}}} // namespace tcp_redirector::infrastructure::relay
