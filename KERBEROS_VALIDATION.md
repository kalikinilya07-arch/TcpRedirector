# Валидация аутентификации Kerberos в TcpRedirector

## 📋 Резюме

**Дата проверки:** 2026-07-07  
**Статус:** 🔴 **КРИТИЧЕСКИЕ БАГИ ОБНАРУЖЕНЫ**  
**Работоспособность:** ❌ Kerberos-аутентификация **НЕ РАБОТАЕТ** в runtime (работает только после перезапуска службы)

---

## ✅ Что работает корректно

### 1. **SSPI Implementation (Kerberos/NTLM)**
- ✅ [`auth_sspi.cpp:71-79`](TcpRedirector/src/service/TcpRedirectorService/infrastructure/auth/auth_sspi.cpp:71) — `AcquireCredentialsHandle` вызывается с `NULL` (использует текущего пользователя)
- ✅ [`auth_sspi.cpp:54-62`](TcpRedirector/src/service/TcpRedirectorService/infrastructure/auth/auth_sspi.cpp:54) — логирование имени пользователя через `GetUserNameW`
- ✅ [`auth_sspi.cpp:132-135`](TcpRedirector/src/service/TcpRedirectorService/infrastructure/auth/auth_sspi.cpp:132) — флаги `ISC_REQ_CONFIDENTIALITY`, `ISC_REQ_INTEGRITY`, `ISC_REQ_MUTUAL_AUTH`
- ✅ [`TcpRelayServer.h:329-356`](TcpRedirector/src/service/TcpRedirectorService/infrastructure/relay/TcpRelayServer.h:329) — выбор метода аутентификации на основе `m_kerberosAuth`

### 2. **Конфигурация на диске (config.json)**
- ✅ [`JsonConfigRepository.cs:124-128`](TcpRedirector/src/gui/TcpRedirectorGUI/Infrastructure/Config/JsonConfigRepository.cs:124) — поле `"kerberos"` записывается
- ✅ [`ConfigManager.cpp:243-248`](TcpRedirector/src/service/TcpRedirectorService/infrastructure/config/ConfigManager.cpp:243) — поле `"kerberos"` читается при загрузке
- ✅ [`CompositionRoot.h:79-90`](TcpRedirector/src/service/TcpRedirectorService/CompositionRoot.h:79) — `kerberos_auth` инициализируется при старте службы

### 3. **Безопасность хранения пароля**
- ✅ [`ConfigManager.cpp:417-543`](TcpRedirector/src/service/TcpRedirectorService/infrastructure/config/ConfigManager.cpp:417) — DPAPI (`CryptProtectData`/`CryptUnprotectData`)
- ✅ [`JsonConfigRepository.cs:142-145`](TcpRedirector/src/gui/TcpRedirectorGUI/Infrastructure/Config/JsonConfigRepository.cs:142) — `encryptedPassword` сохраняется в config.json
- ✅ [`ConfigManager.cpp:50-63`](TcpRedirector/src/service/TcpRedirectorService/infrastructure/config/ConfigManager.cpp:50) — пароль дешифруется только в службе
- ✅ [`SettingsViewModel.cs:91`](TcpRedirector/src/gui/TcpRedirectorGUI/Adapters/Driving/Wpf/ViewModels/SettingsViewModel.cs:91) — пароль **НЕ** загружается в UI (безопасность)

---

## 🔴 Критические баги

### **BUG #1: Kerberos-флаг не передается через IPC**

**Приоритет:** 🔴 **CRITICAL**  
**Статус:** ❌ **Broken**

#### Цепочка проблем:

1. **GUI → Service IPC:** Kerberos-флаг не отправляется
   - **Файл:** [`IpcClient.cs:110-121`](TcpRedirector/src/gui/TcpRedirectorGUI/Infrastructure/Ipc/IpcClient.cs:110)
   - **Проблема:** В JSON-RPC запросе `set_config` отсутствует поле `kerberos_enabled`
   ```csharp
   public async Task<bool> SetConfigAsync(ProxyConfig config)
   {
       var r = await Call("set_config", new
       {
           host = config.Host,
           port = config.Port,
           auth_required = config.AuthRequired,
           login = config.Login ?? "",
           set_password = !string.IsNullOrEmpty(config.Password)
           // ❌ ОТСУТСТВУЕТ: kerberos_enabled = config.KerberosEnabled
       });
       return r?.GetProperty("status").GetString() == "success";
   }
   ```

2. **Service IPC Handler:** Kerberos-флаг не читается
   - **Файл:** [`IpcHandler.h:107-119`](TcpRedirector/src/service/TcpRedirectorService/adapters/driving/IpcHandler.h:107)
   - **Проблема:** Поле `kerberos_enabled` не извлекается из JSON
   ```cpp
   void SetConfig(const std::string& params, nlohmann::json& result) {
       auto j = nlohmann::json::parse(params);
       domain::ProxyConfig config;
       // ... host, port, auth_required, login ...
       config.has_password = j.value("set_password", false);
       // ❌ ОТСУТСТВУЕТ: config.kerberos_auth = j.value("kerberos_enabled", false);
       m_configManager->SetProxyConfig(config);
       result["status"] = "success";
   }
   ```

