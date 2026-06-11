// test_packet_gen.cpp
// Unit-тесты для packet_generator (парсинг аргументов, форматирование пакетов)
// Запускается через Catch2, автообнаружение VS Code Testing

#include <catch2/catch_test_macros.hpp>
#include <string>
#include <vector>
#include <sstream>
#include <cstring>

// ============================================================
// Тестируемые функции — вынесены из packet_generator.cpp
// ============================================================

struct PacketGenArgs {
    std::string dest_ip = "127.0.0.1";
    int dest_port = 3128;
    std::string protocol = "tcp";
    int count = 1;
    bool valid = true;
};

static bool ParsePacketGenArgs(PacketGenArgs& args, int argc, const char* argv[]) {
    args = PacketGenArgs{};
    for (int i = 1; i < argc; i++) {
        if (i + 1 < argc && strcmp(argv[i], "--dest_ip") == 0) {
            args.dest_ip = argv[++i];
        } else if (i + 1 < argc && strcmp(argv[i], "--dest_port") == 0) {
            args.dest_port = atoi(argv[++i]);
            if (args.dest_port <= 0 || args.dest_port > 65535) return false;
        } else if (i + 1 < argc && strcmp(argv[i], "--protocol") == 0) {
            args.protocol = argv[++i];
            if (args.protocol != "tcp" && args.protocol != "udp") return false;
        } else if (i + 1 < argc && strcmp(argv[i], "--count") == 0) {
            args.count = atoi(argv[++i]);
            if (args.count <= 0 || args.count > 10000) return false;
        } else {
            return false; // неизвестный аргумент
        }
    }
    args.valid = true;
    return true;
}

static std::string FormatPacketData(int packet_num) {
    char buf[256];
    int n = snprintf(buf, sizeof(buf),
        "PACKET #%d FROM packet_generator TIME=%llu",
        packet_num, (unsigned long long)0);
    return std::string(buf, n);
}

// ============================================================
// ТЕСТЫ
// ============================================================

TEST_CASE("ParsePacketGenArgs: default values", "[packet_gen][args]") {
    PacketGenArgs args;
    const char* argv[] = {"packet_generator.exe"};
    REQUIRE(ParsePacketGenArgs(args, 1, argv));
    REQUIRE(args.dest_ip == "127.0.0.1");
    REQUIRE(args.dest_port == 3128);
    REQUIRE(args.protocol == "tcp");
    REQUIRE(args.count == 1);
}

TEST_CASE("ParsePacketGenArgs: all params explicit", "[packet_gen][args]") {
    PacketGenArgs args;
    const char* argv[] = {
        "packet_generator.exe",
        "--dest_ip", "10.0.0.1",
        "--dest_port", "8080",
        "--protocol", "udp",
        "--count", "50"
    };
    REQUIRE(ParsePacketGenArgs(args, 9, argv));
    REQUIRE(args.dest_ip == "10.0.0.1");
    REQUIRE(args.dest_port == 8080);
    REQUIRE(args.protocol == "udp");
    REQUIRE(args.count == 50);
}

TEST_CASE("ParsePacketGenArgs: port out of range 0", "[packet_gen][args][invalid]") {
    PacketGenArgs args;
    const char* argv[] = {"test", "--dest_port", "0"};
    REQUIRE_FALSE(ParsePacketGenArgs(args, 3, argv));
}

TEST_CASE("ParsePacketGenArgs: port out of range 65536", "[packet_gen][args][invalid]") {
    PacketGenArgs args;
    const char* argv[] = {"test", "--dest_port", "65536"};
    REQUIRE_FALSE(ParsePacketGenArgs(args, 3, argv));
}

TEST_CASE("ParsePacketGenArgs: invalid protocol", "[packet_gen][args][invalid]") {
    PacketGenArgs args;
    const char* argv[] = {"test", "--protocol", "quic"};
    REQUIRE_FALSE(ParsePacketGenArgs(args, 3, argv));
}

TEST_CASE("ParsePacketGenArgs: count = 0", "[packet_gen][args][invalid]") {
    PacketGenArgs args;
    const char* argv[] = {"test", "--count", "0"};
    REQUIRE_FALSE(ParsePacketGenArgs(args, 3, argv));
}

TEST_CASE("ParsePacketGenArgs: count > 10000", "[packet_gen][args][invalid]") {
    PacketGenArgs args;
    const char* argv[] = {"test", "--count", "10001"};
    REQUIRE_FALSE(ParsePacketGenArgs(args, 3, argv));
}

TEST_CASE("ParsePacketGenArgs: unknown flag", "[packet_gen][args][invalid]") {
    PacketGenArgs args;
    const char* argv[] = {"test", "--unknown_flag"};
    REQUIRE_FALSE(ParsePacketGenArgs(args, 2, argv));
}

TEST_CASE("FormatPacketData: string format", "[packet_gen][format]") {
    auto s = FormatPacketData(1);
    REQUIRE(s.find("PACKET #1") != std::string::npos);
    REQUIRE(s.find("packet_generator") != std::string::npos);
    REQUIRE(s.find("TIME=") != std::string::npos);
}

TEST_CASE("FormatPacketData: packet number 9999", "[packet_gen][format]") {
    auto s = FormatPacketData(9999);
    REQUIRE(s.find("PACKET #9999") != std::string::npos);
}

TEST_CASE("FormatPacketData: does not exceed buffer", "[packet_gen][format]") {
    auto s = FormatPacketData(1);
    REQUIRE(s.size() < 256);
}