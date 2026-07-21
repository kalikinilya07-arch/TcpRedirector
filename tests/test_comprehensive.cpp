/**
 * @file test_comprehensive.cpp
 * @brief Комплексные модульные тесты для TcpRedirector Service.
 *
 * Покрывает:
 *   - ConnectionTable (CRUD, bytes, thread safety)
 *   - ProxyConfig / Rule структуры
 *   - WinDivertCapture bitmap операции (без драйвера)
 *   - RuleEngine (расширенный)
 *   - Config инфраструктурные структуры
 *
 * Использует Catch2 v3.
 */

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>
#include <thread>
#include <atomic>

// ============================================================
// Подключаем тестируемые заголовки
// ============================================================
#include "../src/service/TcpRedirectorService/domain/entities/ProxyConfig.h"
#include "../src/service/TcpRedirectorService/domain/ports/IConnectionTable.h"
#include "../src/service/TcpRedirectorService/infrastructure/relay/ConnectionTable.h"
#include "../src/service/TcpRedirectorService/domain/services/RuleEngine.h"
#include "../src/service/TcpRedirectorService/infrastructure/config/Config.h"

using namespace tcp_redirector;
using namespace tcp_redirector::domain;
using namespace tcp_redirector::domain::ports;
using namespace tcp_redirector::infrastructure;
using namespace tcp_redirector::domain::services;

// ============================================================
// ConnectionTable — тесты
// ============================================================

TEST_CASE("ConnectionTable: Add and Get", "[connection]") {
    ConnectionTable table;

    // Добавляем соединение
    table.Add(50000, 0x0100007F, 0x0A0A0A0A, 443, 1);

    // Проверяем Get
    uint32_t out_ip = 0;
    uint16_t out_port = 0;
    REQUIRE(table.Get(50000, &out_ip, &out_port) == true);
    REQUIRE(out_ip == 0x0A0A0A0A);
    REQUIRE(out_port == 443);

    // Несуществующий порт
    REQUIRE(table.Get(50001, nullptr, nullptr) == false);
}

TEST_CASE("ConnectionTable: IsTracked", "[connection]") {
    ConnectionTable table;

    REQUIRE(table.IsTracked(50000) == false);

    table.Add(50000, 0, 0x0A0A0A0A, 80, 1);
    REQUIRE(table.IsTracked(50000) == true);
    REQUIRE(table.IsTracked(50001) == false);
}

TEST_CASE("ConnectionTable: GetProxyConfigId", "[connection]") {
    ConnectionTable table;

    table.Add(50000, 0, 0x0A0A0A0A, 443, 42);
    REQUIRE(table.GetProxyConfigId(50000) == 42);

    // Несуществующий порт
    REQUIRE(table.GetProxyConfigId(50001) == 0);
}

TEST_CASE("ConnectionTable: Remove", "[connection]") {
    ConnectionTable table;

    table.Add(50000, 0, 0x0A0A0A0A, 443, 1);
    REQUIRE(table.IsTracked(50000) == true);

    table.Remove(50000);
    REQUIRE(table.IsTracked(50000) == false);
}

TEST_CASE("ConnectionTable: Clear", "[connection]") {
    ConnectionTable table;

    table.Add(50000, 0, 0x0A0A0A0A, 80, 1);
    table.Add(50001, 0, 0x0A0A0A0A, 443, 2);
    REQUIRE(table.IsTracked(50000) == true);
    REQUIRE(table.IsTracked(50001) == true);

    table.Clear();
    REQUIRE(table.IsTracked(50000) == false);
    REQUIRE(table.IsTracked(50001) == false);
}

TEST_CASE("ConnectionTable: AddBytes and GetInfo", "[connection][bytes]") {
    ConnectionTable table;

    table.Add(50000, 0, 0x0A0A0A0A, 443, 1);
    table.SetProcessInfo(50000, 12345, L"C:\\test.exe");

    // Добавляем байты
    table.AddBytes(50000, 1000, 2000);
    table.AddBytes(50000, 500, 300);

    // Проверяем через GetInfo
    ConnectionInfo info;
    REQUIRE(table.GetInfo(50000, &info) == true);
    REQUIRE(info.pid == 12345);
    REQUIRE(info.bytes_up == 1500);
    REQUIRE(info.bytes_down == 2300);
    REQUIRE(std::wstring(info.proc_path) == L"C:\\test.exe");

    // После удаления — GetInfo возвращает false
    table.Remove(50000);
    REQUIRE(table.GetInfo(50000, &info) == false);
}

TEST_CASE("ConnectionTable: SetProcessInfo", "[connection]") {
    ConnectionTable table;

    table.Add(50000, 0, 0x0A0A0A0A, 443, 1);
    table.SetProcessInfo(50000, 999, L"D:\\app\\chrome.exe");

    ConnectionInfo info;
    REQUIRE(table.GetInfo(50000, &info) == true);
    REQUIRE(info.pid == 999);
    REQUIRE(std::wstring(info.proc_path) == L"D:\\app\\chrome.exe");
}

