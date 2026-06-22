#pragma once

//
// WinDivertCapture — captures TCP packets via WinDivert, identifies target process,
// performs DST modification to redirect to local relay server (TcpRelayServer).
//
// АРХИТЕКТУРА (v2 — PID fix):
// В отличие от v1, где PID проверялся ТОЛЬКО для SYN (что давало PID=0),
// v2 проверяет PID для КАЖДОГО untracked outbound TCP-пакета, используя
// per-port bitmap для кэширования решения (DIRECT/PROXY/BLOCK).
//
// Вдохновлено ProxyBridge (https://github.com/InterceptSuite/ProxyBridge).
// Анализ и план: docs/PID_FIX_PLAN.md
//

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winsock2.h>
#include <ws2tcpip.h>

#include <vector>
#include <queue>
#include <mutex>
#include <thread>
#include <atomic>
#include <functional>
#include <optional>
#include <string>
#include <unordered_map>
#include <memory>

#include <windivert.h>
#include "../../domain/ports/ICapture.h"
#include "../../domain/ports/IConnectionTable.h"

namespace tcp_redirector {
namespace infrastructure {

class WinDivertCapture : public domain::ports::ICapture {
public:
    WinDivertCapture();
    ~WinDivertCapture() override;

    // ICapture interface (через неё работают все клиенты)
    bool Open() override;
    void Close() override;
    bool IsOpen() const override;

    void SetTargetProcess(const std::wstring& exePath) override {
        m_targetProcessPath = exePath;
        m_targetPid = 0;
    }

    void SetConnectionTable(domain::ports::IConnectionTable* table) override {
        m_connTable = table;
    }

    void SetRelayPort(uint16_t port) override {
        m_relayPort = port;
    }

    void SetProxyConfig(const std::string& host, uint16_t port) override {
        m_proxyHost = host;
        m_proxyPort = port;
    }

    std::vector<domain::RedirectEvent> GetPendingRedirects(
        uint32_t timeout_ms = 1000) override;
    bool AckRedirect(uint64_t redirect_id) override;
    domain::DriverStats GetStats() override;
    void* GetEventHandle() const override;

private:
    bool LoadWinDivertApi();
    void CaptureLoop();

    // PID + rule check: единая точка входа
    // Returns: 0 = DIRECT, 1 = PROXY, 2 = BLOCK
    // Если PROXY, out_proxy_config_id заполняется
    // out_pid / out_proc_path заполняются при любом non-zero PID
    int CheckProcessRule(uint32_t src_ip, uint16_t src_port,
                         uint32_t dst_ip, uint16_t dst_port,
                         uint32_t* out_proxy_config_id,
                         uint32_t* out_pid = nullptr,
                         wchar_t* out_proc_path = nullptr,
                         DWORD out_proc_path_size = 0);

    // Получить короткое имя файла из полного пути (для логов)
    static const wchar_t* ShortName(const wchar_t* path) {
        const wchar_t* p = wcsrchr(path, L'\\');
        return p ? p + 1 : path;
    }

    // Per-port decision bitmap (как в ProxyBridge)
    // Два битмапа по 2048 LONG = 8 KB каждый
    //   portDecided[bit] = 1 → решение кэшировано
    //   portDirect[bit]  = 1 → решение DIRECT (bit clear = PROXY/BLOCK)
    // Три состояния:
    //   decided=0 → нет кэша, нужно вызвать CheckProcessRule
    //   decided=1, direct=1 → DIRECT, пропустить без изменений
    //   decided=1, direct=0 → PROXY/BLOCK, уже в connection table
    //
    // Thread safety: InterlockedOr/And для записи; aligned 32-bit read для чтения
    // (x86/x64 aligned read атомарен)
    // НЕ статические — каждый экземпляр WinDivertCapture имеет свой bitmap.
    // static → non-static для тестируемости (глобальное состояние запрещено).
    LONG m_portDecided[2048]{0};
    LONG m_portDirect[2048]{0};

    // inline helpers для битмапов
    bool IsPortDecided(uint16_t port) const {
        return (m_portDecided[port >> 5] >> (port & 31)) & 1;
    }
    bool IsPortDirect(uint16_t port) const {
        return (m_portDirect[port >> 5] >> (port & 31)) & 1;
    }
    void SetPortDirect(uint16_t port) {
        InterlockedOr(&m_portDecided[port >> 5], (LONG)(1u << (port & 31)));
        InterlockedOr(&m_portDirect[port >> 5],  (LONG)(1u << (port & 31)));
    }
    void SetPortDecided(uint16_t port) {
        // decided, direct=0 → PROXY/BLOCK
        InterlockedOr(&m_portDecided[port >> 5], (LONG)(1u << (port & 31)));
        // m_portDirect bit stays 0
    }
    void ClearPort(uint16_t port) {
        InterlockedAnd(&m_portDecided[port >> 5], (LONG)~(1u << (port & 31)));
        InterlockedAnd(&m_portDirect[port >> 5],  (LONG)~(1u << (port & 31)));
    }

    // PID helpers (используются внутри CheckProcessRule)
    uint32_t FindPidBySourcePort(uint16_t src_port);
    uint32_t FindTargetPid();
    bool     IsTargetProcess(uint32_t pid);

