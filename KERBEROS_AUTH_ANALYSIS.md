# Анализ Kerberos-аутентификации на прокси

## Обнаруженные проблемы

### 1. **Пустой username в config.json**
```json
"auth": {
  "enabled": true,
  "username": "",  // ← ПУСТОЕ!
  "kerberos": true
}
```
**Проблема:** В `SettingsViewModel.cs` (строка 90) `Login` загружается из config, но в `auth_sspi.cpp` (строка 57-62) используется `GetUserNameW()` — текущий пользователь системы. Если прокси требует явного username, это может вызвать ошибку.

**Решение:** Заполнить `username` в config.json или использовать `GetUserNameW()` для получения текущего пользователя.

---

### 2. **Base64-декодер в `auth_sspi.cpp` ломает токены с padding**
```cpp
// auth_sspi.cpp, строка 269
if (c == '=') break;  // ← ОСТАНОВИТЬСЯ НА '='!
```
**Проблема:** SSPI-токены часто имеют padding `=`. Декодер останавливается на первом `=`, обрезая токен. Это ломает аутентификацию (H2 из REVIEW.md).

**Исправление:** Удалить проверку `if (c == '=') break;` — padding должен обрабатываться корректно.

---

### 3. **SPN формируется как `HTTP/<proxy_host>`**
```cpp
// auth_sspi.cpp, строка 225-227
std::string MakeSpn(const std::string& proxyHost) {
    return "HTTP/" + proxyHost;
}
```
**Проблема:** SPN должен точно совпадать с настройкой прокси. Если прокси настроен на `HTTP/proxy.example.com:3128`, а в config.json `127.0.0.1`, Kerberos-билет будет отклонен.

**Решение:** Убедиться, что `proxy.host` в config.json совпадает с FQDN прокси (например, `proxy.company.local`).

---

### 4. **Отсутствие обработки `SEC_E_NO_CREDENTIALS`**
```cpp
// auth_sspi.cpp, строка 84-85
return (sc == SEC_E_NO_CREDENTIALS) ? SspiResult::NoCredentials : SspiResult::Error;
```
**Проблема:** Если Kerberos-билет недоступен (нет в кэше), возвращается `NoCredentials`, но в `TcpRelayServer.h` (строка 340-345) это приводит к `sspiAvailable = false`, и аутентификация падает в Basic Auth (если `m_proxyAuthRequired`).

**Решение:** Добавить явное логирование и fallback на Basic Auth с паролем из config.

---

### 5. **Пароль в UI не используется при Kerberos**
```csharp
// SettingsViewModel.cs, строка 91
Password = "";  // ← СБРАСЫВАЕТСЯ!
```
**Проблема:** В `SettingsViewModel.LoadFromConfig()` пароль всегда сбрасывается в пустую строку (строка 91), даже если в config.json есть `encryptedPassword`. Это означает, что:
- При включении Kerberos пароль игнорируется (используется SSPI)
- При отключении Kerberos пароль пустой → Basic Auth не работает

**Решение:** Загружать `encryptedPassword` из config и расшифровывать его через `ConfigManager.GetPlainPassword()`.

---

## Как работает Kerberos-аутентификация (по коду)

### 1. **Инициализация (TcpRelayServer.h, строка 316-356)**
```cpp
// При первом CONNECT:
infrastructure::SspiContext sspiCtx;
std::string sspiToken;
bool sspiInitDone = false;

// Вызывается SspiNegotiate() с пустым токеном
auto r = infrastructure::SspiNegotiate(sspiCtx, "", sspiToken,
    infrastructure::MakeSpn(m_proxyHost));
```

### 2. **AcquireCredentialsHandle (auth_sspi.cpp, строка 71-86)**
```cpp
// Используется пакет "Negotiate" — Windows сам выбирает Kerberos/NTLM
sc = AcquireCredentialsHandleW(
    NULL,                           // Текущий пользователь
    pkgName,                        // "Negotiate"
    SECPKG_CRED_OUTBOUND,
    NULL, NULL, NULL,
    &ctx.credentials, NULL);
```
**Итог:** Получаются учетные данные текущего пользователя (Kerberos-билет из кэша или NTLM-токен).

### 3. **InitializeSecurityContext (auth_sspi.cpp, строка 137-149)**
```cpp
sc = InitializeSecurityContextW(
    &ctx.credentials,
    NULL,                           // Первый вызов → NULL
    wSpn.data(),                    // "HTTP/proxy_host"
    reqFlags, 0, SECURITY_NATIVE_DREP,
    NULL, 0, &ctx.context,
    &outBufDesc, &outFlags, NULL);
```
**Итог:** Создается начальный токен Negotiate (Kerberos-билет или NTLM-токен).

### 4. **Отправка токена (TcpRelayServer.h, строка 353-355)**
```cpp
if (!sspiToken.empty()) {
    connect_req += "Proxy-Authorization: Negotiate " + sspiToken + "\r\n";
}
```

### 5. **Обработка 407 (TcpRelayServer.h, строка 389-426)**
```cpp
else if (m_kerberosAuth && sspiAvailable && strstr(resp_buf, "407") != nullptr) {
    std::string challenge = infrastructure::Parse407Challenge(resp_buf);
    auto r = infrastructure::SspiNegotiate(sspiCtx, challenge, sspiToken,
        infrastructure::MakeSpn(m_proxyHost));
    goto retry_connect;  // Повторить CONNECT с новым токеном
}
```
**Итог:** Если прокси требует аутентификации, продолжается SSPI-цикл.

---

## Критические замечания

| Проблема | Файл | Строка | Последствия |
|----------|------|--------|-------------|
| Base64-декодер обрезает токен | auth_sspi.cpp | 269 | Ломает SSPI-токены с padding |
| Пустой username | config.json | - | Может вызвать ошибку Kerberos |
| SPN не совпадает с прокси | TcpRelayServer.h | 338 | Kerberos-билет отклонен |
| Пароль не загружается | SettingsViewModel.cs | 91 | Basic Auth не работает при отключении Kerberos |
| SEC_E_NO_CREDENTIALS не обрабатывается | auth_sspi.cpp | 84-85 | Падает в Error вместо fallback |

---

## Рекомендации

1. **Исправить Base64-декодер** — убрать `if (c == '=') break;` в `auth_sspi.cpp` (строка 269)
2. **Заполнить username в config.json** — использовать `GetUserNameW()` для получения текущего пользователя
3. **Убедиться в SPN** — `proxy.host` должен совпадать с FQDN прокси (например, `proxy.company.local`)
4. **Загружать пароль из config** — в `SettingsViewModel.LoadFromConfig()` расшифровывать `encryptedPassword`
5. **Добавить fallback** — если Kerberos недоступен, использовать Basic Auth с паролем из config

---

## Вывод

**Kerberos-аутентификация НЕ работает корректно в текущей реализации** из-за:
- Base64-декодера, ломающего токены
- Пустого username в config
- Отсутствия fallback на Basic Auth при ошибке Kerberos

**Для работы Kerberos требуется:**
- Валидный Kerberos-билет в кэше (пользователь должен быть в домене AD)
- Правильный SPN на прокси
- Исправление Base64-декодера

**Альтернатива:** Использовать Basic Auth с паролем из config (но для этого нужно исправить загрузку пароля в SettingsViewModel).