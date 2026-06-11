// packet_generator.cpp
// Изолированная тестовая заглушка. Не имеет зависимостей от основного проекта.
// Компиляция: g++ packet_generator.cpp -o packet_generator.exe -lws2_32
// Использование: packet_generator.exe --dest_ip 127.0.0.1 --dest_port 3128 --protocol tcp --count 5

#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#pragma comment(lib, "ws2_32.lib")

int main(int argc, char* argv[]) {
    const char* dest_ip = "127.0.0.1";
    int dest_port = 3128;
    const char* protocol = "tcp";
    int count = 1;

    // Парсинг аргументов командной строки
    for (int i = 1; i < argc; i++) {
        if (i + 1 < argc && strcmp(argv[i], "--dest_ip") == 0) {
            dest_ip = argv[++i];
        } else if (i + 1 < argc && strcmp(argv[i], "--dest_port") == 0) {
            dest_port = atoi(argv[++i]);
        } else if (i + 1 < argc && strcmp(argv[i], "--protocol") == 0) {
            protocol = argv[++i];
        } else if (i + 1 < argc && strcmp(argv[i], "--count") == 0) {
            count = atoi(argv[++i]);
        } else {
            fprintf(stderr, "Неизвестный аргумент: %s\n", argv[i]);
            fprintf(stderr, "Использование: packet_generator.exe --dest_ip IP --dest_port PORT --protocol tcp|udp --count N\n");
            return 1;
        }
    }

    if (dest_port <= 0 || dest_port > 65535) {
        fprintf(stderr, "ОШИБКА: Некорректный порт %d\n", dest_port);
        return 1;
    }
    if (count <= 0 || count > 10000) {
        fprintf(stderr, "ОШИБКА: Некорректное количество %d (допустимо 1-10000)\n", count);
        return 1;
    }

    // Инициализация Winsock
    WSADATA wsaData;
    if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0) {
        fprintf(stderr, "ОШИБКА: WSAStartup не удалась (err=%d)\n", WSAGetLastError());
        return 1;
    }

    // Разрешение целевого хоста
    struct addrinfo hints = {0}, *addrResult = NULL;
    hints.ai_family = AF_INET;
    hints.ai_socktype = (strcmp(protocol, "udp") == 0) ? SOCK_DGRAM : SOCK_STREAM;
    hints.ai_protocol = (strcmp(protocol, "udp") == 0) ? IPPROTO_UDP : IPPROTO_TCP;

    char port_str[16];
    snprintf(port_str, sizeof(port_str), "%d", dest_port);

    int rc = getaddrinfo(dest_ip, port_str, &hints, &addrResult);
    if (rc != 0 || !addrResult) {
        fprintf(stderr, "ОШИБКА: Не удалось разрешить %s (err=%d)\n", dest_ip, rc);
        WSACleanup();
        return 1;
    }

    bool is_tcp = (strcmp(protocol, "tcp") == 0);
    int sent_count = 0;

    printf("[PACKET_GEN] Назначение: %s:%d, протокол: %s, количество: %d\n",
           dest_ip, dest_port, protocol, count);

    for (int i = 0; i < count; i++) {
        // Создание сокета
        SOCKET sock = socket(addrResult->ai_family, addrResult->ai_socktype, addrResult->ai_protocol);
        if (sock == INVALID_SOCKET) {
            fprintf(stderr, "ОШИБКА: socket() не удался (err=%d)\n", WSAGetLastError());
            continue;
        }

        // Формирование payload: номер пакета + метка времени
        char packet_data[256];
        int data_len = snprintf(packet_data, sizeof(packet_data),
            "PACKET #%d FROM packet_generator TIME=%llu",
            i + 1, (unsigned long long)GetTickCount64());

        if (is_tcp) {
            // TCP: connect + send + close
            if (connect(sock, addrResult->ai_addr, (int)addrResult->ai_addrlen) == SOCKET_ERROR) {
                int err = WSAGetLastError();
                // WSAECONNREFUSED — ожидаемо, если порт закрыт
                if (err == WSAECONNREFUSED) {
                    printf("[PACKET_GEN] TCP-соединение отклонено (порт %d закрыт)\n", dest_port);
                } else {
                    fprintf(stderr, "ОШИБКА: connect() не удался (err=%d)\n", err);
                }
                closesocket(sock);
                continue;
            }

            int bytes_sent = send(sock, packet_data, data_len, 0);
            if (bytes_sent == SOCKET_ERROR) {
                fprintf(stderr, "ОШИБКА: send() не удался (err=%d)\n", WSAGetLastError());
            } else {
                sent_count++;
                printf("[PACKET_GEN] Отправлен пакет #%d на %s:%d (%d байт)\n",
                       i + 1, dest_ip, dest_port, bytes_sent);
            }

            // Ожидание ответа (с таймаутом 1 сек)
            char recv_buf[1024];
            int timeout = 1000;
            setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, (const char*)&timeout, sizeof(timeout));
            int bytes_recv = recv(sock, recv_buf, sizeof(recv_buf) - 1, 0);
            if (bytes_recv > 0) {
                recv_buf[bytes_recv] = '\0';
                printf("[PACKET_GEN] Получен ответ на пакет #%d: %.*s\n",
                       i + 1, bytes_recv < 100 ? bytes_recv : 100, recv_buf);
            }

            closesocket(sock);

        } else {
            // UDP: sendto без установки соединения
            int bytes_sent = sendto(sock, packet_data, data_len, 0,
                                    addrResult->ai_addr, (int)addrResult->ai_addrlen);
            if (bytes_sent == SOCKET_ERROR) {
                fprintf(stderr, "ОШИБКА: sendto() не удался (err=%d)\n", WSAGetLastError());
            } else {
                sent_count++;
                printf("[PACKET_GEN] Отправлен пакет #%d на %s:%d (%d байт)\n",
                       i + 1, dest_ip, dest_port, bytes_sent);
            }
            closesocket(sock);
        }

        // Небольшая пауза между пакетами
        if (i < count - 1) Sleep(100);
    }

    freeaddrinfo(addrResult);
    WSACleanup();

    printf("[PACKET_GEN] Отправлено: %d/%d пакетов\n", sent_count, count);
    return (sent_count > 0) ? 0 : 1;
}