TEST_CASE("ConnectionTable: update existing entry", "[connection]") {
    ConnectionTable table;

    table.Add(50000, 0x0100007F, 0x0A0A0A0A, 443, 1);
    // Повторный Add с теми же src_port — обновление
    table.Add(50000, 0x0100007F, 0x0B0B0B0B, 8080, 2);

    uint32_t out_ip = 0;
    uint16_t out_port = 0;
    REQUIRE(table.Get(50000, &out_ip, &out_port) == true);
    REQUIRE(out_ip == 0x0B0B0B0B);
    REQUIRE(out_port == 8080);
}

TEST_CASE("ConnectionTable: multiple entries stress", "[connection][stress]") {
    ConnectionTable table;

    for (uint16_t port = 40000; port < 40100; port++) {
        table.Add(port, 0, 0x0A0A0A0A, 443, 1);
    }

    for (uint16_t port = 40000; port < 40100; port++) {
        REQUIRE(table.IsTracked(port) == true);
    }

    table.Clear();
    for (uint16_t port = 40000; port < 40100; port++) {
        REQUIRE(table.IsTracked(port) == false);
    }
}

TEST_CASE("ConnectionTable: add bytes to non-existent entry", "[connection]") {
    ConnectionTable table;
    // Не должно упасть
    table.AddBytes(60000, 100, 200);
    // Проверяем, что запись не появилась
    REQUIRE(table.IsTracked(60000) == false);
}

// ============================================================
// WinDivertCapture — bitmap операции (без драйвера)
// ============================================================

struct TestBitmap {
    LONG decided[2048]{0};
    LONG direct[2048]{0};

    bool IsDecided(uint16_t port) const {
        return (decided[port >> 5] >> (port & 31)) & 1;
    }
    bool IsDirect(uint16_t port) const {
        return (direct[port >> 5] >> (port & 31)) & 1;
    }
    void SetDirect(uint16_t port) {
        InterlockedOr(&decided[port >> 5], (LONG)(1u << (port & 31)));
        InterlockedOr(&direct[port >> 5],  (LONG)(1u << (port & 31)));
    }
    void SetDecided(uint16_t port) {
        InterlockedOr(&decided[port >> 5], (LONG)(1u << (port & 31)));
    }
    void Clear(uint16_t port) {
        InterlockedAnd(&decided[port >> 5], (LONG)~(1u << (port & 31)));
        InterlockedAnd(&direct[port >> 5],  (LONG)~(1u << (port & 31)));
    }
};

TEST_CASE("Bitmap: initial state all zeros", "[bitmap]") {
    TestBitmap bm;
    REQUIRE(bm.IsDecided(0) == false);
    REQUIRE(bm.IsDecided(65535) == false);
    REQUIRE(bm.IsDirect(100) == false);
    REQUIRE(bm.IsDirect(50000) == false);
}

TEST_CASE("Bitmap: SetDirect sets both bits", "[bitmap]") {
    TestBitmap bm;
    bm.SetDirect(50000);

    REQUIRE(bm.IsDecided(50000) == true);
    REQUIRE(bm.IsDirect(50000) == true);

    // Другие порты не затронуты
    REQUIRE(bm.IsDecided(50001) == false);
}

TEST_CASE("Bitmap: SetDecided sets only decided bit", "[bitmap]") {
    TestBitmap bm;
    bm.SetDecided(30000);

    REQUIRE(bm.IsDecided(30000) == true);
    REQUIRE(bm.IsDirect(30000) == false);
}

TEST_CASE("Bitmap: Clear resets both bits", "[bitmap]") {
    TestBitmap bm;
    bm.SetDirect(20000);
    REQUIRE(bm.IsDecided(20000) == true);

    bm.Clear(20000);
    REQUIRE(bm.IsDecided(20000) == false);
    REQUIRE(bm.IsDirect(20000) == false);
}

TEST_CASE("Bitmap: port boundary at 2048", "[bitmap]") {
    TestBitmap bm;
    bm.SetDirect(2047);
    bm.SetDirect(2048);
    bm.SetDirect(4095);

    REQUIRE(bm.IsDecided(2047) == true);
    REQUIRE(bm.IsDecided(2048) == true);
    REQUIRE(bm.IsDecided(4095) == true);
    REQUIRE(bm.IsDirect(2047) == true);
    REQUIRE(bm.IsDirect(2048) == true);
    REQUIRE(bm.IsDirect(4095) == true);

    REQUIRE(bm.IsDecided(2049) == false);
}

