#pragma once

/**
 * @file auth_sspi.h
 * @brief Модуль SSPI-аутентификации (Negotiate/Kerberos) для HTTP-прокси.
 *
 * Предоставляет функции для получения токенов Negotiate через SSPI
 * с использованием учётной записи текущего пользователя Windows.
 * Не зависит от WinDivert или других компонентов перехвата.
 *
 * Используется в TcpRelayServer для заголовка Proxy-Authorization: Negotiate.
 *
 * Разработчик: Kalikin Iliya
 */

#define WIN32_LEAN_AND_MEAN
#define SECURITY_WIN32
#include <windows.h>
#include <security.h>
#include <sspi.h>
#include <string>
#include <vector>
#include <cstdint>

#pragma comment(lib, "secur32.lib")

namespace tcp_redirector {
namespace infrastructure {

/**
 * @brief Код результата операции SSPI.
 */
enum class SspiResult {
    Success,        ///< Токен получен, продолжаем
    Complete,       ///< Аутентификация завершена
    NeedContinue,   ///< Требуется ещё один цикл (сервер прислал 407 с новым challenge)
    NoCredentials,  ///< Kerberos-билет не найден / SSPI недоступен
    Error           ///< Внутренняя ошибка SSPI
};

/**
 * @brief Контекст SSPI-сессии между вызовами.
 *
 * Хранит дескрипторы credentials и security context.
 * Создаётся один раз на соединение, переиспользуется при повторных вызовах.
 */
struct SspiContext {
    CredHandle    credentials{0, 0};  ///< Дескриптор учётных данных
    CtxtHandle    context{0, 0};      ///< Дескриптор контекста безопасности
    bool          initialized = false; ///< Флаг инициализации
    std::string   targetSpn;          ///< SPN для целевого прокси (HTTP/host)

    ~SspiContext() { Release(); }

    /**
     * @brief Освободить все дескрипторы SSPI.
     * Безопасен для многократного вызова.
     */
    void Release();
};

/**
 * @brief Выполнить SSPI-цикл Negotiate.
 *
 * Первый вызов (с пустым serverToken) получает начальный токен
 * для заголовка Proxy-Authorization: Negotiate <base64>.
 *
 * Последующие вызовы (с serverToken из заголовка WWW-Authenticate: Negotiate)
 * обрабатывают challenge от сервера и возвращают следующий токен.
 *
 * @param ctx           Контекст SSPI (создаётся при первом вызове)
 * @param serverToken   Токен от сервера (base64), пустая строка при первом вызове
 * @param outToken      [out] Полученный токен (base64) для заголовка Proxy-Authorization
 * @param spn           SPN для целевого хоста (HTTP/proxy.example.com)
 * @return SspiResult   Результат операции
 */
SspiResult SspiNegotiate(SspiContext& ctx,
                         const std::string& serverToken,
                         std::string& outToken,
                         const std::string& spn);

/**
 * @brief Разобрать HTTP-ответ 407 и извлечь значение challenge.
 *
 * Ищет заголовок Proxy-Authenticate: Negotiate <token>,
 * возвращает сам токен (base64). Если заголовка нет — пустая строка.
 *
 * @param responseBody  Тело/заголовки HTTP-ответа от прокси
 * @return std::string  Токен challenge (base64) или пустая строка
 */
std::string Parse407Challenge(const std::string& responseBody);

/**
 * @brief Получить SPN для SSPI на основе хоста прокси.
 *
 * Формирует строку "HTTP/<host>" в ASCII.
 *
 * @param proxyHost  Хост прокси-сервера
 * @return std::string SPN-строка
 */
std::string MakeSpn(const std::string& proxyHost);

/**
 * @brief Утилита: Base64-кодирование бинарных данных.
 */
std::string SspiBase64Encode(const std::vector<uint8_t>& data);

/**
 * @brief Утилита: Base64-декодирование строки.
 */
std::vector<uint8_t> SspiBase64Decode(const std::string& data);

/**
 * @brief Получить текстовое описание ошибки SSPI.
 * @param sc  SECURITY_STATUS
 * @return Строка с описанием ошибки
 */
std::string SspiErrorText(SECURITY_STATUS sc);

} // namespace infrastructure
} // namespace tcp_redirector