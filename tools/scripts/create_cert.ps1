$certPath = "tools\certs\TcpRedirector.pfx"
$cerPath  = "tools\certs\TcpRedirector.cer"
$password = ConvertTo-SecureString -String "tcp123" -Force -AsPlainText

Write-Host "Creating self-signed code signing certificate..." -ForegroundColor Cyan

$cert = New-SelfSignedCertificate `
    -Type CodeSigningCert `
    -Subject "CN=TcpRedirector" `
    -KeyUsage DigitalSignature `
    -CertStoreLocation "Cert:\CurrentUser\My"

Write-Host "  Thumbprint: $($cert.Thumbprint)"

# Create directory
New-Item -ItemType Directory -Path "tools\certs" -Force | Out-Null

# Export to .pfx
Export-PfxCertificate -Cert $cert -FilePath $certPath -Password $password | Out-Null
Write-Host "  PFX exported: $certPath"

# Export to .cer (public key only)
Export-Certificate -Cert $cert -FilePath $cerPath -Type CERT | Out-Null
Write-Host "  CER exported: $cerPath"

# Clean up from personal store
Remove-Item "Cert:\CurrentUser\My\$($cert.Thumbprint)" -Force | Out-Null

Write-Host "Certificate created successfully!" -ForegroundColor Green
