// test_mock_proxy.cpp
// Unit-тесты для mock_proxy (форматирование ответов, чтение аргументов)
// Запускается через Catch2, автообнаружение VS Code Testing

#include <catch2/catch_test_macros.hpp>
#include <string>
#include <cstring>
#include <vector>
#include <sstream>

// ============================================================
// Тестируемые функции — вынесены из mock_proxy.cpp
// ============================================================

struct MockProxyArgs {
    int port = 3128;
    std::string protocol = "tcp";
    bool valid = true;
};

static bool ParseMockProxyArgs(MockProxyArgs& args, int argc, const char* argv[]) {
    args = MockProxyArgs{};
    for (int i = 1; i < argc; i++) {
        if (i + 1 < argc && strcmp(argv[i], "--port") == 0) {
            args.port = atoi(argv[++i]);
            if (args.port <= 0 || args.port > 65535) return false;
        } else if (i + 1 < argc && strcmp(argv[i], "--protocol") == 0) {
            args.protocol = argv[++i];
            if (args.protocol != "tcp" && args.protocol != "udp") return false;
        } else {
            return false;
        }
    }
    args.valid = true;
    return true;
}

static std::string FormatConnectResponse() {
    return "HTTP/1.1 200 Connection established\r\n\r\n";
}

static std::string FormatClientInfo(const char* ip, int port, int client_id) {
    char buf[256];
    snprintf(buf, sizeof(buf),
        "[MOCK_PROXY] --- Client #%d connected: %s:%d ---",
        client_id, ip, port);
    return std::string(buf);
}

static std::string FormatRecvLog(int client_id, int bytes, const char* data, int show_len) {
    char buf[512];
    int actual_show = (bytes < show_len) ? bytes : show_len;
    snprintf(buf, sizeof(buf),
        "[MOCK_PROXY] Client #%d -> received %d bytes: %.*s",
        client_id, bytes, actual_show, data);
    return std::string(buf);
}

// ============================================================
// ТЕСТЫ
// ============================================================

TEST_CASE("ParseMockProxyArgs: default values", "[mock_proxy][args]") {
    MockProxyArgs args;
    const char* argv[] = {"mock_proxy.exe"};
    REQUIRE(ParseMockProxyArgs(args, 1, argv));
    REQUIRE(args.port == 3128);
    REQUIRE(args.protocol == "tcp");
}

TEST_CASE("ParseMockProxyArgs: custom port + UDP", "[mock_proxy][args]") {
    MockProxyArgs args;
    const char* argv[] = {"mock_proxy.exe", "--port", "5353", "--protocol", "udp"};
    REQUIRE(ParseMockProxyArgs(args, 5, argv));
    REQUIRE(args.port == 5353);
    REQUIRE(args.protocol == "udp");
}

TEST_CASE("ParseMockProxyArgs: invalid port 0", "[mock_proxy][args][invalid]") {
    MockProxyArgs args;
    const char* argv[] = {"test", "--port", "0"};
    REQUIRE_FALSE(ParseMockProxyArgs(args, 3, argv));
}

TEST_CASE("ParseMockProxyArgs: invalid port 70000", "[mock_proxy][args][invalid]") {
    MockProxyArgs args;
    const char* argv[] = {"test", "--port", "70000"};
    REQUIRE_FALSE(ParseMockProxyArgs(args, 3, argv));
}

TEST_CASE("ParseMockProxyArgs: invalid protocol", "[mock_proxy][args][invalid]") {
    MockProxyArgs args;
    const char* argv[] = {"test", "--protocol", "sctp"};
    REQUIRE_FALSE(ParseMockProxyArgs(args, 3, argv));
}

TEST_CASE("ParseMockProxyArgs: unknown flag", "[mock_proxy][args][invalid]") {
    MockProxyArgs args;
    const char* argv[] = {"test", "--unknown"};
    REQUIRE_FALSE(ParseMockProxyArgs(args, 2, argv));
}

TEST_CASE("FormatConnectResponse: correct format", "[mock_proxy][response]") {
    auto resp = FormatConnectResponse();
    REQUIRE(resp == "HTTP/1.1 200 Connection established\r\n\r\n");
}

TEST_CASE("FormatConnectResponse: length", "[mock_proxy][response]") {
    auto resp = FormatConnectResponse();
    REQUIRE(resp.size() == 39); // HTTP/1.1 200 Connection established\r\n\r\n
}

TEST_CASE("FormatClientInfo: format", "[mock_proxy][format]") {
    auto s = FormatClientInfo("192.168.1.1", 12345, 1);
    REQUIRE(s.find("Client #1") != std::string::npos);
    REQUIRE(s.find("192.168.1.1") != std::string::npos);
    REQUIRE(s.find("12345") != std::string::npos);
}

TEST_CASE("FormatClientInfo: different client", "[mock_proxy][format]") {
    auto s = FormatClientInfo("10.0.0.5", 80, 42);
    REQUIRE(s.find("Client #42") != std::string::npos);
    REQUIRE(s.find("10.0.0.5") != std::string::npos);
    REQUIRE(s.find("80") != std::string::npos);
}

TEST_CASE("FormatRecvLog: short data", "[mock_proxy][format]") {
    const char* data = "GET / HTTP/1.1\r\n";
    auto s = FormatRecvLog(1, 16, data, 200);
    REQUIRE(s.find("16 bytes") != std::string::npos);
    REQUIRE(s.find("GET") != std::string::npos);
}

TEST_CASE("FormatRecvLog: show_len shorter than data", "[mock_proxy][format]") {
    const char* data = "0123456789ABCDEF";
    auto s = FormatRecvLog(1, 16, data, 5);
    REQUIRE(s.find("16 bytes") != std::string::npos);
    REQUIRE(s.find("01234") != std::string::npos);
    REQUIRE(s.find("56789") == std::string::npos); // truncated
}