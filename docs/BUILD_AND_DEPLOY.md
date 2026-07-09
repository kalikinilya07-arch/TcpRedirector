# Build & Deploy Pipeline — TcpRedirector

## Структура директорий

```
TcpRedirector/
├── VERSION                   # Текущая версия (major.minor.patch)
├── src/                      # Исходный код
├── docs/                     # Документация
│   └── BUILD_AND_DEPLOY.md   # Этот файл
├── build/                    # Временная сборочная директория (создаётся build.bat)
│   ├── TcpRedirectorService.exe
│   ├── WinDivert.dll
│   ├── WinDivert64.sys
│   └── gui/
├── releases/                 # Версионированные релизы (создаются deploy.bat)
│   ├── v1.0.0/
│   ├── v1.0.1/
│   └── ...
├── build.bat                 # Сборка → build/
├── deploy.bat                # Релиз → releases/vX.Y.Z/
└── run.bat                   # Быстрый запуск из build/
```

### Правила

- **`build/`** — временная папка. Полностью очищается при каждом запуске `build.bat`. Только для отладки.
- **`releases/vX.Y.Z/`** — версионированные релизы. Создаются `deploy.bat`. Никогда не удаляются автоматически.
- **`VERSION`** — содержит текущую версию. `deploy.bat` автоинкрементирует PATCH (+1) после создания релиза.
- **`docs/`** — вся документация. Никаких бинарных файлов.

---

## Предварительные требования

| Компонент | Проверка |
|-----------|----------|
| Visual Studio 2022 | `build.bat` найдёт автоматически через `vswhere.exe` |
| .NET 9.0 SDK | `dotnet --version` |
| Git | `git --version` |

---

## Полный пайплайн

### 1. Сборка

```batch
cd C:\Users\user\Desktop\TcpRedirector
build.bat
```

**Что происходит:**
1. Читает `VERSION`
2. Находит Visual Studio, настраивает окружение
3. Очищает `build\`
4. Компилирует и запускает unit-тесты (6 тестов)
5. Собирает GUI (`dotnet publish --self-contained -r win-x64`)
6. Собирает C++ сервис (`msbuild Release x64`)
7. Копирует `WinDivert.dll` + `WinDivert64.sys`

**Результат:** `build\` готов к отладке.

### 2. Быстрая отладка

```batch
run.bat
```

Или вручную:
```batch
start build\gui\TcpRedirectorGUI.exe
```

### 3. Релиз

```batch
deploy.bat
```

**Что происходит:**
1. Читает `VERSION` (например, `1.0.0`)
2. Создаёт `releases\v1.0.0\`
3. Копирует артефакты из `build\` → `releases\v1.0.0\`
4. Удаляет отладочные файлы (`.pdb`, `createdump.exe`, лишние языки)
5. Автоинкрементирует PATCH: `1.0.0` → `1.0.1` в `VERSION`

**Результат:** `releases\v1.0.0\` — готовый к распространению релиз.

### 4. Установка релиза

Запустить от **Администратора**:

```batch
releases\v1.0.0\install.bat
```

---

## Ручная сборка (по шагам)

### C++ сервис

```batch
cd src\service\TcpRedirectorService
del /Q "build\service\x64\Release\obj\*.obj"  2>nul
del /Q "build\service\x64\Release\obj\*.iobj" 2>nul
del /Q "build\service\x64\Release\obj\*.ipdb" 2>nul
msbuild TcpRedirectorService.vcxproj /p:Configuration=Release /p:Platform=x64 /nologo
copy /Y "build\service\x64\Release\TcpRedirectorService.exe" "..\..\..\build\"
```

### GUI

```batch
cd src\gui\TcpRedirectorGUI
dotnet publish TcpRedirectorGUI.csproj -c Release --self-contained true -r win-x64 --nologo -o "..\..\..\build\gui"
```

### WinDivert

```batch
copy /Y "external\WinDivert\WinDivert-2.2.2-A\x64\WinDivert.dll" "build\"
copy /Y "external\WinDivert\WinDivert-2.2.2-A\x64\WinDivert64.sys" "build\"
```

---

## Версионирование

Файл `VERSION` содержит версию в формате `MAJOR.MINOR.PATCH`.

| Действие | Команда |
|----------|---------|
| Поднять MAJOR (`1.0.0` → `2.0.0`) | Отредактировать `VERSION` вручную |
| Поднять MINOR (`1.0.0` → `1.1.0`) | Отредактировать `VERSION` вручную |
| Поднять PATCH (`1.0.0` → `1.0.1`) | Автоматически после `deploy.bat` |

---

## Примечания

- **Kerberos/Basic auth** — взаимоисключающие (GUI + бэкенд + IPC)
- **Драйвер WinDivert** — автостарт через `EnsureDriverRunning()` при запуске сервиса
- **GUI** — self-contained (не требует .NET Runtime)
- **Сервис** — требует прав Администратора (WinDivert)
- **Конфигурация** — `%ProgramData%\TcpRedirector\config.json`
- **Логи** — `%ProgramData%\TcpRedirector\logs\`