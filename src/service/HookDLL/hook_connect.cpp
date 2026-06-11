#include "hook_connect.h"
#include <cstdint>
#include <cstdio>
#include <cstring>

#pragma comment(lib, "ws2_32.lib")

// ---- Configuration ----
static const char* g_proxy_host = "127.0.0.1";
static uint16_t g_proxy_port = 3128;
static bool g_hook_enabled = true;
static CRITICAL_SECTION g_hook_lock;

// ---- Detour (x64 JMP) ----
// We overwrite the first 12 bytes of connect() with:
//   48 B8 XX XX XX XX XX XX XX XX   mov rax, <target_address>
//   FF E0                            jmp rax
static const size_t DETOUR_SIZE = 12;

// Original function type
typedef int (WSAAPI* ConnectFn)(SOCKET s, const struct sockaddr* name, int namelen);
static ConnectFn RealConnect = nullptr;

// Trampoline: saved original bytes + jump back to original+12
static uint8_t* Trampoline = nullptr;

// Saved original bytes
static uint8_t OriginalBytes[DETOUR_SIZE] = {0};

// ---- Forward declaration ----
static int WSAAPI MyConnect(SOCKET s, const struct sockaddr* name, int namelen);

// ---- Memory helpers ----
static uint8_t* AllocExecutable(size_t size) {
    return (uint8_t*)VirtualAlloc(nullptr, size, MEM_COMMIT | MEM_RESERVE,
                                   PAGE_EXECUTE_READWRITE);
}

static void FreeExecutable(void* p) {
    if (p) VirtualFree(p, 0, MEM_RELEASE);
}

// ---- Build detour bytes: mov rax, addr; jmp rax ----
static void BuildDetour(uint8_t* buf, void* target) {
    // mov rax, imm64:  48 B8 <8-byte-address>
    buf[0] = 0x48;
    buf[1] = 0xB8;
    memcpy(buf + 2, &target, sizeof(void*));
    // jmp rax: FF E0
    buf[10] = 0xFF;
    buf[11] = 0xE0;
}

// ---- Find connect() in ws2_32.dll ----
static ConnectFn FindConnect() {
    HMODULE hMod = GetModuleHandleW(L"ws2_32.dll");
    if (!hMod) {
        // ws2_32 might not be loaded yet; force load it
        hMod = LoadLibraryW(L"ws2_32.dll");
        if (!hMod) return nullptr;
    }
    return (ConnectFn)GetProcAddress(hMod, "connect");
}

// ---- Install the detour ----
bool InstallConnectHook() {
    InitializeCriticalSection(&g_hook_lock);

    ConnectFn connectAddr = FindConnect();
    if (!connectAddr) {
        return false;
    }

    RealConnect = connectAddr;

    // Read original 12 bytes
    memcpy(OriginalBytes, RealConnect, DETOUR_SIZE);

    // Allocate trampoline: saved bytes + jmp back to original+12
    Trampoline = AllocExecutable(DETOUR_SIZE + 14);
    if (!Trampoline) return false;

    // Copy original bytes to trampoline
    memcpy(Trampoline, OriginalBytes, DETOUR_SIZE);

    // Build: jmp [original + DETOUR_SIZE]
    //   FF 25 XX XX XX XX  (jmp qword ptr [rip+offset])
    uint8_t* jmpAddr = Trampoline + DETOUR_SIZE;
    uint8_t* targetAddr = (uint8_t*)RealConnect + DETOUR_SIZE;
    jmpAddr[0] = 0xFF;
    jmpAddr[1] = 0x25;
    // RIP-relative offset from jmpAddr+6 to targetAddr
    int32_t offset = (int32_t)(targetAddr - (jmpAddr + 6));
    memcpy(jmpAddr + 2, &offset, sizeof(int32_t));

    // Make original function writable
    DWORD oldProtect = 0;
    VirtualProtect((void*)RealConnect, DETOUR_SIZE, PAGE_EXECUTE_READWRITE, &oldProtect);

    // Write detour
    BuildDetour((uint8_t*)RealConnect, MyConnect);

    // Restore protection
    VirtualProtect((void*)RealConnect, DETOUR_SIZE, oldProtect, &oldProtect);

    // Flush instruction cache
    FlushInstructionCache(GetCurrentProcess(), (void*)RealConnect, DETOUR_SIZE);

    return true;
}

