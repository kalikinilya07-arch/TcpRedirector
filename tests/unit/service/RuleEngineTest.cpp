#include <cassert>
#include <iostream>
#include <thread>
#include <atomic>
#include <chrono>
#include <vector>
#include "../../../src/service/TcpRedirectorService/domain/services/RuleEngine.h"

using namespace tcp_redirector::domain::services;
using namespace tcp_redirector::domain;
using tcp_redirector::infrastructure::AppRule;
using tcp_redirector::infrastructure::PortRange;

void TestWildcardMatch() {
    std::cout << "Test: WildcardMatch..." << std::endl;

    RuleEngine engine;

    // Test exact match via ShouldRedirect
    Rule rule1;
    rule1.id = "1";
    rule1.type = RuleType::ProcessName;
    rule1.action = RuleAction::Proxy;
    rule1.pattern = L"chrome.exe";
    rule1.priority = 1;
    rule1.enabled = true;
    engine.AddRule(rule1);

    assert(engine.ShouldRedirect(L"chrome.exe", L"") == true);
    assert(engine.ShouldRedirect(L"firefox.exe", L"") == false);

    std::cout << "  PASS" << std::endl;
}

void TestPathRule() {
    std::cout << "Test: PathRule..." << std::endl;

    RuleEngine engine;

    Rule rule1;
    rule1.id = "1";
    rule1.type = RuleType::ProcessPath;
    rule1.action = RuleAction::Proxy;
    rule1.pattern = L"C:\\Program Files\\";
    rule1.priority = 1;
    rule1.enabled = true;
    engine.AddRule(rule1);

    assert(engine.ShouldRedirect(L"chrome.exe",
        L"C:\\Program Files\\Google\\Chrome\\chrome.exe") == true);
    assert(engine.ShouldRedirect(L"app.exe",
        L"D:\\Custom\\app.exe") == false);

    std::cout << "  PASS" << std::endl;
}

void TestGlobalRule() {
    std::cout << "Test: GlobalRule..." << std::endl;

    RuleEngine engine;

    // Global direct rule
    Rule rule1;
    rule1.id = "1";
    rule1.type = RuleType::Global;
    rule1.action = RuleAction::Direct;
    rule1.pattern = L"*";
    rule1.priority = 999;
    rule1.enabled = true;
    engine.AddRule(rule1);

    assert(engine.ShouldRedirect(L"anything.exe", L"") == false);

    std::cout << "  PASS" << std::endl;
}

void TestPriorityOrder() {
    std::cout << "Test: PriorityOrder..." << std::endl;

    RuleEngine engine;

    // Global proxy rule (low priority)
    Rule globalRule;
    globalRule.id = "global";
    globalRule.type = RuleType::Global;
    globalRule.action = RuleAction::Proxy;
    globalRule.pattern = L"*";
    globalRule.priority = 999;
    globalRule.enabled = true;
    engine.AddRule(globalRule);

    // Specific direct rule (high priority)
    Rule directRule;
    directRule.id = "direct";
    directRule.type = RuleType::ProcessName;
    directRule.action = RuleAction::Direct;
    directRule.pattern = L"ignore.exe";
    directRule.priority = 1;
    directRule.enabled = true;
    engine.AddRule(directRule);

    // Specific process should match before global
    assert(engine.ShouldRedirect(L"ignore.exe", L"") == false);

    // Other processes fall through to global
    assert(engine.ShouldRedirect(L"other.exe", L"") == true);

    std::cout << "  PASS" << std::endl;
}

void TestDisabledRule() {
    std::cout << "Test: DisabledRule..." << std::endl;

    RuleEngine engine;

    Rule rule1;
    rule1.id = "1";
    rule1.type = RuleType::ProcessName;
    rule1.action = RuleAction::Proxy;
    rule1.pattern = L"chrome.exe";
    rule1.priority = 1;
    rule1.enabled = false;  // DISABLED
    engine.AddRule(rule1);

    // Disabled rule should not match
    assert(engine.ShouldRedirect(L"chrome.exe", L"") == false);

    std::cout << "  PASS" << std::endl;
}

