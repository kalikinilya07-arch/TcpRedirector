# WinDivert Error 5 Troubleshooting

## Step 1: Run CMD as Administrator

1. Press `Windows Key` + `R`
2. Type `cmd`
3. Press `Ctrl` + `Shift` + `Enter`
4. Click `Yes` on UAC prompt

## Step 2: Check WinDivert Driver

Run these commands in the admin CMD window:

```cmd
sc query WinDivert
```

**Send the output.**

If "The specified service does not exist" → go to Step 3.

If "STATE: STOPPED" → run:
```cmd
net start WinDivert
```
Then try the application again.

If "STATE: RUNNING" → go to Step 4.

## Step 3: Add Windows Defender Exclusion

Windows Defender may delete WinDivert driver. Add exclusion first:

```cmd
powershell -Command "Add-MpPreference -ExclusionPath 'C:\ProgramData\TcpRedirector'"
powershell -Command "Add-MpPreference -ExclusionPath 'C:\Windows\System32\drivers\WinDivert64.sys'"
```

**Send the output.**

## Step 4: Find and Install WinDivert Driver

```cmd
dir /s /b C:\WinDivertInstall.exe 2>nul || dir /s /b C:\WinDivert*.exe 2>nul || dir /s /b D:\WinDivert*.exe 2>nul
```

**Send the output.**

If you see a path like `C:\WinDivert\WinDivertInstall.exe`, run:
```cmd
"C:\WinDivert\WinDivertInstall.exe" install
```

(Replace the path with the one found above.)

**Send the output.**

If nothing found → download WinDivert from https://github.com/basil00/WinDivert/releases , extract, run `WinDivertInstall.exe install` from the extracted folder.

After installation, try the application again.

## Step 4: Run Application and Capture Log

```cmd
cd C:\Users\user\Desktop\TcpRedirector\build
TcpRedirectorService.exe --console 2>C:\Users\user\Desktop\windivert_debug.log
```

Wait 10 seconds, then press `Ctrl` + `C`.

**Send the file `C:\Users\user\Desktop\windivert_debug.log`**

## Step 5: Check Admin Rights

```cmd
whoami /groups | find "S-1-5-32-544"
```

**Send the output.**

If empty → you are NOT running as Administrator. Repeat Step 1.

## Summary: Send these 3 things

| # | File / Command output |
|---|----------------------|
| 1 | Output of `sc query WinDivert` |
| 2 | File `C:\Users\user\Desktop\windivert_debug.log` |
| 3 | Output of `whoami /groups | find "S-1-5-32-544"` |