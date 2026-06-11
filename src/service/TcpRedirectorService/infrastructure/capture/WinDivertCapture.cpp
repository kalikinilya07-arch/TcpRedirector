#include "WinDivertCapture.h"
#include <cstdio>
#include <cstring>
#include <iphlpapi.h>
#include <tcpmib.h>
#include <psapi.h>
#include <tlhelp32.h>
#include <vector>

#pragma comment(lib, "iphlpapi.lib")
#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "psapi.lib")

#ifndef TCP_TABLE_OWNER_PID_ALL
#define TCP_TABLE_OWNER_PID_ALL 5
#endif

// File logging - writes to both console and file
#define LOG(...) do { \
    printf(__VA_ARGS__); \
    if (g_LogFile) { fprintf(g_LogFile, __VA_ARGS__); fflush(g_LogFile); } \
} while(0)

static FILE* g_LogFile = nullptr;

namespace tcp_redirector {
namespace infrastructure {

// ---- WinDivert API dynamic loading ----
bool WinDivertCapture::WinDivertApi::Load() {
    if (dll) return true;
    dll = LoadLibraryW(L"WinDivert.dll");
    if (!dll) return false;

    #define LOAD(fn, name) \
        fn = reinterpret_cast<decltype(fn)>(GetProcAddress(dll, name)); \
        if (!fn) { Unload(); return false; }

    LOAD(Open, "WinDivertOpen");
    LOAD(Recv, "WinDivertRecv");
    LOAD(Send, "WinDivertSend");
    LOAD(Close, "WinDivertClose");
    LOAD(Shutdown, "WinDivertShutdown");
    LOAD(SetParam, "WinDivertSetParam");
    LOAD(GetParam, "WinDivertGetParam");
    LOAD(HelperParsePacket, "WinDivertHelperParsePacket");
    LOAD(HelperCalcChecksums, "WinDivertHelperCalcChecksums");

    #undef LOAD
    return true;
}

void WinDivertCapture::WinDivertApi::Unload() {
    Open = nullptr; Recv = nullptr; Send = nullptr;
    Close = nullptr; Shutdown = nullptr;
    SetParam = nullptr; GetParam = nullptr;
    HelperParsePacket = nullptr; HelperCalcChecksums = nullptr;
    if (dll) { FreeLibrary(dll); dll = nullptr; }
}

WinDivertCapture::WinDivertCapture() {
    m_hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
}

WinDivertCapture::~WinDivertCapture() {
    Close();
    if (m_hEvent) { CloseHandle(m_hEvent); m_hEvent = nullptr; }
    if (g_LogFile) { fclose(g_LogFile); g_LogFile = nullptr; }
}

bool WinDivertCapture::Open() {
    if (m_running) return true;
    if (!LoadWinDivertApi()) {
        LOG("[WinDivert] FAILED to load WinDivert.dll!\n");
        return false;
    }

    // Auto-init log file
    if (!g_LogFile) {
        wchar_t logPath[MAX_PATH] = {0};
        GetEnvironmentVariableW(L"ProgramData", logPath, MAX_PATH);
        wcscat_s(logPath, L"\\TcpRedirector\\logs\\windivert_debug.log");
        // Create directory
        for (wchar_t* p = logPath; *p; p++) {
            if (*p == L'\\') { *p = 0; CreateDirectoryW(logPath, nullptr); *p = L'\\'; }
        }
        g_LogFile = _wfopen(logPath, L"a");
        if (g_LogFile) {
            LOG("[LOG] Debug log opened: %ls\n", logPath);
        } else {
            LOG("[LOG] WARNING: Could not open log file\n");
        }
    }

    LOG("[WinDivert] Opening handle with filter: \"true\" ...\n");
    m_handle = m_api.Open("true", WINDIVERT_LAYER_NETWORK, 0, 0);

    if (!m_handle || m_handle == INVALID_HANDLE_VALUE) {
        LOG("[WinDivert] Failed (err=%lu).\n", GetLastError());
        m_api.Unload();
        return false;
    }

    LOG("[WinDivert] Handle opened: %p\n", (void*)m_handle);

    // Configure queue parameters (matching ProxyBridge approach)
    if (m_api.SetParam) {
        m_api.SetParam(m_handle, WINDIVERT_PARAM_QUEUE_LENGTH, 16384);
        m_api.SetParam(m_handle, WINDIVERT_PARAM_QUEUE_TIME, 2000);
        m_api.SetParam(m_handle, WINDIVERT_PARAM_QUEUE_SIZE, 33553920);
        LOG("[WinDivert] Queue configured\n");
    }

    m_running = true;
    m_initialized = true;
    m_captureThread = std::thread(&WinDivertCapture::CaptureLoop, this);
    return true;
}

void WinDivertCapture::Close() {
    m_running = false;
    if (m_handle) { m_api.Shutdown(m_handle, WINDIVERT_SHUTDOWN_BOTH); }
    if (m_captureThread.joinable()) m_captureThread.join();
    if (m_handle) { m_api.Close(m_handle); m_handle = nullptr; }
    m_api.Unload();
    m_initialized = false;
}

bool WinDivertCapture::IsOpen() const { return m_initialized && m_running; }

// ---- Capture Loop ----
void WinDivertCapture::CaptureLoop() {
    LOG("[WinDivert] Capture loop started\n");

    uint8_t* packet = (uint8_t*)malloc(0xFFFF);
    WINDIVERT_ADDRESS addr;
    UINT recvLen = 0;
    uint64_t pktCount = 0;

    if (!packet) {
        LOG("[WinDivert] FATAL: malloc failed\n");
        return;
    }

    while (m_running) {
        // Recv with queue_time 2000ms - returns every 2s even if no packets
        if (!m_api.Recv(m_handle, (PVOID)packet, 0xFFFF, &recvLen, &addr)) {
            DWORD err = GetLastError();
            if (err == ERROR_NO_MORE_ITEMS) {
                LOG("[WD] ERROR_NO_MORE_ITEMS - shutting down\n");
                break;
            }
            // Recv failed - log and retry (don't exit loop)
            LOG("[WD] Recv failed: err=%lu (retrying)\n", err);
            Sleep(100);
            continue;
        }

        if (recvLen == 0) continue;

        // HEARTBEAT: log every 50 packets
        pktCount++;
        if (pktCount % 50 == 0) {
            LOG("[WD] HEARTBEAT: %llu packets captured, %llu redirected\n",
                   pktCount, m_redirects_emitted.load());
        }

        bool shouldBlock = false;

        // Parse packet headers
        WINDIVERT_IPHDR* ipHdr = nullptr;
        WINDIVERT_IPV6HDR* ipv6Hdr = nullptr;
        uint8_t protocol = 0;
        WINDIVERT_TCPHDR* tcpHdr = nullptr;
        void* data = nullptr;
        UINT dataLen = 0;

        m_api.HelperParsePacket(packet, recvLen, &ipHdr, &ipv6Hdr,
                                 &protocol, nullptr, nullptr, &tcpHdr,
                                 nullptr, &data, &dataLen, nullptr, nullptr);

        if (ipHdr && tcpHdr) {
            m_packets_captured++;

            uint16_t srcPort = ntohs(tcpHdr->SrcPort);
            uint16_t dstPort = ntohs(tcpHdr->DstPort);
            bool isSyn = tcpHdr->Syn ? true : false;
            bool isSynOnly = (tcpHdr->Syn && !tcpHdr->Ack);

            // Format IPs for logging
            char srcIP[16], dstIP[16];
            snprintf(srcIP, sizeof(srcIP), "%u.%u.%u.%u",
                     (ipHdr->SrcAddr >> 0) & 0xFF, (ipHdr->SrcAddr >> 8) & 0xFF,
                     (ipHdr->SrcAddr >> 16) & 0xFF, (ipHdr->SrcAddr >> 24) & 0xFF);
            snprintf(dstIP, sizeof(dstIP), "%u.%u.%u.%u",
                     (ipHdr->DstAddr >> 0) & 0xFF, (ipHdr->DstAddr >> 8) & 0xFF,
                     (ipHdr->DstAddr >> 16) & 0xFF, (ipHdr->DstAddr >> 24) & 0xFF);

            // Determine action label: PROXIED/SKIPPED
            const char* action = "SKIPPED";
            bool isTargetSyn = false;

            uint32_t pid = 0;

            // Check SYN-only from target process
            if (isSynOnly) {
                pid = FindPidBySourcePort(srcPort);
                if (pid != 0 && IsTargetProcess(pid)) {
                    isTargetSyn = true;
                    action = "PROXIED";
                    LOG("[WD] *** TARGET SYN PID=%u %s:%u -> %s:%u ***\n",
                           pid, srcIP, srcPort, dstIP, dstPort);
                }
            }

            // Log first 100 packets, then every 100th
            if (pktCount <= 100 || pktCount % 100 == 0 || isTargetSyn) {
                snprintf(srcIP, sizeof(srcIP), "%u.%u.%u.%u",
                         (ipHdr->SrcAddr >> 0) & 0xFF, (ipHdr->SrcAddr >> 8) & 0xFF,
                         (ipHdr->SrcAddr >> 16) & 0xFF, (ipHdr->SrcAddr >> 24) & 0xFF);
                snprintf(dstIP, sizeof(dstIP), "%u.%u.%u.%u",
                         (ipHdr->DstAddr >> 0) & 0xFF, (ipHdr->DstAddr >> 8) & 0xFF,
                         (ipHdr->DstAddr >> 16) & 0xFF, (ipHdr->DstAddr >> 24) & 0xFF);

                LOG("[%s] #%llu %s %s:%u -> %s:%u [%c%c%c%c%c] len=%u\n",
                    action, pktCount,
                    isSynOnly ? "SYN" : "TCP",
                    srcIP, srcPort, dstIP, dstPort,
                    tcpHdr->Syn ? 'S' : '-',
                    tcpHdr->Ack ? 'A' : '-',
                    tcpHdr->Fin ? 'F' : '-',
                    tcpHdr->Rst ? 'R' : '-',
                    tcpHdr->Psh ? 'P' : '-',
                    dataLen);
            }

            if (isTargetSyn) {
                domain::RedirectEvent event;
                    event.redirect_id = m_nextFlowId++;
                    event.pid = pid;
                    event.process_path = m_targetProcessPath;
                    event.original_address_v4 = ipHdr->DstAddr;
                    event.original_port = dstPort;
                    m_redirects_emitted++;
                    {
                        std::lock_guard<std::mutex> lock(m_queueMutex);
                        if (m_eventQueue.size() < MAX_QUEUE_SIZE)
                            m_eventQueue.push(event);
                    }
                    SetEvent(m_hEvent);
                    shouldBlock = true;
                } else if (pktCount <= 20) {
                    LOG("[SKIPPED] #%llu SYN (no target PID) %s:%u -> %s:%u PID=%u\n",
                        pktCount, srcIP, srcPort, dstIP, dstPort, pid);
                }
            }

            // Periodically find target PID
            static uint64_t lastCheck = 0;
            if (pktCount - lastCheck >= 200 || m_targetPid == 0) {
                uint32_t newPid = FindTargetPid();
                if (newPid != m_targetPid) {
                    m_targetPid = newPid;
                    LOG("[WD] Target PID: %u\n", newPid ? newPid : 0);
                }
                lastCheck = pktCount;
            }
        }

        // Always re-inject unless blocking
        UINT sendLen = 0;
        if (!shouldBlock) {
            if (!m_api.Send(m_handle, (PVOID)packet, recvLen, &sendLen, &addr)) {
                LOG("[WD] SEND FAIL #%llu: err=%lu\n", pktCount, GetLastError());
            }
        }
    }

