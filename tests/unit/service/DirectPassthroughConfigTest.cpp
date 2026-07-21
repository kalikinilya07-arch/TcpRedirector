// DirectPassthroughConfigTest.cpp
//
// Задача 3: юнит-тесты для конфигурации DIRECT-passthrough (Config.h).
// Проверяют:
//   • DirectFallback ⇄ string round-trip и регистро-независимый парсинг;
//   • дефолты WintunSettings (backward-compat: passthrough OFF, fallback drop);
//   • отклонение неизвестных значений (out не трогается).
//
// Config.h — header-only (только <string>/<vector>/<cstdint>), поэтому тест
// не требует линковки с сервисом.  Сборка (из каталога теста):
//   cl /EHsc /std:c++17 DirectPassthroughConfigTest.cpp && DirectPassthroughConfigTest.exe

#include <cassert>
#include <iostream>
#include <string>

#include "../../../src/service/TcpRedirectorService/infrastructure/config/Config.h"

using tcp_redirector::infrastructure::DirectFallback;
using tcp_redirector::infrastructure::DirectFallbackToString;
using tcp_redirector::infrastructure::DirectFallbackFromString;
using tcp_redirector::infrastructure::WintunSettings;

void TestDirectFallbackToString() {
    std::cout << "Test: DirectFallbackToString..." << std::endl;
    assert(std::string(DirectFallbackToString(DirectFallback::Drop))  == "drop");
    assert(std::string(DirectFallbackToString(DirectFallback::Proxy)) == "proxy");
    std::cout << "  PASS" << std::endl;
}

void TestDirectFallbackFromString() {
    std::cout << "Test: DirectFallbackFromString..." << std::endl;
    DirectFallback f = DirectFallback::Proxy;

    assert(DirectFallbackFromString("drop", f) && f == DirectFallback::Drop);
    assert(DirectFallbackFromString("proxy", f) && f == DirectFallback::Proxy);

    // Регистро-независимость.
    f = DirectFallback::Drop;
    assert(DirectFallbackFromString("PROXY", f) && f == DirectFallback::Proxy);
    f = DirectFallback::Proxy;
    assert(DirectFallbackFromString("Drop", f) && f == DirectFallback::Drop);

    // Неизвестное значение → false, out НЕ тронут.
    f = DirectFallback::Proxy;
    assert(!DirectFallbackFromString("bogus", f));
    assert(f == DirectFallback::Proxy); // не изменилось

    std::cout << "  PASS" << std::endl;
}

void TestDirectFallbackRoundTrip() {
    std::cout << "Test: DirectFallbackRoundTrip..." << std::endl;
    for (DirectFallback in : { DirectFallback::Drop, DirectFallback::Proxy }) {
        DirectFallback out = DirectFallback::Drop;
        assert(DirectFallbackFromString(DirectFallbackToString(in), out));
        assert(out == in);
    }
    std::cout << "  PASS" << std::endl;
}

void TestWintunDefaultsBackwardCompat() {
    std::cout << "Test: WintunDefaultsBackwardCompat..." << std::endl;
    WintunSettings w;
    // КРИТИЧНО: по умолчанию passthrough ВЫКЛЮЧЕН → прежнее fail-fast/drop
    // поведение сохраняется (нулевое изменение при отсутствии конфига).
    assert(w.direct_passthrough == false);
    assert(w.direct_fallback == DirectFallback::Drop);
    assert(w.direct_egress_interface.empty());
    // route-optimization по умолчанию true, но активна только при passthrough=true.
    assert(w.direct_route_optimization == true);
    std::cout << "  PASS" << std::endl;
}

int main() {
    std::cout << "=== DirectPassthroughConfigTest ===" << std::endl;
    TestDirectFallbackToString();
    TestDirectFallbackFromString();
    TestDirectFallbackRoundTrip();
    TestWintunDefaultsBackwardCompat();
    std::cout << "ALL TESTS PASSED" << std::endl;
    return 0;
}
