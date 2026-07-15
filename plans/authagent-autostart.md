# AuthAgent Auto-Start Plan

## Задача
Служба (SYSTEM) должна автоматически запускать AuthAgent в сессии пользователя при недоступности pipe.

## Решение
`KerberosAgentProvider::EnsureConnected()` при неудаче соединения запускает `TcpRedirectorAuthAgent.exe` в активной консольной сессии через WTS API.

## Flow

```
KerberosAgentProvider::EnsureConnected()
  → ConnectToAgent() — пробуем подключиться к pipe
  → НЕУДАЧА
  → LaunchAuthAgentInUserSession()
      → WTSGetActiveConsoleSessionId() — получаем ID активной сессии
      → WTSQueryUserToken() — получаем токен пользователя
      → DuplicateTokenEx() — дублируем для CreateProcessAsUser
      → CreateEnvironmentBlock() — окружение пользователя
      → CreateProcessAsUserW() — запуск AuthAgent.exe в сессии
      → Ожидание 2 секунды (пока создастся pipe)
  → Повторная попытка ConnectToAgent()
```

## Файлы

### `KerberosAgentProvider.h`
- Добавить `static bool LaunchAuthAgentInUserSession();`
- Добавить `static std::atomic<bool> s_launchInProgress;` (защита от множественного запуска)

### `KerberosAgentProvider.cpp`
- `#include <wtsapi32.h>`, `#include <userenv.h>`
- `#pragma comment(lib, "wtsapi32.lib")`, `#pragma comment(lib, "userenv.lib")`
- Реализация `LaunchAuthAgentInUserSession()` (~40 строк)
- Вызов из `EnsureConnected()` после неудачного ConnectToAgent()

## Безопасность
- AuthAgent.exe запускается в сессии пользователя (имеет Kerberos-тикеты)
- Pipe ACL Everyone RW — служба может подключиться
- Rate-limit: не чаще 1 раза в 30 секунд (через atomic flag)

## Сборка
- `msbuild TcpRedirectorService.vcxproj /p:Configuration=Release /p:Platform=x64`
- Новые зависимости: wtsapi32.lib, userenv.lib (уже могут быть в проекте)