void TestRemoveRule() {
    std::cout << "Test: RemoveRule..." << std::endl;

    RuleEngine engine;

    Rule rule1;
    rule1.id = "1";
    rule1.type = RuleType::ProcessName;
    rule1.action = RuleAction::Proxy;
    rule1.pattern = L"chrome.exe";
    engine.AddRule(rule1);
    assert(engine.ShouldRedirect(L"chrome.exe", L"") == true);

    engine.RemoveRule("1");
    assert(engine.ShouldRedirect(L"chrome.exe", L"") == false);

    std::cout << "  PASS" << std::endl;
}

// =====================================================================
// WP4: Port-aware matching (MatchForFlow / SetAppRules) tests
// =====================================================================

static AppRule MakeRule(const std::string& pattern,
                        const std::vector<uint16_t>& ports = {},
                        const std::vector<PortRange>& ranges = {},
                        bool route_all = false,
                        const std::string& proxy_id = "default",
                        const std::string& exe_path = "") {
    AppRule r;
    r.pattern = pattern;
    r.ports = ports;
    r.port_ranges = ranges;
    r.route_all_traffic = route_all;
    r.proxy_id = proxy_id;
    r.exe_path = exe_path;
    return r;
}

void TestMatchForFlow_NoAppRules_ReturnsUnmatched() {
    std::cout << "Test: MatchForFlow_NoAppRules_ReturnsUnmatched..." << std::endl;

    RuleEngine engine;
    auto result = engine.MatchForFlow("firefox.exe", "C:\\firefox.exe", 443);
    assert(result.matched == false);
    assert(result.route_all_traffic == false);
    assert(result.proxy_id.empty());

    std::cout << "  PASS" << std::endl;
}

void TestMatchForFlow_ByProcessNameAndPort_Matches() {
    std::cout << "Test: MatchForFlow_ByProcessNameAndPort_Matches..." << std::endl;

    RuleEngine engine;
    engine.SetAppRules({ MakeRule("firefox.exe", {443}) });

    auto ok = engine.MatchForFlow("firefox.exe", "", 443);
    assert(ok.matched == true);
    assert(ok.route_all_traffic == false);
    assert(ok.proxy_id == "default");

    auto miss_name = engine.MatchForFlow("chrome.exe", "", 443);
    assert(miss_name.matched == false);

    std::cout << "  PASS" << std::endl;
}

void TestMatchForFlow_ProcessMatches_PortNot_ReturnsUnmatched() {
    std::cout << "Test: MatchForFlow_ProcessMatches_PortNot_ReturnsUnmatched..." << std::endl;

    RuleEngine engine;
    engine.SetAppRules({ MakeRule("firefox.exe", {443, 80}) });

    auto miss = engine.MatchForFlow("firefox.exe", "", 22);
    assert(miss.matched == false);

    std::cout << "  PASS" << std::endl;
}

void TestMatchForFlow_RouteAllTraffic_MatchesAnyPort() {
    std::cout << "Test: MatchForFlow_RouteAllTraffic_MatchesAnyPort..." << std::endl;

    RuleEngine engine;
    engine.SetAppRules({ MakeRule("firefox.exe", {}, {}, /*route_all=*/true) });

    auto a = engine.MatchForFlow("firefox.exe", "", 1);
    auto b = engine.MatchForFlow("firefox.exe", "", 443);
    auto c = engine.MatchForFlow("firefox.exe", "", 65535);
    assert(a.matched && a.route_all_traffic);
    assert(b.matched && b.route_all_traffic);
    assert(c.matched && c.route_all_traffic);

    // Non-matching process still doesn't match.
    auto d = engine.MatchForFlow("chrome.exe", "", 443);
    assert(d.matched == false);

    std::cout << "  PASS" << std::endl;
}

void TestMatchForFlow_PortRange_InclusiveBoundsMatch() {
    std::cout << "Test: MatchForFlow_PortRange_InclusiveBoundsMatch..." << std::endl;

    RuleEngine engine;
    PortRange range;
    range.from = 8000;
    range.to   = 8100;
    engine.SetAppRules({ MakeRule("app.exe", {}, {range}) });

    auto lo_edge = engine.MatchForFlow("app.exe", "", 8000);
    auto hi_edge = engine.MatchForFlow("app.exe", "", 8100);
    auto mid     = engine.MatchForFlow("app.exe", "", 8050);
    auto below   = engine.MatchForFlow("app.exe", "", 7999);
    auto above   = engine.MatchForFlow("app.exe", "", 8101);

    assert(lo_edge.matched);
    assert(hi_edge.matched);
    assert(mid.matched);
    assert(below.matched == false);
    assert(above.matched == false);

    std::cout << "  PASS" << std::endl;
}