3. **ConfigManager:** Kerberos-флаг не обновляется
   - **Файл:** [`ConfigManager.cpp:65-78`](TcpRedirector/src/service/TcpRedirectorService/infrastructure/config/ConfigManager.cpp:65)
   - **Проблема:** `m_config.auth.kerberos` не обновляется при вызове `SetProxyConfig`
   ```cpp
   bool ConfigManager::SetProxyConfig(const domain::ProxyConfig& config) {
       std::unique_lock lock(m_mutex);
       m_config.proxy.host = std::string(config.host.begin(), config.host.end());
       m_config.proxy.port = config.port;
       m_config.auth.enabled = config.auth_required;
       m_config.auth.username = std::string(config.login.begin(), config.login.end());
       // ❌ ОТСУТСТВУЕТ: m_config.auth.kerberos = config.kerberos_auth;
       // ...
   }
   ```

#### Последствия:
- Чекбокс "Use Kerberos protocol" в UI **не влияет** на работу службы в runtime
- Изменение Kerberos-флага требует **перезапуска службы** (чтение config.json при старте работает)
- Пользователь вводит пароль, но служба пытается использовать Kerberos (или наоборот)

---

### **BUG #2: Пароль не передается через IPC (только для Basic Auth)**

**Приоритет:** 🟡 **MEDIUM** (критично только при использовании Basic Auth без перезапуска)
**Статус:** ⚠️ **Workaround exists** (перезапуск службы или Kerberos)

#### Контекст:
Этот баг **НЕ влияет** на Kerberos-аутентификацию, так как Kerberos использует текущего пользователя автоматически (пароль не нужен).

Баг критичен **только** для сценария:
- Пользователь использует **Basic Auth** (Kerberos=false)
- Пользователь меняет пароль в UI
- Пользователь НЕ хочет перезапускать службу

#### Проблема:

**Файл:** [`IpcClient.cs:118`](TcpRedirector/src/gui/TcpRedirectorGUI/Infrastructure/Ipc/IpcClient.cs:118)

```csharp
set_password = !string.IsNullOrEmpty(config.Password)  // ❌ Только флаг!
// НЕТ: password = config.Password
```

- IPC отправляет только **флаг** `set_password: true/false`
- **Сам пароль** не передается в службу через IPC

#### Последствия (только для Basic Auth):
- Изменение пароля в UI **не влияет** на работу службы в runtime
- Новый пароль используется только после **перезапуска службы** (при чтении config.json)
- Basic Auth с новым паролем **не работает** до перезапуска

#### Workaround:
1. **Рекомендуемый:** Использовать Kerberos (пароль не нужен)
2. **Альтернатива:** Перезапустить службу после изменения пароля

#### Анализ безопасности:
Передача пароля через Named Pipe (IPC) **не безопасна** без дополнительной защиты:
- ✅ **Положительно:** Пароль зашифрован DPAPI в config.json
- ❌ **Проблема:** Named Pipe не защищен (любой локальный пользователь может перехватить)
- 💡 **Рекомендация:** Вместо передачи пароля через IPC, лучше:
  1. Записать зашифрованный пароль в config.json (уже работает)
  2. Послать IPC-команду `reload_config` (предлагается в Шаге 3)
  3. Служба перечитает config.json и получит зашифрованный пароль

#### Вывод:
Это **не критичный баг**, так как:
- Kerberos (рекомендуемый режим) не использует пароль
- Есть workaround (перезапуск службы)
- Исправление (Шаг 3) улучшает UX, но не обязательно для работоспособности

---

### **BUG #3: Base64-декодер обрезает SSPI-токены на `=`**

**Приоритет:** 🔴 **HIGH**  
**Статус:** ❌ **Broken** (из REVIEW.md — баг H2)

**Файл:** [`auth_sspi.cpp:253-280`](TcpRedirector/src/service/TcpRedirectorService/infrastructure/auth/auth_sspi.cpp:253)

```cpp
std::vector<uint8_t> SspiBase64Decode(const std::string& data) {
    // ...
    for (size_t i = 0; i < data.size(); ++i) {
        unsigned char c = data[i];
        if (c == '=') break;  // ❌ ОБРЕЗАЕТ ТОКЕН!
        // ...
    }
}
```

#### Последствия:
- SSPI-токены с padding `=` декодируются **не полностью**
- Kerberos-токены **испорчены** → прокси отклоняет аутентификацию
- Decryption DPAPI-пароля также использует Base64 → пароль **не расшифровывается**

#### Пример:
```
Токен:       "YWJjZA=="  (Base64 для "abcd")
Декодируется: "abc"      (обрезано на первом '=')
Ожидается:   "abcd"
```

