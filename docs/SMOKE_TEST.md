# Smoke Test — TcpRedirector v1.1.0

## 1. Установка службы (Администратор)

```bat
:: Проверить статус службы
sc query TcpRedirectorService
:: STATE: 4 RUNNING

:: Проверить порт relay
netstat -ano | findstr 34010
:: TCP 0.0.0.0:34010 LISTENING
```

## 2. AuthAgent (Администратор)

```bat
:: Проверить Task Scheduler
schtasks /query /tn "TcpRedirectorAuthAgent"
:: Ready

:: Проверить Named Pipe
dir \\.\pipe\TcpRedirectorAuth
:: Файл должен существовать после входа пользователя
```

## 3. GUI (Пользователь, без прав администратора)

```
1. Запустить TcpRedirectorGUI.exe
2. Проверить индикатор: 🟢 Service running
3. Открыть вкладку Statistics
4. Проверить отображение трафика
5. Открыть Settings
6. Проверить загрузку конфигурации
```

## 4. Перехват трафика

```
1. Убедиться, что целевое приложение запущено
2. Проверить, что трафик проходит через прокси
3. Проверить Connections в GUI
```

## 5. Kerberos (если настроено)

```
1. Убедиться, что AuthAgent запущен (dir \\.\pipe\TcpRedirectorAuth)
2. Проверить логи службы: %ProgramData%\TcpRedirector\logs\
3. Kerberos должен работать без ошибок SEC_E_NO_CREDENTIALS
```

## 6. Устойчивость

```
1. Остановить службу: net stop TcpRedirectorService
2. GUI должен показать 🔴 Service unavailable
3. Запустить службу: net start TcpRedirectorService
4. GUI должен автоматически переподключиться → 🟢 Service running
```

## 7. Перезагрузка

```
1. Перезагрузить компьютер
2. После загрузки: sc query TcpRedirectorService → RUNNING
3. После входа пользователя: dir \\.\pipe\TcpRedirectorAuth → существует
4. Запустить GUI → 🟢 Service running
```
