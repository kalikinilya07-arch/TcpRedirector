# Настройки Kaspersky для исключений

## 1. Исключение файлов и папок
- Добавить в исключения следующие пути:
  - `C:\\Program Files\\TcpRedirector\\*`
  - `C:\\ProgramData\\TcpRedirector\\*`
  - `C:\\Windows\\System32\\drivers\\WinDivert64.sys`

## 2. Исключение процессов
- В разделе «Защита → Мониторинг активности → Исключения» добавить:
  - `TcpRedirectorService.exe`
  - `TcpRedirectorGUI.exe`

## 3. Исключение драйвера WinDivert
- В разделе «Исключения → Исключения по типу объекта» добавить файл `WinDivert64.sys` как «Исключение драйвера».

## 4. Отключение конкретных проверок (если необходимо)
- Отключить «Поведенческий анализ» для процессов `TcpRedirectorService.exe` и `TcpRedirectorGUI.exe`.
- Отключить «Контроль каналов связи» для named pipe с именем, используемым `PipeServer.h`.

## 5. Добавление в список доверенных приложений
- В разделе «Защита → Доверенные приложения» добавить:
  - `TcpRedirectorService.exe`
  - `TcpRedirectorGUI.exe`

## 6. После настройки
- Перезагрузить Kaspersky сервис: `net stop klmservice && net start klmservice`
- Проверить, исчезли ли ложные срабатывания в журнале событий.