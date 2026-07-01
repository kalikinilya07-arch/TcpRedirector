# План: Реструктуризация UI — единая вкладка + конфигурируемый график

## Задача 1: Перенести статистику на главную панель, удалить вкладку Statistics

### Текущая структура
```
┌──────────────────────────────────────────┐
│ [Settings] [Statistics]    ← Tab bar     │
├──────────────────────────────────────────┤
│ Start Stop ● Running  Conn:5 Traffic:1MB │
├──────────────────────────────────────────┤
│ ┌─ Settings (scrollable) ──────────────┐ │
│ │ Proxy / Auth / Rules / Log Level     │ │
│ └──────────────────────────────────────┘ │
│ ИЛИ                                       │
│ ┌─ Statistics ─────────────────────────┐ │
│ │ [Traffic graph: last 60s]            │ │
│ │ Total: N  RX: X  TX: Y  Active: Z   │ │
│ └──────────────────────────────────────┘ │
├──────────────────────────────────────────┤
│ TcpRedirector v1.0.0           Connected │
└──────────────────────────────────────────┘
```

### Целевая структура
```
┌──────────────────────────────────────────┐
│ TcpRedirector                   ← Лого   │
├──────────────────────────────────────────┤
│ Start Stop ● Running  Conn:5 Traffic:1MB │
├──────────────────────────────────────────┤
│ ┌─ Settings (scrollable, ~2/3 высоты) ─┐│
│ │ Proxy / Auth / Rules / Log Level     ││
│ └──────────────────────────────────────┘ │
│ ┌─ Traffic graph (~1/3 высоты) ────────┐│
│ │ [Traffic graph: last 1h by default]  ││
│ │ Total: N  RX: X  TX: Y  Active: Z   ││
│ └──────────────────────────────────────┘ │
├──────────────────────────────────────────┤
│ TcpRedirector v1.0.0           Connected │
└──────────────────────────────────────────┘
```

### Изменения

| Файл | Действие |
|------|----------|
| [`MainWindow.xaml`](TcpRedirector/src/gui/TcpRedirectorGUI/MainWindow.xaml) | Удалить Tab bar (Row 0), удалить вкладку Statistics, переразбить контент: верх (Settings, `2*`) + низ (график, `1*`) |
| [`ShellViewModel.cs`](TcpRedirector/src/gui/TcpRedirectorGUI/Adapters/Driving/Wpf/ViewModels/ShellViewModel.cs) | Удалить `ActiveTab`, `NavCommand`, всё связанное с навигацией |

---

## Задача 2: График — конфигурируемое окно (по умолчанию 1 час)

### Текущее
- [`TrafficGraph.cs:141`](TcpRedirector/src/gui/TcpRedirectorGUI/Adapters/Driving/Wpf/Controls/TrafficGraph.cs:141): `var tStart = now - 60_000` — жёстко 60 секунд
- [`StatsViewModel.cs:13`](TcpRedirector/src/gui/TcpRedirectorGUI/Adapters/Driving/Wpf/ViewModels/StatsViewModel.cs:13): `MaxGraphPoints = 60` — жёстко 60 точек

### Целевое
- Добавить `WindowSeconds` dependency property в `TrafficGraph` (по умолчанию 3600)
- `StatsViewModel` читает `graphWindowSec` из конфига, передаёт в `TrafficGraph`
- Максимум точек = `graphWindowSec` (при опросе каждую секунду)

### Конфиг (`stats` секция)
```json
"stats": {
    "updateIntervalMs": 1000,    // уже есть — интервал опроса
    "graphWindowSec": 3600       // новое — окно графика (по умолч. 1 час)
}
```

### Изменения

| Файл | Действие |
|------|----------|
| [`TrafficGraph.cs`](TcpRedirector/src/gui/TcpRedirectorGUI/Adapters/Driving/Wpf/Controls/TrafficGraph.cs) | `WindowSeconds` DP, заменить `60_000` на `WindowSeconds * 1000` |
| [`StatsViewModel.cs`](TcpRedirector/src/gui/TcpRedirectorGUI/Adapters/Driving/Wpf/ViewModels/StatsViewModel.cs) | `MaxGraphPoints` из конфига, `GraphWindowSec` property |
| [`config.json`](C:/ProgramData/TcpRedirector/config.json) | Добавить `graphWindowSec: 3600` |
| [`IConfigRepository.cs`](TcpRedirector/src/gui/TcpRedirectorGUI/Domain/Ports/IConfigRepository.cs) | Метод `ReadInt("stats", "graphWindowSec", 3600)` |

---

## Задача 3: Интервал опроса из конфига

### Текущее
- [`ShellViewModel.cs:319`](TcpRedirector/src/gui/TcpRedirectorGUI/Adapters/Driving/Wpf/ViewModels/ShellViewModel.cs:319): `await Task.Delay(1000, ct)` — жёстко 1 сек

### Целевое
- Читать `updateIntervalMs` из конфига (уже есть `stats.updateIntervalMs`)
- По умолчанию 1000 мс

### Изменения

| Файл | Действие |
|------|----------|
| [`ShellViewModel.cs`](TcpRedirector/src/gui/TcpRedirectorGUI/Adapters/Driving/Wpf/ViewModels/ShellViewModel.cs:319) | `_config.ReadInt("stats", "updateIntervalMs", 1000)` |

---

## Порядок реализации

1. [`TrafficGraph.cs`](TcpRedirector/src/gui/TcpRedirectorGUI/Adapters/Driving/Wpf/Controls/TrafficGraph.cs) — добавить `WindowSeconds` DP
2. [`StatsViewModel.cs`](TcpRedirector/src/gui/TcpRedirectorGUI/Adapters/Driving/Wpf/ViewModels/StatsViewModel.cs) — чтение `graphWindowSec` из конфига, `MaxGraphPoints`, `GraphWindowSec`
3. [`ShellViewModel.cs`](TcpRedirector/src/gui/TcpRedirectorGUI/Adapters/Driving/Wpf/ViewModels/ShellViewModel.cs) — удалить навигацию, читать `updateIntervalMs`
4. [`MainWindow.xaml`](TcpRedirector/src/gui/TcpRedirectorGUI/MainWindow.xaml) — реструктуризация: удалить таб-бар, перепланировать Grid
5. [`config.json`](C:/ProgramData/TcpRedirector/config.json) — добавить `graphWindowSec`
6. Сборка + деплой
