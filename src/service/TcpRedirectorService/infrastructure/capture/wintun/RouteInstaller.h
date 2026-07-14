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
#include <vector>

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
     * @brief Установить «лестницу» split-tunnel маршрутов через указанный адаптер.
     *
     * Вместо одиночной пары 0.0.0.0/1 + 128.0.0.0/1 раскладывает 0.0.0.0/0
     * набором маршрутов длиной префикса `ladder_prefix` (2^ladder_prefix штук),
     * ЗА ВЫЧЕТОМ carve-out диапазонов, которые нельзя заворачивать в TUN:
     *   • 127.0.0.0/8   — loopback (наш relay + loopback-прокси);
     *   • 0.0.0.0/8     — "this network";
     *   • 169.254.0.0/16 — link-local.
     * Carve-out реализован как complement: те /ladder_prefix-подсети, что целиком
     * попадают в исключаемый диапазон, НЕ ставятся; частично пересекающиеся —
     * дробятся глубже, пока чистая часть не отделится от исключаемой.  На практике
     * при выровненных /8- и /16-границах достаточно пропускать целые листья.
     *
     * Чем больше `ladder_prefix`, тем специфичнее маршруты и тем увереннее они
     * выигрывают longest-prefix-match у сосуществующих full-tunnel VPN.
     *
     * @param luid          LUID Wintun-адаптера (из WintunAdapter::Luid()).
     * @param next_hop_ipv4 Строка вида "10.6.7.1" — sanity-check формата (маршрут
     *                      ставится on-link, значение next-hop не используется).
     * @param metric        Route-метрика листьев (обычно 1).
     * @param ladder_prefix Длина префикса листьев (1..8).  1 = историческая пара /1.
     * @param outError      [out, обязателен] Диагностика при провале.
     * @return true при полной установке.  При частичном провале уже поставленные
     *         маршруты откатываются перед возвратом false.
     */
    static bool InstallSplitTunnel(NET_LUID luid,
                                   const std::wstring& next_hop_ipv4,
                                   uint32_t metric,
                                   int ladder_prefix,
                                   std::string* outError);

    /**
     * @brief Удалить ВСЕ split-tunnel маршруты на указанном LUID.
     *
     * Перечисляет IPv4-таблицу и удаляет все NETMGMT on-link (NextHop=0.0.0.0)
     * маршруты на данном LUID с PrefixLength в [1..8] — т.е. всю лестницу любой
     * глубины, которую мы могли поставить (историческую пару /1 в том числе).
     * ERROR_NOT_FOUND трактуется как success.
     *
     * @param luid      LUID Wintun-адаптера.
     * @param outError  [out, опционально] Диагностика при провале.  Может быть nullptr.
     * @return true, если все найденные маршруты удалены (или их не было).
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

    /**
     * @brief Убрать «протёкшие» proxy-bypass /32 маршруты от прошлых запусков.
     *
     * F1-фикс (см. plans/wintun_no_traffic_debug_2026-07-14.md): InstallHostBypass
     * ставит <proxy>/32 с Protocol=NETMGMT; при смене proxy.host между запусками
     * или грязном стопе старый /32 остаётся и НАВСЕГДА уводит этот dst мимо
     * туннеля (как самый специфичный префикс).  Здесь мы перечисляем IPv4-таблицу
     * и удаляем ВСЕ /32 c Protocol=NETMGMT, КРОМЕ тех, что заданы в keep-множестве
     * (текущие proxy-IP).  Так снимаются исключительно наши старые bypass'ы:
     * обычные системные /32 host-маршруты имеют иной Protocol.
     *
     * @param keep_ipv4_be  IPv4 (network byte order), которые НЕ трогать (актуальный прокси).
     * @param outError      [out, опционально] диагностика.
     * @return true при успехе (частичные ошибки удаления логируются в outError, но
     *         не считаются фатальными).
     */
    static bool CleanupStaleBypass(const std::vector<uint32_t>& keep_ipv4_be,
                                   std::string* outError);

    /**
     * @brief F2: задать низкую interface-метрику IPv4 у Wintun-адаптера.
     *
     * При РАВНОЙ длине префикса с другим адаптером Windows выбирает маршрут с
     * меньшей суммарной метрикой.  По умолчанию у Wintun AutomaticMetric=Enabled
     * (метрика ~5, как у Ethernet) — это делает исход ничьей недетерминированным.
     * Отключаем авто и задаём фиксированную низкую метрику, чтобы при равенстве
     * префиксов выигрывать.
     *
     * @param luid    LUID Wintun-адаптера.
     * @param metric  Желаемая interface-метрика (напр. 1).
     * @param outError[out, опционально] диагностика.
     * @return true при успехе.
     */
    static bool SetInterfaceMetric(NET_LUID luid, uint32_t metric, std::string* outError);

    /**
     * @brief Завернуть ВЕСЬ IPv6 в Wintun-адаптер (маршруты ::/1 + 8000::/1).
     *
     * ЗАЧЕМ: туннель/relay/HTTP CONNECT — только IPv4.  На dual-stack хостах ОС
     * (RFC 6724) предпочитает глобальный IPv6 → трафик уходит по физическому
     * IPv6 мимо IPv4-TUN.  Заворачивая IPv6 в TUN, мы отдаём эти пакеты
     * embedded-движку, который отвечает TCP RST на IPv6-SYN → приложение
     * мгновенно откатывается на IPv4.  Маршруты on-link (NextHop = ::),
     * Protocol=NETMGMT (не persistent через reboot).
     *
     * @param luid     LUID Wintun-адаптера.
     * @param metric   Метрика маршрутов (обычно 1).
     * @param outError [out, опционально] диагностика.
     * @return true при полной установке (или откате при частичном провале).
     */
    static bool InstallIpv6CatchAll(NET_LUID luid, uint32_t metric, std::string* outError);

    /**
     * @brief Снять IPv6 catch-all маршруты, поставленные InstallIpv6CatchAll.
     *
     * Перечисляет IPv6-таблицу и удаляет наши on-link (NextHop=::) NETMGMT
     * маршруты /1 на данном LUID.  ERROR_NOT_FOUND трактуется как success.
     */
    static bool UninstallIpv6CatchAll(NET_LUID luid, std::string* outError);

    /**
     * @brief Задать низкую interface-метрику IPv6 у Wintun-адаптера (AF_INET6).
     *        Аналог SetInterfaceMetric для IPv4.  Non-fatal при отсутствии IPv6.
     */
    static bool SetInterfaceMetricV6(NET_LUID luid, uint32_t metric, std::string* outError);
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
