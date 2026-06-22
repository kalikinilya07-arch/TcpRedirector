# Аудит гексагональной архитектуры TcpRedirectorService

Дата аудита: 2026-06-22  
Ветка: `feature/dst-modification-relay`

---

## 1. Соответствие структуры проекта

Структура директорий в целом следует гексагональной архитектуре:

```
TcpRedirectorService/
├── main.cpp                         ← точка входа (composition root)
├── adapters/
│   ├── driven/                      ← исходящие адаптеры
│   └── driving/                     ← входящие адаптеры
├── domain/
│   ├── entities/                    ← сущности домена
│   ├── ports/                       ← порты (интерфейсы)
│   └── services/                    ← сервисы домена
└── infrastructure/                  ← инфраструктура (5 подсистем)
```

**Оценка: структура корректна.** Слои domain → infrastructure → adapters соблюдены.

---

## 2. Детальный анализ каждого слоя

### 2.1 Domain / Ports (`domain/ports/`)

| Файл | Назначение | Статус |
|------|-----------|--------|
| `IProxyConnector.h` | Порт для прокси-соединений | ✅ Чистый интерфейс |
| `ICapture.h` | Порт захвата трафика (ISP) | ✅ Чистый интерфейс |
| `IDriverCommunicator.h` | Порт драйвера (только IOCTL) | ✅ Чистый интерфейс |
| `IConnectionMonitor.h` | Мониторинг соединений | ✅ Чистый интерфейс |
| `IConfigStore.h` | Хранилище конфигов | ✅ Чистый интерфейс |

**Нарушения:**
- `IDriverCommunicator` содержит методы `QueryProcess` и `GetEventHandle`, которые не относятся к IOCTL-драйверу, а специфичны для WinDivert. Это нарушение ISP (Interface Segregation) — интерфейс слишком широкий.
- `ICapture` пересекается с `IDriverCommunicator` (оба содержат `Open`, `Close`, `IsOpen`, `GetPendingRedirects`, `AckRedirect`). Дублирование обязанностей — два порта, которые по сути описывают одно и то же.

### 2.2 Domain / Entities (`domain/entities/`)

| Файл | Назначение | Статус |
|------|-----------|--------|
| `ProxyConfig.h` | Конфиг прокси, правила, события | ✅ Без зависимостей от инфраструктуры |

**Нарушения:**
- Структура `Rule` содержит `created`/`modified` (временные метки) — это persistence-аспект, который лучше вынести в инфраструктурный слой.
- Пароль хранится как `encrypted_password` (зашифрованный) — вопрос: шифрование происходит на уровне entities или persistence? Если в entities, это нарушение, т.к. entities не должны знать о шифровании.

### 2.3 Domain / Services (`domain/services/`)

| Файл | Назначение | Статус |
|------|-----------|--------|
| `ConnectionTracker.h` | Трекинг соединений | ⚠️ Не читался |
| `RuleEngine.h` | Правила маршрутизации | ⚠️ Не читался |

**Потенциальная проблема:** `ConnectionTracker` — это может быть логика, которая дублирует `ConnectionTable` из `infrastructure/relay/`. Если `ConnectionTracker` — это domain-сервис, а `ConnectionTable` — инфраструктура, то связь должна быть через порт, а не дублированием.

---

## 3. Анализ границ слоёв

### 3.1 Зависимости (правильные)

```
domain/ → ничего (чистые интерфейсы)
infrastructure/ → domain/ (реализует порты)
adapters/ → domain/ (вызывает порты через composition root)
```

**Примеры правильных зависимостей:**
- `WinDivertCapture.h` : `public domain::ports::IDriverCommunicator` ✅
- `ServiceMain.h` использует `IDriverCommunicator*` для capture ✅
- `ConfigManager.h` : `public domain::ports::IConfigStore` ✅

### 3.2 Нарушения изоляции

**Проблема: инфраструктура содержит "голые" классы без портов**

Следующие классы не реализуют доменный порт:

| Класс | Файл | Проблема |
|-------|------|----------|
| `ConnectionTable` | `infrastructure/relay/ConnectionTable.h` | Голая структура данных, не порт |
| `TcpRelayServer` | `infrastructure/relay/TcpRelayServer.h` | Нет порта — ServiceMain напрямую вызывает relay |
| `WinDivertCapture` | `infrastructure/capture/WinDivertCapture.h` | Реализует `IDriverCommunicator`, но содержит методы `SetTargetProcess`, `SetRelayPort`, `SetProxyConfig`, `SetConnectionTable` — которых нет в порте |
| `TcpRelayServer` | вызывает `m_connTable` (голая ссылка на ConnectionTable) | Нет порта для релея |

**Рекомендация:** для `ConnectionTable` и `TcpRelayServer` нужно создать порты в `domain/ports/`.

### 3.3 Смешение адаптеров и инфраструктуры

`WinDivertCapture` находится в `infrastructure/capture/`, но реализует `IDriverCommunicator` (порт из `domain/ports/`). Формально это адаптер, а не инфраструктура. По гексагональной архитектуре **адаптеры** — это то, что реализует порты для внешнего мира.

Правильное размещение:
- `WinDivertCapture` → `adapters/driven/` (т.к. это implementation порта IDriverCommunicator)
- `TcpRelayServer`, `ConnectionTable`, `PipeServer`, `Logger`, `ConfigManager` → по логике это infrastructure + адаптеры

Фактически папка `infrastructure/` содержит смесь:
- **Чистая инфраструктура** (WinDivert API, сокеты)
- **Исходящие адаптеры** (WinDivertCapture — реализация IDriverCommunicator, ConfigManager — реализация IConfigStore, Logger — реализация ILogSink)

