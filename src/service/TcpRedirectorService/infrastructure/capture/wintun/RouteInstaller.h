#pragma once

/**
 * @file RouteInstaller.h
 * @brief WP10 — RAII-хелпер для установки split-tunnel маршрутов на Wintun-адаптер.
 *
 * НАЗНАЧЕНИЕ:
 *   Устанавливает пару маршрутов 0.0.0.0/1 и 128.0.0.0/1, которые «перекрывают»
 *   маршрут по умолчанию (0.0.0.0/0) без его удаления — стандартный
 *   split-tunnel трюк WireGuard. При Close() маршруты удаляются.
 *
 *   Используется IP Helper (netioapi):
 *     • InitializeIpForwardEntry(&row)
 *     • row.InterfaceLuid = luid; row.DestinationPrefix = X/1; row.NextHop = gw;
 *     • row.Protocol = MIB_IPPROTO_NETMGMT; row.Metric = <низкий>
 *     • CreateIpForwardEntry2(&row) — ERROR_OBJECT_ALREADY_EXISTS трактуется как success.
 *     • DeleteIpForwardEntry2(&row) — ERROR_NOT_FOUND игнорируется.
 *
 * CRASH-SAFETY:
 *   RouteScope — RAII-обёртка, которая гарантирует Uninstall на выходе из
 *   области видимости, если её не «разоружить» через Release(). Внутри
 *   WintunCapture это защищает от orphan-маршрутов при исключении между
 *   InstallSplitTunnel() и завершением Open().
 *
 *   Маршруты создаются с протоколом MIB_IPPROTO_NETMGMT: они не переживают
 *   перезагрузку и не попадают в persistent route table.
 */

#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>       // sockaddr_in — тянется netioapi.h транзитивно.
#include <windows.h>
#include <ifdef.h>          // NET_LUID
// <netioapi.h> НЕ включаем в заголовке: он требует правильного порядка
// winsock2/windows.h у всех транзитивных потребителей.  Всё, что нужно
// (MIB_IPFORWARD_ROW2, CreateIpForwardEntry2), используется только в .cpp.

#include <cstdint>
#include <string>

namespace tcp_redirector {
namespace infrastructure {
namespace capture {
namespace wintun {

/**
 * @brief Ставит/снимает split-tunnel маршруты через один Wintun-адаптер.
 *
 * Все методы статичны — состояние не требуется, идентификатор адаптера
 * приходит параметром (LUID).  Для управления временем жизни маршрутов
 * см. RouteScope ниже.
 */
class RouteInstaller {
public:
    /**
     * @brief Установить пару маршрутов 0.0.0.0/1 и 128.0.0.0/1 через указанный
     *        адаптер.
     *
     * @param luid          LUID Wintun-адаптера (из WintunAdapter::Luid()).
     * @param next_hop_ipv4 Строка вида "10.6.7.1" — IP-шлюз внутри туннеля.
     *                      Совпадает с IP-адресом самого TUN-интерфейса (host часть CIDR).
     * @param metric        Метрика для маршрута.  Должна быть НИЖЕ, чем у
     *                      действующего дефолт-маршрута через физический интерфейс,
     *                      чтобы наши /1 маршруты выиграли выборку.  Рекомендуемое
     *                      значение — 1..5.  §6.8 плана называет 4; мы даём вызывающему
     *                      выбирать.
     * @param outError      [out, обязателен] Диагностика при провале.
     * @return true при полной установке ОБОИХ маршрутов.  При частичном провале
     *         (например, первый прошёл, второй сломался) уже поставленный
     *         маршрут откатывается перед возвратом false.
     */
    static bool InstallSplitTunnel(NET_LUID luid,
                                   const std::wstring& next_hop_ipv4,
                                   uint32_t metric,
                                   std::string* outError);

