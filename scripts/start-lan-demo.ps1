param()
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$dist = Join-Path $root 'dist'
if (-not (Test-Path "$dist\FSRemoteMessageServer.exe")) { throw 'Run scripts/build.ps1 first.' }

$data = Join-Path $root 'lan-demo-data'
$session = Join-Path $data ([guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Force -Path $session | Out-Null
$appData = Join-Path $env:APPDATA 'FSRemote\FSRemoteMessages'
$serverConfig = Join-Path $appData 'lan-server.json'
$serverLog = Join-Path $session 'server.log'
$serverError = Join-Path $session 'server-error.log'

$server = Start-Process -FilePath "$dist\FSRemoteMessageServer.exe" -ArgumentList '--lan' `
    -WorkingDirectory $dist -WindowStyle Hidden -RedirectStandardOutput $serverLog `
    -RedirectStandardError $serverError -PassThru
try {
    $ready = $false
    for ($i = 0; $i -lt 80; $i++) {
        Start-Sleep -Milliseconds 100
        if ($server.HasExited) { throw "Server failed. See $serverError" }
        if ((Test-Path $serverLog) -and (Get-Content $serverLog -Raw) -match 'Listening') {
            $ready = $true
            break
        }
    }
    if (-not $ready) { throw "Server startup timed out. See $serverLog" }
    if (-not (Test-Path $serverConfig)) { throw "Automatic configuration was not created: $serverConfig" }
    Copy-Item (Join-Path $appData 'client-device-a.json') (Join-Path $session 'client-device-a.json') -Force
    Copy-Item (Join-Path $appData 'client-device-b.json') (Join-Path $session 'client-device-b.json') -Force
    Copy-Item $serverConfig (Join-Path $session 'server.json') -Force
    [System.IO.File]::WriteAllText((Join-Path $session 'process.json'),
        (@{ pid = $server.Id; started = (Get-Date).ToUniversalTime().ToString('o') } | ConvertTo-Json))
    Write-Host "LAN server is running."
    Get-Content $serverLog -Tail 5
    Write-Host "Copy client-device-a.json or client-device-b.json to each computer beside FSRemoteMessages.exe."
    Write-Host "Config export: $session"
    Write-Host "Stop with: Stop-Process -Id $($server.Id)"
} catch {
    if (-not $server.HasExited) { $server.Kill() }
    throw
}
