#include <cassert>
#include <iostream>
#include "../../../src/service/TcpRedirectorService/domain/services/RuleEngine.h"

using namespace tcp_redirector::domain::services;
using namespace tcp_redirector::domain;

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

int main() {
    std::cout << "=== TcpRedirector Service Unit Tests ===" << std::endl;
    std::cout << std::endl;

    TestWildcardMatch();
    TestPathRule();
    TestGlobalRule();
    TestPriorityOrder();
    TestDisabledRule();
    TestRemoveRule();

    std::cout << std::endl;
    std::cout << "All tests passed!" << std::endl;

    return 0;
}