---

## 🟡 UI: Избыточные поля аутентификации

### **ISSUE #4: Login/Password показываются при Kerberos**

**Приоритет:** 🟡 **MEDIUM** (UX проблема)

**Файл:** [`MainWindow.xaml:142-156`](TcpRedirector/src/gui/TcpRedirectorGUI/MainWindow.xaml:142)

```xml
<!-- Login и Password видны всегда, когда AuthRequired=true -->
<TextBlock Text="Login:" ... />
<TextBox Text="{Binding Settings.Login, Mode=TwoWay}" ... />

<TextBlock Text="Password:" ... />
<PasswordBox x:Name="PwdBox" ... />

<!-- Checkbox Kerberos ниже, но не скрывает Login/Password -->
<CheckBox Content="Use Kerberos protocol"
          IsChecked="{Binding Settings.KerberosEnabled, Mode=TwoWay}" ... />
```

#### Проблемы:
1. **Kerberos не требует Login/Password** — использует текущего пользователя автоматически
2. **Пользователь вводит пароль**, но он **не используется** если Kerberos включен
3. **Confusing UX** — непонятно, когда нужен пароль, а когда нет

#### Рекомендация:
Скрыть Login/Password поля когда `KerberosEnabled=true`:

```xml
<TextBlock Text="Login:" ... 
           Visibility="{Binding Settings.KerberosEnabled, 
                        Converter={StaticResource InvertedBoolToVis}}" />
<!-- ... то же для Password ... -->

<CheckBox Content="Use Kerberos protocol (uses current Windows user)" ... />
```

---

## 📊 Таблица приоритетов

| # | Баг | Файлы | Критичность | Работоспособность |
|---|-----|-------|-------------|-------------------|
| 1 | Kerberos-флаг не передается через IPC | `IpcClient.cs:118`<br>`IpcHandler.h:116`<br>`ConfigManager.cpp:72` | 🔴 **CRITICAL** | ❌ **Broken** |
| 3 | Base64-декодер обрезает SSPI-токены и DPAPI-пароли | `auth_sspi.cpp:269` | 🔴 **HIGH** | ❌ **Broken** |
| 2 | Пароль не передается через IPC (только Basic Auth) | `IpcClient.cs:118` | 🟡 **MEDIUM** | ⚠️ **Workaround exists** |
| 4 | Login/Password показываются при Kerberos | `MainWindow.xaml:142-156` | 🟡 **MEDIUM** | ⚠️ **Confusing UX** |

### Пояснение приоритетов:

**Баг #1 (CRITICAL)** — ломает динамическое переключение между Kerberos и Basic Auth
**Баг #3 (HIGH)** — ломает Kerberos-токены И DPAPI-дешифрование паролей
**Баг #2 (MEDIUM)** — влияет только на Basic Auth при изменении пароля без перезапуска (есть workaround)
**Issue #4 (MEDIUM)** — UX проблема, не влияет на работоспособность

---

## 🔧 Пошаговый план исправления

### **Шаг 1: Исправить Base64-декодер** ⏱️

**Приоритет:** 🔴 **CRITICAL** (ломает Kerberos и DPAPI)

#### Файл: [`auth_sspi.cpp:253-280`](TcpRedirector/src/service/TcpRedirectorService/infrastructure/auth/auth_sspi.cpp:253)

#### Изменения:

1. **Удалить строку 269:**
   ```cpp
   // БЫЛО:
   if (c == '=') break;  // ❌ УДАЛИТЬ
   
   // СТАЛО:
   // (эта строка отсутствует)
   ```

2. **Удалить строку ~289 (инициализация таблицы):**
   ```cpp
   // БЫЛО:
   D['='] = 0;  // ❌ УДАЛИТЬ
   
   // СТАЛО:
   // (эта строка отсутствует)
   ```

3. **Добавить корректную обработку padding:**
   ```cpp
   std::vector<uint8_t> SspiBase64Decode(const std::string& data) {
       static const int D[256] = { /* таблица декодирования без '=' */ };
       
       std::vector<uint8_t> out;
       std::vector<unsigned char> buf;
       buf.reserve(4);
       
       for (size_t i = 0; i < data.size(); ++i) {
           unsigned char c = data[i];
           
           // Пропускаем padding и пробелы
           if (c == '=' || c == ' ' || c == '\r' || c == '\n' || c == '\t') {
               continue;  // ✅ ПРОПУСКАЕМ, НЕ BREAK
           }
           
           if (D[c] == -1) continue;  // Невалидный символ
           
           buf.push_back(c);
           
           if (buf.size() == 4) {
               out.push_back((D[buf[0]] << 2) | (D[buf[1]] >> 4));
               out.push_back((D[buf[1]] << 4) | (D[buf[2]] >> 2));
               out.push_back((D[buf[2]] << 6) | D[buf[3]]);
               buf.clear();
           }
       }
       
       // Обработка оставшихся байтов
       if (buf.size() >= 2) {
           out.push_back((D[buf[0]] << 2) | (D[buf[1]] >> 4));
           if (buf.size() >= 3) {
               out.push_back((D[buf[1]] << 4) | (D[buf[2]] >> 2));
           }
       }
       
       return out;
   }
   ```