---

## 4. Composition Root

`main.cpp` содержит `ServiceMain()` — функцию, которая вызывает `TcpRedirectorService::Initialize()` из `ServiceMain.h`.

`ServiceMain.h` — это фактически **composition root в заголовочном файле**. Весь граф зависимостей собирается в `Initialize()`:
- new Logger → new ConfigManager (log sink) → new WinDivertCapture → ...

**Нарушение:** composition root в `.h`-файле — плохая практика для C++. Все зависимости создаются в конструкторе класса, а не в отдельной функции main.
- Усложняет тестирование (нет способа подменить зависимости через конструктор)
- Смешивает логику композиции с бизнес-логикой сервиса
- Класс `TcpRedirectorService` хранит прямые ссылки на `WinDivertCapture*`, `TcpRelayServer*`, `ConnectionTable*` вместо абстракций (портов)

---

## 5. Тестируемость

### 5.1 Проблемы

- **WinDivertCapture.h** содержит статические bitmap (`m_portDecided[2048]`, `m_portDirect[2048]`) 
- **ConnectionTable** — конкретный класс, а не интерфейс. ServiceMain хранит `ConnectionTable*`, и `TcpRelayServer` получает прямую ссылку. Невозможно подмокнуть для тестов.
- **Logger** — конкретный класс, не порт (хотя Logger.h определяет `ILogSink` в `domain/ports/`, но используется конкретный класс)
- **TcpRelayServer** лично управляет сокетами (`WSASocket`, `bind`, `listen`, `accept`) — всё внутри класса, не подменяемо для тестов

### 5.2 Что хорошо

- `ICapture` IS: используется в ServiceMain как абстракция, хотя некоторые методы capture вызываются напрямую
- `IConfigStore` IS: ConfigManager реализует через порт
- SSL/шифрование паролей изолировано

---

## 6. Итоговые нарушения (от критических к косметическим)

### ❌ Критические

1. **Composition root в .h-файле** — `ServiceMain.h` делает `new` для всех зависимостей, жёстко связывая реализации. Невозможно тестировать изолированно.

2. **Отсутствие портов для Relay и ConnectionTable** — `TcpRelayServer` и `ConnectionTable` не имеют доменных интерфейсов. ServiceMain хранит конкретные ссылки, а не абстракции.

3. **WinDivertCapture не реализует собственные методы через порт** — `SetTargetProcess`, `SetRelayPort`, `SetProxyConfig`, `SetConnectionTable` — это сигнатура адаптера, которой нет в доменном порте. Любая замена WinDivertCapture на другой драйвер потребует изменения composition root.

### ⚠️ Серьёзные

4. **Дублирование портов IDriverCommunicator и ICapture** — два интерфейса с практически одинаковой сигнатурой. Нарушение ISP и DRY.

5. **Инфраструктура размыта** — `infrastructure/` содержит как чистую инфраструктуру (sockets), так и адаптеры (WinDivertCapture, ConfigManager). Нарушение чёткости слоёв.

### 🔧 Средние

6. **ConnectionTable используется напрямую** — вместо абстракции `IConnectionTable` везде используется конкретный класс.

7. **Статические члены класса в WinDivertCapture** — `m_portDecided[2048]` и `m_portDirect[2048]` — статические `LONG` массивы. Это global state, не совместимый с IoC-контейнерами и тестированием.

### 📝 Косметические

8. **Entity содержит persistence-данные** — `Rule.created`/`Rule.modified` — временные метки не являются бизнес-логикой, а относятся к инфраструктуре.

9. **Хранилище пароля в Domain.Entities** — `ProxyConfig.encrypted_password` — если пароль зашифрован, то кто отвечает за шифрование? Если domain — нарушение SRP.

10. **main.cpp — только ServiceMain()** — точка входа не выполняет composition. Он просто вызывает `TcpRedirectorService::Initialize()`, которая сама всё собирает. Это нарушает принцип голого composition root.

---

## 7. Рекомендации (на будущее)

1. **Вынести composition из ServiceMain.h** — создать отдельный `CompositionRoot.cpp`, который через конструкторы инжектит зависимости.
2. **Создать порт `IRelayServer`** в `domain/ports/` и реализовать его `TcpRelayServer`.
3. **Создать порт `IConnectionTable`** в `domain/ports/` и реализовать его `ConnectionTable`.
4. **Убрать статические bitmaps** — сделать их нестатическими, передавать через конструктор.
5. **Слить IDriverCommunicator и ICapture** в один порт, либо удалить дублирующиеся методы.
6. **Перенести WinDivertCapture в `adapters/driven/`** — это исходящий адаптер.
7. **Перенести временные метки из Rule** в инфраструктурный слой (persistence).

---

## 8. Сводная оценка

| Критерий | Оценка |
|----------|--------|
| Структура директорий | ✅ Хорошо (domain/adapters/infrastructure) |
| Чистота портов (ISP) | ⚠️ Есть дублирование |
| Изоляция слоёв | ⚠️ Инфраструктура смешана с адаптерами |
| Composition root | ❌ В .h-файле, жёсткая связь |
| Тестируемость | ❌ Из-за статики и прямых зависимостей |
| Доменная модель | ✅ Чистая, без внешних зависимостей |

**Общий вывод:** Проект имеет правильную гексагональную структуру директорий и базовое разделение слоёв, но страдает от трёх основных проблем: composition root в h-файле, отсутствие портов для relay/connectionTable, и дублирование портов. В текущем виде проект работоспособен, но расширение функционала и тестирование будут затруднены.