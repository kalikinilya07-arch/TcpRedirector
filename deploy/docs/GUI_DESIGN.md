# Проектирование GUI с тремя вкладками

## 0. Контекст: C++ сервис + C# GUI через IPC

**Текущая архитектура:**
```
[C++ Service]                   Named Pipe            [C# WPF GUI]
  ConfigManager  ◄── IPC: get_config/set_config ────► SettingsTab
  AsyncLogger    ◄── IPC: get_logs                  ──► LogsTab
  StatsCollector ◄── IPC: get_stats/reset_stats    ──► StatsTab
  WinDivertCapture ── (напрямую не доступен из GUI)
```

GUI **НЕ может** напрямую вызывать C++ методы. Всё — через IPC.
IpcHandler (C++) маршрутизирует IPC-запросы к ConfigManager/Logger/StatsCollector.

---

## 1. Главное окно (MainWindow)

### 1.1. Компоновка

```
┌─────────────────────────────────────────────────────────────┐
│ [Название приложения]                                   _ □ X │
├─────────────────────────────────────────────────────────────┤
│ [Настройки]  [Логи]  [Статистика]   ← TabControl           │
├─────────────────────────────────────────────────────────────┤
│                                                             │
│           (содержимое активной вкладки)                     │
│                                                             │
├─────────────────────────────────────────────────────────────┤
│ Статус-бар: [Сервис: ЗАПУЩЕН] [Пакетов: 1 234] [Ошибок: 0] │
└─────────────────────────────────────────────────────────────┘
```

### 1.2. Статус-бар

| Элемент | Источник данных | IPC метод |
|---------|----------------|-----------|
| Состояние сервиса | m_running / m_initialized | `get_status` |
| Всего пакетов | StatsSnapshot.totalPackets | `get_stats` |
| Ошибок | StatsSnapshot.errors | `get_stats` |
| Соединений | ConnectionTracker | `get_connections` |

Обновление статус-бара: таймер 2 секунды → `get_stats` + `get_status`.

### 1.3. Иконка в трее

- При сворачивании — в трей
- В контекстном меню: "Показать", "Выход"
- Нотификации при ошибках (красная иконка)

---

## 2. Вкладка "Настройки" (SettingsTab)

### 2.1. Элементы

```
┌─────────────────────────────────────────────────────────────┐
│  Настройки приложения                                       │
│  ┌───────────────────────────────────────────────────────┐  │
│  │ Путь к EXE: [C:\...\TransfersClient.exe] [Browse…]   │  │
│  │ Имя EXE:   TransfersClient.exe  (авто, read-only)    │  │
│  └───────────────────────────────────────────────────────┘  │
│                                                             │
│  Настройки прокси                                           │
│  ┌───────────────────────────────────────────────────────┐  │
│  │ ☑ Перенаправлять на прокси                           │  │
│  │ Хост:  [127.0.0.1              ]  Порт: [3128      ] │  │
│  └───────────────────────────────────────────────────────┘  │
│                                                             │
│  Авторизация                                                │
│  ┌───────────────────────────────────────────────────────┐  │
│  │ ☑ Требуется авторизация                              │  │
│  │ Логин: [user                    ]                     │  │
│  │ Пароль: [********               ]  [Сменить пароль]  │  │
│  │ ☐ Использовать Kerberos/Negotiate                    │  │
│  └───────────────────────────────────────────────────────┘  │
│                                                             │
│  Логирование                                                │
│  ┌───────────────────────────────────────────────────────┐  │
│  │ Уровень: [▼ Info                        ]             │  │
│  │ ☑ Писать в файл                                      │  │
│  │ Макс. размер: [10] MB                                │  │
│  └───────────────────────────────────────────────────────┘  │
│                                                             │
│                                                             │
│              [Сохранить]  [Отмена]  [По умолчанию]          │
└─────────────────────────────────────────────────────────────┘
```

### 2.2. Жизненный цикл

1. **При открытии вкладки**: IPC `get_config` → заполнить поля Config
2. **При изменении поля**: пометить как "изменено" (но не сохранять сразу)
3. **Кнопка "Сохранить"**: IPC `set_config` — отправляет весь Config целиком
4. **Кнопка "Отмена"**: IPC `get_config` → сброс всех полей к исходным
5. **Кнопка "По умолчанию"**: заполнить дефолтными значениями (без IPC)