#### Тестирование:
```cpp
// Тест 1: Padding
assert(SspiBase64Decode("YWJjZA==") == "abcd");  // 2x '='
assert(SspiBase64Decode("YWJj") == "abc");       // Без padding

// Тест 2: SSPI-токен (реальный)
auto token = "TlRMTVNTUAA...==";  // Negotiate token
auto decoded = SspiBase64Decode(token);
assert(decoded.size() > 0);  // Не пустой
```

---

### **Шаг 2: Передать Kerberos-флаг через IPC** ⏱️

**Приоритет:** 🔴 **CRITICAL**

#### 2.1. Исправить GUI → Service (IPC Client)

**Файл:** [`IpcClient.cs:110-121`](TcpRedirector/src/gui/TcpRedirectorGUI/Infrastructure/Ipc/IpcClient.cs:110)

```csharp
public async Task<bool> SetConfigAsync(ProxyConfig config)
{
    var r = await Call("set_config", new
    {
        host = config.Host,
        port = config.Port,
        auth_required = config.AuthRequired,
        login = config.Login ?? "",
        set_password = !string.IsNullOrEmpty(config.Password),
        kerberos_enabled = config.KerberosEnabled  // ✅ ДОБАВИТЬ
    });
    return r?.GetProperty("status").GetString() == "success";
}
```

#### 2.2. Исправить Service IPC Handler

**Файл:** [`IpcHandler.h:107-119`](TcpRedirector/src/service/TcpRedirectorService/adapters/driving/IpcHandler.h:107)

```cpp
void SetConfig(const std::string& params, nlohmann::json& result) {
    auto j = nlohmann::json::parse(params);
    domain::ProxyConfig config;
    std::string host = j["host"].get<std::string>();
    config.host = std::wstring(host.begin(), host.end());
    config.port = j["port"].get<uint16_t>();
    config.auth_required = j.value("auth_required", false);
    config.login = std::wstring(j.value("login", std::string()).begin(),
                                 j.value("login", std::string()).end());
    config.has_password = j.value("set_password", false);
    config.kerberos_auth = j.value("kerberos_enabled", false);  // ✅ ДОБАВИТЬ
    
    m_configManager->SetProxyConfig(config);
    result["status"] = "success";
}
```

#### 2.3. Исправить ConfigManager

**Файл:** [`ConfigManager.cpp:65-78`](TcpRedirector/src/service/TcpRedirectorService/infrastructure/config/ConfigManager.cpp:65)

```cpp
bool ConfigManager::SetProxyConfig(const domain::ProxyConfig& config) {
    std::unique_lock lock(m_mutex);
    Config oldCfg = m_config;
    
    m_config.proxy.host = std::string(config.host.begin(), config.host.end());
    m_config.proxy.port = config.port;
    m_config.proxy.enabled = true;
    m_config.auth.enabled = config.auth_required;
    m_config.auth.username = std::string(config.login.begin(), config.login.end());
    m_config.auth.kerberos = config.kerberos_auth;  // ✅ ДОБАВИТЬ
    
    if (config.has_password && !m_config.auth.encryptedPassword.empty()) {
        // пароль уже зашифрован — оставляем
    }
    
    NotifyListeners(oldCfg, m_config);
    return true;
}
```

#### Тестирование:
1. Запустить GUI
2. Включить "Authentication required"
3. Включить "Use Kerberos protocol"
4. Нажать "Save"
5. **БЕЗ перезапуска службы** попробовать подключиться
6. Ожидается: Kerberos-аутентификация работает

---

### **Шаг 3: Исправить передачу пароля (опционально)** ⏱️

**Приоритет:** 🟡 **MEDIUM** (нужно для Basic Auth без перезапуска)

#### ⚠️ Проблема безопасности:
Named Pipe **не имеет Security Descriptor** → любой локальный пользователь может:
1. Перехватить IPC-сообщения
2. Прочитать пароль в cleartext
3. Или подменить конфигурацию

#### 🔒 Рекомендуемый подход (без передачи пароля через IPC):

**3.1. GUI записывает зашифрованный пароль в config.json**

Это уже работает в [`SettingsViewModel.cs:142-173`](TcpRedirector/src/gui/TcpRedirectorGUI/Adapters/Driving/Wpf/ViewModels/SettingsViewModel.cs:142):

