// mock_proxy.cpp
// Minimal TCP/UDP test proxy server for integration testing.
// Logs all activity to stdout AND optionally to a log file.
// Compilation: g++ mock_proxy.cpp -o mock_proxy.exe -lws2_32
// Usage: mock_proxy.exe --port 3128 --protocol tcp [--log proxy_log.txt]

#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>

#pragma comment(lib, "ws2_32.lib")

static volatile bool g_running = true;
static FILE* g_logFile = NULL;

static void Log(const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    vprintf(fmt, args);
    va_end(args);
    if (g_logFile) {
        va_start(args, fmt);
        vfprintf(g_logFile, fmt, args);
        va_end(args);
        fflush(g_logFile);
    }
}

BOOL WINAPI CtrlHandler(DWORD ctrlType) {
    (void)ctrlType;
    Log("\n[MOCK_PROXY] Ctrl+C received. Shutting down...\n");
    g_running = false;
    return TRUE;
}

static void HandleClient(SOCKET client_sock, struct sockaddr_in* client_addr, int client_id) {
    char client_ip[64];
    snprintf(client_ip, sizeof(client_ip), "%s", inet_ntoa(client_addr->sin_addr));

    Log("[MOCK_PROXY] --- Client #%d connected: %s:%d ---\n",
        client_id, client_ip, ntohs(client_addr->sin_port));

    char buf[4096];
    int total_recv = 0;

    while (g_running) {
        int timeout_ms = 5000;
        setsockopt(client_sock, SOL_SOCKET, SO_RCVTIMEO,
                   (const char*)&timeout_ms, sizeof(timeout_ms));

        int bytes = recv(client_sock, buf, sizeof(buf) - 1, 0);
        if (bytes > 0) {
            buf[bytes] = '\0';
            total_recv += bytes;
            int show_len = (bytes < 200) ? bytes : 200;
            Log("[MOCK_PROXY] Client #%d -> received %d bytes: %.*s\n",
                client_id, bytes, show_len, buf);

            const char* response = "HTTP/1.1 200 Connection established\r\n\r\n";
            send(client_sock, response, (int)strlen(response), 0);
            Log("[MOCK_PROXY] Client #%d <- sent CONNECT response\n", client_id);

        } else if (bytes == 0) {
            Log("[MOCK_PROXY] Client #%d disconnected (closed connection)\n", client_id);
            break;
        } else {
            int err = WSAGetLastError();
            if (err == WSAETIMEDOUT) {
                Log("[MOCK_PROXY] Client #%d recv timeout (%ds), total: %d bytes\n",
                    client_id, timeout_ms / 1000, total_recv);
                break;
            } else if (err == WSAECONNRESET) {
                Log("[MOCK_PROXY] Client #%d connection reset (RST)\n", client_id);
                break;
            } else {
                Log("[MOCK_PROXY] Client #%d recv error: %d\n", client_id, err);
                break;
            }
        }
    }

    closesocket(client_sock);
    Log("[MOCK_PROXY] --- Client #%d disconnected (total received: %d bytes) ---\n",
        client_id, total_recv);
}

