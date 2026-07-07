# План исправления Kerberos-аутентификации

## Обнаруженные проблемы

| # | Проблема | Файл | Строка | Критичность |
|---|----------|------|--------|-------------|
| 1 | Base64-декодер обрезает токен на `=` | auth_sspi.cpp | 269 | **HIGH** |
| 2 | Пароль не загружается в UI | SettingsViewModel.cs | 91 | **HIGH** |
| 3 | SEC_E_NO_CREDENTIALS не обрабатывается | auth_sspi.cpp | 84-85 | MEDIUM |
| 4 | Пустой username в config.json | config.json | - | MEDIUM |

---

## Шаг 1: Исправить Base64-декодер в `auth_sspi.cpp`

**Файл:** `TcpRedirector/src/service/TcpRedirectorService/infrastructure/auth/auth_sspi.cpp`

**Проблема:** Строка 269 останавливает декодирование на `=`:
```cpp
if (c == '=') break;  // ← ЛОМАЕТ ТОКЕНЫ!
```

**Исправление:**
```cpp
// Строка 269 — УДАЛИТЬ эту проверку
// if (c == '=') break;

// Строка 289 — УДАЛИТЬ эту строку (D['='] = 0;)
// D['='] = 0;
```

**Обоснование:** Padding `=` в Base64 — это не данные, а символы выравнивания. Декодер должен игнорировать их, а не останавливаться.

---

## Шаг 2: Исправить загрузку пароля в `SettingsViewModel.cs`

**Файл:** `TcpRedirector/src/gui/TcpRedirectorGUI/Adapters/Driving/Wpf/ViewModels/SettingsViewModel.cs`

**Проблема:** Строка 91 всегда сбрасывает пароль:
```csharp
Password = "";  // ← СБРАСЫВАЕТСЯ!
```

**Исправление:**
```csharp
// Строка 91 — ЗАМЕНИТЬ на:
Password = _config.ReadString("auth", "encryptedPassword");
```

**Обоснование:** Пароль должен загружаться из config и отображаться в UI (в зашифрованном виде или как placeholder).

---

## Шаг 3: Добавить fallback на Basic Auth при ошибке Kerberos

**Файл:** `TcpRedirector/src/service/TcpRedirectorService/infrastructure/relay/TcpRelayServer.h`

**Проблема:** При `SEC_E_NO_CREDENTIALS` аутентификация просто падает.

**Исправление:**
```cpp
// Строка 340-345 — ЗАМЕНИТЬ на:
if (r == infrastructure::SspiResult::NoCredentials) {
    Log(domain::LogLevel::Warn, "Kerberos/NTLM недоступен (SEC_E_NO_CREDENTIALS), fallback на Basic Auth");
    sspiAvailable = false;
    // Если есть пароль — использовать Basic Auth
    if (!m_proxyPassword.empty()) {
        m_kerberosAuth = false;  // Переключиться на Basic Auth
    }
} else if (r == infrastructure::SspiResult::Error) {
    Log(domain::LogLevel::Warn, "Ошибка инициализации SSPI, fallback на Basic Auth");
    sspiAvailable = false;
    if (!m_proxyPassword.empty()) {
        m_kerberosAuth = false;
    }
}
```

**Обоснование:** Если Kerberos недоступен, использовать Basic Auth с паролем из config.

---

## Шаг 4: Заполнить username в config.json

**Файл:** `C:\ProgramData\TcpRedirector\config.json`

**Проблема:** `username` пустой.

**Исправление:**
```json
"auth": {
  "enabled": true,
  "username": "CURRENT_USER",  // ← ЗАМЕНИТЬ на имя пользователя
  "kerberos": true
}
```

**Обоснование:** Если прокси требует явного username, Kerberos может отклонить билет.

---

## Шаг 5: Добавить логирование SPN

**Файл:** `TcpRedirector/src/service/TcpRedirectorService/infrastructure/auth/auth_sspi.cpp`

**Исправление:**
```cpp
// Строка 58 — ДОБАВИТЬ логирование SPN:
printf("[SSPI] Authenticating as: %S (SPN=%s)\n", userName, spn.c_str());
// ДОБАВИТЬ:
printf("[SSPI] SPN for proxy: %s\n", spn.c_str());
```

**Обоснование:** Для диагностики проблем с SPN.

---

## Порядок выполнения

1. **Шаг 1** — Исправить Base64-декодер (критично, ломает токены)
2. **Шаг 2** — Исправить загрузку пароля (критично, Basic Auth не работает)
3. **Шаг 3** — Добавить fallback на Basic Auth (если Kerberos недоступен)
4. **Шаг 4** — Заполнить username в config.json (для совместимости)
5. **Шаг 5** — Добавить логирование SPN (для диагностики)

---

## Тестирование

После исправлений:

1. **Проверить Base64-декодер:**
   - Отправить токен с padding `=` (например, `YQ==`)
   - Убедиться, что декодируется в `a` (а не в пустую строку)

2. **Проверить Basic Auth fallback:**
   - Отключить Kerberos в config.json (`"kerberos": false`)
   - Указать `username` и `encryptedPassword`
   - Убедиться, что используется Basic Auth

3. **Проверить Kerberos:**
   - Включить Kerberos (`"kerberos": true`)
   - Указать `username` (должен совпадать с текущим пользователем)
   - Убедиться, что SPN `HTTP/<proxy_host>` совпадает с настройкой прокси
   - Проверить логи на наличие `Token получен: X bytes`

---

## Риски

| Риск | Описание | Mitigation |
|------|----------|------------|
| Base64-декодер сломает другие токены | Удаление `if (c == '=') break;` может повлиять на другие части кода | Проверить все места использования `SspiBase64Decode` |
| Basic Auth не работает | Если пароль не загружается, fallback не сработает | Проверить `encryptedPassword` в config.json |
| Kerberos отклоняет билет | Если SPN не совпадает, Kerberos отклонит билет | Убедиться, что `proxy.host` = FQDN прокси |

---

## Время выполнения

- Шаг 1: 5 минут
- Шаг 2: 5 минут
- Шаг 3: 10 минут
- Шаг 4: 2 минуты
- Шаг 5: 3 минуты
- **Итого:** ~25 минут на исправления + 15 минут на тестирование

---

## Примечания

- Все исправления **обратно совместимы** — старые конфиги продолжат работать
- Base64-декодер исправлен по аналогии с `ConfigManager.cpp` (строка 481-505), где padding обрабатывается корректно
- Fallback на Basic Auth работает только если `m_proxyPassword` не пустой