```csharp
private async Task SaveAsync()
{
    var currentPwd = Password;
    
    // ✅ Запись в config.json (с DPAPI шифрованием)
    var ok = _config.WriteFull(
        new ProxyConfig { /* ... */ },
        exePath ?? "",
        [.. Rules]);
    
    // Вместо передачи пароля через IPC, послать команду "reload"
    if (_svc is { IsConnected: true })
    {
        await _svc.ReloadConfigAsync();  // ✅ НОВЫЙ МЕТОД
    }
}
```

**3.2. Добавить IPC-метод `reload_config`**

**Файл:** [`IpcClient.cs`](TcpRedirector/src/gui/TcpRedirectorGUI/Infrastructure/Ipc/IpcClient.cs) (новый метод)

```csharp
public async Task<bool> ReloadConfigAsync()
{
    var r = await Call("reload_config", null);
    return r?.GetProperty("status").GetString() == "success";
}
```

**Файл:** [`IpcHandler.h`](TcpRedirector/src/service/TcpRedirectorService/adapters/driving/IpcHandler.h) (новый handler)

```cpp
void ReloadConfig(nlohmann::json& result) {
    bool ok = m_configManager->Load();  // Перечитать config.json
    result["status"] = ok ? "success" : "error";
    if (!ok) {
        result["error"] = "Failed to reload config.json";
    }
}
```

#### Преимущества:
- ✅ Пароль **не передается** через IPC (безопасность)
- ✅ Пароль остается зашифрованным DPAPI
- ✅ Изменения применяются **без перезапуска службы**

#### Альтернатива (передача пароля через IPC):
Если все же нужна передача пароля, добавить **Security Descriptor** для Named Pipe:

```cpp
// В PipeServer.h:CreateNamedPipeW
SECURITY_ATTRIBUTES sa = {0};
sa.nLength = sizeof(SECURITY_ATTRIBUTES);
// SDDL: разрешить доступ только SYSTEM и Administrators
const wchar_t* sddl = L"D:(A;;GA;;;SY)(A;;GA;;;BA)";
ConvertStringSecurityDescriptorToSecurityDescriptorW(
    sddl, SDDL_REVISION_1, &sa.lpSecurityDescriptor, NULL);

HANDLE newPipe = CreateNamedPipeW(
    pipeName,
    PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED,
    PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE | PIPE_WAIT,
    PIPE_UNLIMITED_INSTANCES,
    4096, 4096, 0,
    &sa);  // ✅ С Security Descriptor
```

---

### **Шаг 4: Скрыть Login/Password при Kerberos** ⏱️

**Приоритет:** 🟡 **MEDIUM** (UX улучшение)

#### Файл: [`MainWindow.xaml:142-156`](TcpRedirector/src/gui/TcpRedirectorGUI/MainWindow.xaml:142)

#### Изменения:

1. **Добавить InvertedBoolToVisibilityConverter:**

**Файл:** [`MainWindow.xaml.cs:71-80`](TcpRedirector/src/gui/TcpRedirectorGUI/MainWindow.xaml.cs:71) (новый конвертер)

```csharp
public class InvertedBoolToVisConverter : IValueConverter
{
    public object Convert(object value, Type t, object p, CultureInfo c)
    {
        return value is bool b && b ? Visibility.Collapsed : Visibility.Visible;
    }
    
    public object ConvertBack(object value, Type t, object p, CultureInfo c)
        => throw new NotSupportedException();
}
```

2. **Зарегистрировать конвертер в ресурсах:**

```xml
<Window.Resources>
    <!-- ... существующие конвертеры ... -->
    <local:InvertedBoolToVisConverter x:Key="InvertedBoolToVis"/>
</Window.Resources>
```

3. **Применить к Login/Password полям:**

```xml
<!-- Login поле скрыто при Kerberos -->
<TextBlock Grid.Row="0" Grid.Column="0" Text="Login:"
           VerticalAlignment="Center" Margin="0,2"
           Visibility="{Binding Settings.KerberosEnabled, 
                        Converter={StaticResource InvertedBoolToVis}}"/>
<TextBox Grid.Row="0" Grid.Column="1"
         Text="{Binding Settings.Login, Mode=TwoWay}"
         Margin="0,2"
         Visibility="{Binding Settings.KerberosEnabled, 
                      Converter={StaticResource InvertedBoolToVis}}"/>

<!-- Password поле скрыто при Kerberos -->
<TextBlock Grid.Row="0" Grid.Column="2" Text="Password:"
           VerticalAlignment="Center" Margin="8,2,0,2"
           Visibility="{Binding Settings.KerberosEnabled, 
                        Converter={StaticResource InvertedBoolToVis}}"/>
<PasswordBox Grid.Row="0" Grid.Column="3"
             x:Name="PwdBox" Margin="8,2,0,2"
             Visibility="{Binding Settings.KerberosEnabled, 
                          Converter={StaticResource InvertedBoolToVis}}"/>

<!-- Checkbox Kerberos с подсказкой -->
<CheckBox Grid.Row="1" Grid.Column="1" Grid.ColumnSpan="3"
          Content="Use Kerberos protocol (uses current Windows user)"
          IsChecked="{Binding Settings.KerberosEnabled, Mode=TwoWay}"
          Margin="0,2"
          ToolTip="Kerberos uses your Windows credentials automatically. No password needed."/>
```