    free(packet);
    LOG("[WinDivert] Capture loop ended (%llu packets)\n", pktCount);
}

bool WinDivertCapture::LoadWinDivertApi() { return m_api.Load(); }

uint32_t WinDivertCapture::FindPidBySourcePort(uint16_t src_port) {
    for (int attempt = 0; attempt < 3; attempt++) {
        if (attempt > 0) Sleep(1);
        ULONG size = 0;
        GetExtendedTcpTable(nullptr, &size, FALSE, AF_INET,
                            static_cast<TCP_TABLE_CLASS>(TCP_TABLE_OWNER_PID_ALL), 0);
        if (size == 0) continue;
        std::vector<uint8_t> buffer(size);
        auto* table = reinterpret_cast<PMIB_TCPTABLE_OWNER_PID>(buffer.data());
        if (GetExtendedTcpTable(table, &size, FALSE, AF_INET,
                                static_cast<TCP_TABLE_CLASS>(TCP_TABLE_OWNER_PID_ALL), 0) != NO_ERROR)
            continue;
        for (DWORD i = 0; i < table->dwNumEntries; i++) {
            const auto& row = table->table[i];
            if (ntohs(static_cast<u_short>(row.dwLocalPort)) == src_port)
                return row.dwOwningPid;
        }
    }
    return 0;
}

uint32_t WinDivertCapture::FindTargetPid() {
    HANDLE hSnapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (hSnapshot == INVALID_HANDLE_VALUE) return 0;
    PROCESSENTRY32W pe = { sizeof(pe) };
    if (Process32FirstW(hSnapshot, &pe)) {
        do {
            if (pe.th32ProcessID == 0 || pe.th32ProcessID == GetCurrentProcessId()) continue;
            HANDLE hProcess = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pe.th32ProcessID);
            if (!hProcess) continue;
            wchar_t path[MAX_PATH] = {0};
            DWORD size = MAX_PATH;
            BOOL ok = QueryFullProcessImageNameW(hProcess, 0, path, &size);
            CloseHandle(hProcess);
            if (ok && _wcsicmp(path, m_targetProcessPath.c_str()) == 0) {
                CloseHandle(hSnapshot);
                return pe.th32ProcessID;
            }
        } while (Process32NextW(hSnapshot, &pe));
    }
    CloseHandle(hSnapshot);
    return 0;
}

