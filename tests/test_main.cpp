// test_main.cpp
// Главная точка входа для Catch2 (Catch2WithMain предоставляет main())
// Тесты автоматически обнаруживаются VS Code Testing Tab

#define CATCH_CONFIG_MAIN
#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_session.hpp>

// Эта функция вызывается перед всеми тестами (если нужно)
// Можно оставить пустой — Catch2 сам управляет жизненным циклом
