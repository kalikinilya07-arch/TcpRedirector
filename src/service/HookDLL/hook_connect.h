#pragma once

//
// Hook for ws2_32.dll!connect() — redirects target process TCP connections
// through an upstream HTTP proxy using the CONNECT method.
//
// On x64, installs a 12-byte JMP detour:
//   mov rax, addr; jmp rax
//
// Original bytes are saved in a trampoline for calling the real connect().
//

#include <windows.h>
#include <ws2tcpip.h>
#include <cstdint>

#ifdef __cplusplus
extern "C" {
#endif

// Install the connect() hook. Called from DllMain worker thread.
// Returns true on success.
bool InstallConnectHook();

// Remove the connect() hook and restore original bytes.
void RemoveConnectHook();

// Get the proxy address to redirect to. Set via config.
void SetProxyConfig(const char* host, uint16_t port);

#ifdef __cplusplus
}
#endif