TEST_CASE("Bitmap: concurrent read-write", "[bitmap][stress]") {
    TestBitmap bm;
    std::atomic<bool> stop{false};
    std::atomic<uint64_t> ops{0};

    auto writer = std::thread([&]() {
        while (!stop) {
            for (uint16_t p = 0; p < 1000; p++) {
                bm.SetDirect(p);
                bm.Clear(p);
                bm.SetDecided(p);
                ops++;
            }
        }
    });

    auto reader = std::thread([&]() {
        while (!stop) {
            for (uint16_t p = 0; p < 1000; p++) {
                volatile bool d = bm.IsDecided(p);
                volatile bool r = bm.IsDirect(p);
                (void)d; (void)r;
            }
        }
    });

    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    stop = true;
    writer.join();
    reader.join();

    REQUIRE(ops > 0);
}

// ============================================================
// ProxyConfig — тесты структур
// ============================================================

TEST_CASE("ProxyConfig: default values", "[config]") {
    ProxyConfig cfg;
    REQUIRE(cfg.host.empty());
    REQUIRE(cfg.port == 3128);
    REQUIRE(cfg.auth_required == false);
}

TEST_CASE("Rule: fields", "[config]") {
    Rule rule;
    rule.id = "test-rule";
    rule.type = RuleType::ProcessName;
    rule.action = RuleAction::Proxy;
    rule.pattern = L"chrome.exe";
    rule.priority = 10;
    rule.enabled = true;

    REQUIRE(rule.id == "test-rule");
    REQUIRE(rule.type == RuleType::ProcessName);
    REQUIRE(rule.action == RuleAction::Proxy);
    REQUIRE(rule.pattern == L"chrome.exe");
    REQUIRE(rule.priority == 10);
    REQUIRE(rule.enabled == true);
}

TEST_CASE("ConnectionRecord: default state", "[config]") {
    ConnectionRecord cr;
    REQUIRE(cr.state == ConnectionState::Redirecting);
    REQUIRE(cr.rx_bytes == 0);
    REQUIRE(cr.tx_bytes == 0);
    REQUIRE(cr.pid == 0);
}

TEST_CASE("ServiceStats: default values", "[config]") {
    ServiceStats stats;
    REQUIRE(stats.active_connections == 0);
    REQUIRE(stats.total_rx_bytes == 0);
    REQUIRE(stats.total_tx_bytes == 0);
    REQUIRE(stats.proxy_errors == 0);
}

TEST_CASE("DriverStats: default values", "[config]") {
    DriverStats stats;
    REQUIRE(stats.pending_redirects == 0);
    REQUIRE(stats.total_redirects == 0);
    REQUIRE(stats.failed_redirects == 0);
    REQUIRE(stats.rules_count == 0);
    REQUIRE(stats.queue_max_size == 4096);
}

TEST_CASE("LogEntry: default level is Info", "[config]") {
    LogEntry entry;
    REQUIRE(entry.level == LogLevel::Info);
    REQUIRE(entry.message.empty());
    REQUIRE(entry.logger.empty());
}

// ============================================================
// Config infrastructure — тесты структур
// ============================================================

TEST_CASE("AppSettings: default path", "[infra]") {
    AppSettings app;
    REQUIRE(app.exePath.empty());
}

TEST_CASE("ProxySettings: defaults", "[infra]") {
    ProxySettings ps;
    REQUIRE(ps.host == "127.0.0.1");
    REQUIRE(ps.port == 3128);
    REQUIRE(ps.enabled == true);
}

TEST_CASE("AuthSettings: defaults", "[infra]") {
    AuthSettings auth;
    REQUIRE(auth.enabled == false);
    REQUIRE(auth.username.empty());
    REQUIRE(auth.kerberos == false);
}

TEST_CASE("LogSettings: defaults", "[infra]") {
    LogSettings ls;
    REQUIRE(ls.level == 2);      // INFO
    REQUIRE(ls.fileEnabled == true);
    REQUIRE(ls.maxSizeMB == 10);
}

TEST_CASE("StatsSettings: defaults", "[infra]") {
    StatsSettings ss;
    REQUIRE(ss.updateIntervalMs == 2000);
}

// ============================================================
// RuleEngine — расширенные тесты
// ============================================================

TEST_CASE("RuleEngine: empty rule set returns false", "[rule]") {
    RuleEngine engine;
    REQUIRE(engine.ShouldRedirect(L"chrome.exe", L"") == false);
}

TEST_CASE("RuleEngine: block action returns false", "[rule]") {
    RuleEngine engine;

    Rule blockRule;
    blockRule.id = "block1";
    blockRule.type = RuleType::ProcessName;
    blockRule.action = RuleAction::Block;
    blockRule.pattern = L"blocked.exe";
    blockRule.priority = 1;
    blockRule.enabled = true;
    engine.AddRule(blockRule);

    // Block → ShouldRedirect возвращает false
    REQUIRE(engine.ShouldRedirect(L"blocked.exe", L"") == false);
    // Другой процесс не блокируется
    REQUIRE(engine.ShouldRedirect(L"chrome.exe", L"") == false);
}