### 2.3. Обновление фильтра WinDivert при изменении настроек

При изменении `exePath` или `proxy.enabled` **C++ сервис** должен динамически обновить WinDivertCapture без перезапуска.

**Механизм:**

```
GUI: set_config → IPC → IpcHandler::SetConfig → ConfigManager::UpdateConfig()
                                                      │
                                                      ▼
                                              NotifyListeners()
                                                      │
                                                      ▼
                                              WinDivertCapture::OnConfigChanged()
                                                      │
                                                      ├── если exePath изменился:
                                                      │   m_targetProcessPath = newExePath;
                                                      │   m_targetPid = 0;  // сброс PID
                                                      │
                                                      └── если proxy.enabled изменился:
                                                          m_proxyEnabled.store(newValue);
                                                          // CaptureLoop читает atomic — без блокировки
```

**Почему не нужно пересоздавать WinDivert handle:**
- Фильтр WinDivert (`"true"`) — захватывает ВСЕ пакеты
- Решение о блокировке/пропуске — в `CaptureLoop` (C++ код)
- Изменение `exePath` → `IsTargetProcess()` начнёт искать новый процесс
- Изменение `proxy.enabled` → `m_proxyEnabled` atomic → CaptureLoop либо создаёт RedirectEvent, либо нет

**Никакого перезапуска WinDivert не требуется.**

---

## 3. Вкладка "Логи" (LogsTab)

### 3.1. Элементы

```
┌─────────────────────────────────────────────────────────────┐
│  ☑ Автопрокрутка           [Очистить]  [Сохранить в файл]   │
├─────────────────────────────────────────────────────────────┤
│                                                             │
│  [2026-06-11 14:30:45] [INFO ] [service    ] Initialized    │
│  [2026-06-11 14:30:46] [DEBUG] [capture    ] #1 SYN ...     │
│  [2026-06-11 14:30:47] [ERROR] [proxy      ] connect fail   │
│  [2026-06-11 14:30:48] [INFO ] [redirect   ] Redirected:... │
│                                                             │
└─────────────────────────────────────────────────────────────┘
```

### 3.2. Получение логов: Push + Pull

**Вариант A: Pull (работает сейчас)**
- Таймер 2 секунды: IPC `get_logs` → JSON → отображение
- Ring buffer в Logger хранит 2000 записей
- Каждый вызов — возвращает все записи, GUI показывает только новые (по timestamp)

**Вариант B: Push (рекомендуемый)**
- Logger::RegisterListener() регистрирует callback, который отправляет каждую запись в Named Pipe
- GUI получает лог в реальном времени
- Для этого нужен отдельный IPC-канал (PushChannel) или расширение существующего PipeServer

**Решение: комбинированный подход.**
1. При старте: Pull `get_logs` (загрузить последние 100 записей)
2. После старта: Push через отдельный Named Pipe `TcpRedirectorLogPipe`
3. Если Push отвалился (GUI закрыт) — при следующем открытии Pull

### 3.3. Кнопки

| Кнопка | Действие |
|--------|----------|
| **Очистить** | Очищает `ListView.ItemsSource` (только GUI, не трогает Logger) |
| **Сохранить в файл** | SaveFileDialog → запись текущего содержимого в выбранный файл |
| **Автопрокрутка** | Если включена — ScrollToEnd() при каждом новом логе |

### 3.4. Цветовая кодировка строк

| Уровень | Цвет в GUI | Фон |
|---------|-----------|-----|
| ERROR | Красный | #FF4444 |
| WARN | Жёлтый | #FFD700 |
| INFO | Белый | #FFFFFF |
| DEBUG | Серый | #888888 |
| TRACE | Тёмно-серый | #555555 |

---

## 4. Вкладка "Статистика" (StatsTab)

### 4.1. Элементы