bool WinDivertCapture::IsTargetProcess(uint32_t pid) {
    if (pid == 0) return false;
    if (pid == m_targetPid) return true;
    HANDLE hProcess = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!hProcess) return false;
    wchar_t path[MAX_PATH] = {0};
    DWORD size = MAX_PATH;
    BOOL ok = QueryFullProcessImageNameW(hProcess, 0, path, &size);
    CloseHandle(hProcess);
    if (ok && _wcsicmp(path, m_targetProcessPath.c_str()) == 0) {
        m_targetPid = pid;
        return true;
    }
    // Also check by process name (TransfersClient.exe) for flexibility
    return false;
}

std::vector<domain::RedirectEvent> WinDivertCapture::GetPendingRedirects(uint32_t timeout_ms) {
    if (!m_running) return {};
    if (timeout_ms > 0) WaitForSingleObject(m_hEvent, timeout_ms);
    ResetEvent(m_hEvent);
    std::vector<domain::RedirectEvent> result;
    std::lock_guard<std::mutex> lock(m_queueMutex);
    while (!m_eventQueue.empty()) {
        result.push_back(m_eventQueue.front());
        m_eventQueue.pop();
    }
    return result;
}

bool WinDivertCapture::AckRedirect(uint64_t redirect_id) { (void)redirect_id; return true; }
bool WinDivertCapture::UpdateRules(const std::vector<domain::Rule>& rules) { (void)rules; return true; }

domain::DriverStats WinDivertCapture::GetStats() {
    domain::DriverStats stats;
    stats.total_redirects = m_redirects_emitted;
    return stats;
}

std::optional<domain::ports::ProcessInfo> WinDivertCapture::QueryProcess(uint32_t pid) {
    (void)pid; return std::nullopt;
}

void* WinDivertCapture::GetEventHandle() const { return m_hEvent; }

} // namespace infrastructure
} // namespace tcp_redirector