    // DST modification helpers
    void ModifyDstToRelay(uint8_t* packet, UINT packetLen, WINDIVERT_ADDRESS& addr,
                          WINDIVERT_IPHDR* ipHdr, WINDIVERT_TCPHDR* tcpHdr,
                          uint32_t orig_dest_ip, uint16_t orig_dest_port);
    void RestoreFromRelay(uint8_t* packet, UINT packetLen, WINDIVERT_ADDRESS& addr,
                          WINDIVERT_IPHDR* ipHdr, WINDIVERT_TCPHDR* tcpHdr);
    bool IsLocalhost(uint32_t ip);
    bool IsLocalhostPair(WINDIVERT_IPHDR* ipHdr) {
        return IsLocalhost(ntohl(ipHdr->SrcAddr)) &&
               IsLocalhost(ntohl(ipHdr->DstAddr));
    }
    void SwapIpAndDirection(WINDIVERT_IPHDR* ipHdr, WINDIVERT_ADDRESS& addr) const {
        uint32_t temp = ipHdr->DstAddr;
        ipHdr->DstAddr = ipHdr->SrcAddr;
        ipHdr->SrcAddr = temp;
        addr.Outbound = FALSE;
    }

    // WinDivert state
    HANDLE m_handle = nullptr;
    std::thread m_captureThread;
    std::atomic<bool> m_running{false};
    std::atomic<bool> m_initialized{false};

    // Target process
    std::wstring m_targetProcessPath = L"C:\\Projects\\china\\police_sec\\TransfersClient.exe";
    std::atomic<uint32_t> m_targetPid{0};

    // Proxy config (одиночный, упрощённый — как в .pbprofile)
    std::string m_proxyHost = "127.0.0.1";
    uint16_t    m_proxyPort = 3128;

    // Localhost bypass (как в ProxyBridge LocalhostViaProxy)
    std::atomic<bool> m_localhostViaProxy{false};

    // DST modification relay
    domain::ports::IConnectionTable* m_connTable = nullptr;
    uint16_t m_relayPort = 34010;

    // Stats
    std::atomic<uint64_t> m_packets_captured{0};
    std::atomic<uint64_t> m_redirects_emitted{0};

    // Redirect event queue (заглушка, не используется)
    std::mutex m_queueMutex;
    std::queue<domain::RedirectEvent> m_eventQueue;
    HANDLE m_hEvent = nullptr;
    static constexpr size_t MAX_QUEUE_SIZE = 4096;
    std::atomic<uint64_t> m_nextFlowId{1};

    // WinDivert API
    struct WinDivertApi {
        HMODULE dll = nullptr;
        typedef HANDLE(WINAPI* PFN_Open)(const char* filter, WINDIVERT_LAYER layer,
                                          int16_t priority, uint64_t flags);
        typedef BOOL(WINAPI* PFN_Recv)(HANDLE handle, PVOID packet, UINT packetLen,
                                        UINT* recvLen, WINDIVERT_ADDRESS* addr);
        typedef BOOL(WINAPI* PFN_Send)(HANDLE handle, PVOID packet, UINT packetLen,
                                        UINT* sendLen, const WINDIVERT_ADDRESS* addr);
        typedef BOOL(WINAPI* PFN_Close)(HANDLE handle);
        typedef BOOL(WINAPI* PFN_Shutdown)(HANDLE handle, WINDIVERT_SHUTDOWN how);
        typedef BOOL(WINAPI* PFN_HelperParsePacket)(const void* packet, UINT packetLen,
                     WINDIVERT_IPHDR** ppIpHdr, WINDIVERT_IPV6HDR** ppIpv6Hdr,
                     uint8_t* pProtocol, WINDIVERT_ICMPHDR** ppIcmpHdr,
                     WINDIVERT_ICMPV6HDR** ppIcmpv6Hdr, WINDIVERT_TCPHDR** ppTcpHdr,
                     WINDIVERT_UDPHDR** ppUdpHdr, void** ppData, UINT* pDataLen,
                     void** ppNext, UINT* pNextLen);
        typedef BOOL(WINAPI* PFN_HelperCalcChecksums)(void* packet, UINT packetLen,
                     WINDIVERT_ADDRESS* addr, uint64_t flags);
        typedef BOOL(WINAPI* PFN_SetParam)(HANDLE handle, WINDIVERT_PARAM param, uint64_t value);
        typedef BOOL(WINAPI* PFN_GetParam)(HANDLE handle, WINDIVERT_PARAM param, uint64_t* value);

        PFN_Open Open = nullptr;
        PFN_Recv Recv = nullptr;
        PFN_Send Send = nullptr;
        PFN_Close Close = nullptr;
        PFN_Shutdown Shutdown = nullptr;
        PFN_SetParam SetParam = nullptr;
        PFN_GetParam GetParam = nullptr;
        PFN_HelperParsePacket HelperParsePacket = nullptr;
        PFN_HelperCalcChecksums HelperCalcChecksums = nullptr;

        bool Load();
        void Unload();
    } m_api;

};


} // namespace infrastructure
} // namespace tcp_redirector