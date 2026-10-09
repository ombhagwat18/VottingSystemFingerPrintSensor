# Writes server/config.json from environment variables set by setup.bat.
# Values come from the environment so special characters in passwords never touch cmd quoting.
param([Parameter(Mandatory = $true)][string]$Out)

$phone = ($env:VS_PHONE -replace '\D', '')
if ($phone.Length -eq 10) { $phone = '91' + $phone }
if ($phone.Length -gt 0 -and $phone.Length -ne 12) {
    Write-Host "Warning: phone '$env:VS_PHONE' is not 10 digits (or 91 + 10). Check it in config.json." -ForegroundColor Yellow
}

$port = if ($env:VS_PORT) { $env:VS_PORT.Trim() } else { 'auto' }
if (-not $port) { $port = 'auto' }

$cfg = [ordered]@{
    admin_user  = $(if ($env:VS_ADMIN) { $env:VS_ADMIN } else { 'admin' })
    admin_pass  = $env:VS_ADMINPASS
    api_key     = $env:VS_APIKEY
    admin_phone = $phone
    http_port   = 8080
    bind        = '0.0.0.0'
    serial_port = $port
    booth       = 'Voting Booth 1'
    max_id      = 127
}
$json = $cfg | ConvertTo-Json
[System.IO.File]::WriteAllText($Out, $json, (New-Object System.Text.UTF8Encoding($false)))
Write-Host "Wrote $Out"