void TestMatchForFlow_MultiplePortsAndRanges_UnionSemantics() {
    std::cout << "Test: MatchForFlow_MultiplePortsAndRanges_UnionSemantics..." << std::endl;

    RuleEngine engine;
    PortRange r1; r1.from = 8000; r1.to = 8010;
    PortRange r2; r2.from = 9000; r2.to = 9000;
    engine.SetAppRules({ MakeRule("app.exe", {80, 443}, {r1, r2}) });

    // Discrete ports
    assert(engine.MatchForFlow("app.exe", "", 80).matched);
    assert(engine.MatchForFlow("app.exe", "", 443).matched);
    // First range
    assert(engine.MatchForFlow("app.exe", "", 8000).matched);
    assert(engine.MatchForFlow("app.exe", "", 8005).matched);
    assert(engine.MatchForFlow("app.exe", "", 8010).matched);
    // Second (degenerate) range
    assert(engine.MatchForFlow("app.exe", "", 9000).matched);
    // Outside union
    assert(engine.MatchForFlow("app.exe", "", 22).matched == false);
    assert(engine.MatchForFlow("app.exe", "", 8011).matched == false);
    assert(engine.MatchForFlow("app.exe", "", 8999).matched == false);
    assert(engine.MatchForFlow("app.exe", "", 9001).matched == false);

    std::cout << "  PASS" << std::endl;
}

void TestMatchForFlow_TwoRules_FirstMatchWins() {
    std::cout << "Test: MatchForFlow_TwoRules_FirstMatchWins..." << std::endl;

    RuleEngine engine;
    // Rule 0: matches firefox+443 -> proxy "A"
    // Rule 1: matches firefox+443 -> proxy "B" (never reached — insertion order)
    engine.SetAppRules({
        MakeRule("firefox.exe", {443}, {}, false, "A"),
        MakeRule("firefox.exe", {443}, {}, false, "B"),
    });

    auto m = engine.MatchForFlow("firefox.exe", "", 443);
    assert(m.matched == true);
    assert(m.proxy_id == "A");

    // If first rule's port doesn't match but second does, second wins
    // (rules do NOT stop after process-match if port fails).
    engine.SetAppRules({
        MakeRule("firefox.exe", {80},  {}, false, "A"),
        MakeRule("firefox.exe", {443}, {}, false, "B"),
    });
    auto m2 = engine.MatchForFlow("firefox.exe", "", 443);
    assert(m2.matched == true);
    assert(m2.proxy_id == "B");

    std::cout << "  PASS" << std::endl;
}

void TestMatchForFlow_ExePathSpecified_MustMatch() {
    std::cout << "Test: MatchForFlow_ExePathSpecified_MustMatch..." << std::endl;

    RuleEngine engine;
    // Rule ties to a specific exe path (case-insensitive suffix).
    engine.SetAppRules({
        MakeRule("firefox.exe", {443}, {}, false, "default",
                 "\\Mozilla Firefox\\firefox.exe"),
    });

    // Different install path — must not match.
    auto miss = engine.MatchForFlow(
        "firefox.exe", "C:\\OtherApp\\firefox.exe", 443);
    assert(miss.matched == false);

    // Correct suffix (case-insensitive).
    auto ok = engine.MatchForFlow(
        "firefox.exe",
        "C:\\Program Files\\MOZILLA FIREFOX\\firefox.exe",
        443);
    assert(ok.matched == true);

    // Correct suffix, wrong port.
    auto wrong_port = engine.MatchForFlow(
        "firefox.exe",
        "C:\\Program Files\\Mozilla Firefox\\firefox.exe",
        22);
    assert(wrong_port.matched == false);

    std::cout << "  PASS" << std::endl;
}

