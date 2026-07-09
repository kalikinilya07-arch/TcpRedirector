# Анализ совместимости с антивирусами (Windows Defender + Kaspersky)

## ⚠️ Критические факторы риска (High Risk)

### 1. WinDivert — перехват сетевого трафика
**Файлы:** `WinDivertCapture.cpp/h`, `WinDivert64.sys`, `WinDivert.dll`
- **Defender:** Классифицируется как `HackTool:Win32/WinDivert` или `Trojan:Win32/WinDivert`
- **Kaspersky:** `HEUR:Trojan.Win32.Generic` или `not-a-virus:RiskTool.Win32.WinDivert`
- **Причина:** Драйвер ядра для перехвата/модификации пакетов — типичный функционал снайферов и руткитов

### 2. Named Pipe без Security Descriptor
**Файл:** `PipeServer.h` (строка 169-176)
```cpp
HANDLE newPipe = CreateNamedPipeW(
    pipeName.c_str(),
    PIPE_ACCESS_DUPLEX,
    PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE | PIPE_WAIT,
    PIPE_UNLIMITED_INSTANCES,  // ← нет SECURITY_ATTRIBUTES
    4096, 4096, 0, NULL);      // ← NULL = все могут подключиться
```
- **Defender:** `Behavior:Win32/SuspiciousPipeActivity`
- **Kaspersky:** `PDM:Trojan.Win32.Generic` (подозрительная IPC активность)

### 3. Установка сервиса + запуск в консольном режиме
**Файл:** `ServiceController.cs` (строки 48-68)
```csharp
Arguments = "--console",
UseShellExecute = true,
CreateNoWindow = false
```
- **Defender:** `Behavior:Win32/ServiceInstallAbuse`
- **Kaspersky:** `PDM:Trojan.Win32.ServiceManipulation`
- **Проблема:** Сервис устанавливается в SCM, но запускается как пользовательский процесс — нетипичное поведение

### 4. Агрессивное убийство процессов
**Файл:** `ServiceController.cs` (строки 81-86)
```csharp
var procs = Process.GetProcessesByName(ProcessName);
foreach (var p in procs) { p.Kill(); p.WaitForExit(5000); }
```
- **Defender:** `Behavior:Win32/ProcessTermination`
- **Kaspersky:** `PDM:Trojan.Win32.ProcessKill`

### 5. Жёстко заданные учётные данные / слабая аутентификация
**Файл:** `auth_sspi.cpp` (строки 253-280) — Base64 декодер принимает `=` как данные
**Файл:** `config.json` — пароли в открытом виде
- **Defender:** `Trojan:Win32/CredentialTheft`
- **Kaspersky:** `HEUR:Trojan-PSW.Win32.Generic`

---

## 🟡 Средние риски (Medium Risk)

### 6. PID lookup на каждый пакет
**Файл:** `WinDivertCapture.cpp` (строка 318-379) — полный перебор TCP таблицы + снапшот процессов
- Высокая нагрузка на CPU — поведенческий триггер "майнинг/брутфорс"

### 7. Отсутствие валидации конфига
**Файл:** `JsonConfigRepository.cs`, `ConfigManager.cpp` — нет атомарного сохранения, нет подписи конфига
- Возможность подмены конфига → перенаправление трафика

### 8. Рекурсивный захват мьютекса в ConnectionTracker
**Файл:** `ConnectionTable.h` — потенциальный дедлок
- Может привести к зависанию сервиса → поведенческий флаг "нестабильное ПО"

### 9. Отсутствие try/catch в критических путях
**Файл:** `main.cpp` — есть, но не везде
- Краш сервиса → перезапуск → цикл крашей → флаг "подозрительная активность"

---

## 🟢 Низкие риски / Информационные (Low/Info)

### 10. Логирование в ProgramData
**Файл:** `Logger.cpp` — `getenv("ProgramData")` может вернуть `nullptr` (M4)
- Не критично, но вызывает предупреждения статического анализа

