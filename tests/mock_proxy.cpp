// mock_proxy.cpp
// Изолированная тестовая заглушка — минимальный TCP/UDP сервер-прокси.
// Не имеет зависимостей от основного проекта TcpRedirector.
// Компиляция: cl.exe mock_proxy.cpp /Fe:mock_proxy.exe /link ws2_32.lib
// Использование: mock_proxy.exe --port 3128 --protocol tcp

#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#pragma comment(lib, "ws2_32.lib")

static volatile bool g_running = true;

BOOL WINAPI CtrlHandler(DWORD ctrlType) {
    (void)ctrlType;
    printf("\n[MOCK_PROXY] Получен сигнал остановки. Завершение...\n");
    g_running = false;
    return TRUE;
}

static void HandleClient(SOCKET client_sock, struct sockaddr_in* client_addr, int client_id) {
    char client_ip[64];
    snprintf(client_ip, sizeof(client_ip), "%s", inet_ntoa(client_addr->sin_addr));

    printf("[MOCK_PROXY] --- Клиент #%d подключён: %s:%d ---\n",
           client_id, client_ip, ntohs(client_addr->sin_port));

    char buf[4096];
    int total_recv = 0;

    // Читаем данные от клиента
    while (g_running) {
        int timeout_ms = 5000;
        setsockopt(client_sock, SOL_SOCKET, SO_RCVTIMEO, (const char*)&timeout_ms, sizeof(timeout_ms));

        int bytes = recv(client_sock, buf, sizeof(buf) - 1, 0);
        if (bytes > 0) {
            buf[bytes] = '\0';
            total_recv += bytes;

            // Показываем первые 200 символов
            int show_len = (bytes < 200) ? bytes : 200;
            printf("[MOCK_PROXY] Клиент #%d -> получено %d байт: %.*s\n",
                   client_id, bytes, show_len, buf);

            // Отправляем ответ (имитация CONNECT-ответа от прокси)
            const char* response =
                "HTTP/1.1 200 Connection established\r\n\r\n";
            send(client_sock, response, (int)strlen(response), 0);
            printf("[MOCK_PROXY] Клиент #%d <- отправлен CONNECT-ответ\n", client_id);

            // Продолжаем чтение (туннельный режим)
        } else if (bytes == 0) {
            printf("[MOCK_PROXY] Клиент #%d отключился (закрыл соединение)\n", client_id);
            break;
        } else {
            int err = WSAGetLastError();
            if (err == WSAETIMEDOUT) {
                printf("[MOCK_PROXY] Клиент #%d таймаут чтения (%d сек), всего получено: %d байт\n",
                       client_id, timeout_ms / 1000, total_recv);
                break;
            } else if (err == WSAECONNRESET) {
                printf("[MOCK_PROXY] Клиент #%d сбросил соединение (RST)\n", client_id);
                break;
            } else {
                printf("[MOCK_PROXY] Клиент #%d ошибка recv: %d\n", client_id, err);
                break;
            }
        }
    }

    closesocket(client_sock);
    printf("[MOCK_PROXY] --- Клиент #%d отключён (получено всего: %d байт) ---\n",
           client_id, total_recv);
}

