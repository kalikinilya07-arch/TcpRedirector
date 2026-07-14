cmake_minimum_required(VERSION 3.20)
project(TcpRedirectorAuthAgent VERSION 1.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# nlohmann/json (header-only)
find_package(nlohmann_json REQUIRED)

add_executable(TcpRedirectorAuthAgent
    main.cpp
    SspiEngine.cpp
    ContextStore.cpp
)

target_include_directories(TcpRedirectorAuthAgent PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${nlohmann_json_INCLUDE_DIRS}
)

target_link_libraries(TcpRedirectorAuthAgent PRIVATE
    secur32.lib
    advapi32.lib
)

# Windows subsystem: console (для отладки)
set_target_properties(TcpRedirectorAuthAgent PROPERTIES
    WIN32_EXECUTABLE OFF
)

# Копирование в выходную директорию
set_target_properties(TcpRedirectorAuthAgent PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
)