### 11. Не-ASCII строки через wstring(s.begin(), s.end())
**28 мест** — потенциальная порча данных, но не вирусоподобно

---

## 📋 Рекомендации по настройке исключений (БЕЗ ИЗМЕНЕНИЯ КОДА)

### Для Windows Defender (PowerShell от админа):

```powershell
# 1. Исключение папки с бинарниками
Add-MpPreference -ExclusionPath "C:\Program Files\TcpRedirector"
Add-MpPreference -ExclusionPath "C:\ProgramData\TcpRedirector"

# 2. Исключение процессов
Add-MpPreference -ExclusionProcess "TcpRedirectorService.exe"
Add-MpPreference -ExclusionProcess "TcpRedirectorGUI.exe"

# 3. Исключение драйвера WinDivert
Add-MpPreference -ExclusionPath "C:\Windows\System32\drivers\WinDivert64.sys"
Add-MpPreference -ExclusionExtension ".sys"

# 4. Исключение Named Pipe (по маске)
# Defender не поддерживает исключение пайпов напрямую, но можно отключить мониторинг:
Set-MpPreference -DisableBehaviorMonitoring $false  # оставить включённым
# Вместо этого — подписать бинарники сертификатом (см. ниже)
```

### Для Kaspersky (через интерфейс или klcfg):

```
1. Настройки → Угрозы и исключения → Исключения → Добавить:
   - Путь: C:\Program Files\TcpRedirector\*
   - Путь: C:\ProgramData\TcpRedirector\*
   - Процесс: TcpRedirectorService.exe
   - Процесс: TcpRedirectorGUI.exe

2. Настройки → Защита → Контроль программ → Доверие:
   - Добавить TcpRedirectorService.exe в "Доверенные"
   - Добавить TcpRedirectorGUI.exe в "Доверенные"

3. Настройки → Защита → Мониторинг активности → Исключения:
   - Исключить активность Named Pipe для TcpRedirectorService.exe
```

---

## 🔐 Долгосрочные меры (требуют изменений, но для справки)

| Мера | Описание | Сложность |
|------|----------|-----------|
| **Подпись кода (EV Code Signing)** | Устраняет 90% эвристических срабатываний | Средняя |
| **Security Descriptor для Named Pipe** | `SECURITY_ATTRIBUTES` с DACL только для SYSTEM/Admins | Низкая |
| **Шифрование config.json** | DPAPI или AES-256, ключ в реестре/TPM | Средняя |
| **Валидация конфига (хеш/подпись)** | Проверка целостности при загрузке | Низкая |
| **Правильное завершение потоков** | Join вместо Kill, graceful shutdown | Средняя |
| **Кэширование PID** | Не сканировать таблицу на каждый пакет | Низкая |

---

## ✅ Чек-лист перед деплоем

- [ ] Добавить папки в исключения Defender/Kaspersky
- [ ] Протестировать установку сервиса (`--install`) на чистой VM с обоими АВ
- [ ] Проверить запуск в консольном режиме (`--console`)
- [ ] Проверить IPC (GUI ↔ Service) через named pipe
- [ ] Проверить перехват трафика (WinDivert загружается без ошибок)
- [ ] Убедиться, что логи пишутся в `C:\ProgramData\TcpRedirector\logs\`
- [ ] Запустить `sigcheck -i TcpRedirectorService.exe` — проверить подпись
- [ ] Отправить сэмплы в VirusTotal / Kaspersky Threat Intelligence Portal для предварительной проверки

---

## 📞 Контакты для false positive

| Вендор | Портал |
|--------|--------|
| Microsoft Defender | https://www.microsoft.com/en-us/wdsi/filesubmission |
| Kaspersky | https://opentip.kaspersky.com/ |
| VirusTotal | https://www.virustotal.com/gui/home/upload |

---

*Анализ основан на статическом коде. Динамическое тестирование на целевых системах обязательно.*