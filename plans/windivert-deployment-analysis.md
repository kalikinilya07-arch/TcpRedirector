# WinDivert Deployment Analysis

## How WinDivert Works

| Operation | Requires Admin? | When |
|-----------|----------------|------|
| `WinDivertOpen()` loads driver dynamically | YES | First call, if driver not running |
| `WinDivertOpen()` opens handle to running driver | NO | If driver already loaded as service |
| `WinDivertInstall.exe install` (one-time) | YES | Initial setup — creates service |
| Defender deletes `WinDivert64.sys` | — | Happens automatically |

## Current Flow (the problem)

```
TcpRedirectorService.exe --console (admin)
  └→ WinDivertOpen()
       ├→ Loads WinDivert64.sys dynamically? → YES (admin) → works
       └→ WinDivert64.sys was deleted by Defender? → err=2 FILE_NOT_FOUND
```

Even with admin rights, if Defender deleted the .sys file, nothing can load it.

## Two Deployment Models

### Model A: Admin runs console mode (current, fragile)

- Admin launches `TcpRedirectorService.exe --console`
- WinDivertOpen loads driver dynamically (needs admin)
- **Problem**: Defender may delete driver file
- **Problem**: Non-admin cannot run at all

### Model B: WinDivert as permanent Windows service (robust)

```
Step 1 (one-time, admin): WinDivertInstall.exe install
  └→ Copies WinDivert64.sys to C:\Windows\System32\drivers\
  └→ Creates service "WinDivert" (auto-start)
  └→ Service loads driver at boot

Step 2 (any user, any time): TcpRedirectorService.exe --console
  └→ WinDivertOpen()
       └→ Driver already running via service → SUCCESS (no admin needed)
```

**Advantages:**
- Non-admin users can run the application
- Driver stays loaded even after reboot
- Defender exclusion protects the .sys file

## What The Application Currently Does

In [`main.cpp`](TcpRedirector/src/service/TcpRedirectorService/main.cpp:78-97):
```
--install-service : installs TcpRedirectorService as Windows Service
--uninstall-service : removes it
```

But there is NO `--install-windivert` command.

## Recommended: Add `--install-windivert` flag

The application should handle WinDivert installation itself, without requiring the user to find `WinDivertInstall.exe`.

### Implementation Plan

Add to [`main.cpp`](TcpRedirector/src/service/TcpRedirectorService/main.cpp:68-151):

```cpp
else if (arg == "--install-windivert") {
    // 1. Add Defender exclusion
    system("powershell Add-MpPreference -ExclusionPath 'C:\\Windows\\System32\\drivers\\WinDivert64.sys'");

    // 2. Find WinDivertInstall.exe next to our exe
    wchar_t path[MAX_PATH];
    GetModuleFileNameW(NULL, path, MAX_PATH);
    std::filesystem::path exeDir = std::filesystem::path(path).parent_path();

    auto installer = exeDir / "WinDivertInstall.exe";
    if (!exists(installer)) {
        // Also check external/WinDivert/ relative to project root
        installer = exeDir.parent_path().parent_path() / "external" / "WinDivert" / "WinDivertInstall.exe";
    }

    if (exists(installer)) {
        std::wstring cmd = L"\"" + installer.wstring() + L"\" install";
        _wsystem(cmd.c_str());
        printf("WinDivert driver installed.\n");
    } else {
        printf("WinDivertInstall.exe not found.\n");
        return 1;
    }
    return 0;
}
```

### Updated Deployment Flow

```
=== FIRST TIME SETUP (admin does once) ===
TcpRedirectorService.exe --install-windivert
  └→ Adds Defender exclusion for WinDivert64.sys
  └→ Installs WinDivert as Windows service (auto-start)

=== NORMAL USAGE (any user) ===
TcpRedirectorService.exe --console
  └→ WinDivertOpen() → driver already running → SUCCESS
```

## Summary: Answer to Your Questions

| Question | Answer |
|----------|--------|
| Переустановка драйвера требуется? | **Да, если Defender удалил файл.** Но достаточно сделать один раз после добавления исключения. |
| Он разве не ставится при запуске от администратора? | `WinDivertOpen()` **загружает** драйвер динамически, но не **устанавливает** его как службу. Если Defender удалил .sys — загружать нечего. |
| Можно как службу установить? | **Да.** `WinDivertInstall.exe install` создаёт службу WinDivert с автостартом. После этого любой пользователь может работать. |
| Как быть с non-admin пользователем? | Админ делает `--install-windivert` один раз. Non-admin запускает `--console`. WinDivert уже работает как служба. |