```
┌─────────────────────────────────────────────────────────────┐
│  Общая статистика                    [Сбросить статистику]  │
│                                                             │
│  Всего пакетов:    12 345                                    │
│  TCP пакетов:      12 300                                    │
│  UDP пакетов:      45                                        │
│  Перенаправлено:   1 234                                     │
│  Ошибок:           0                                         │
│                                                             │
│  Размеры пакетов:                                            │
│  Всего байт:       15 678 901                                │
│  Средний:          1 270 байт                                │
│  Минимальный:      60 байт                                   │
│  Максимальный:     65 535 байт                               │
│                                                             │
│  Скорость:                                                   │
│  Пакетов/сек:      1 234                                     │
└─────────────────────────────────────────────────────────────┘
```

### 4.2. Обновление

- Таймер: **2000 мс** (берётся из `Config.stats.updateIntervalMs`)
- IPC `get_stats` → `StatsCollector::GetStats()` → отображение
- GUI сравнивает предыдущее значение с новым:
  - Если значение выросло — зелёный цвет на 1 секунду
  - Если ошибки > 0 — красный цвет

### 4.3. Кнопка "Сбросить статистику"

- IPC `reset_stats` → `StatsCollector::Reset()` → GUI обнуляет отображение

---

## 5. Потоки и синхронизация

### 5.1. Потоки

