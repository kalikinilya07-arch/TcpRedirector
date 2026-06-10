#pragma once

//
// Npcap capture module — user-mode replacement for kernel WFP driver.
// Captures outgoing TCP SYN packets, identifies processes, matches rules,
// and emits RedirectEvents for the service to process.
//
// Npcap driver (npcap.sys) is WHQL-signed by Microsoft — works with
// Secure Boot, no test mode required.
//

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winsock2.h>
#include <ws2tcpip.h>
#include <iphlpapi.h>
#include <tcpmib.h>

#include "pcap_min.h"
#include <vector>
#include <queue>
#include <mutex>
#include <thread>
#include <atomic>
#include <functional>
#include <optional>
#include <string>

#include "../../domain/ports/IDriverCommunicator.h"

namespace tcp_redirector {
namespace infrastructure {

//
// NpcapCapture — implements IDriverCommunicator using Npcap packet capture
// instead of kernel IOCTL.
//
class NpcapCapture : public domain::ports::IDriverCommunicator {
public:
    NpcapCapture();
    ~NpcapCapture() override;

    // IDriverCommunicator interface
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

    // Capture statistics
    struct CaptureStats {
        uint64_t packets_captured = 0;
        uint64_t redirects_emitted = 0;
        uint64_t pid_lookups_failed = 0;
        uint64_t syn_filtered = 0;
    };
    CaptureStats GetCaptureStats() const;

private:
    // Packet capture thread
    void CaptureLoop();
    static void PacketHandler(u_char* user, const struct pcap_pkthdr* header,
                              const u_char* packet);

    // Packet processing helpers
    void ProcessSynPacket(const u_char* packet, uint32_t length);

    // PID lookup by source port + destination
    uint32_t FindPidBySourcePort(uint16_t src_port,
                                  uint32_t dst_addr, uint16_t dst_port);

    // Redirect event queue management
    void EnqueueEvent(const domain::RedirectEvent& event);
    std::vector<domain::RedirectEvent> DrainQueue();

    // Load pcap API at runtime (no SDK/.lib needed)
    bool LoadPcapApi();

    // Check if Npcap runtime DLL is available
    static bool IsNpcapInstalled();

    // IP/port helpers
    static std::string IpToString(uint32_t addr);
    static std::wstring GetProcessNameByPid(uint32_t pid);
    static std::wstring GetProcessPathByPid(uint32_t pid);

    PcapApi m_api;
    pcap_t* m_handle = nullptr;
    std::thread m_captureThread;
    std::atomic<bool> m_running{false};
    std::atomic<bool> m_initialized{false};
    HANDLE m_hEvent = nullptr;

    // Redirect event queue
    std::mutex m_queueMutex;
    std::queue<domain::RedirectEvent> m_eventQueue;
    static constexpr size_t MAX_QUEUE_SIZE = 4096;

    // Current rules (for local matching)
    std::mutex m_rulesMutex;
    std::vector<domain::Rule> m_rules;

    // Statistics
    CaptureStats m_stats;

    // Redirect ID counter
    std::atomic<uint64_t> m_nextRedirectId{1};
};

} // namespace infrastructure
} // namespace tcp_redirector
