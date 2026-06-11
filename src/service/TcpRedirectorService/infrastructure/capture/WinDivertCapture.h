#pragma once

//
// WinDivert capture module — replaces NpcapCapture for actual TCP redirection.
// Uses WinDivert (WHQL-signed kernel driver) to intercept, block, and redirect
// TCP SYN packets from the target process through an HTTP CONNECT proxy.
//
// Architecture:
//   1. WinDivert captures outbound TCP SYN packets
//   2. For target process: BLOCK original SYN, create CONNECT tunnel to proxy,
//      spoof SYN-ACK back to process, bridge all subsequent data through tunnel
//   3. For non-target: re-inject original packet unchanged (pass-through)
//
// WinDivert driver (WinDivert64.sys) is WHQL-signed by Microsoft — works with
// Secure Boot, no Test Mode, no EV certificate needed.
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
#include "../../domain/ports/IDriverCommunicator.h"

namespace tcp_redirector {
namespace infrastructure {

//
// WinDivertCapture — implements IDriverCommunicator using WinDivert.
//
class WinDivertCapture : public domain::ports::IDriverCommunicator {
public:
    WinDivertCapture();
    ~WinDivertCapture() override;

    // IDriverCommunicator interface
    // File logging for remote debugging
    bool InitLog(const std::wstring& logPath);
    void Log(const char* fmt, ...);

    bool Open() override;
    void Close() override;
    bool IsOpen() const override;

    std::vector<domain::RedirectEvent> GetPendingRedirects(
        uint32_t timeout_ms = 1000) override;
    bool AckRedirect(uint64_t redirect_id) override;
    bool UpdateRules(const std::vector<domain::Rule>& rules) override;
    domain::DriverStats GetStats() override;
    std::optional<domain::ports::ProcessInfo> QueryProcess(uint32_t pid) override;
    void* GetEventHandle() const override;

private:
    // Load WinDivert DLL at runtime
    bool LoadWinDivertApi();

    // Capture thread
    void CaptureLoop();

    // Proxy connection thread for a single redirected flow
    struct RedirectFlow;
    void ProxyBridge(RedirectFlow* flow);

    // TCP connection tracking entry
    struct RedirectFlow {
        uint64_t id;
        uint32_t src_addr;
        uint16_t src_port;
        uint32_t dst_addr;
        uint16_t dst_port;
        uint32_t proxy_addr;
        uint16_t proxy_port;

        // TCP state tracking
        uint32_t client_seq = 0;
        uint32_t server_seq = 0;
        uint32_t client_ack = 0;
        uint32_t server_ack = 0;

        // Proxy connection
        SOCKET proxy_sock = INVALID_SOCKET;
        HANDLE bridge_thread = nullptr;
        bool connected = false;
        bool closed = false;
    };

    // Find PID from TCP table
    uint32_t FindPidBySourcePort(uint16_t src_port);

    // Check if PID belongs to target process
    bool IsTargetProcess(uint32_t pid);

    // Spoof TCP packets
    bool SendSpoofedSynAck(const RedirectFlow& flow);
    bool SendSpoofedPacket(const RedirectFlow& flow, const uint8_t* data, int dataLen, uint8_t flags);
    bool SendSpoofedRst(const RedirectFlow& flow);

    // Calculate checksum helper
    uint16_t TcpChecksum(const uint8_t* packet, int ipHdrLen, int totalLen);

    // Redirect event queue
    std::mutex m_queueMutex;
    std::queue<domain::RedirectEvent> m_eventQueue;
    HANDLE m_hEvent = nullptr;
    static constexpr size_t MAX_QUEUE_SIZE = 4096;

    // Flow tracking
    std::mutex m_flowsMutex;
    std::unordered_map<uint64_t, std::unique_ptr<RedirectFlow>> m_flows;
    std::atomic<uint64_t> m_nextFlowId{1};

    // WinDivert state
    HANDLE m_handle = nullptr;
    std::thread m_captureThread;
    std::atomic<bool> m_running{false};
    std::atomic<bool> m_initialized{false};

    // Target process info
    std::wstring m_targetProcessPath = L"C:\\Projects\\china\\police_sec\\TransfersClient.exe";
    std::atomic<uint32_t> m_targetPid{0};
    uint32_t FindTargetPid();

    // WinDivert API function pointers
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

    // Stats
    std::atomic<uint64_t> m_packets_captured{0};
    std::atomic<uint64_t> m_redirects_emitted{0};
    std::atomic<uint64_t> m_pid_lookups_failed{0};
    std::atomic<uint64_t> m_flows_active{0};

    // Debug log file
    FILE* m_logFile = nullptr;
    std::mutex m_logMutex;
};

} // namespace infrastructure
} // namespace tcp_redirector