int main(int argc, char* argv[]) {
    int port = 3128;
    const char* protocol = "tcp";

    // Парсинг аргументов
    for (int i = 1; i < argc; i++) {
        if (i + 1 < argc && strcmp(argv[i], "--port") == 0) {
            port = atoi(argv[++i]);
        } else if (i + 1 < argc && strcmp(argv[i], "--protocol") == 0) {
            protocol = argv[++i];
        } else {
            fprintf(stderr, "Неизвестный аргумент: %s\n", argv[i]);
            fprintf(stderr, "Использование: mock_proxy.exe --port PORT --protocol tcp|udp\n");
            return 1;
        }
    }

    if (port <= 0 || port > 65535) {
        fprintf(stderr, "ОШИБКА: Некорректный порт: %d\n", port);
        return 1;
    }

    // Регистрация обработчика Ctrl+C
    SetConsoleCtrlHandler(CtrlHandler, TRUE);

    // Инициализация Winsock
    WSADATA wsaData;
    if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0) {
        fprintf(stderr, "ОШИБКА: WSAStartup (err=%d)\n", WSAGetLastError());
        return 1;
    }

    bool is_tcp = (strcmp(protocol, "tcp") == 0);

    printf("============================================================\n");
    printf("  MOCK_PROXY (тестовая заглушка)\n");
    printf("  Слушаем: 0.0.0.0:%d, протокол: %s\n", port, protocol);
    printf("  Нажмите Ctrl+C для выхода\n");
    printf("============================================================\n");

    SOCKET listen_sock = socket(AF_INET, is_tcp ? SOCK_STREAM : SOCK_DGRAM, 0);
    if (listen_sock == INVALID_SOCKET) {
        fprintf(stderr, "ОШИБКА: socket() (err=%d)\n", WSAGetLastError());
        WSACleanup();
        return 1;
    }

    // Разрешить переиспользование порта для быстрого перезапуска
    int reuse = 1;
    setsockopt(listen_sock, SOL_SOCKET, SO_REUSEADDR, (const char*)&reuse, sizeof(reuse));

    struct sockaddr_in addr;
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons((u_short)port);

    if (bind(listen_sock, (struct sockaddr*)&addr, sizeof(addr)) == SOCKET_ERROR) {
        fprintf(stderr, "ОШИБКА: bind() порт %d (err=%d) — возможно порт занят\n",
                port, WSAGetLastError());
        closesocket(listen_sock);
        WSACleanup();
        return 1;
    }

    if (is_tcp) {
        if (listen(listen_sock, SOMAXCONN) == SOCKET_ERROR) {
            fprintf(stderr, "ОШИБКА: listen() (err=%d)\n", WSAGetLastError());
            closesocket(listen_sock);
            WSACleanup();
            return 1;
        }

        printf("[MOCK_PROXY] TCP-сервер запущен на порту %d. Ожидание подключений...\n", port);

        int client_id = 0;
        while (g_running) {
            // Настройка таймаута accept с помощью select
            fd_set read_fds;
            FD_ZERO(&read_fds);
            FD_SET(listen_sock, &read_fds);
            struct timeval tv = {1, 0}; // 1 сек таймаут

            int sel = select(0, &read_fds, NULL, NULL, &tv);
            if (sel == SOCKET_ERROR) {
                fprintf(stderr, "ОШИБКА: select() (err=%d)\n", WSAGetLastError());
                break;
            }
            if (sel == 0) continue; // Таймаут — проверяем g_running

            struct sockaddr_in client_addr;
            int addr_len = sizeof(client_addr);
            SOCKET client_sock = accept(listen_sock, (struct sockaddr*)&client_addr, &addr_len);
            if (client_sock == INVALID_SOCKET) {
                int err = WSAGetLastError();
                if (err != WSAEINTR) {
                    fprintf(stderr, "ОШИБКА: accept() (err=%d)\n", err);
                }
                continue;
            }

            client_id++;
            HandleClient(client_sock, &client_addr, client_id);
        }
    } else {
        // UDP
        printf("[MOCK_PROXY] UDP-сервер запущен на порту %d. Ожидание дейтаграмм...\n", port);

        char buf[65535];
        while (g_running) {
            int timeout_ms = 1000;
            setsockopt(listen_sock, SOL_SOCKET, SO_RCVTIMEO, (const char*)&timeout_ms, sizeof(timeout_ms));

            struct sockaddr_in client_addr;
            int addr_len = sizeof(client_addr);
            int bytes = recvfrom(listen_sock, buf, sizeof(buf) - 1, 0,
                                (struct sockaddr*)&client_addr, &addr_len);
            if (bytes > 0) {
                buf[bytes] = '\0';
                printf("[MOCK_PROXY] UDP-пакет от %s:%d (%d байт): %.*s\n",
                       inet_ntoa(client_addr.sin_addr),
                       ntohs(client_addr.sin_port),
                       bytes, min(bytes, 200), buf);
            } else if (bytes == SOCKET_ERROR) {
                int err = WSAGetLastError();
                if (err != WSAETIMEDOUT) {
                    fprintf(stderr, "ОШИБКА: recvfrom() (err=%d)\n", err);
                    break;
                }
            }
        }
    }

    closesocket(listen_sock);
    WSACleanup();

    printf("[MOCK_PROXY] Завершён.\n");
    return 0;
}