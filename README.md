# TcpRedirector

Прозрачный TCP-прокси-редиректор для Windows. Перехватывает TCP-трафик выбранных приложений на уровне ядра (WinDivert) и перенаправляет через upstream HTTP-прокси с поддержкой Kerberos/Negotiate аутентификации.

## Характеристики

| Параметр | Значение |
|----------|----------|
| Языки | C++ (сервис), C# (GUI) |
| Строк кода | ~91 000 |
| Платформа | Windows 10/11 x64 |
| Требования | .NET 9.0 Windows Desktop Runtime, WinDivert |
| Права | Администратор (обязательно) |

## Установка на голую Windows

### 1. Установить .NET 9.0 Runtime
Скачать и установить **Windows Desktop Runtime 9.0 (x64)**:
```
https://dotnet.microsoft.com/download/dotnet/9.0
```

### 2. Скопировать папку `deploy` на целевую машину
Вся программа — это папка [`deploy/`](deploy/). Скопируйте её в `C:\Program Files\TcpRedirector\`.

```
C:\Program Files\TcpRedirector\
├── TcpRedirectorService.exe   # C++ сервис (WinDivert)
├── WinDivert.dll              # Драйвер перехвата пакетов
├── WinDivert64.sys            # Драйвер ядра
├── gui\
│   ├── TcpRedirectorGUI.exe   # GUI (WPF, .NET 9.0)
│   └── *.dll                  # Зависимости .NET
├── config.json                # Пример конфига (авто-создаётся)
├── install_service.bat        # Установка как Windows-сервис
├── install_windivert.bat      # Установка драйвера WinDivert
├── run_console.bat            # Запуск в консольном режиме
└── uninstall_service.bat      # Удаление сервиса
```

### 3. Запустить GUI от Администратора
ПКМ по `gui\TcpRedirectorGUI.exe` → **Run as Administrator**.

GUI автоматически найдёт и запустит `TcpRedirectorService.exe`.

## Архитектура

```
┌──────────────┐    IPC (TCP 34011)    ┌──────────────────┐
│  GUI (WPF)   │◄─────────────────────►│  C++ Service     │
│  net9.0      │                       │  WinDivert       │
└──────┬───────┘                       └────────┬─────────┘
       │ config.json                             │
       │ (атомарная запись)                       │ WinDivert
       ▼                                         ▼
  %ProgramData%\                         Перехват TCP-пакетов
  TcpRedirector\                          на уровне ядра
  config.json
```

### Принцип работы
1. **GUI** — единственный писатель `config.json`. Сервис читает конфиг при старте и хранит в памяти.
2. **Правила**: добавил правило → трафик приложения перенаправляется. Удалил → не перенаправляется. Всё.
3. **Сохранение**: кнопка Save → атомарная запись в `config.json` + IPC-синхронизация с сервисом.
4. **Перечитывание**: конфиг перечитывается при старте GUI, после Start и после Stop сервиса.

## Сборка из исходников

### C++ сервис
Открыть `src/service/TcpRedirectorService/TcpRedirectorService.vcxproj` в Visual Studio 2022+, собрать `Release | x64`.

### C# GUI
```bash
dotnet build src/gui/TcpRedirectorGUI/TcpRedirectorGUI.csproj -c Release
```

## Тесты
```bash
cd tests/build2 && ctest -C Release
```

## Коммиты (последние)
```
441b1a6 fix: remove Enabled checkbox — rule presence = active
45e2ba4 chore: fix comments, remove unused AuthUsername
b148ca2 fix: two-tier save — disk + IPC
0327cdd fix: config.json persistence — atomic writes
```