#### Результат:
- ✅ При `KerberosEnabled=false`: Login/Password видны
- ✅ При `KerberosEnabled=true`: Login/Password скрыты
- ✅ Tooltip объясняет, что Kerberos использует текущего пользователя

---

### **Шаг 5: Добавить fallback на Basic Auth** ⏱️

**Приоритет:** 🟡 **MEDIUM**

#### Файл: [`TcpRelayServer.h:329-356`](TcpRedirector/src/service/TcpRedirectorService/infrastructure/relay/TcpRelayServer.h:329)

#### Проблема:
Если Kerberos недоступен (`SEC_E_NO_CREDENTIALS`), соединение просто падает.

#### Исправление:

```cpp
// ConnectionHandler в TcpRelayServer.h (строка ~340)

if (m_proxyAuthRequired && !m_kerberosAuth) {
    // Basic Auth
    std::string basic = m_proxyUser + ":" + m_proxyPassword;
    connect_req += "Proxy-Authorization: Basic " + Base64Encode(basic) + "\r\n";
    
} else if (m_kerberosAuth && sspiAvailable) {
    // Kerberos/NTLM
    if (!sspiInitDone) {
        auto r = infrastructure::SspiNegotiate(sspiCtx, "", sspiToken,
            infrastructure::MakeSpn(m_proxyHost));
        
        // ✅ ДОБАВИТЬ: Fallback на Basic Auth при ошибке
        if (r == infrastructure::SspiResult::NoCredentials) {
            Log(domain::LogLevel::Warn, 
                "[Proxy] Kerberos unavailable (SEC_E_NO_CREDENTIALS), fallback to Basic Auth");
            sspiAvailable = false;
            
            // Если есть пароль — использовать Basic Auth
            if (!m_proxyPassword.empty()) {
                std::string basic = m_proxyUser + ":" + m_proxyPassword;
                connect_req += "Proxy-Authorization: Basic " + Base64Encode(basic) + "\r\n";
            } else {
                Log(domain::LogLevel::Error, 
                    "[Proxy] No password available for Basic Auth fallback");
                errorMsg = "Kerberos unavailable and no password configured";
                return false;
            }
            
        } else if (r == infrastructure::SspiResult::Error) {
            Log(domain::LogLevel::Warn, 
                "[Proxy] SSPI initialization failed, fallback to Basic Auth");
            sspiAvailable = false;
            
            if (!m_proxyPassword.empty()) {
                std::string basic = m_proxyUser + ":" + m_proxyPassword;
                connect_req += "Proxy-Authorization: Basic " + Base64Encode(basic) + "\r\n";
            }
        }
        // ... остальной код SSPI ...
    }
}
```

#### Преимущества:
- ✅ Если Kerberos недоступен, автоматически пробуем Basic Auth
- ✅ Повышает надежность аутентификации
- ✅ Логируется причина переключения

---

## 📅 Порядок выполнения

### Фаза 1: Критические баги (обязательно)

1. ✅ **Шаг 1** — Исправить Base64-декодер (30 минут)
2. ✅ **Шаг 2** — Передать Kerberos-флаг через IPC (20 минут)
   - 2.1. `IpcClient.cs` — добавить `kerberos_enabled`
   - 2.2. `IpcHandler.h` — читать `kerberos_enabled`
   - 2.3. `ConfigManager.cpp` — обновлять `m_config.auth.kerberos`

### Фаза 2: Улучшения (желательно)

3. ⚪ **Шаг 3** — Добавить `reload_config` IPC (15 минут, опционально)
4. ⚪ **Шаг 4** — Скрыть Login/Password при Kerberos (10 минут)
5. ⚪ **Шаг 5** — Добавить fallback на Basic Auth (15 минут)

---

## 🧪 План тестирования

### Тест 1: Base64-декодер

```cpp
// Unit-тест в RuleEngineTest.cpp
void TestBase64Decode() {
    // Тест padding
    auto d1 = infrastructure::SspiBase64Decode("YWJjZA==");  // "abcd"
    assert(d1.size() == 4 && d1[0] == 'a' && d1[3] == 'd');
    
    // Тест без padding
    auto d2 = infrastructure::SspiBase64Decode("YWJj");  // "abc"
    assert(d2.size() == 3);
    
    // Тест реального SSPI токена
    auto token = "TlRMTVNTUAABAAAAl4II4g...==";
    auto decoded = infrastructure::SspiBase64Decode(token);
    assert(decoded.size() > 0);
    assert(decoded[0] == 'N');  // "NTLMSSP"
}
```

### Тест 2: Kerberos runtime config