    /**
     * @brief Удалить пару маршрутов 0.0.0.0/1 и 128.0.0.0/1 на указанном LUID.
     *
     * ERROR_NOT_FOUND трактуется как success (маршрут уже отсутствует).
     * Если одно из удалений вернуло другую ошибку — второй маршрут всё равно
     * пробуется удалить, чтобы минимизировать шанс на orphan-маршрут.
     *
     * @param luid      LUID Wintun-адаптера.
     * @param outError  [out, опционально] Диагностика при провале.  Может быть nullptr.
     * @return true, если оба маршрута удалены (или отсутствовали).
     */
    static bool UninstallSplitTunnel(NET_LUID luid,
                                     std::string* outError);

    /**
     * @brief Только для диагностики: проверяет, установлен ли хотя бы один из
     *        двух /1 маршрутов на данном LUID.
     *
     * Не используется production-кодом; предназначен для юнит-тестов и
     * IPC-статусной диагностики.
     */
    static bool IsInstalled(NET_LUID luid);

    /**
     * @brief Установить host-bypass маршрут /32 к заданному IPv4 через
     *        СУЩЕСТВУЮЩИЙ (физический) путь, минуя Wintun-туннель.
     *
     * ЗАЧЕМ: split-tunnel маршруты 0.0.0.0/1 + 128.0.0.0/1 захватывают ВЕСЬ
     * IPv4-трафик, включая исходящее соединение relay'я к вышестоящему
     * прокси.  Без исключения это соединение снова заворачивается в туннель →
     * петля, и удалённый прокси недостижим.  /32-маршрут «более специфичен»,
     * чем /1, поэтому всегда выигрывает выборку и уводит трафик к прокси на
     * физический интерфейс.
     *
     * ВАЖНО: вызывать ДО InstallSplitTunnel — иначе GetBestRoute2 вернёт наш
     * же туннельный /1 как «лучший» маршрут.
     *
     * @param dst_ipv4_be IPv4 назначения (network byte order) — IP прокси.
     * @param outError     [out, опционально] диагностика.
     * @return true при успехе (или если bypass не требуется — loopback).
     */
    static bool InstallHostBypass(uint32_t dst_ipv4_be, std::string* outError);

    /**
     * @brief Снять host-bypass маршрут /32, установленный InstallHostBypass.
     * @param dst_ipv4_be IPv4 назначения (network byte order).
     * @param outError     [out, опционально] диагностика.
     */
    static bool UninstallHostBypass(uint32_t dst_ipv4_be, std::string* outError);
};

/**
 * @brief RAII-обёртка: держит признак «маршруты установлены» и вызывает
 *        UninstallSplitTunnel в деструкторе, если Release() не был вызван.
 *
 * Использование внутри WintunCapture::Open():
 *
 *     RouteScope routes(luid);
 *     if (!RouteInstaller::InstallSplitTunnel(luid, gw, 1, &err)) return false;
 *     routes.MarkInstalled();
 *     ...
 *     if (dependent_step_failed) return false;   // ~RouteScope снимет маршруты
 *     routes.Release();                           // передача владения WintunCapture
 */
class RouteScope {
public:
    explicit RouteScope(NET_LUID luid) : m_luid(luid) {}

    ~RouteScope() {
        if (m_installed) {
            std::string err;
            (void)RouteInstaller::UninstallSplitTunnel(m_luid, &err);
        }
    }

    RouteScope(const RouteScope&) = delete;
    RouteScope& operator=(const RouteScope&) = delete;
    RouteScope(RouteScope&&) = delete;
    RouteScope& operator=(RouteScope&&) = delete;

    /// Отметить, что маршруты успешно установлены — деструктор их снимет.
    void MarkInstalled() { m_installed = true; }

    /// Передать владение вовне — деструктор ничего делать не будет.
    void Release() { m_installed = false; }

    /// Установлены ли маршруты в момент запроса.
    bool IsInstalled() const { return m_installed; }

private:
    NET_LUID m_luid;
    bool     m_installed = false;
};

} // namespace wintun
} // namespace capture
} // namespace infrastructure
} // namespace tcp_redirector