// ---- Remove the detour ----
void RemoveConnectHook() {
    if (!RealConnect) return;

    EnterCriticalSection(&g_hook_lock);

    // Restore original bytes
    DWORD oldProtect = 0;
    VirtualProtect((void*)RealConnect, DETOUR_SIZE, PAGE_EXECUTE_READWRITE, &oldProtect);
    memcpy((void*)RealConnect, OriginalBytes, DETOUR_SIZE);
    VirtualProtect((void*)RealConnect, DETOUR_SIZE, oldProtect, &oldProtect);
    FlushInstructionCache(GetCurrentProcess(), (void*)RealConnect, DETOUR_SIZE);

    FreeExecutable(Trampoline);
    Trampoline = nullptr;
    RealConnect = nullptr;

    LeaveCriticalSection(&g_hook_lock);
    DeleteCriticalSection(&g_hook_lock);
}

// ---- Set proxy config ----
void SetProxyConfig(const char* host, uint16_t port) {
    g_proxy_host = host;
    g_proxy_port = port;
}

// ---- Send CONNECT request and read response ----
static bool SendConnect(SOCKET s, const char* host, uint16_t port) {
    char req[512];
    int len = snprintf(req, sizeof(req),
        "CONNECT %s:%u HTTP/1.1\r\nHost: %s:%u\r\n\r\n",
        host, (unsigned)port, host, (unsigned)port);

    if (send(s, req, len, 0) == SOCKET_ERROR) {
        return false;
    }

    // Read response
    char resp[1024];
    int received = recv(s, resp, sizeof(resp) - 1, 0);
    if (received <= 0) return false;

    resp[received] = 0;

    // Check for "200 Connection Established"
    if (strstr(resp, "200") && strstr(resp, "Connection established")) {
        return true;
    }
    // Also accept just "200" in the first line
    if (strstr(resp, "HTTP/1.1 200") || strstr(resp, "HTTP/1.0 200")) {
        return true;
    }

    return false;
}

// ---- Hooked connect() ----
static int WSAAPI MyConnect(SOCKET s, const struct sockaddr* name, int namelen) {
    // Call original if hook is disabled or not an IPv4 address
    if (!g_hook_enabled || !name || name->sa_family != AF_INET) {
        return Trampoline ? ((ConnectFn)Trampoline)(s, name, namelen) :
                            RealConnect(s, name, namelen);
    }

    // Save original destination
    const sockaddr_in* orig = (const sockaddr_in*)name;
    char orig_ip[64] = {0};
    inet_ntop(AF_INET, &orig->sin_addr, orig_ip, sizeof(orig_ip));
    uint16_t orig_port = ntohs(orig->sin_port);

    // Build proxy address
    sockaddr_in proxy_addr = {};
    proxy_addr.sin_family = AF_INET;
    proxy_addr.sin_port = htons(g_proxy_port);
    inet_pton(AF_INET, g_proxy_host, &proxy_addr.sin_addr);

    // Call original connect() but to proxy instead
    int result = Trampoline ? ((ConnectFn)Trampoline)(s, (const sockaddr*)&proxy_addr, sizeof(proxy_addr)) :
                              RealConnect(s, (const sockaddr*)&proxy_addr, sizeof(proxy_addr));

    if (result != 0) {
        // Connection to proxy failed
        return result;
    }

    // Send CONNECT to establish tunnel
    if (!SendConnect(s, orig_ip, orig_port)) {
        // CONNECT failed — close socket
        closesocket(s);
        return SOCKET_ERROR;
    }

    return 0; // Success — socket is now tunneled through proxy
}