/**
 * @file WintunCapture.cpp
 * @brief WP10 — реализация ICapture-фасада для Wintun-режима захвата.
 */

#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <windows.h>
#include <objbase.h>            // CLSIDFromString

#include <cstdint>
#include <cstring>
#include <memory>
#include <optional>
#include <string>

#include "WintunCapture.h"

#include "wintun/WintunApi.h"
#include "wintun/WintunAdapter.h"
#include "wintun/WintunSession.h"
#include "wintun/ITunEngine.h"
#include "wintun/Tun2SocksEngineEmbedded.h"
#include "wintun/Tun2SocksEngineExternal.h"
#include "wintun/RouteInstaller.h"

#include "../utf8_convert.h"

namespace tcp_redirector {
namespace infrastructure {

// ---------------------------------------------------------------------------
// Хелперы логирования — тонкие обёртки с nullptr-guard'ом.
// Логгер-тег "wintun" — совпадает с preflight-тегом, чтобы GUI/файл-фильтр
// могли группировать сообщения по компоненту.
// ---------------------------------------------------------------------------

void WintunCapture::LogInfo(const std::string& msg) const {
    if (m_log) m_log->Log(domain::LogLevel::Info, "wintun", msg);
}
void WintunCapture::LogWarn(const std::string& msg) const {
    if (m_log) m_log->Log(domain::LogLevel::Warn, "wintun", msg);
}
void WintunCapture::LogError(const std::string& msg) const {
    if (m_log) m_log->Log(domain::LogLevel::Error, "wintun", msg);
}
void WintunCapture::LogDebug(const std::string& msg) const {
    if (m_log) m_log->Log(domain::LogLevel::Debug, "wintun", msg);
}

// ---------------------------------------------------------------------------
// Конструктор / деструктор
// ---------------------------------------------------------------------------

WintunCapture::WintunCapture(WintunSettings settings,
                             uint16_t relay_port,
                             domain::ports::ILogSink* log)
    : m_settings(std::move(settings)),
      m_relayPort(relay_port),
      m_log(log) {
}

WintunCapture::~WintunCapture() {
    // Идемпотентный TearDown — если Open() ранее не удался, TearDown уже
    // прошёл; если удался и Close не был вызван — снимаем всё сейчас.
    TearDown();
}

// ---------------------------------------------------------------------------
// Static helpers
// ---------------------------------------------------------------------------

bool WintunCapture::ExtractGatewayFromCidr(const std::string& cidr,
                                           std::wstring& gwOut,
                                           std::string* outError) {
    // В tunnel_ipv4_cidr хранится «шлюз/префикс», а не «сеть/префикс» —
    // ср. дефолт "10.6.7.1/24".  Извлекаем host-часть как есть.
    const auto slash = cidr.find('/');
    if (slash == std::string::npos || slash == 0) {
        if (outError) *outError = "tunnel_ipv4_cidr is not a.b.c.d/N: '" + cidr + "'";
        return false;
    }
    const std::string host = cidr.substr(0, slash);
    // Валидация — четыре октета, разделённых точками.
    int dots = 0;
    int cur = -1;
    for (size_t i = 0; i <= host.size(); ++i) {
        const char c = (i < host.size()) ? host[i] : '.';
        if (c == '.') {
            if (cur < 0 || cur > 255) {
                if (outError) *outError = "tunnel_ipv4_cidr host-part is invalid: '" + host + "'";
                return false;
            }
            dots++;
            cur = -1;
        } else if (c >= '0' && c <= '9') {
            if (cur < 0) cur = 0;
            cur = cur * 10 + (c - '0');
        } else {
            if (outError) *outError = "tunnel_ipv4_cidr host-part is invalid: '" + host + "'";
            return false;
        }
    }
    if (dots != 4) {
        if (outError) *outError = "tunnel_ipv4_cidr host-part is invalid: '" + host + "'";
        return false;
    }
    gwOut = Utf8ToWide(host);
    return true;
}

bool WintunCapture::TryParseGuid(const std::string& s, GUID& out) {
    if (s.empty()) return false;
    std::wstring ws = Utf8ToWide(s);
    // CLSIDFromString требует фигурных скобок; добавляем, если их нет.
    if (!ws.empty() && ws.front() != L'{') {
        ws = L"{" + ws + L"}";
    }
    HRESULT hr = CLSIDFromString(ws.c_str(), &out);
    return SUCCEEDED(hr);
}

// ---------------------------------------------------------------------------
// Стат-счётчики (проксируем к движку)
// ---------------------------------------------------------------------------

uint64_t WintunCapture::GetTotalRxBytes() const {
    return m_engine ? m_engine->RxBytes() : 0ull;
}
uint64_t WintunCapture::GetTotalTxBytes() const {
    return m_engine ? m_engine->TxBytes() : 0ull;
}
uint32_t WintunCapture::GetActiveConnections() const {
    if (m_engine) return m_engine->ActiveFlows();
    return m_connTable
        ? static_cast<uint32_t>(m_connTable->GetTrackedCount())
        : 0u;
}

// ---------------------------------------------------------------------------
// Open
// ---------------------------------------------------------------------------

bool WintunCapture::Open() {
    if (m_open.load(std::memory_order_acquire)) {
        LogDebug("Open() called on already-open capture (idempotent no-op)");
        return true;
    }

    // Шаг 1: engine kind — WP12 добавил поддержку external.
    //   • Embedded → сервис владеет и адаптером, И data-сессией; движок
    //     Tun2SocksEngineEmbedded читает/пишет пакеты через WintunSession.
    //   • External → сервис владеет ТОЛЬКО адаптером (создание, IPv4,
    //     маршруты); сессию открывает дочерний tun2socks.exe своими
    //     собственными вызовами wintun.dll (см. §6 ownership table плана).
    //     Значит на этом branch мы НЕ вызываем WintunSession::Start.
    const bool useEmbedded = (m_settings.engine == WintunEngineKind::Embedded);

    std::string err;

    // Шаг 2: загрузка wintun.dll
    m_api = capture::wintun::WintunApi::Load(&err);
    if (!m_api) {
        LogError("WintunApi::Load failed: " + err);
        return false;
    }
    // Полный путь до DLL — DEBUG (не INFO): не хотим засвечивать install-путь.
    LogDebug("wintun.dll loaded from: " + WideToUtf8(m_api->DllPath())
             + ", driver_version=0x" + [&]{
                 char b[16]; std::snprintf(b, sizeof(b), "%08X", m_api->RunningDriverVersion());
                 return std::string(b);
               }());

    // Шаг 3: параметры адаптера — стабильный GUID при наличии в config'е.
    capture::wintun::WintunAdapter::CreateParams cp;
    cp.name        = Utf8ToWide(m_settings.adapter_name);
    cp.tunnel_type = L"TcpRedirector";
    GUID g{};
    if (TryParseGuid(m_settings.adapter_guid, g)) {
        cp.requested_guid = g;
        LogDebug("Using configured adapter_guid: " + m_settings.adapter_guid);
    } else if (!m_settings.adapter_guid.empty()) {
        LogWarn("adapter_guid is set but not a valid GUID string; wintun will assign a random one");
    }

    // Шаг 4: создание/открытие адаптера.
    m_adapter = capture::wintun::WintunAdapter::CreateOrOpen(m_api, cp, &err);
    if (!m_adapter) {
        LogError("WintunAdapter::CreateOrOpen failed: " + err);
        m_api.reset();
        return false;
    }
    LogInfo("Adapter ready: name='" + m_settings.adapter_name + "'");

    // Шаг 5: назначение IPv4-адреса.
    err.clear();
    if (!m_adapter->ConfigureIpv4(Utf8ToWide(m_settings.tunnel_ipv4_cidr), &err)) {
        // Если у адаптера уже есть IP (переоткрытый после краха),
        // ConfigureIpv4 попробовал Set, тоже упал.  Это не 100%-фатал в
        // hardening'е, но в v1 WP10 мы жёстко отказываем — иначе движок
        // будет пробовать поднять netif без IP-адреса и лишь запутает
        // диагностику.
        LogError("WintunAdapter::ConfigureIpv4('" + m_settings.tunnel_ipv4_cidr
                 + "') failed: " + err);
        m_adapter.reset();
        m_api.reset();
        return false;
    }
    LogDebug("Adapter IPv4 configured: " + m_settings.tunnel_ipv4_cidr);

    // Шаг 6: extract gateway (host-часть CIDR — 10.6.7.1/24 → "10.6.7.1").
    std::wstring gwW;
    if (!ExtractGatewayFromCidr(m_settings.tunnel_ipv4_cidr, gwW, &err)) {
        LogError("Cannot derive gateway from tunnel_ipv4_cidr: " + err);
        m_adapter.reset();
        m_api.reset();
        return false;
    }

    // Шаг 7: split-tunnel маршруты через RAII-scope.  Если ниже упадём —
    // деструктор RouteScope снимет маршруты, чтобы не оставить orphan.
    capture::wintun::RouteScope routeScope(m_adapter->Luid());
    err.clear();
    // Метрика 1 — гарантированно ниже любого дефолт-маршрута; §6.8 плана
    // указывает 4, но мы выбираем 1 для однозначного выигрыша при
    // произвольных конфигурациях сети (в т.ч. VPN-стэках с metric=5).
    if (!capture::wintun::RouteInstaller::InstallSplitTunnel(
            m_adapter->Luid(), gwW, /*metric=*/1u, &err)) {
        LogError("RouteInstaller::InstallSplitTunnel failed: " + err);
        m_adapter.reset();
        m_api.reset();
        return false;
    }
    routeScope.MarkInstalled();
    LogDebug("Split-tunnel routes installed (0.0.0.0/1 + 128.0.0.0/1 via "
             + WideToUtf8(gwW) + ", metric=1)");

    // Шаг 8: data-сессия — ТОЛЬКО для embedded-движка.
    //   External-движок открывает сессию сам (§6 ownership table).
    if (useEmbedded) {
        err.clear();
        auto sessionUnique = capture::wintun::WintunSession::Start(
            m_api, m_adapter->Handle(),
            capture::wintun::WintunSession::kDefaultCapacity,
            &err);
        if (!sessionUnique) {
            LogError("WintunSession::Start failed: " + err);
            // routeScope снимет маршруты автоматически.
            m_adapter.reset();
            m_api.reset();
            return false;
        }
        m_session = std::shared_ptr<capture::wintun::WintunSession>(sessionUnique.release());
    }

    // Шаг 9: инстанциация движка по значению wintun.engine.
    err.clear();
    std::unique_ptr<capture::wintun::ITunEngine> engine;
    switch (m_settings.engine) {
        case WintunEngineKind::Embedded: {
            auto embedded = std::make_unique<capture::wintun::Tun2SocksEngineEmbedded>(
                m_session, m_relayPort, /*on_flow=*/nullptr);

            // Задача 2: фильтрация по процессу внутри embedded-движка.
            // Прокидываем RuleEngine + флаг + target-path.  При
            // process_filter_enabled=true движок применяет apps[]-правила
            // (PROXY/DIRECT/BLOCK), как WinDivert; иначе — старое поведение
            // Option 2b (весь TCP через прокси).
            capture::wintun::EmbeddedProcessFilter pf;
            pf.enabled             = m_settings.process_filter_enabled;
            pf.rule_engine         = m_ruleEngine;
            pf.target_process_path = m_targetProcessPath;
            pf.proxy_configured    = (!m_proxyHost.empty() && m_proxyPort != 0);
            pf.log                 = m_log;
            embedded->SetProcessFilter(pf);

            if (pf.enabled && pf.rule_engine) {
                LogInfo("Process filter ENABLED for embedded engine "
                        "(apps[] rules applied inside tunnel)");
            } else {
                LogInfo("Process filter DISABLED for embedded engine "
                        "(all IPv4-TCP proxied — Option 2b)");
            }

            engine = std::move(embedded);
            break;
        }
        case WintunEngineKind::External: {
            // Ребёнок опирается на:
            //   • имя адаптера (мы уже создали его выше),
            //   • ExternalEngineSettings (executable, extra_args, socks5_listen,
            //     restart_on_crash, restart_backoff_ms).
            // ServiceMain уже забиндил SOCKS5-listener на socks5_listen (WP12a).
            //
            // Задача 2 — ОГРАНИЧЕНИЕ external-режима:
            //   Фильтрация по конкретному процессу на уровне отдельного
            //   соединения здесь технически невозможна тем же способом, что в
            //   embedded/WinDivert.  Трафик терминирует дочерний tun2socks.exe
            //   и форвардит его на loopback-SOCKS5.  На приёме SOCKS5 источник
            //   любого соединения — это САМ tun2socks.exe (его PID), а не
            //   оригинальное приложение; tun2socks к тому же НЕ сохраняет
            //   source-порт исходного приложения.  Поэтому резолв процесса по
            //   source-порту вернёт tun2socks, а не целевое приложение.
            //   Fallback: весь трафик туннеля проксируется (как и раньше).
            //   Если нужна фильтрация по процессу — используйте engine=embedded
            //   или capture_mode=windivert.
            if (m_settings.process_filter_enabled) {
                LogWarn("Process filter is requested (process_filter_enabled=true) "
                        "but engine=external does NOT support per-process filtering "
                        "(PID at SOCKS5 boundary is tun2socks.exe itself). "
                        "All tunneled TCP will be proxied. "
                        "Use engine=embedded or capture_mode=windivert for per-process rules.");
            }
            engine = std::make_unique<capture::wintun::Tun2SocksEngineExternal>(
                Utf8ToWide(m_settings.adapter_name),
                m_settings.external_engine,
                m_log);
            break;
        }
    }
    if (!engine) {
        LogError("WintunCapture: unknown engine kind, refusing to start");
        m_session.reset();
        m_adapter.reset();
        m_api.reset();
        return false;
    }

    if (!engine->Start(&err)) {
        LogError(std::string(useEmbedded ? "Tun2SocksEngineEmbedded"
                                          : "Tun2SocksEngineExternal")
                 + "::Start failed: " + err);
        // Уборка: сессия закроется через m_session.reset(); routeScope снимет маршруты.
        m_session.reset();
        m_adapter.reset();
        m_api.reset();
        return false;
    }
    m_engine = std::move(engine);

    // Всё поднялось — передаём владение маршрутами в поле m_routesInstalled,
    // routeScope при выходе больше ничего не делает.
    m_routesInstalled = true;
    routeScope.Release();

    m_open.store(true, std::memory_order_release);

    LogInfo("WintunCapture opened: adapter='" + m_settings.adapter_name
            + "' tunnel=" + m_settings.tunnel_ipv4_cidr
            + " gateway=" + WideToUtf8(gwW)
            + " relay_port=" + std::to_string(m_relayPort)
            + " engine=" + (useEmbedded ? "embedded" : "external"));
    return true;
}

// ---------------------------------------------------------------------------
// Close
// ---------------------------------------------------------------------------

void WintunCapture::Close() {
    if (!m_open.exchange(false, std::memory_order_acq_rel)) {
        return;  // ничего не открывали
    }
    TearDown();
    LogInfo("WintunCapture closed");
}

// ---------------------------------------------------------------------------
// TearDown — LIFO-порядок уборки, вызывается из Close и из деструктора.
// Идемпотентен и не бросает исключений — все ошибки только логируются.
// ---------------------------------------------------------------------------

void WintunCapture::TearDown() {
    // 1. Останов движка (закрывает все flow'ы, снимает netif).
    if (m_engine) {
        try {
            m_engine->Stop();
        } catch (const std::exception& e) {
            LogWarn(std::string("engine.Stop threw: ") + e.what());
        } catch (...) {
            LogWarn("engine.Stop threw unknown exception");
        }
        m_engine.reset();
    }

    // 2. Закрытие data-сессии (EndSession) — деструктор WintunSession.
    if (m_session) {
        m_session.reset();
    }

    // 3. Снятие split-tunnel маршрутов.  Делаем ДО удаления адаптера, потому
    //    что снятие после того, как LUID перестанет существовать, вернёт
    //    ошибку.  При этом Wintun при CloseAdapter уносит собственные
    //    connected-маршруты (IPv4-адрес адаптера), а наши /1-маршруты
    //    остались бы висеть — их надо снять здесь.
    if (m_routesInstalled && m_adapter) {
        std::string err;
        if (!capture::wintun::RouteInstaller::UninstallSplitTunnel(
                m_adapter->Luid(), &err)) {
            LogWarn("RouteInstaller::UninstallSplitTunnel: " + err);
        } else {
            LogDebug("Split-tunnel routes uninstalled");
        }
        m_routesInstalled = false;
    }

    // 4. Закрытие/удаление адаптера.
    if (m_adapter) {
        m_adapter.reset();  // деструктор WintunAdapter вызывает CloseAdapter.
    }

    // 5. Выгрузка DLL (после того как все объекты, держащие функции, разошлись).
    if (m_api) {
        m_api.reset();
    }
}

} // namespace infrastructure
} // namespace tcp_redirector
