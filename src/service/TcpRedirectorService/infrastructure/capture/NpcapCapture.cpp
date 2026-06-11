#include "NpcapCapture.h"
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <psapi.h>
#include <string>
#include <tlhelp32.h>

// TCP_TABLE_OWNER_PID_CONNECT may not be defined with WIN32_LEAN_AND_MEAN
#ifndef TCP_TABLE_OWNER_PID_CONNECT
#define TCP_TABLE_OWNER_PID_CONNECT 5
#endif

#pragma comment(lib, "iphlpapi.lib")
#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "psapi.lib")

namespace tcp_redirector {
namespace infrastructure {

// Context passed to pcap_dispatch callback — identifies which adapter
struct CaptureContext {
    NpcapCapture* self;
    int adapterIndex;
};

NpcapCapture::NpcapCapture() {
    m_hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
}

NpcapCapture::~NpcapCapture() {
    Close();
    if (m_hEvent) {
        CloseHandle(m_hEvent);
        m_hEvent = nullptr;
    }
}

bool NpcapCapture::LoadPcapApi() {
    return m_api.Load();
}

bool NpcapCapture::Open() {
    if (m_running) return true;

    if (!LoadPcapApi()) {
        printf("[Npcap] FAILED to load wpcap.dll! Is Npcap installed?\n");
        return false;
    }

    char errbuf[PCAP_ERRBUF_SIZE] = {0};

    // Enumerate network devices
    pcap_if_t* allDevs = nullptr;
    if (m_api.findalldevs(&allDevs, errbuf) != 0) {
        printf("[Npcap] pcap_findalldevs failed: %s\n", errbuf);
        m_api.Unload();
        return false;
    }

    // === DEBUG: List ALL available adapters ===
    printf("[Npcap] Available adapters:\n");
    int devIndex = 0;
    for (pcap_if_t* d = allDevs; d != nullptr; d = d->next, devIndex++) {
        printf("[Npcap]   [%d] %s\n", devIndex, d->name ? d->name : "(no name)");
        if (d->description) {
            printf("[Npcap]       desc: %s\n", d->description);
        }
        printf("[Npcap]       flags: 0x%x %s%s%s%s\n",
               d->flags,
               (d->flags & PCAP_IF_LOOPBACK) ? "LOOPBACK " : "",
               (d->flags & PCAP_IF_UP) ? "UP " : "DOWN ",
               (d->flags & PCAP_IF_RUNNING) ? "RUNNING " : "",
               (d->flags & 0x10) ? "WIRELESS " : "");
        if (d->addresses) {
            for (pcap_addr_t* a = d->addresses; a; a = a->next) {
                if (a->addr && a->addr->sa_family == AF_INET) {
                    char ip[32] = "?";
                    struct sockaddr_in* sin = (struct sockaddr_in*)a->addr;
                    unsigned char* p = (unsigned char*)&sin->sin_addr;
                    snprintf(ip, sizeof(ip), "%u.%u.%u.%u", p[0], p[1], p[2], p[3]);
                    printf("[Npcap]       IP: %s\n", ip);
                }
            }
        }
    }
    printf("[Npcap] Total adapters: %d\n", devIndex);

    // === Open ALL suitable adapters ===
    m_running = true;

    for (pcap_if_t* d = allDevs; d != nullptr; d = d->next) {
        // Skip DOWN adapters (not LOOPBACK - we WANT loopback for local traffic!)
        if (!(d->flags & PCAP_IF_UP)) continue;
        // Skip WAN Miniports
        if (d->description && std::string(d->description).find("WAN Miniport") != std::string::npos) continue;
        // Skip Bluetooth
        if (d->description && std::string(d->description).find("Bluetooth") != std::string::npos) continue;
        // Skip Wi-Fi Direct Virtual
        if (d->description && std::string(d->description).find("Wi-Fi Direct Virtual") != std::string::npos) continue;

        // Open this adapter
        pcap_t* handle = m_api.open_live(d->name, 65536, 1, 1000, errbuf);
        if (!handle) {
            printf("[Npcap]   SKIP %s — open failed: %s\n",
                   d->name ? d->name : "?", errbuf);
            continue;
        }

        // Set TCP filter
        struct bpf_program fp;
        if (m_api.compile(handle, &fp, "tcp", 1, PCAP_NETMASK_UNKNOWN) == 0) {
            m_api.setfilter(handle, &fp);
            m_api.freecode(&fp);
        }

        // Extract IP for logging
        std::string ipStr;
        if (d->addresses) {
            for (pcap_addr_t* a = d->addresses; a; a = a->next) {
                if (a->addr && a->addr->sa_family == AF_INET) {
                    struct sockaddr_in* sin = (struct sockaddr_in*)a->addr;
                    unsigned char* p = (unsigned char*)&sin->sin_addr;
                    char buf[32];
                    snprintf(buf, sizeof(buf), "%u.%u.%u.%u", p[0], p[1], p[2], p[3]);
                    ipStr = buf;
                    break;
                }
            }
        }

        int idx = static_cast<int>(m_handles.size());
        m_handles.push_back(handle);
        m_adapterInfos.push_back({
            d->name ? d->name : "",
            d->description ? d->description : "",
            ipStr
        });

        printf("[Npcap]   OPEN [%d] %s (%s) IP=%s\n",
               idx,
               d->description ? d->description : d->name ? d->name : "?",
               d->name ? d->name : "?",
               ipStr.c_str());
    }

    m_api.freealldevs(allDevs);

    if (m_handles.empty()) {
        printf("[Npcap] No suitable adapters opened!\n");
        m_running = false;
        return false;
    }

    printf("[Npcap] Opened %zu adapter(s)\n", m_handles.size());

    // Start capture threads
    m_initialized = true;
    for (int i = 0; i < static_cast<int>(m_handles.size()); i++) {
        m_captureThreads.emplace_back(&NpcapCapture::CaptureLoop, this, i);
    }

    return true;
}

void NpcapCapture::Close() {
    m_running = false;

    // Break all capture loops
    for (auto* handle : m_handles) {
        if (handle) m_api.breakloop(handle);
    }

    // Join all threads
    for (auto& t : m_captureThreads) {
        if (t.joinable()) t.join();
    }

    // Close all handles
    for (auto* handle : m_handles) {
        if (handle) m_api.close(handle);
    }

    m_handles.clear();
    m_captureThreads.clear();
    m_adapterInfos.clear();

    m_api.Unload();
    m_initialized = false;
}

bool NpcapCapture::IsOpen() const {
    return m_initialized && m_running && !m_handles.empty();
}

void NpcapCapture::CaptureLoop(int adapterIndex) {
    if (adapterIndex < 0 || adapterIndex >= static_cast<int>(m_handles.size())) return;

    pcap_t* handle = m_handles[adapterIndex];
    const auto& info = m_adapterInfos[adapterIndex];
    const char* desc = info.desc.empty() ? info.name.c_str() : info.desc.c_str();

    printf("[Npcap] Thread [%d] started on: %s\n", adapterIndex, desc);

    CaptureContext ctx = { this, adapterIndex };

    while (m_running && handle) {
        int ret = m_api.dispatch(handle, 10, PacketHandler,
                                 reinterpret_cast<u_char*>(&ctx));
        if (ret < 0) {
            printf("[Npcap] Thread [%d] pcap_dispatch error: %d\n", adapterIndex, ret);
            break;
        }
    }

    printf("[Npcap] Thread [%d] stopped\n", adapterIndex);
}

void NpcapCapture::PacketHandler(u_char* user, const struct pcap_pkthdr* header,
                                  const u_char* packet) {
    auto* ctx = reinterpret_cast<CaptureContext*>(user);
    ctx->self->ProcessSynPacket(packet, header->len, ctx->adapterIndex);
}

void NpcapCapture::ProcessSynPacket(const u_char* packet, uint32_t length, int adapterIndex) {
    if (length < 40) return; // Ethernet(14) + IP(20) + TCP min

    m_stats.packets_captured++;
    uint64_t pktNum = m_stats.packets_captured;

    const u_char* ip_hdr = packet + 14;

    // Validate IP version
    uint8_t ip_ver = (ip_hdr[0] >> 4) & 0x0F;
    if (ip_ver != 4) return; // IPv4 only for MVP

    // Extract IP header fields (big-endian)
    uint8_t ip_hdr_len = (ip_hdr[0] & 0x0F) * 4;
    uint32_t src_addr = (ip_hdr[12] << 24) | (ip_hdr[13] << 16) |
                        (ip_hdr[14] << 8) | ip_hdr[15];
    uint32_t dst_addr = (ip_hdr[16] << 24) | (ip_hdr[17] << 16) |
                        (ip_hdr[18] << 8) | ip_hdr[19];

    // Extract TCP header fields
    const u_char* tcp_hdr = ip_hdr + ip_hdr_len;
    uint16_t src_port = (tcp_hdr[0] << 8) | tcp_hdr[1];
    uint16_t dst_port = (tcp_hdr[2] << 8) | tcp_hdr[3];
    uint8_t tcp_flags = tcp_hdr[13];
    uint8_t ttl = ip_hdr[8];

    // Build flags string
    char flag_buf[32] = {0};
    if (tcp_flags & 0x02) strcat_s(flag_buf, sizeof(flag_buf), "SYN ");
    if (tcp_flags & 0x10) strcat_s(flag_buf, sizeof(flag_buf), "ACK ");
    if (tcp_flags & 0x01) strcat_s(flag_buf, sizeof(flag_buf), "FIN ");
    if (tcp_flags & 0x04) strcat_s(flag_buf, sizeof(flag_buf), "RST ");
    if (tcp_flags & 0x08) strcat_s(flag_buf, sizeof(flag_buf), "PSH ");
    if (flag_buf[0] == 0) strcpy_s(flag_buf, sizeof(flag_buf), "NONE ");

    // Get adapter info
    const auto& info = (adapterIndex >= 0 && adapterIndex < static_cast<int>(m_adapterInfos.size()))
        ? m_adapterInfos[adapterIndex] : AdapterInfo{"?", "?", "?"};
    const char* adapterDesc = info.desc.empty() ? info.name.c_str() : info.desc.c_str();

    // Log every packet up to #100, then every 50th
    bool logPkt = (pktNum <= 100 || pktNum % 50 == 0);
    if (logPkt) {
        printf("[PKT #%llu] [%s] %u.%u.%u.%u:%u -> %u.%u.%u.%u:%u [%s] TTL=%u len=%u\n",
               pktNum, adapterDesc,
               (src_addr >> 24) & 0xFF, (src_addr >> 16) & 0xFF,
               (src_addr >> 8) & 0xFF, src_addr & 0xFF, src_port,
               (dst_addr >> 24) & 0xFF, (dst_addr >> 16) & 0xFF,
               (dst_addr >> 8) & 0xFF, dst_addr & 0xFF, dst_port,
               flag_buf, ttl, length);
    }

    // === Diagnostic: dump ALL TCP connections from target process when PID changes ===
    static uint32_t lastDumpedPid = 0;

    // Refresh target PID cache periodically
    if (m_stats.packets_captured - m_lastTargetScan >= 50 || m_targetPid == 0) {
        uint32_t newPid = FindTargetPid();
        if (newPid != m_targetPid) {
            m_targetPid = newPid;
            if (newPid) {
                printf("[TARGET] Found TransfersClient.exe PID=%u\n", newPid);
                // Dump all TCP connections for this PID
                DumpProcessConnections(newPid);
                lastDumpedPid = newPid;
            } else {
                printf("[TARGET] TransfersClient.exe NOT running\n");
            }
        } else if (m_targetPid != 0 && lastDumpedPid != m_targetPid) {
            // PID unchanged but first dump
            DumpProcessConnections(m_targetPid);
            lastDumpedPid = m_targetPid;
        }
        m_lastTargetScan.store(m_stats.packets_captured.load());
    }

    // === Find PID from TCP table ===
    uint32_t pid = FindPidBySourcePort(src_port, dst_addr, dst_port);

    m_stats.syn_filtered++;

    // Determine if this packet is from target process
    bool isTargetProcess = false;
    if (pid != 0 && pid != GetCurrentProcessId()) {
        if (pid == m_targetPid) {
            isTargetProcess = true;
        } else {
            isTargetProcess = IsPidTargetProcess(pid);
            if (isTargetProcess) {
                m_targetPid = pid; // update cached PID
            }
        }
    }

    const char* actionLabel = isTargetProcess ? "PROXIED" : "SKIPPED";

    // === LOG ALL packets with PROXIED/SKIPPED marking ===
    if (logPkt || isTargetProcess) {
        printf("[%s] #%llu [%s] PID=%u %u.%u.%u.%u:%u -> %u.%u.%u.%u:%u [%s]\n",
               actionLabel, pktNum, adapterDesc, pid,
               (src_addr >> 24) & 0xFF, (src_addr >> 16) & 0xFF,
               (src_addr >> 8) & 0xFF, src_addr & 0xFF, src_port,
               (dst_addr >> 24) & 0xFF, (dst_addr >> 16) & 0xFF,
               (dst_addr >> 8) & 0xFF, dst_addr & 0xFF, dst_port,
               flag_buf);
    }

    // === Only redirect target process traffic ===
    if (isTargetProcess) {
        domain::RedirectEvent event;
        event.redirect_id = m_nextRedirectId++;
        event.pid = pid;
        event.process_path = m_targetProcessPath;
        event.original_address_v4 = dst_addr;
        event.original_port = dst_port;
        event.redirect_local_port = 0;
        event.is_ipv6 = false;

        m_stats.redirects_emitted++;
        EnqueueEvent(event);
        SetEvent(m_hEvent);
    }
}

void NpcapCapture::DumpProcessConnections(uint32_t pid) {
    printf("[TCP TABLE] All connections for PID=%u:\n", pid);
    int count = 0;

    ULONG size = 0;
    GetExtendedTcpTable(nullptr, &size, FALSE, AF_INET,
                        TCP_TABLE_OWNER_PID_ALL, 0);
    if (size == 0) return;

    std::vector<uint8_t> buffer(size);
    auto* table = reinterpret_cast<PMIB_TCPTABLE_OWNER_PID>(buffer.data());

    DWORD ret = GetExtendedTcpTable(table, &size, FALSE, AF_INET,
                                    TCP_TABLE_OWNER_PID_ALL, 0);
    if (ret != NO_ERROR) return;

    for (DWORD i = 0; i < table->dwNumEntries; i++) {
        const auto& row = table->table[i];
        if (row.dwOwningPid != pid) continue;

        count++;
        uint16_t lport = ntohs(static_cast<u_short>(row.dwLocalPort));
        uint16_t rport = ntohs(static_cast<u_short>(row.dwRemotePort));

        char laddr[32], raddr[32];
        snprintf(laddr, sizeof(laddr), "%u.%u.%u.%u",
                 (row.dwLocalAddr >> 0) & 0xFF, (row.dwLocalAddr >> 8) & 0xFF,
                 (row.dwLocalAddr >> 16) & 0xFF, (row.dwLocalAddr >> 24) & 0xFF);
        snprintf(raddr, sizeof(raddr), "%u.%u.%u.%u",
                 (row.dwRemoteAddr >> 0) & 0xFF, (row.dwRemoteAddr >> 8) & 0xFF,
                 (row.dwRemoteAddr >> 16) & 0xFF, (row.dwRemoteAddr >> 24) & 0xFF);

        const char* state = "?";
        switch (row.dwState) {
            case 1: state = "CLOSED"; break;
            case 2: state = "LISTEN"; break;
            case 3: state = "SYN_SENT"; break;
            case 4: state = "SYN_RCVD"; break;
            case 5: state = "ESTABLISHED"; break;
            case 6: state = "FIN_WAIT1"; break;
            case 7: state = "FIN_WAIT2"; break;
            case 8: state = "CLOSE_WAIT"; break;
            case 9: state = "CLOSING"; break;
            case 10: state = "LAST_ACK"; break;
            case 11: state = "TIME_WAIT"; break;
            case 12: state = "DELETE_TCB"; break;
        }

        printf("[TCP TABLE]   %s:%u -> %s:%u [%s]\n",
               laddr, lport, raddr, rport, state);
    }

    if (count == 0) {
        printf("[TCP TABLE]   (no TCP connections found for PID=%u)\n", pid);
    }
    printf("[TCP TABLE] Total %d connection(s) for PID=%u\n", count, pid);
}

uint32_t NpcapCapture::FindPidBySourcePort(uint16_t src_port,
                                            uint32_t dst_addr, uint16_t dst_port) {
    for (int attempt = 0; attempt < 3; attempt++) {
        if (attempt > 0) Sleep(1);

        ULONG size = 0;
        DWORD ret = GetExtendedTcpTable(nullptr, &size, FALSE, AF_INET,
                                        TCP_TABLE_OWNER_PID_ALL, 0);
        if (ret != ERROR_INSUFFICIENT_BUFFER || size == 0) continue;

        std::vector<uint8_t> buffer(size);
        auto* table = reinterpret_cast<PMIB_TCPTABLE_OWNER_PID>(buffer.data());

        ret = GetExtendedTcpTable(table, &size, FALSE, AF_INET,
                                   TCP_TABLE_OWNER_PID_ALL, 0);
        if (ret != NO_ERROR) continue;

        for (DWORD i = 0; i < table->dwNumEntries; i++) {
            const auto& row = table->table[i];
            uint16_t row_lport = ntohs(static_cast<u_short>(row.dwLocalPort));
            if (row_lport == src_port) {
                return row.dwOwningPid;
            }
        }
    }

    return 0;
}

uint32_t NpcapCapture::FindTargetPid() {
    printf("[TARGET SCAN] Scanning all processes for: \"%ls\"\n", m_targetProcessPath.c_str());

    HANDLE hSnapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (hSnapshot == INVALID_HANDLE_VALUE) {
        printf("[TARGET SCAN] CreateToolhelp32Snapshot failed: %lu\n", GetLastError());
        return 0;
    }

    PROCESSENTRY32W pe = { sizeof(pe) };
    uint32_t foundPid = 0;
    DWORD processCount = 0;

    if (Process32FirstW(hSnapshot, &pe)) {
        do {
            processCount++;
            if (pe.th32ProcessID == 0 || pe.th32ProcessID == GetCurrentProcessId())
                continue;

            bool shouldLog = (processCount <= 5);

            HANDLE hProcess = OpenProcess(
                PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pe.th32ProcessID);
            if (!hProcess) {
                if (shouldLog) {
                    printf("[TARGET SCAN]   PID=%u (%ls) - OpenProcess failed: %lu\n",
                           pe.th32ProcessID, pe.szExeFile, GetLastError());
                }
                continue;
            }

            wchar_t path[MAX_PATH] = {0};
            DWORD size = MAX_PATH;
            BOOL ok = QueryFullProcessImageNameW(hProcess, 0, path, &size);
            CloseHandle(hProcess);

            if (!ok) {
                if (shouldLog) {
                    printf("[TARGET SCAN]   PID=%u (%ls) - QueryFullProcessImageName failed: %lu\n",
                           pe.th32ProcessID, pe.szExeFile, GetLastError());
                }
                continue;
            }

            bool match = (ok && _wcsicmp(path, m_targetProcessPath.c_str()) == 0);
            if (match || shouldLog) {
                wprintf(L"[TARGET SCAN]   PID=%u (%ls) path=\"%ls\" match=%d\n",
                        pe.th32ProcessID, pe.szExeFile, path, match ? 1 : 0);
            }

            if (match) {
                foundPid = pe.th32ProcessID;
                printf("[TARGET SCAN] FOUND! PID=%u for TransfersClient.exe\n", foundPid);
                break;
            }
        } while (Process32NextW(hSnapshot, &pe));
    }

    CloseHandle(hSnapshot);
    printf("[TARGET SCAN] Scanned %lu processes, result: PID=%u\n", processCount, foundPid);
    return foundPid;
}

bool NpcapCapture::IsPidTargetProcess(uint32_t pid) {
    if (pid == 0) return false;
    if (pid == m_targetPid) return true;

    auto path = GetProcessPathByPid(pid);
    return _wcsicmp(path.c_str(), m_targetProcessPath.c_str()) == 0;
}

void NpcapCapture::EnqueueEvent(const domain::RedirectEvent& event) {
    std::lock_guard<std::mutex> lock(m_queueMutex);
    if (m_eventQueue.size() < MAX_QUEUE_SIZE) {
        m_eventQueue.push(event);
    }
}

std::vector<domain::RedirectEvent> NpcapCapture::DrainQueue() {
    std::vector<domain::RedirectEvent> result;
    std::lock_guard<std::mutex> lock(m_queueMutex);
    while (!m_eventQueue.empty()) {
        result.push_back(m_eventQueue.front());
        m_eventQueue.pop();
    }
    return result;
}

std::vector<domain::RedirectEvent> NpcapCapture::GetPendingRedirects(
    uint32_t timeout_ms) {
    if (!m_running) return {};

    if (timeout_ms > 0) {
        WaitForSingleObject(m_hEvent, timeout_ms);
    }
    ResetEvent(m_hEvent);

    return DrainQueue();
}

bool NpcapCapture::AckRedirect(uint64_t redirect_id) {
    (void)redirect_id;
    return true;
}

bool NpcapCapture::UpdateRules(const std::vector<domain::Rule>& rules) {
    std::lock_guard<std::mutex> lock(m_rulesMutex);
    m_rules = rules;
    return true;
}

domain::DriverStats NpcapCapture::GetStats() {
    domain::DriverStats stats;
    stats.pending_redirects = 0;
    {
        std::lock_guard<std::mutex> lock(m_queueMutex);
        stats.pending_redirects = static_cast<uint32_t>(m_eventQueue.size());
    }
    stats.total_redirects = m_stats.redirects_emitted;
    stats.failed_redirects = m_stats.pid_lookups_failed;
    stats.rules_count = 0;
    {
        std::lock_guard<std::mutex> lock(m_rulesMutex);
        stats.rules_count = static_cast<uint32_t>(m_rules.size());
    }
    return stats;
}

std::optional<domain::ports::ProcessInfo> NpcapCapture::QueryProcess(uint32_t pid) {
    domain::ports::ProcessInfo info;
    info.pid = pid;
    info.name = GetProcessNameByPid(pid);
    info.path = GetProcessPathByPid(pid);
    if (info.name.empty()) return std::nullopt;
    return info;
}

void* NpcapCapture::GetEventHandle() const {
    return m_hEvent;
}

// Static helpers

bool NpcapCapture::IsNpcapInstalled() {
    PcapApi api;
    return api.Load();
}

std::string NpcapCapture::IpToString(uint32_t addr) {
    char buf[16];
    snprintf(buf, sizeof(buf), "%u.%u.%u.%u",
             (addr >> 24) & 0xFF, (addr >> 16) & 0xFF,
             (addr >> 8) & 0xFF, addr & 0xFF);
    return buf;
}

std::wstring NpcapCapture::GetProcessNameByPid(uint32_t pid) {
    HANDLE hSnapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (hSnapshot == INVALID_HANDLE_VALUE) return L"";

    PROCESSENTRY32W pe = { sizeof(pe) };
    std::wstring name;
    if (Process32FirstW(hSnapshot, &pe)) {
        do {
            if (pe.th32ProcessID == pid) {
                name = pe.szExeFile;
                break;
            }
        } while (Process32NextW(hSnapshot, &pe));
    }
    CloseHandle(hSnapshot);
    return name;
}

std::wstring NpcapCapture::GetProcessPathByPid(uint32_t pid) {
    HANDLE hProcess = OpenProcess(
        PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!hProcess) return L"";

    wchar_t path[MAX_PATH] = {0};
    DWORD size = MAX_PATH;
    BOOL ok = QueryFullProcessImageNameW(hProcess, 0, path, &size);
    CloseHandle(hProcess);

    return ok ? std::wstring(path) : L"";
}

} // namespace infrastructure
} // namespace tcp_redirector