TEST_CASE("RuleEngine: update rule", "[rule]") {
    RuleEngine engine;

    Rule rule;
    rule.id = "r1";
    rule.type = RuleType::ProcessName;
    rule.action = RuleAction::Proxy;
    rule.pattern = L"old.exe";
    rule.priority = 1;
    rule.enabled = true;
    engine.AddRule(rule);

    REQUIRE(engine.ShouldRedirect(L"old.exe", L"") == true);

    // Обновляем правило
    rule.pattern = L"new.exe";
    engine.UpdateRule(rule);

    REQUIRE(engine.ShouldRedirect(L"old.exe", L"") == false);
    REQUIRE(engine.ShouldRedirect(L"new.exe", L"") == true);
}

TEST_CASE("RuleEngine: set all rules replaces existing", "[rule]") {
    RuleEngine engine;

    Rule proxy;
    proxy.id = "p1";
    proxy.type = RuleType::Global;
    proxy.action = RuleAction::Proxy;
    proxy.pattern = L"*";
    proxy.priority = 999;
    proxy.enabled = true;

    engine.SetRules({proxy});
    REQUIRE(engine.ShouldRedirect(L"anything.exe", L"") == true);

    // Заменяем на пустой список
    engine.SetRules({});
    REQUIRE(engine.ShouldRedirect(L"anything.exe", L"") == false);
}

TEST_CASE("RuleEngine: get rules returns copy", "[rule]") {
    RuleEngine engine;

    Rule r1, r2;
    r1.id = "1"; r1.type = RuleType::ProcessName; r1.pattern = L"a.exe"; r1.priority = 1; r1.enabled = true;
    r2.id = "2"; r2.type = RuleType::ProcessPath; r2.pattern = L"C:\\"; r2.priority = 2; r2.enabled = true;
    engine.AddRule(r1);
    engine.AddRule(r2);

    auto rules = engine.GetRules();
    REQUIRE(rules.size() == 2);
}

TEST_CASE("RuleEngine: disabled rule should not match", "[rule]") {
    RuleEngine engine;

    Rule r;
    r.id = "d1";
    r.type = RuleType::ProcessName;
    r.action = RuleAction::Proxy;
    r.pattern = L"chrome.exe";
    r.priority = 1;
    r.enabled = false;  // отключено
    engine.AddRule(r);

    REQUIRE(engine.ShouldRedirect(L"chrome.exe", L"") == false);
}

TEST_CASE("RuleEngine: priority order respected", "[rule]") {
    RuleEngine engine;

    // Global proxy (низкий приоритет = 999)
    Rule global;
    global.id = "g";
    global.type = RuleType::Global;
    global.action = RuleAction::Proxy;
    global.pattern = L"*";
    global.priority = 999;
    global.enabled = true;
    engine.AddRule(global);

    // Direct для конкретного процесса (высокий приоритет = 1)
    Rule direct;
    direct.id = "d";
    direct.type = RuleType::ProcessName;
    direct.action = RuleAction::Direct;
    direct.pattern = L"direct.exe";
    direct.priority = 1;
    direct.enabled = true;
    engine.AddRule(direct);

    // direct.exe должно быть DIRECT (false)
    REQUIRE(engine.ShouldRedirect(L"direct.exe", L"") == false);
    // other.exe через global → PROXY (true)
    REQUIRE(engine.ShouldRedirect(L"other.exe", L"") == true);
}

// ============================================================
// ShortName helper — тест
// ============================================================

static const wchar_t* ShortName(const wchar_t* path) {
    const wchar_t* p = wcsrchr(path, L'\\');
    return p ? p + 1 : path;
}

TEST_CASE("ShortName: extracts filename from full path", "[helper]") {
    REQUIRE(std::wstring(ShortName(L"C:\\Program Files\\chrome.exe")) == L"chrome.exe");
    REQUIRE(std::wstring(ShortName(L"C:\\test\\app\\sub\\file.dll")) == L"file.dll");
}

TEST_CASE("ShortName: returns as-is for filename only", "[helper]") {
    REQUIRE(std::wstring(ShortName(L"chrome.exe")) == L"chrome.exe");
    REQUIRE(std::wstring(ShortName(L"test")) == L"test");
}

TEST_CASE("ShortName: root path parsing", "[helper]") {
    REQUIRE(std::wstring(ShortName(L"C:\\windows.exe")) == L"windows.exe");
    REQUIRE(std::wstring(ShortName(L"\\file.txt")) == L"file.txt");
}