# Удаление TcpRedirector v1.1.0

## Автоматическое удаление

Запустите удаление через:
- Панель управления → Программы и компоненты → TcpRedirector → Удалить
- Или запустите `TcpRedirectorSetup-1.1.0.exe` и выберите "Remove"

## Ручное удаление

```bat
rem 1. Остановка службы
net stop TcpRedirectorService

rem 2. Удаление службы
"C:\Program Files\TcpRedirector\TcpRedirectorService.exe" --uninstall

rem 3. Удаление Task Scheduler
schtasks /delete /tn "TcpRedirectorAuthAgent" /f

rem 4. Удаление WinDivert (опционально)
"C:\Program Files\TcpRedirector\WinDivertInstall.exe" uninstall

rem 5. Удаление файлов программы
rmdir /s /q "C:\Program Files\TcpRedirector"

rem 6. Удаление данных (ОСТОРОЖНО: удаляет конфигурацию и логи!)
rmdir /s /q "%ProgramData%\TcpRedirector"
```

Шаг 6 можно пропустить, если вы планируете переустановку.
