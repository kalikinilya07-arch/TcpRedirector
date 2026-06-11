cmake_minimum_required(VERSION 3.20)
project(TcpRedirectorTests LANGUAGES CXX)

# ============================================================
# Стандарт C++
# ============================================================
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# ============================================================
# CTest — интеграция с VS Code Testing
# ============================================================
include(CTest)
enable_testing()

# ============================================================
# Catch2 через FetchContent (самостоятельно скачивается)
# ============================================================
include(FetchContent)
FetchContent_Declare(
    Catch2
    GIT_REPOSITORY https://github.com/catchorg/Catch2.git
    GIT_TAG v3.7.1
)
FetchContent_MakeAvailable(Catch2)
list(APPEND CMAKE_MODULE_PATH "${catch2_SOURCE_DIR}/extras")

# ============================================================
# Общие исходники тестов
#   packet_generator.cpp  — тестируем парсинг аргументов + формат пакетов
#   mock_proxy.cpp        — тестируем формат ответов + обработку данных
# ============================================================
set(TEST_SOURCES
    test_main.cpp
    test_packet_gen.cpp
    test_mock_proxy.cpp
)

# ============================================================
# Сборка тестового исполняемого файла
# ============================================================
add_executable(tcp_redirector_tests ${TEST_SOURCES})

target_link_libraries(tcp_redirector_tests
    PRIVATE
        Catch2::Catch2WithMain
        ws2_32
)

target_include_directories(tcp_redirector_tests
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${CMAKE_CURRENT_SOURCE_DIR}/catch2
)

# ============================================================
# Регистрация тестов в CTest
# ============================================================
include(Catch)
catch_discover_tests(tcp_redirector_tests)