int main(int argc, char* argv[]) {
    int port = 3128;
    const char* protocol = "tcp";
    const char* log_path = NULL;

    for (int i = 1; i < argc; i++) {
        if (i + 1 < argc && strcmp(argv[i], "--port") == 0) {
            port = atoi(argv[++i]);
        } else if (i + 1 < argc && strcmp(argv[i], "--protocol") == 0) {
            protocol = argv[++i];
        } else if (i + 1 < argc && strcmp(argv[i], "--log") == 0) {
            log_path = argv[++i];
        } else {
            fprintf(stderr, "Unknown arg: %s\n", argv[i]);
            fprintf(stderr, "Usage: mock_proxy.exe --port PORT --protocol tcp|udp [--log FILE]\n");
            return 1;
        }
    }

    if (port <= 0 || port > 65535) {
        fprintf(stderr, "ERROR: Invalid port: %d\n", port);
        return 1;
    }

    // Open log file if specified
    if (log_path) {
        g_logFile = fopen(log_path, "w");
        if (g_logFile) {
            fprintf(stdout, "[MOCK_PROXY] Logging to file: %s\n", log_path);
        } else {
            fprintf(stderr, "[MOCK_PROXY] Failed to open log file: %s\n", log_path);
        }
    }

    SetConsoleCtrlHandler(CtrlHandler, TRUE);

    WSADATA wsaData;
    if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0) {
        fprintf(stderr, "ERROR: WSAStartup (err=%d)\n", WSAGetLastError());
        return 1;
    }

    bool is_tcp = (strcmp(protocol, "tcp") == 0);

    Log("============================================================\n");
    Log("  MOCK_PROXY (test stub)\n");
    Log("  Listening: 0.0.0.0:%d, protocol: %s\n", port, protocol);
    Log("  Press Ctrl+C to exit\n");
    Log("============================================================\n");

    SOCKET listen_sock = socket(AF_INET, is_tcp ? SOCK_STREAM : SOCK_DGRAM, 0);
    if (listen_sock == INVALID_SOCKET) {
        fprintf(stderr, "ERROR: socket() (err=%d)\n", WSAGetLastError());
        WSACleanup();
        return 1;
    }

    int reuse = 1;
    setsockopt(listen_sock, SOL_SOCKET, SO_REUSEADDR, (const char*)&reuse, sizeof(reuse));

    struct sockaddr_in addr;
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons((u_short)port);

    if (bind(listen_sock, (struct sockaddr*)&addr, sizeof(addr)) == SOCKET_ERROR) {
        fprintf(stderr, "ERROR: bind() port %d (err=%d) - maybe port in use\n",
                port, WSAGetLastError());
        closesocket(listen_sock);
        WSACleanup();
        return 1;
    }

    if (is_tcp) {
        if (listen(listen_sock, SOMAXCONN) == SOCKET_ERROR) {
            fprintf(stderr, "ERROR: listen() (err=%d)\n", WSAGetLastError());
            closesocket(listen_sock);
            WSACleanup();
            return 1;
        }

        Log("[MOCK_PROXY] TCP server started on port %d. Waiting for connections...\n", port);

        int client_id = 0;
        while (g_running) {
            fd_set read_fds;
            FD_ZERO(&read_fds);
            FD_SET(listen_sock, &read_fds);
            struct timeval tv = {1, 0};

            int sel = select(0, &read_fds, NULL, NULL, &tv);
            if (sel == SOCKET_ERROR) {
                fprintf(stderr, "ERROR: select() (err=%d)\n", WSAGetLastError());
                break;
            }
            if (sel == 0) continue;

            struct sockaddr_in client_addr;
            int addr_len = sizeof(client_addr);
            SOCKET client_sock = accept(listen_sock, (struct sockaddr*)&client_addr, &addr_len);
            if (client_sock == INVALID_SOCKET) {
                int err = WSAGetLastError();
                if (err != WSAEINTR) {
                    fprintf(stderr, "ERROR: accept() (err=%d)\n", err);
                }
                continue;
            }

            client_id++;
            HandleClient(client_sock, &client_addr, client_id);
        }
    } else {
        Log("[MOCK_PROXY] UDP server started on port %d. Waiting for datagrams...\n", port);

        char buf[65535];
        while (g_running) {
            int timeout_ms = 1000;
            setsockopt(listen_sock, SOL_SOCKET, SO_RCVTIMEO,
                       (const char*)&timeout_ms, sizeof(timeout_ms));

            struct sockaddr_in client_addr;
            int addr_len = sizeof(client_addr);
            int bytes = recvfrom(listen_sock, buf, sizeof(buf) - 1, 0,
                                 (struct sockaddr*)&client_addr, &addr_len);
            if (bytes > 0) {
                buf[bytes] = '\0';
                Log("[MOCK_PROXY] UDP packet from %s:%d (%d bytes): %.*s\n",
                    inet_ntoa(client_addr.sin_addr),
                    ntohs(client_addr.sin_port),
                    bytes, (bytes < 200) ? bytes : 200, buf);
            } else if (bytes == SOCKET_ERROR) {
                int err = WSAGetLastError();
                if (err != WSAETIMEDOUT) {
                    fprintf(stderr, "ERROR: recvfrom() (err=%d)\n", err);
                    break;
                }
            }
        }
    }

    closesocket(listen_sock);
    WSACleanup();

    if (g_logFile) {
        fclose(g_logFile);
    }

    Log("[MOCK_PROXY] Finished.\n");
    return 0;
}