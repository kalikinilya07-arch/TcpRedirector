#include "WinDivertCapture.h"
#include <cstdio>
#include <cstdarg>
#include <cstring>
#include <iphlpapi.h>
#include <tcpmib.h>
#include <tlhelp32.h>
#include <vector>

#pragma comment(lib, "iphlpapi.lib")
#pragma comment(lib, "ws2_32.lib")

#ifndef TCP_TABLE_OWNER_PID_ALL
#define TCP_TABLE_OWNER_PID_ALL 5
#endif

// File logging — пишем в windivert_debug.log
static FILE* g_wdLogFile = nullptr;

// Log level macros for WinDivert capture logging
#define WD_ERROR(...) WdLog(domain::LogLevel::Error, __VA_ARGS__)
#define WD_WARN(...)  WdLog(domain::LogLevel::Warn, __VA_ARGS__)
#define WD_INFO(...)  WdLog(domain::LogLevel::Info, __VA_ARGS__)
#define WD_DEBUG(...) WdLog(domain::LogLevel::Debug, __VA_ARGS__)
#define WD_TRACE(...) WdLog(domain::LogLevel::Trace, __VA_ARGS__)

namespace tcp_redirector {
namespace infrastructure {

// ---- WdLog: write to console, debug file + forward to ILogSink ----
void WinDivertCapture::WdLog(domain::LogLevel level, const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    char buf[2048];
    vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);

    // Always write to console (--console mode needs this)
    SYSTEMTIME st; GetLocalTime(&st);
    fprintf(stdout, "[%04u-%02u-%02u %02u:%02u:%02u.%03u] %s",
        st.wYear, st.wMonth, st.wDay,
        st.wHour, st.wMinute, st.wSecond, st.wMilliseconds, buf);
    fflush(stdout);

    // Always write to debug file
    if (g_wdLogFile) {
        fprintf(g_wdLogFile, "[%04u-%02u-%02u %02u:%02u:%02u.%03u] %s",
            st.wYear, st.wMonth, st.wDay,
            st.wHour, st.wMinute, st.wSecond, st.wMilliseconds, buf);
        fflush(g_wdLogFile);
    }

    // Log through ILogSink if available
    if (m_logSink) {
        m_logSink->Log(level, "windivert", std::string(buf));
    }
}

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
    // Инициализация bitmap
    memset(m_portDecided, 0, sizeof(m_portDecided));
    memset(m_portDirect, 0, sizeof(m_portDirect));
}

WinDivertCapture::~WinDivertCapture() {
    Close();
    if (m_hEvent) { CloseHandle(m_hEvent); m_hEvent = nullptr; }
    if (g_wdLogFile) { fclose(g_wdLogFile); g_wdLogFile = nullptr; }
}

