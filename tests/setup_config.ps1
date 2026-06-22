# setup_config.ps1
# Запускать от имени Администратора!
# Щёлкните правой кнопкой -> "Run with PowerShell" или
#	PowerShell -ExecutionPolicy Bypass -File setup_config.ps1

$configPath = "C:\ProgramData\TcpRedirector\config.json"

# Создаём директорию, если нет
$dir = Split-Path $configPath -Parent
if (-not (Test-Path $dir)) {
    New-Item -ItemType Directory -Path $dir -Force | Out-Null
}

$config = @{
    app = @{
        exePath = "C:\Users\user\Desktop\TcpRedirector\tests\packet_generator.exe"
    }
    proxy = @{
        enabled = $true
        host = "127.0.0.1"
        port = 3128
    }
    auth = @{
        enabled = $false
        username = ""
        encryptedPassword = ""
        kerberos = $false
    }
    log = @{
        level = 3
        fileEnabled = $true
        maxSizeMB = 10
    }
    stats = @{
        updateIntervalMs = 2000
    }
    rules = @()
}

$config | ConvertTo-Json -Depth 10 | Out-File -FilePath $configPath -Encoding ascii -Force
Write-Host "[OK] Config written to $configPath"
Write-Host "[INFO] exePath = packet_generator.exe -> google.com:80"
Write-Host "[INFO] proxy   = 127.0.0.1:3128"