1. Запустить службу
2. Открыть GUI
3. Включить "Authentication required"
4. Включить "Use Kerberos protocol"
5. Нажать "Save"
6. **БЕЗ перезапуска** попробовать подключиться к прокси
7. Проверить логи: `[SSPI] Authenticating as: <username>`
8. Ожидается: `Proxy-Authorization: Negotiate <token>`

### Тест 3: Basic Auth runtime config

1. Запустить службу
2. Открыть GUI
3. Включить "Authentication required"
4. **Отключить** "Use Kerberos protocol"
5. Ввести Login/Password
6. Нажать "Save"
7. **БЕЗ перезапуска** попробовать подключиться
8. Проверить логи: `Proxy-Authorization: Basic <base64>`

### Тест 4: UI скрытие полей

1. Включить "Use Kerberos protocol"
2. Проверить: Login/Password поля **скрыты**
3. Отключить "Use Kerberos protocol"
4. Проверить: Login/Password поля **видны**

### Тест 5: Fallback на Basic Auth

1. Настроить Kerberos в config.json
2. Запустить службу под пользователем **без Kerberos credentials**
3. Попробовать подключиться к прокси
4. Ожидается: Лог `Kerberos unavailable, fallback to Basic Auth`
5. Ожидается: Соединение устанавливается через Basic Auth

---

## ⚠️ Риски и mitigation

| Риск | Описание | Mitigation |
|------|----------|------------|
| **Base64-декодер сломает DPAPI** | Изменение декодера может повлиять на дешифрование паролей | Добавить unit-тест для DPAPI (`ConfigManager::DecryptPassword`) |
| **IPC ломает совместимость** | Добавление поля `kerberos_enabled` может сломать старые GUI | Использовать `j.value("kerberos_enabled", false)` с default значением |
| **Pароль через IPC небезопасен** | Named Pipe без Security Descriptor уязвим | Использовать `reload_config` вместо передачи пароля; или добавить SDDL |
| **Fallback ломает Kerberos-only прокси** | Если прокси требует только Kerberos, fallback на Basic Auth будет отклонен | Добавить опцию конфигурации `"strict_kerberos": true` |

---

## 📊 Итоговая таблица изменений

| Файл | Метод/Строка | Изменение | Приоритет |
|------|--------------|-----------|-----------|
| `auth_sspi.cpp` | `SspiBase64Decode:269` | Удалить `if (c == '=') break;` | 🔴 **CRITICAL** |
| `IpcClient.cs` | `SetConfigAsync:118` | Добавить `kerberos_enabled = config.KerberosEnabled` | 🔴 **CRITICAL** |
| `IpcHandler.h` | `SetConfig:116` | Добавить `config.kerberos_auth = j.value("kerberos_enabled", false);` | 🔴 **CRITICAL** |
| `ConfigManager.cpp` | `SetProxyConfig:72` | Добавить `m_config.auth.kerberos = config.kerberos_auth;` | 🔴 **CRITICAL** |
| `IpcClient.cs` | (новый метод) | Добавить `ReloadConfigAsync()` | 🟡 **MEDIUM** |
| `IpcHandler.h` | (новый handler) | Добавить `ReloadConfig()` | 🟡 **MEDIUM** |
| `MainWindow.xaml.cs` | (новый конвертер) | Добавить `InvertedBoolToVisConverter` | 🟡 **MEDIUM** |
| `MainWindow.xaml` | `<TextBlock Login>:142` | Добавить `Visibility={Binding ..., InvertedBoolToVis}` | 🟡 **MEDIUM** |
| `MainWindow.xaml` | `<PasswordBox>:150` | Добавить `Visibility={Binding ..., InvertedBoolToVis}` | 🟡 **MEDIUM** |
| `TcpRelayServer.h` | `ConnectionHandler:340` | Добавить fallback на Basic Auth при `NoCredentials` | 🟡 **MEDIUM** |

---

## ✅ Checklist для утверждения плана

- [ ] **Шаг 1** (Base64) — критичный, блокирует Kerberos и DPAPI
- [ ] **Шаг 2** (IPC Kerberos) — критичный, runtime config не работает
- [ ] **Шаг 3** (reload_config) — желательный, улучшает безопасность
- [ ] **Шаг 4** (UI скрытие) — желательный, улучшает UX
- [ ] **Шаг 5** (fallback) — желательный, повышает надежность

---

## 📝 Дополнительные замечания

### 1. **Безопасность Named Pipe**

Текущая реализация в [`PipeServer.h:169-176`](TcpRedirector/src/service/TcpRedirectorService/infrastructure/ipc/PipeServer.h:169):

```cpp
HANDLE newPipe = CreateNamedPipeW(
    m_pipeName.c_str(),
    PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED,
    PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE | PIPE_WAIT,
    PIPE_UNLIMITED_INSTANCES,
    4096, 4096, 0,
    NULL);  // ❌ Нет Security Descriptor!
```

