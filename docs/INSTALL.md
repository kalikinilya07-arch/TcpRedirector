# Установка TcpRedirector v1.1.0

## Для администратора (однократно)

### Предварительные требования
- Права локального администратора
- Windows 10/11 x64 или Windows Server 2019+

### Шаг 1: Исключение Defender

Перед установкой добавьте папки в исключения Windows Defender:

```powershell
Add-MpPreference -ExclusionPath "C:\Program Files\TcpRedirector"
Add-MpPreference -ExclusionPath "C:\ProgramData\TcpRedirector"
```

### Шаг 2: Копирование файлов

```bat
mkdir "C:\Program Files\TcpRedirector"
copy bin\* "C:\Program Files\TcpRedirector\"
```

### Шаг 3: Создание директории данных

```bat
mkdir "%ProgramData%\TcpRedirector"
mkdir "%ProgramData%\TcpRedirector\logs"
```

### Шаг 4: Установка WinDivert драйвера

```bat
"C:\Program Files\TcpRedirector\install_windivert.bat"
```

Или вручную:
```bat
sc create WinDivert binPath="C:\Program Files\TcpRedirector\WinDivert64.sys" type=kernel start=demand
sc start WinDivert
```

### Шаг 5: Установка и запуск службы

```bat
"C:\Program Files\TcpRedirector\TcpRedirectorService.exe" --install
net start TcpRedirectorService
```

### Шаг 6: Регистрация AuthAgent в Task Scheduler

```bat
schtasks /create /tn "TcpRedirectorAuthAgent" /xml "C:\Program Files\TcpRedirector\TcpRedirectorAuthAgent.xml" /f
```

### Шаг 7: Проверка

```bat
sc query TcpRedirectorService
:: STATE: 4 RUNNING

netstat -ano | findstr 34010
:: TCP 0.0.0.0:34010 LISTENING

schtasks /query /tn "TcpRedirectorAuthAgent"
:: Ready
```

---

## Сборка инсталлятора (опционально)

Если требуется один .exe-файл для распространения:

1. Установите [Inno Setup 6](https://jrsoftware.org/isinfo.php)
2. Запустите:
```bat
iscc installer\TcpRedirectorSetup.iss
```
3. Инсталлятор будет создан в `release\1.1.0\installer\TcpRedirectorSetup-1.1.0.exe`

---

## Для пользователя (ежедневно)

### Запуск GUI

Запустите `TcpRedirectorGUI.exe` из меню Пуск или из `C:\Program Files\TcpRedirector`.

**Права администратора не требуются.**

GUI автоматически подключится к службе. Индикатор состояния:
- 🟢 Service running — служба доступна
- 🔴 Service unavailable — обратитесь к администратору

### Проверка Kerberos

AuthAgent запускается автоматически при входе пользователя. Kerberos работает, если:
- Пользователь находится в домене
- Прокси поддерживает Kerberos/Negotiate
- Настроен режим `KerberosPreferred` (по умолчанию)

При недоступности Kerberos GUI автоматически переключится на Basic Auth.
