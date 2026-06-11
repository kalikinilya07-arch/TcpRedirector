# Статус проекта TcpRedirector

## ✅ Реализовано

### 1. Конфигурационная система (ConfigManager)
- **Config.h** — полная структура Config с 5 секциями: app, proxy, auth, log, stats
- **ConfigManager.h/.cpp** — загрузка/сохранение config.json в `%ProgramData%\TcpRedirector\`, thread-safe (shared_mutex), DPAPI-шифрование пароля, listener-механизм
- JSON-формат с `exePath`, `proxy.enabled`, настройками уровней логирования
- ✅ **Исправлен баг**: двойной временный объект в LoadImpl() (UB — итераторы от разных std::string)
- ✅ **Исправлен баг**: SetProxyConfig() вызывал Save() — затирал config.json; заменён на UpdateConfigNoSave()

### 2. Система логирования (AsyncLogger → Logger)
- **Logger.h/.cpp** — асинхронная очередь (mutex + cv + WriterThread), batch-pop, ring buffer (2000 записей)
- Цветной вывод в консоль, ротация файлов при превышении maxSizeMB
- Listener-механизм для подписки GUI
- ✅ **Исправлен баг**: фильтр уровней был инвертирован — `Warn(3) > Info(2)` → фильтровались (заменено на правильную проверку)

### 3. Система сбора статистики (StatsCollector)
- **StatsCollector.h/.cpp** — lock-free атомарные счётчики (fetch_add relaxed)
- OnPacket() + GetStats() + Reset()
- Минимальное/максимальное время через compare_exchange_weak
- Per-second rate через дифференциальное вычисление в GetStats()

### 4. Замена хардкода на ConfigManager
- **ServiceMain.h**: proxy host/port читается из ConfigManager вместо хардкода
- **ServiceMain.h**: правило фильтрации создаётся из exePath конфига (ProcessPath)
- **WinDivertCapture.h**: SetTargetProcess() вызывается из конфига
- **ICapture.h**: добавлен SetTargetProcess() в порт

### 5. Таймстемпы в debug-лог
- **WinDivertCapture.cpp**: LOG() макрос теперь включает `[YYYY-MM-DD HH:MM:SS.mmm]`

### 6. Интеграционный тест
- **integration_test.bat** — двухфазный тест (Phase 1: packet_generator → mock_proxy, Phase 2: WinDivert)
- **prepare_test_build.bat** — подготовка тестовой сборки
- **run_integration_test.bat** — запуск теста

### 7. Проектная документация
- **ANALYSIS.md** — архитектурный анализ (4 ключевых участка, функции НЕ ТРОГАТЬ)
- **CONFIG_DESIGN.md** — дизайн конфигурации
- **LOGGER_DESIGN.md** — дизайн логгера
- **STATS_DESIGN.md** — дизайн статистики
- **REVIEW.md** — cross-review (7 найденных проблем)
- **GUI_DESIGN.md** — дизайн WPF GUI с 3 вкладками

---

## 🔄 В процессе / Известные проблемы

### 1. Редирект через прокси — не полный
- **Текущее состояние**: SYN блокируется, ProxySession создаётся, CONNECT отправляется, но `m_localSocket = INVALID_SOCKET` — данные из прокси-туннеля падают в пустоту
- **Причина**: при рефакторинге удалены функции `RedirectFlow` и packet injection (спуфинг SYN+ACK)
- **Сейчас трафик идёт напрямую** (SYN не блокируется), прокси-туннель создаётся для мониторинга и статистики
- **Нужно**: восстановить packet injection для полного редиректа

### 2. WinDivert.dll — ручное копирование
- Бинарь в `build\service\x64\Release\` не содержит WinDivert.dll
- **Решение**: бинарь копируется в `C:\Users\user\Desktop\TcpRedirector\build\service\` (там лежит WinDivert.dll)

### 3. Конфиг затирается старым бинарём
- Старый бинарь (без ConfigManager) при старте вызывает SetProxyConfig() → Save() → перезаписывает config.json в старом формате (без секции "app")
- **Решение**: запускать только новый бинарь, старый не использовать

### 4. Логгер — некоторые Warn/Error не пишутся при уровне Info
- Исправлено в коде, но требует пересборки для применения

---

## 📋 План дальнейших работ

### Этап 5 (следующий): GUI каркас с тремя вкладками
- C# WPF приложение (TcpRedirectorGUI)
- 3 вкладки: Settings, Logs, Stats
- IPC через Named Pipe (IpcClient.cs реализован)
- ServiceController.cs для управления Windows Service (start/stop/restart)

### Этап 6: Восстановление полного редиректа
- Добавить packet injection (спуфинг SYN+ACK) на основе оригинального кода из git commit 93e8185 или переписать
- Настроить m_localSocket для BridgeLoop
- Замкнуть цепочку: SYN → блок → SYN+ACK спуфинг → туннель → бридж

### Этап 7: Unit-тесты
- Catch2 тесты (инфраструктура готова, CMakeLists.txt + test_main.cpp)
- 23 теста проходят через MinGW + CTest

### Этап 8: Интеграционное тестирование (Phase 2)
- WinDivert требует прав администратора
- Проверить редирект реального трафика через mock_proxy