bool WinDivertCapture::Open() {
    if (m_running) return true;
    if (!LoadWinDivertApi()) {
        WD_ERROR("[WinDivert] FAILED to load WinDivert.dll!\n");
        return false;
    }

    // Auto-init log file
    if (!g_wdLogFile) {
        wchar_t logPath[MAX_PATH] = {0};
        GetEnvironmentVariableW(L"ProgramData", logPath, MAX_PATH);
        wcscat_s(logPath, L"\\TcpRedirector\\logs\\windivert_debug.log");
        for (wchar_t* p = logPath; *p; p++) {
            if (*p == L'\\') { *p = 0; CreateDirectoryW(logPath, nullptr); *p = L'\\'; }
        }
        g_wdLogFile = _wfopen(logPath, L"a");
        if (g_wdLogFile) {
            WD_DEBUG("[LOG] Debug log opened: %ls\n", logPath);
        }
    }

    // Фильтр: все outbound TCP + loopback + relay порт (как в ProxyBridge)
    char filter[512];
    snprintf(filter, sizeof(filter),
        "(tcp and (outbound or loopback or (tcp.DstPort == %d or tcp.SrcPort == %d)))",
        m_relayPort, m_relayPort);

    WD_INFO("[WinDivert] Opening handle with filter: \"%s\" ...\n", filter);
    m_handle = m_api.Open(filter, WINDIVERT_LAYER_NETWORK, 0, 0);

    if (!m_handle || m_handle == INVALID_HANDLE_VALUE) {
        WD_WARN("[WinDivert] Failed (err=%lu). Falling back to \"true\"\n", GetLastError());
        m_handle = m_api.Open("true", WINDIVERT_LAYER_NETWORK, 0, 0);
        if (!m_handle || m_handle == INVALID_HANDLE_VALUE) {
            WD_ERROR("[WinDivert] Failed with \"true\" too (err=%lu).\n", GetLastError());
            m_api.Unload();
            return false;
        }
    }

    WD_INFO("[WinDivert] Handle opened: %p\n", (void*)m_handle);

    // Configure queue parameters (H5: check handle validity before use)
    if (m_api.SetParam && m_handle && m_handle != INVALID_HANDLE_VALUE) {
        m_api.SetParam(m_handle, WINDIVERT_PARAM_QUEUE_LENGTH, 16384);
        m_api.SetParam(m_handle, WINDIVERT_PARAM_QUEUE_TIME, 2000);
        m_api.SetParam(m_handle, WINDIVERT_PARAM_QUEUE_SIZE, 33553920);
        WD_DEBUG("[WinDivert] Queue configured\n");
    }

    // Pre-find target PID if process is already running
    if (m_targetPid == 0 && !m_targetProcessPath.empty()) {
        FindTargetPid();
    }

    // Очищаем bitmap при старте
    memset(m_portDecided, 0, sizeof(m_portDecided));
    memset(m_portDirect, 0, sizeof(m_portDirect));

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

// =====================================================================
// CaptureLoop — 4-шаговая обработка (как в ProxyBridge packet_processor)
// Шаг 1: Per-port bitmap (fast path)
// Шаг 2: Relay response → restore original DST
// Шаг 3: Tracked connection → DST modify
// Шаг 4: Untracked → CheckProcessRule → DIRECT/PROXY/BLOCK
// =====================================================================
void WinDivertCapture::CaptureLoop() {
    WD_INFO("[WD] CaptureLoop started (relayPort=%u, proxy=%s:%u)\n",
        m_relayPort, m_proxyHost.c_str(), m_proxyPort);

    uint8_t* packet = (uint8_t*)malloc(0xFFFF);
    WINDIVERT_ADDRESS addr;
    UINT recvLen = 0;
    uint64_t pktCount = 0;

    if (!packet) {
        WD_ERROR("[WD] FATAL: malloc failed\n");
        return;
    }

    while (m_running) {
        if (!m_api.Recv(m_handle, (PVOID)packet, 0xFFFF, &recvLen, &addr)) {
            DWORD err = GetLastError();
            if (err == ERROR_NO_MORE_ITEMS) {
                WD_ERROR("[WD] ERROR_NO_MORE_ITEMS\n");
                break;
            }
            Sleep(10);
            continue;
        }

        if (recvLen == 0) continue;
        pktCount++;

        // Parse packet
        WINDIVERT_IPHDR* ipHdr = nullptr;
        WINDIVERT_IPV6HDR* ipv6Hdr = nullptr;
        uint8_t protocol = 0;
        WINDIVERT_TCPHDR* tcpHdr = nullptr;

        m_api.HelperParsePacket(packet, recvLen, &ipHdr, &ipv6Hdr,
                                 &protocol, nullptr, nullptr, &tcpHdr,
                                 nullptr, nullptr, nullptr, nullptr, nullptr);

        // Только IPv4 TCP
        if (!ipHdr || !tcpHdr) {
            // non-TCP или IPv6 — пропускаем без изменений
            m_api.Send(m_handle, (PVOID)packet, recvLen, nullptr, &addr);
            continue;
        }

        m_packets_captured++;
        bool modified = false;
        uint16_t srcPort = ntohs(tcpHdr->SrcPort);
        uint16_t dstPort = ntohs(tcpHdr->DstPort);

        // ==== Шаг 1: Per-port bitmap (fast path) ====
        if (IsPortDecided(srcPort)) {
            if (tcpHdr->Fin || tcpHdr->Rst) {
                ClearPort(srcPort);
            }
            if (IsPortDirect(srcPort)) {
                if (tcpHdr->Fin || tcpHdr->Rst) {
                    // Log connection close for previously MISSED connections
                }
                // DIRECT — отправляем без изменений
                m_api.Send(m_handle, (PVOID)packet, recvLen, nullptr, &addr);
                continue;
            }
            // decided, not direct (PROXY/BLOCK) — fall through к connection track
        }

        // ==== Шаг 2: Relay response (data from target back to client) ====
        if (srcPort == m_relayPort && m_connTable != nullptr) {
            uint32_t origIp = 0;
            uint16_t origPort = 0;
            if (m_connTable->Get(dstPort, &origIp, &origPort)) {
                RestoreFromRelay(packet, recvLen, addr, ipHdr, tcpHdr);
                modified = true;

                // Track downstream bytes (target → client)
                m_connTable->AddBytes(dstPort, 0, recvLen);

                if (tcpHdr->Fin || tcpHdr->Rst) {
                    // Log connection summary
                    domain::ports::ConnectionInfo info;
                    if (m_connTable->GetInfo(dstPort, &info)) {
                        WD_DEBUG("[PROXIED] %ls (srcPort=%u) closed: up=%llu down=%llu total=%llu\n",
                            ShortName(info.proc_path), dstPort,
                            info.bytes_up, info.bytes_down,
                            info.bytes_up + info.bytes_down);
                        m_totalRxBytes.fetch_add(info.bytes_up, std::memory_order_relaxed);
                        m_totalTxBytes.fetch_add(info.bytes_down, std::memory_order_relaxed);
                    }
                    m_connTable->Remove(dstPort);
                    ClearPort(dstPort);
                }
            }
        }
        // ==== Шаг 3: Tracked connection (data from client to relay) ====
        else if (m_connTable != nullptr &&
                 m_connTable->IsTracked(srcPort)) {

            // Track upstream bytes (client → target via relay)
            m_connTable->AddBytes(srcPort, recvLen, 0);

            if (tcpHdr->Fin || tcpHdr->Rst) {
                // Log connection summary
                domain::ports::ConnectionInfo info;
                if (m_connTable->GetInfo(srcPort, &info)) {
                    WD_DEBUG("[PROXIED] %ls (srcPort=%u) closed: up=%llu down=%llu total=%llu\n",
                        ShortName(info.proc_path), srcPort,
                        info.bytes_up, info.bytes_down,
                        info.bytes_up + info.bytes_down);
                    m_totalRxBytes.fetch_add(info.bytes_up, std::memory_order_relaxed);
                    m_totalTxBytes.fetch_add(info.bytes_down, std::memory_order_relaxed);
                }
                m_connTable->Remove(srcPort);
                ClearPort(srcPort);
            }

            tcpHdr->DstPort = htons(m_relayPort);
            if (!IsLocalhostPair(ipHdr)) {
                SwapIpAndDirection(ipHdr, addr);
            }
            modified = true;
        }
        // ==== Шаг 4: Untracked outbound → CheckProcessRule ====
        else if (addr.Outbound && m_connTable != nullptr &&
                 !m_connTable->IsTracked(srcPort)) {

            uint32_t proxyCfgId = 0;
            uint32_t pid = 0;
            wchar_t procPath[MAX_PATH] = {0};
            int action = CheckProcessRule(
                ntohl(ipHdr->SrcAddr), srcPort,
                ntohl(ipHdr->DstAddr), dstPort,
                &proxyCfgId, &pid, procPath, MAX_PATH);

            if (action == 0) { // DIRECT
                SetPortDirect(srcPort);
                m_api.Send(m_handle, (PVOID)packet, recvLen, nullptr, &addr);
                const wchar_t* name = procPath[0] ? ShortName(procPath) : nullptr;
                if (name) {
                    WD_TRACE("[MISSED] %ls srcPort=%u\n", name, srcPort);
                }
                continue;
            }
            else if (action == 2) { // BLOCK
                SetPortDecided(srcPort);
                continue;
            }
            else if (action == 1) { // PROXY
                uint32_t origDestIp = ipHdr->DstAddr;
                uint16_t origDestPort = dstPort;

                m_connTable->Add(srcPort, ipHdr->SrcAddr,
                                origDestIp, origDestPort, proxyCfgId);
                if (pid != 0 && procPath[0]) {
                    m_connTable->SetProcessInfo(srcPort, pid, procPath);
                }
                SetPortDecided(srcPort);
                m_redirects_emitted++;

                ModifyDstToRelay(packet, recvLen, addr, ipHdr, tcpHdr,
                                origDestIp, origDestPort);
                modified = true;

                char dstIP[16];
                snprintf(dstIP, sizeof(dstIP), "%u.%u.%u.%u",
                    (origDestIp >> 0) & 0xFF, (origDestIp >> 8) & 0xFF,
                    (origDestIp >> 16) & 0xFF, (origDestIp >> 24) & 0xFF);
                WD_TRACE("[PROXIED] %ls srcPort=%u %s:%u bytes=%u to %s:%u\n",
                    ShortName(procPath), srcPort, dstIP, origDestPort, recvLen,
                    m_proxyHost.c_str(), m_proxyPort);
            }
        }

        // Отправка (с чексуммами если modified)
        if (modified) {
            if (m_api.HelperCalcChecksums) {
                m_api.HelperCalcChecksums(packet, recvLen, &addr, 0);
            }
        }
        if (!m_api.Send(m_handle, (PVOID)packet, recvLen, nullptr, &addr)) {
            if (pktCount % 1000 == 0) {
                WD_WARN("[WD] SEND FAIL #%llu err=%lu\n", pktCount, GetLastError());
            }
        }
    }

    free(packet);
    WD_INFO("[WD] Capture loop ended (%llu packets, %llu redirected)\n",
        pktCount, m_redirects_emitted.load());
}

// =====================================================================
// CheckProcessRule — проверяет PID и правила для пакета
// Returns: 0=DIRECT, 1=PROXY, 2=BLOCK
// =====================================================================
int WinDivertCapture::CheckProcessRule(uint32_t src_ip, uint16_t src_port,
                                        uint32_t dst_ip, uint16_t dst_port,
                                        uint32_t* out_proxy_config_id,
                                        uint32_t* out_pid,
                                        wchar_t* out_proc_path,
                                        DWORD out_proc_path_size) {
    (void)src_ip;
    (void)dst_ip;
    (void)dst_port;

    // 1. Find PID by source port
    uint32_t pid = FindPidBySourcePort(src_port);

    if (pid == 0) {
        // PID не найден — возможно приложение ещё не в TCP-таблице.
        // Попробуем найти target PID, если ещё не нашли
        if (m_targetPid == 0 && !m_targetProcessPath.empty()) {
            uint32_t newPid = FindTargetPid();
            if (newPid != 0) {
                m_targetPid = newPid;
            }
        }
        // Если targetPid известен, а PID=0 — возможно helper процесс,
        // который ещё не создал запись. Возвращаем DIRECT — следующий пакет
        // этого же соединения (ACK/DATA) снова вызовет CheckProcessRule.
        return 0; // DIRECT
    }

    // 2. Исключаем собственный процесс (loop prevention)
    if (pid == GetCurrentProcessId()) {
        return 0; // DIRECT
    }

    // 3. Получаем имя процесса
    wchar_t procPath[MAX_PATH] = {0};
    {
        HANDLE hProcess = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
        if (!hProcess) {
            return 0; // DIRECT
        }
        DWORD size = MAX_PATH;
        BOOL ok = QueryFullProcessImageNameW(hProcess, 0, procPath, &size);
        CloseHandle(hProcess);
        if (!ok) {
            return 0; // DIRECT
        }
    }

    // Заполняем out-параметры PID/пути (для логов)
    if (out_pid) *out_pid = pid;
    if (out_proc_path && out_proc_path_size > 0) {
        wcscpy_s(out_proc_path, out_proc_path_size, procPath);
    }

    // 4. Проверка: совпадает с target process?
    if (_wcsicmp(procPath, m_targetProcessPath.c_str()) == 0) {
        m_targetPid = pid;

        // Проверка, что прокси настроен
        if (m_proxyHost.empty() || m_proxyPort == 0) {
            return 0; // DIRECT
        }
        if (out_proxy_config_id) *out_proxy_config_id = 0;
        return 1; // PROXY
    }

    // 5. Поиск в дереве процессов (helper/child process)
    if (m_targetPid != 0) {
        HANDLE hSnap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
        if (hSnap != INVALID_HANDLE_VALUE) {
            PROCESSENTRY32W pe = { sizeof(pe) };
            uint32_t currentPid = pid;
            int depth = 0;
            while (currentPid != 0 && depth < 10) {
                bool found = false;
                if (Process32FirstW(hSnap, &pe)) {
                    do {
                        if (pe.th32ProcessID == currentPid) {
                            if (pe.th32ProcessID == m_targetPid.load()) {
                                CloseHandle(hSnap);
                                if (m_proxyHost.empty() || m_proxyPort == 0) {
                                    return 0; // DIRECT
                                }
                                if (out_proxy_config_id) *out_proxy_config_id = 0;
                                return 1; // PROXY
                            }
                            currentPid = pe.th32ParentProcessID;
                            found = true;
                            depth++;
                            break;
                        }
                    } while (Process32NextW(hSnap, &pe));
                }
                if (!found) {
                    break;
                }
            }
            CloseHandle(hSnap);
        }
    }

    // 6. Не совпадает — DIRECT
    return 0; // DIRECT
}

// ---- DST Modification helpers ----

void WinDivertCapture::ModifyDstToRelay(uint8_t* packet, UINT packetLen,
                                         WINDIVERT_ADDRESS& addr,
                                         WINDIVERT_IPHDR* ipHdr,
                                         WINDIVERT_TCPHDR* tcpHdr,
                                         uint32_t orig_dest_ip,
                                         uint16_t orig_dest_port) {
    (void)packet;
    (void)packetLen;
    (void)orig_dest_ip;
    (void)orig_dest_port;

    // Меняем порт назначения на relayPort
    tcpHdr->DstPort = htons(m_relayPort);

    if (!IsLocalhostPair(ipHdr)) {
        SwapIpAndDirection(ipHdr, addr);
    }
    // loopback→loopback: только порт, Outbound остаётся TRUE
}

void WinDivertCapture::RestoreFromRelay(uint8_t* packet, UINT packetLen,
                                         WINDIVERT_ADDRESS& addr,
                                         WINDIVERT_IPHDR* ipHdr,
                                         WINDIVERT_TCPHDR* tcpHdr) {
    (void)packet;
    (void)packetLen;

    uint16_t dstPort = ntohs(tcpHdr->DstPort);

    // Восстанавливаем original source port
    uint32_t orig_dest_ip = 0;
    uint16_t orig_dest_port = 0;
    if (m_connTable && m_connTable->Get(dstPort, &orig_dest_ip, &orig_dest_port)) {
        tcpHdr->SrcPort = htons(orig_dest_port);
    }

    if (!IsLocalhostPair(ipHdr)) {
        SwapIpAndDirection(ipHdr, addr);
    }
}

// ---- Localhost check ----

bool WinDivertCapture::IsLocalhost(uint32_t ip) {
    return (ip >> 24) == 127; // 127.x.x.x
}

// ---- PID helpers ----

bool WinDivertCapture::LoadWinDivertApi() { return m_api.Load(); }

uint32_t WinDivertCapture::FindPidBySourcePort(uint16_t src_port) {
    // 1 попытка без Sleep (как в ProxyBridge — если не нашли, вернёмся позже)
    ULONG size = 0;
    GetExtendedTcpTable(nullptr, &size, FALSE, AF_INET,
                        static_cast<TCP_TABLE_CLASS>(TCP_TABLE_OWNER_PID_ALL), 0);
    if (size == 0) return 0;

    std::vector<uint8_t> buffer(size);
    auto* table = reinterpret_cast<PMIB_TCPTABLE_OWNER_PID>(buffer.data());
    if (GetExtendedTcpTable(table, &size, FALSE, AF_INET,
                            static_cast<TCP_TABLE_CLASS>(TCP_TABLE_OWNER_PID_ALL), 0) != NO_ERROR)
        return 0;

    for (DWORD i = 0; i < table->dwNumEntries; i++) {
        // dwLocalPort — DWORD, но содержит порт в network byte order как u_short
        if (ntohs(static_cast<u_short>(table->table[i].dwLocalPort)) == src_port) {
            return table->table[i].dwOwningPid;
        }
    }

    return 0;
}

uint32_t WinDivertCapture::FindTargetPid() {
    HANDLE hSnapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (hSnapshot == INVALID_HANDLE_VALUE) {
        return 0;
    }
    PROCESSENTRY32W pe = { sizeof(pe) };
    int count = 0;
    if (Process32FirstW(hSnapshot, &pe)) {
        do {
            count++;
            if (pe.th32ProcessID == 0 || pe.th32ProcessID == GetCurrentProcessId()) continue;
            HANDLE hProcess = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pe.th32ProcessID);
            if (!hProcess) continue;
            wchar_t path[MAX_PATH] = {0};
            DWORD size = MAX_PATH;
            BOOL ok = QueryFullProcessImageNameW(hProcess, 0, path, &size);
            CloseHandle(hProcess);
            if (ok) {
                // Ищем процесс по имени файла (без пути)
                const wchar_t* fname = wcsrchr(path, L'\\');
                fname = fname ? fname + 1 : path;
                const wchar_t* target = wcsrchr(m_targetProcessPath.c_str(), L'\\');
                target = target ? target + 1 : m_targetProcessPath.c_str();
                if (_wcsicmp(fname, target) == 0) {
                    CloseHandle(hSnapshot);
                    return pe.th32ProcessID;
                }
            }
        } while (Process32NextW(hSnapshot, &pe));
    }
    CloseHandle(hSnapshot);
    return 0;
}

bool WinDivertCapture::IsTargetProcess(uint32_t pid) {
    // Этот метод больше не используется напрямую (заменён на CheckProcessRule).
    // Оставлен для совместимости.
    if (pid == 0) return false;
    if (pid == m_targetPid) return true;
    return false;
}

// ---- Stub methods ----

std::vector<domain::RedirectEvent> WinDivertCapture::GetPendingRedirects(uint32_t timeout_ms) {
    (void)timeout_ms;
    return {};
}

bool WinDivertCapture::AckRedirect(uint64_t redirect_id) {
    (void)redirect_id;
    return true;
}

domain::DriverStats WinDivertCapture::GetStats() {
    domain::DriverStats stats;
    stats.total_redirects = m_redirects_emitted;
    return stats;
}

void* WinDivertCapture::GetEventHandle() const {
    return m_hEvent;
}

} // namespace infrastructure
} // namespace tcp_redirector