**Проблема:** Любой локальный пользователь может подключиться к `\\.\pipe\TcpRedirectorService`.

**Решение:** Добавить SDDL для ограничения доступа (уже упомянуто в REVIEW.md — баг C1).

### 2. **DPAPI Encryption**

Текущая реализация в [`ConfigManager.cpp:417-543`](TcpRedirector/src/service/TcpRedirectorService/infrastructure/config/ConfigManager.cpp:417):

```cpp
std::string ConfigManager::EncryptPassword(const std::wstring& plain) {
    DATA_BLOB in, out;
    // ... DPAPI encryption ...
    return Base64Encode(encrypted);  // ✅ Корректно
}

std::wstring ConfigManager::DecryptPassword(const std::string& encrypted) {
    auto decoded = Base64Decode(encrypted);  // ❌ Использует broken декодер!
    // ... DPAPI decryption ...
}
```

**Проблема:** После исправления Base64-декодера, пароли, зашифрованные **до исправления**, могут быть несовместимы.

**Решение:** Добавить версионирование формата:
```json
"auth": {
  "encryptedPassword": "v2:base64_data",  // v2 = исправленный декодер
  "encryptedPassword_v1": "old_base64"    // Для обратной совместимости
}
```

### 3. **Kerberos SPN Requirements**

Kerberos требует, чтобы SPN (Service Principal Name) совпадал с FQDN прокси-сервера:

```cpp
// auth_sspi.cpp
std::string MakeSpn(const std::string& host) {
    return "HTTP/" + host;  // Например: "HTTP/proxy.domain.com"
}
```

**Требования:**
- `proxy.host` в config.json должен быть **FQDN** (не IP-адрес)
- В Active Directory должен быть зарегистрирован SPN `HTTP/proxy.domain.com`

**Пример конфигурации:**
```json
{
  "proxy": {
    "host": "proxy.example.com",  // ✅ FQDN
    // НЕ: "192.168.1.100"         // ❌ IP не работает с Kerberos
    "port": 3128
  }
}
```

---

## 🎯 Выводы

### Работоспособность Kerberos:
- **При старте службы:** ✅ **РАБОТАЕТ** (config.json читается корректно)
- **Runtime изменения:** ❌ **НЕ РАБОТАЕТ** (IPC не передает `kerberos_enabled`)
- **Base64-декодер:** ❌ **СЛОМАН** (обрезает SSPI-токены и DPAPI-пароли на `=`)

### Работоспособность Basic Auth:
- **При старте службы:** ✅ **РАБОТАЕТ** (пароль читается из config.json и расшифровывается DPAPI)
- **Runtime изменения пароля:** ⚠️ **ТРЕБУЕТ ПЕРЕЗАПУСКА** (пароль не передается через IPC)
- **Примечание:** Для Kerberos пароль **не нужен** (использует текущего пользователя автоматически)

### Безопасность:
- ✅ DPAPI-шифрование пароля в config.json **работает корректно**
- ✅ Пароль **не загружается** в UI (правильное решение для безопасности)
- ❌ Named Pipe **не защищен** (баг C1 из REVIEW.md — любой локальный пользователь может управлять службой)
- ⚠️ Передача пароля через незащищенный Named Pipe **не рекомендуется** (лучше использовать reload_config)

### UI/UX:
- ⚠️ Login/Password показываются при Kerberos (confusing — пользователь вводит пароль, который не используется)
- ⚠️ Нет подсказок о том, что Kerberos использует текущего Windows-пользователя
- ⚠️ Непонятно, когда нужен пароль, а когда нет

### Критичность багов:
1. **CRITICAL:** Kerberos-флаг не передается через IPC (нельзя переключаться между Kerberos/Basic Auth без перезапуска)
2. **HIGH:** Base64-декодер ломает SSPI-токены И DPAPI-дешифрование (критично для обоих режимов)
3. **MEDIUM:** Пароль не передается через IPC (критично только для Basic Auth, есть workaround — перезапуск)
4. **MEDIUM:** UI показывает Login/Password при Kerberos (UX проблема, не влияет на работоспособность)

### Рекомендации:
1. **Обязательно исправить:**
   - Шаг 1 (Base64-декодер) — блокирует Kerberos И DPAPI
   - Шаг 2 (IPC Kerberos-флаг) — блокирует runtime переключение режимов
2. **Желательно (улучшение UX):**
   - Шаг 3 (reload_config) — для безопасной передачи паролей в Basic Auth без перезапуска
   - Шаг 4 (UI скрытие полей) — убирает confusion о необходимости пароля
   - Шаг 5 (fallback на Basic Auth) — повышает надежность при недоступности Kerberos

---

## 📌 Контактная информация

**Документ:** `KERBEROS_VALIDATION.md`  
**Дата:** 2026-07-07  
**Автор:** Automation Analysis  
**Статус:** 🔴 **Requires Critical Fixes**
