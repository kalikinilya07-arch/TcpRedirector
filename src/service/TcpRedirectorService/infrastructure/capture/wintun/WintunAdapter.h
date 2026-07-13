#pragma once

/**
 * @file WintunAdapter.h
 * @brief WP8 — RAII-обёртка над жизненным циклом Wintun-адаптера.
 *
 * НАЗНАЧЕНИЕ:
 *   Отделить «создать/открыть/закрыть» адаптера и «назначить IPv4» от
 *   всего остального (загрузка DLL — WintunApi, ринг данных —
 *   WintunSession, роутинг — WP9/WP10, packet pump — WP10).  Класс не
 *   знает ни про lwIP, ни про RuleEngine, ни про Logger — только
 *   тонкие вызовы функций из WintunApi + IP Helper.
 *
 * КОНТРАКТ RAII:
 *   • Успешный CreateOrOpen() → адаптер существует; деструктор его закроет
 *     (что для Create-варианта означает «удалить», по контракту wintun).
 *   • Провал CreateOrOpen() → unique_ptr пуст, *outError заполнен.
 *   • Экземпляр держит shared_ptr<WintunApi>, поэтому DLL не выгрузится
 *     раньше адаптера.
 *
 * WP8 не связан с сессией данных — Session живёт как отдельный объект
 * (WintunSession), которому передают WINTUN_ADAPTER_HANDLE.  Здесь только
 * управление адаптером и IP-конфигурация.
 */

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <ifdef.h>          // NET_LUID, NET_IFINDEX
// ВНИМАНИЕ: <netioapi.h> НЕ подключаем в заголовке — он транзитивно требует
// <winsock2.h> перед <windows.h>, а транзитивные потребители нашего заголовка
// (main.cpp, WinDivertCapture.h и т.д.) сами настраивают порядок включений.
// Всё, что нужно из netioapi (MIB_UNICASTIPADDRESS_ROW,
// ConvertInterfaceLuidToIndex), используется только в WintunAdapter.cpp.

#include <cstdint>
#include <memory>
#include <optional>
#include <string>

#include "WintunApi.h"

namespace tcp_redirector {
namespace infrastructure {
namespace capture {
namespace wintun {

/**
 * @brief RAII-владелец WINTUN_ADAPTER_HANDLE + утилиты IP-конфигурации.
 */
class WintunAdapter {
public:
    /**
     * @brief Параметры создания/открытия адаптера.
     *
     * Все Unicode-строки; передаются в wintun как есть.  requested_guid
     * рекомендуется задавать, чтобы Wintun сохранял стабильный GUID
     * между рестартами сервиса (иначе GUID случайный и попадает в
     * список сетевых адаптеров как новый интерфейс).
     */
    struct CreateParams {
        std::wstring name;                 //!< Отображаемое имя (макс. 128 wchar).
        std::wstring tunnel_type;          //!< Метка «типа туннеля» для wintun-энумерации.
        std::optional<GUID> requested_guid;//!< Опциональный стабильный GUID.
    };

    /**
     * @brief Создать новый Wintun-адаптер (или открыть существующий с тем же именем).
     *
     * Порядок:
     *   1) CreateAdapter(name, tunnel_type, requested_guid_or_null).
     *   2) Если провал с ERROR_ALREADY_EXISTS → OpenAdapter(name).
     *   3) Если оба провалились — вернуть nullptr, заполнить outError.
     *
     * После успеха вычисляется LUID через GetAdapterLUID.  Дальнейшая
     * настройка IP выполняется отдельным ConfigureIpv4().
     *
     * @param api       Загруженный WintunApi (не nullptr).
     * @param p         Параметры адаптера.
     * @param outError  [out, обязателен] Диагностика при провале.
     * @return unique_ptr на адаптер либо nullptr.
     */
    static std::unique_ptr<WintunAdapter> CreateOrOpen(
        std::shared_ptr<WintunApi> api,
        const CreateParams& p,
        std::string* outError);

    ~WintunAdapter();

    WintunAdapter(const WintunAdapter&) = delete;
    WintunAdapter& operator=(const WintunAdapter&) = delete;
    WintunAdapter(WintunAdapter&&) = delete;
    WintunAdapter& operator=(WintunAdapter&&) = delete;

    /**
     * @brief Handle, который принимают функции WintunApi::StartSession/GetAdapterLUID.
     *
     * Нельзя пропускать этот handle наружу дольше времени жизни WintunAdapter.
     */
    WINTUN_ADAPTER_HANDLE Handle() const { return m_handle; }

    /**
     * @brief LUID адаптера — стабильный сквозной идентификатор для IP Helper.
     */
    NET_LUID Luid() const { return m_luid; }

    /**
     * @brief Отображаемое имя адаптера.
     */
    const std::wstring& Name() const { return m_name; }

    /**
     * @brief Индекс интерфейса, получаемый из LUID.
     * @return 0, если ConvertInterfaceLuidToIndex провалился (маловероятно).
     */
    NET_IFINDEX InterfaceIndex() const;

    /**
     * @brief Назначить IPv4-адрес и маску адаптеру.
     *
     * Разбирает "a.b.c.d/N", вызывает InitializeUnicastIpAddressEntry +
     * CreateUnicastIpAddressEntry; если запись уже есть
     * (ERROR_OBJECT_ALREADY_EXISTS) — пытается SetUnicastIpAddressEntry.
     *
     * @param cidr      Строка "a.b.c.d/N", N ∈ [0..32].
     * @param outError  [out, обязателен] Диагностика при провале.
     * @return true при успехе, false — outError содержит подробности.
     */
    bool ConfigureIpv4(const std::wstring& cidr, std::string* outError);

    /**
     * @brief Ссылка на API (для WintunSession — общий владелец DLL).
     */
    const std::shared_ptr<WintunApi>& Api() const { return m_api; }

private:
    WintunAdapter() = default;

    // Помощник CreateOrOpen: разбор "a.b.c.d/N" в поля.  Возвращает false
    // при синтаксической ошибке.  Не общий утилитарный namespace, потому
    // что его больше никто не использует.
    static bool ParseCidrV4(const std::wstring& cidr, uint32_t& ipBigEndian,
                             uint8_t& prefixOut);

    std::shared_ptr<WintunApi> m_api;
    WINTUN_ADAPTER_HANDLE       m_handle = nullptr;
    NET_LUID                    m_luid{};
    std::wstring                m_name;
};

} // namespace wintun
} // namespace capture
} // namespace infrastructure
} // namespace tcp_redirector