void TestMatchForFlow_ConcurrentSetAndMatch_NoRace() {
    std::cout << "Test: MatchForFlow_ConcurrentSetAndMatch_NoRace..." << std::endl;

    RuleEngine engine;
    engine.SetAppRules({ MakeRule("app.exe", {443}) });

    std::atomic<bool> stop{false};
    std::atomic<uint64_t> matches{0};
    std::atomic<uint64_t> writes{0};

    std::thread writer([&] {
        int flip = 0;
        while (!stop.load(std::memory_order_relaxed)) {
            std::vector<AppRule> next;
            if ((flip++ & 1) == 0) {
                next.push_back(MakeRule("app.exe", {443}));
            } else {
                PortRange r; r.from = 100; r.to = 200;
                next.push_back(MakeRule("app.exe", {}, {r}, false, "alt"));
            }
            engine.SetAppRules(std::move(next));
            writes.fetch_add(1, std::memory_order_relaxed);
        }
    });

    std::thread reader([&] {
        while (!stop.load(std::memory_order_relaxed)) {
            auto a = engine.MatchForFlow("app.exe", "", 443);
            auto b = engine.MatchForFlow("app.exe", "", 150);
            (void)a; (void)b;
            matches.fetch_add(2, std::memory_order_relaxed);
        }
    });

    std::this_thread::sleep_for(std::chrono::milliseconds(150));
    stop.store(true, std::memory_order_relaxed);
    writer.join();
    reader.join();

    // Не крашнулись и обе стороны реально что-то сделали.
    assert(writes.load() > 0);
    assert(matches.load() > 0);

    std::cout << "  PASS (writes=" << writes.load()
              << " matches=" << matches.load() << ")" << std::endl;
}

void TestSetAppRules_ThenSetLegacyRules_BothWorkIndependently() {
    std::cout << "Test: SetAppRules_ThenSetLegacyRules_BothWorkIndependently..." << std::endl;

    RuleEngine engine;

    // Set v2 app rules first.
    engine.SetAppRules({ MakeRule("firefox.exe", {443}) });

    // Then set legacy v1 rules (should not affect v2 storage).
    Rule legacyRule;
    legacyRule.id = "legacy-1";
    legacyRule.type = RuleType::ProcessName;
    legacyRule.action = RuleAction::Proxy;
    legacyRule.pattern = L"chrome.exe";
    legacyRule.priority = 1;
    legacyRule.enabled = true;
    engine.SetRules({legacyRule});

    // v2 API still returns the v2 rule.
    auto v2 = engine.MatchForFlow("firefox.exe", "", 443);
    assert(v2.matched == true);

    // Legacy API still returns the legacy rule.
    assert(engine.ShouldRedirect(L"chrome.exe", L"") == true);
    // v2-rule process is NOT visible in legacy engine (independent storage).
    assert(engine.ShouldRedirect(L"firefox.exe", L"") == false);

    // Clear v2, legacy still works.
    engine.SetAppRules({});
    auto after = engine.MatchForFlow("firefox.exe", "", 443);
    assert(after.matched == false);
    assert(engine.ShouldRedirect(L"chrome.exe", L"") == true);

    std::cout << "  PASS" << std::endl;
}

int main() {
    std::cout << "=== TcpRedirector Service Unit Tests ===" << std::endl;
    std::cout << std::endl;

    // Legacy (WP1..WP3) tests.
    TestWildcardMatch();
    TestPathRule();
    TestGlobalRule();
    TestPriorityOrder();
    TestDisabledRule();
    TestRemoveRule();

    // WP4 port-aware matching tests.
    TestMatchForFlow_NoAppRules_ReturnsUnmatched();
    TestMatchForFlow_ByProcessNameAndPort_Matches();
    TestMatchForFlow_ProcessMatches_PortNot_ReturnsUnmatched();
    TestMatchForFlow_RouteAllTraffic_MatchesAnyPort();
    TestMatchForFlow_PortRange_InclusiveBoundsMatch();
    TestMatchForFlow_MultiplePortsAndRanges_UnionSemantics();
    TestMatchForFlow_TwoRules_FirstMatchWins();
    TestMatchForFlow_ExePathSpecified_MustMatch();
    TestMatchForFlow_ConcurrentSetAndMatch_NoRace();
    TestSetAppRules_ThenSetLegacyRules_BothWorkIndependently();

    std::cout << std::endl;
    std::cout << "All tests passed!" << std::endl;

    return 0;
}