| Поток | Компонент | Что делает |
|-------|-----------|------------|
| **GUI Main** | C# WPF | Message loop, таймеры, UI update |
| **CaptureLoop** | WinDivertCapture (C++) | Recv/Send, OnPacket (StatsCollector), Log (AsyncLogger) |
| **WriterThread** | AsyncLogger (C++) | Запись логов в файл, уведомление listener'ов |
| **IPC Named Pipe** | PipeServer (C++) | Обработка IPC-запросов от GUI |
| **Timer** | GUI (C#) | Каждые 2с: get_stats, get_status |

### 5.2. Синхронизация между потоками

```
GUI Main (C#)                  IPC                    Сервис (C++)
     │                          │                         │
     │── set_config ───────────►│                         │
     │                          │── IpcHandler::SetConfig─►│
     │                          │   ConfigManager::       │
     │                          │     UpdateConfig()      │
     │                          │     shared_mutex lock   │
     │                          │     Save to JSON        │
     │                          │     NotifyListeners()   │
     │                          │         │               │
     │                          │         ▼               │
     │                          │   WinDivertCapture::    │
     │                          │     OnConfigChanged()   │
     │                          │     (m_proxyEnabled    │
     │                          │      atomic store)      │
     │                          │                         │
     │◄── response (success) ───│                         │
     │                          │                         │
     │ Обновить UI              │                         │
```

**Примитивы синхронизации:**

| Граница | Примитив | Назначение |
|---------|----------|------------|
| GUI → IPC | Named Pipe (блокирующий) | Сериализация запросов |
| IPC → ConfigManager | shared_mutex | Чтение: shared_lock, запись: unique_lock |
| IPC → StatsCollector | atomic (lock-free) | GetStats: load(), Reset: store() |
| IPC → Logger | queue + mutex + cv | Push в асинхронную очередь |
| CaptureLoop → StatsCollector | atomic (relaxed) | OnPacket: fetch_add |
| CaptureLoop → Logger | queue + mutex + cv | Push в очередь (быстро) |
| Logger → GUI (push) | Named Pipe | Отдельный канал для логов |

### 5.3. Почему нет deadlock'ов

- **Named Pipe**: не блокирует сервис (PipeServer в отдельном потоке)
- **shared_mutex**: читатели не блокируют друг друга, писатель ждёт всех читателей
- **atomic**: lock-free, нет блокировок
- **Logger queue**: push — микросекунды, writer thread — batch pop

---

## 6. IPC-протокол (расширение существующего)

Существующие методы IPC (из IpcHandler.h):

| Метод | Параметры | Описание |
|-------|-----------|----------|
| `get_config` | — | Возвращает весь Config |
| `set_config` | Config JSON | Обновляет конфигурацию |
| `get_rules` | — | Возвращает правила |
| `set_rules` | Rules JSON | Обновляет правила |
| `get_connections` | — | Активные соединения |
| `get_logs` | — | Последние 500 логов |
| `get_stats` | — | StatsSnapshot |
| `service_status` | — | running/initialized |

**Новые методы:**

| Метод | Параметры | Описание |
|-------|-----------|----------|
| `reset_stats` | — | Сброс StatsCollector |
| `set_log_level` | `{ "level": 2 }` | Уровень лога |
| `get_status` | — | running, initialized, packets, errors |

---

## 7. Новые файлы (C# WPF)

```
TcpRedirector/src/gui/TcpRedirectorGUI/
├── Adapters/
│   └── Driving/
│       └── Wpf/
│           ├── MainWindow.xaml          (главное окно)
│           ├── MainWindow.xaml.cs       (code-behind)
│           ├── App.xaml                 (точка входа)
│           ├── App.xaml.cs
│           ├── ViewModels/
│           │   ├── MainViewModel.cs     (общая VM для Window + статус-бар)
│           │   ├── SettingsViewModel.cs (поля Config + команды)
│           │   ├── LogsViewModel.cs     (ObservableCollection логов)
│           │   └── StatsViewModel.cs    (свойства статистики + таймер)
│           └── Views/
│               ├── SettingsTab.xaml
│               ├── SettingsTab.xaml.cs
│               ├── LogsTab.xaml
│               ├── LogsTab.xaml.cs
│               ├── StatsTab.xaml
│               └── StatsTab.xaml.cs
│
├── Infrastructure/
│   └── Ipc/
│       └── IpcClient.cs   (уже существует — расширить новыми методами)
│
└── Domain/
    └── Ports/
        └── ITcpRedirectorService.cs  (уже существует — расширить интерфейс)
```

---

## 8. Схема взаимодействия (полная)

```
                    ┌──────────────────────┐
                    │     GUI (C# WPF)      │
                    │  ┌────────────────┐   │
                    │  │ MainWindow     │   │
                    │  │  ├─ TabControl │   │
                    │  │  │  ├ SettingsTab   │
                    │  │  │  ├ LogsTab       │
                    │  │  │  └ StatsTab      │
                    │  │  └── StatusBar  │   │
                    │  └────────────────┘   │
                    │         │              │
                    │    IPC через Named Pipe│
                    └─────────┬──────────────┘
                              │
                    ┌─────────▼──────────────┐
                    │   PipeServer (C++)     │
                    │   IpcHandler           │
                    └──────┬───────┬───────┬─┘
                           │       │       │
              ┌────────────▼──┐ ┌──▼──┐ ┌──▼────────────┐
              │ ConfigManager │ │Log- │ │StatsCollector │
              │  Config       │ │ger  │ │  OnPacket()   │
              │  Rules        │ │Ring │ │  GetStats()   │
              │  DPAPI        │ │Buf  │ │  Reset()      │
              │  Listeners    │ │Push │ └───────┬────────┘
              └───────┬───────┘ └──▲───┘         │
                      │            │             │
                      │            │    ┌────────▼────────┐
                      │            │    │WinDivertCapture │
                      │            │    │  CaptureLoop()  │
                      │            │    │  OnPacket(info) │
                      │            │    └─────────────────┘
                      │            │
              ┌───────▼───────┐    │
              │ ProxyEngine   │    │
              │  ConnectToProxy │  │
              └───────────────┘    │
                           ┌──────▼───────┐
                           │  Logger      │
                           │  AsyncQueue  │
                           │  WriterThread│
                           │  File I/O    │
                           └──────────────┘
```

---

## 9. Резюме

| Аспект | Решение |
|--------|---------|
| **GUI технология** | C# WPF (существующая) |
| **Связь с сервисом** | Named Pipe IPC |
| **Вкладки** | 3: Settings, Logs, Stats |
| **Обновление настроек без перезапуска** | Listener callback → atomic флаг в CaptureLoop |
| **Логи** | Pull при старте (get_logs) + Push через Named Pipe |
| **Статистика** | Pull (get_stats) по таймеру 2с |
| **Статус-бар** | running/initialized + пакеты + ошибки |
| **Новые IPC методы** | `reset_stats`, `get_status` |
| **Новые C# файлы** | ~14 (MainWindow, 3 вкладки, 4 VM + расширения) |
| **Изменения в C++** | PipeServer → PushChannel для логов (опционально) |