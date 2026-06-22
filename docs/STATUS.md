# Статус проекта TcpRedirector

## ✅ Реализовано

### 1. Конфигурационная система (ConfigManager)
- **Config.h** — полная структура Config с 5 секциями: app, proxy, auth, log, stats
- **ConfigManager.h/.cpp** — загрузка/сохранение config.json в `%ProgramData%\TcpRedirector\`, thread-safe (shared_mutex), DPAPI-шифрование пароля, listener-механизм
- JSON-формат с `exePath`, `proxy.enabled`, настройками уровней логирования
- ✅ **Исправлен баг**: двойной временный объект в LoadImpl() (UB — итераторы от разных std::string)
- ✅ **Исправлен баг**: SetProxyConfig() вызывал Save() — затирал config.json; заменён на UpdateConfigNoSave()

### 2. Система логирования (Logger)
- **Logger.h/.cpp** — асинхронная очередь (mutex + cv + WriterThread), batch-pop, ring buffer (2000 записей)
- Цветной вывод в консоль, ротация файлов при превышении maxSizeMB
- Listener-механизм для подписки GUI
- ✅ **Исправлен баг**: фильтр уровней был инвертирован

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

## 🔄 Известные особенности

### 1. ProxyBridge / RedirectFlow — не реализованы
- **Текущее состояние**: SYN детектится, RedirectEvent создаётся, HandleRedirect вызывает ProxySession → ConnectToProxy → CONNECT отправляется
- **m_localSocket = INVALID_SOCKET** — BridgeLoop не может пересылать данные, т.к. нет локального сокета
- **Трафик идёт напрямую** (shouldBlock = false, SYN проходит), прокси-сессия создаётся для мониторинга
- **Вывод**: полный редирект через HTTP-прокси с блокировкой SYN НИКОГДА не был реализован в этой кодовой базе. Оригинальный бинарь имел те же ограничения (лог: "Tunnel failed: No response from proxy")

### 2. WinDivert.dll — ручное копирование
- Бинарь в `build\service\x64\Release\` не содержит WinDivert.dll
- **Решение**: бинарь копируется в `C:\Users\user\Desktop\TcpRedirector\build\service\`

### 3. Конфиг затирается старым бинарём
- Старый бинарь (до ConfigManager) при старте вызывает SetProxyConfig() → Save() → перезаписывает config.json в старом формате
- **Решение**: запускать только новый бинарь

### 4. Фильтр логгера
- `Warn(3) > Info(2)` → Warn/Error НЕ пишутся при уровне Info. Для DEBUG-сообщений выставить `"level": 3` в config.json

---

## 📋 План дальнейших работ

### Этап 5: GUI каркас с тремя вкладками
- C# WPF приложение (TcpRedirectorGUI)
- 3 вкладки: Settings, Logs, Stats
- IPC через Named Pipe (IpcClient.cs реализован)
- ServiceController.cs для управления Windows Service

### Этап 6: Интеграционное тестирование Phase 2
- WinDivert требует прав администратора
- Проверить захват и мониторинг трафика через mock_proxy

### Этап 7 (опционально): Полный редирект через прокси
Для реализации нужно:
1. Packet injection (спуфинг SYN+ACK от имени оригинального destination)
2. Создание m_localSocket и связывание с BridgeLoop
3. Перенаправление data-пакетов через прокси-туннель вместо прямого Send