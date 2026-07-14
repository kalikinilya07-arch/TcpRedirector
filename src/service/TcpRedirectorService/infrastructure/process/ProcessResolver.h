#pragma once

/**
 * @file ProcessResolver.h
 * @brief Переиспользуемый резолвер «source-порт → PID → путь процесса».
 *
 * Задача 2 (фильтрация по процессу в Wintun-движке): выделяет из
 * [`WinDivertCapture`](../capture/WinDivertCapture.cpp:1) переиспользуемую
 * логику определения процесса-источника TCP-соединения по его локальному
 * (source) порту.  Раньше эта логика жила ТОЛЬКО в WinDivert; теперь она же
 * нужна embedded-движку Wintun (Tun2SocksEngineEmbedded), чтобы применять
 * те же правила RuleEngine, что и WinDivert.
 *
 * ЧТО ДЕЛАЕТ:
 *   • ResolvePidBySourcePort(srcPort) — ищет владельца локального TCP-порта
 *     через GetExtendedTcpTable(TCP_TABLE_OWNER_PID_ALL).  Результат кэшируется
 *     на PID_CACHE_TTL_MS (30 c) под SRWLOCK, чтобы не сканировать всю
 *     TCP-таблицу на каждый SYN.
 *   • GetProcessPath(pid) — полный путь EXE через
 *     OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION)+QueryFullProcessImageNameW.
 *   • IsDescendantOf(pid, ancestorPid) — обход дерева процессов вверх (до 10
 *     уровней) для ловли helper/child-процессов (аналог §6 CheckProcessRule
 *     в WinDivert).
 *
 * ПОТОКОБЕЗОПАСНОСТЬ:
 *   Все методы потокобезопасны.  Внутренний PID-кэш защищён SRWLOCK.
 *   Резолв можно (и нужно) делать ВНЕ «тяжёлых» локов вызывающей стороны
 *   (например, до захвата core-lock lwIP в embedded-движке).
 *
 * NB: Компонент header-only (inline), чтобы не плодить .cpp и не менять
 *     существующее поведение WinDivert.  iphlpapi.lib/psapi.lib уже линкуются
 *     в TcpRedirectorService.vcxproj.
 */

#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <iphlpapi.h>
#include <tlhelp32.h>

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

#pragma comment(lib, "iphlpapi.lib")

#ifndef TCP_TABLE_OWNER_PID_ALL
#define TCP_TABLE_OWNER_PID_ALL 5
#endif

namespace tcp_redirector {
namespace infrastructure {
namespace process {

/**
 * @brief Резолвер процесса-источника TCP-соединения по source-порту.
 *
 * Одна инстанция на потребителя (движок/капчур).  Держит собственный
 * PID-кэш — не разделяется между инстанциями, чтобы не вносить скрытых
 * зависимостей.
 */
class ProcessResolver {
public:
    ProcessResolver() = default;
    ~ProcessResolver() = default;

    ProcessResolver(const ProcessResolver&) = delete;
    ProcessResolver& operator=(const ProcessResolver&) = delete;

    /**
     * @brief Найти PID владельца локального (source) TCP-порта.
     *
     * Сначала проверяет TTL-кэш; при промахе/протухании сканирует
     * TCP-таблицу через GetExtendedTcpTable и кэширует результат.
     *
     * @param src_port Локальный TCP-порт (host byte order).
     * @return PID владельца или 0, если не найден.
     */
    uint32_t ResolvePidBySourcePort(uint16_t src_port) {
        // 1. Кэш (shared-lock).
        {
            AcquireSRWLockShared(&m_pidCacheLock);
            auto it = m_pidCache.find(src_port);
            if (it != m_pidCache.end()) {
                const uint64_t now = GetTickCount64();
                if (now - it->second.timestamp_ms < PID_CACHE_TTL_MS) {
                    const uint32_t pid = it->second.pid;
                    ReleaseSRWLockShared(&m_pidCacheLock);
                    return pid;
                }
            }
            ReleaseSRWLockShared(&m_pidCacheLock);
        }

        // 2. Скан TCP-таблицы (тяжёлый путь).
        ULONG size = 0;
        GetExtendedTcpTable(nullptr, &size, FALSE, AF_INET,
                            static_cast<TCP_TABLE_CLASS>(TCP_TABLE_OWNER_PID_ALL), 0);
        if (size == 0) return 0;

        std::vector<uint8_t> buffer(size);
        auto* table = reinterpret_cast<PMIB_TCPTABLE_OWNER_PID>(buffer.data());
        if (GetExtendedTcpTable(table, &size, FALSE, AF_INET,
                                static_cast<TCP_TABLE_CLASS>(TCP_TABLE_OWNER_PID_ALL),
                                0) != NO_ERROR) {
            return 0;
        }

        for (DWORD i = 0; i < table->dwNumEntries; ++i) {
            // dwLocalPort — DWORD, содержит порт в network byte order как u_short.
            if (ntohs(static_cast<u_short>(table->table[i].dwLocalPort)) == src_port) {
                const uint32_t pid = table->table[i].dwOwningPid;
                AcquireSRWLockExclusive(&m_pidCacheLock);
                m_pidCache[src_port] = {pid, GetTickCount64()};
                ReleaseSRWLockExclusive(&m_pidCacheLock);
                return pid;
            }
        }
        return 0;
    }

