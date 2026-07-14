# Обновление TcpRedirector v1.1.0

## С предыдущей версии на v1.1.0

### Автоматическое обновление (рекомендуется)

Запустите новый инсталлятор от имени администратора:

```
TcpRedirectorSetup-1.1.0.exe
```

Инсталлятор автоматически:
1. Остановит текущую службу
2. Создаст backup `config.json` → `config.json.bak`
3. Заменит файлы в `C:\Program Files\TcpRedirector`
4. Обновит Task Scheduler для AuthAgent
5. Запустит службу

### Ручное обновление

```bat
rem 1. Остановка службы
net stop TcpRedirectorService

rem 2. Backup конфигурации
copy "%ProgramData%\TcpRedirector\config.json" "%ProgramData%\TcpRedirector\config.json.bak"

rem 3. Замена файлов
copy /Y bin\TcpRedirectorService.exe "C:\Program Files\TcpRedirector\"
copy /Y bin\TcpRedirectorGUI.exe "C:\Program Files\TcpRedirector\"
copy /Y bin\TcpRedirectorAuthAgent.exe "C:\Program Files\TcpRedirector\"

rem 4. Обновление Task Scheduler
schtasks /delete /tn "TcpRedirectorAuthAgent" /f
schtasks /create /tn "TcpRedirectorAuthAgent" /xml "C:\Program Files\TcpRedirector\TcpRedirectorAuthAgent.xml" /f

rem 5. Запуск службы
net start TcpRedirectorService
```

### Проверка обновления

```bat
"C:\Program Files\TcpRedirector\TcpRedirectorService.exe" --version
:: TcpRedirector Service v1.1.0
```

### Примечания по совместимости

- **config.json** — формат не изменился, обратная совместимость полная
- **AuthAgent** — новый компонент, требуется регистрация в Task Scheduler
- **AuthenticationMode** — новое поле в config.json. Если отсутствует, используется `KerberosPreferred`