    /**
     * @brief Полный путь EXE-файла процесса по PID.
     * @param pid PID процесса.
     * @return Полный путь (например "C:\\...\\app.exe") или пустая строка.
     */
    static std::wstring GetProcessPath(uint32_t pid) {
        if (pid == 0) return std::wstring();
        HANDLE hProcess = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
        if (!hProcess) return std::wstring();
        wchar_t path[MAX_PATH] = {0};
        DWORD size = MAX_PATH;
        BOOL ok = QueryFullProcessImageNameW(hProcess, 0, path, &size);
        CloseHandle(hProcess);
        if (!ok) return std::wstring();
        return std::wstring(path);
    }

    /**
     * @brief Короткое имя файла (последний компонент пути) из полного пути.
     * @param full_path Полный путь.
     * @return Имя файла (например "app.exe").
     */
    static std::wstring ShortName(const std::wstring& full_path) {
        const auto pos = full_path.find_last_of(L'\\');
        if (pos != std::wstring::npos) return full_path.substr(pos + 1);
        return full_path;
    }

    /**
     * @brief Является ли процесс pid потомком (или самим) ancestor_pid.
     *
     * Обходит дерево процессов вверх по th32ParentProcessID (до max_depth
     * уровней) — ловит helper/child-процессы, чей трафик логически
     * принадлежит целевому приложению.  Копия семантики §6 CheckProcessRule
     * из WinDivert.
     *
     * @param pid         PID-кандидат.
     * @param ancestor_pid Искомый предок.
     * @param max_depth   Максимум уровней вверх (по умолчанию 10).
     * @return true, если pid == ancestor_pid или один из предков совпал.
     */
    static bool IsDescendantOf(uint32_t pid, uint32_t ancestor_pid,
                               int max_depth = 10) {
        if (pid == 0 || ancestor_pid == 0) return false;
        if (pid == ancestor_pid) return true;

        HANDLE hSnap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
        if (hSnap == INVALID_HANDLE_VALUE) return false;

        bool result = false;
        uint32_t currentPid = pid;
        int depth = 0;
        while (currentPid != 0 && depth < max_depth) {
            PROCESSENTRY32W pe = { sizeof(pe) };
            bool found = false;
            if (Process32FirstW(hSnap, &pe)) {
                do {
                    if (pe.th32ProcessID == currentPid) {
                        if (pe.th32ProcessID == ancestor_pid) {
                            result = true;
                            found = true;
                            break;
                        }
                        currentPid = pe.th32ParentProcessID;
                        found = true;
                        ++depth;
                        break;
                    }
                } while (Process32NextW(hSnap, &pe));
            }
            if (result) break;
            if (!found) break;
        }
        CloseHandle(hSnap);
        return result;
    }

    /**
     * @brief Найти PID первого процесса, чьё имя файла совпадает с именем
     *        файла target_path (регистро-независимо).
     *
     * Используется как fallback, когда target-процесс задан путём, но PID
     * ещё не известен (аналог WinDivertCapture::FindTargetPid).
     *
     * @param target_path Полный путь целевого процесса (сравнивается только
     *                    имя файла).
     * @return PID или 0.
     */
    static uint32_t FindPidByImageName(const std::wstring& target_path) {
        if (target_path.empty()) return 0;
        const std::wstring targetName = ShortName(target_path);

        HANDLE hSnap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
        if (hSnap == INVALID_HANDLE_VALUE) return 0;

        uint32_t result = 0;
        PROCESSENTRY32W pe = { sizeof(pe) };
        if (Process32FirstW(hSnap, &pe)) {
            do {
                if (pe.th32ProcessID == 0 ||
                    pe.th32ProcessID == GetCurrentProcessId()) continue;
                std::wstring path = GetProcessPath(pe.th32ProcessID);
                if (path.empty()) continue;
                if (_wcsicmp(ShortName(path).c_str(), targetName.c_str()) == 0) {
                    result = pe.th32ProcessID;
                    break;
                }
            } while (Process32NextW(hSnap, &pe));
        }
        CloseHandle(hSnap);
        return result;
    }

private:
    struct PidCacheEntry {
        uint32_t pid;
        uint64_t timestamp_ms;  //!< GetTickCount64() на момент кэширования.
    };

    mutable SRWLOCK m_pidCacheLock = SRWLOCK_INIT;
    std::unordered_map<uint16_t, PidCacheEntry> m_pidCache;
    static constexpr uint64_t PID_CACHE_TTL_MS = 30000;  //!< 30 секунд.
};

} // namespace process
} // namespace infrastructure
} // namespace tcp_redirector
