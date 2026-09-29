param([int]$Port = 0)
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$dist = Join-Path $root 'dist'
$data = Join-Path $root 'demo-data'
if (-not (Test-Path "$dist\FSRemoteMessages.exe")) { throw 'Run scripts/build.ps1 first.' }
if ($Port -eq 0) {
    $probe = [System.Net.Sockets.TcpListener]::new([System.Net.IPAddress]::Loopback, 0)
    $probe.Start()
    $Port = $probe.LocalEndpoint.Port
    $probe.Stop()
}
if ($Port -lt 1 -or $Port -gt 65535) { throw 'Invalid port.' }
New-Item -ItemType Directory -Force -Path $data | Out-Null
# Each run owns its configuration and logs; never overwrite another running demo's credentials.
$session = Join-Path $data ([guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $session | Out-Null
function New-Token {
    $bytes = New-Object byte[] 32
    $rng = [System.Security.Cryptography.RandomNumberGenerator]::Create()
    try { $rng.GetBytes($bytes) } finally { $rng.Dispose() }
    return [Convert]::ToBase64String($bytes)
}
function Write-Json($Path, $Value) {
    [System.IO.File]::WriteAllText($Path, ($Value | ConvertTo-Json -Depth 6), [System.Text.UTF8Encoding]::new($false))
}
$tokenA = New-Token
$tokenB = New-Token
Write-Json "$session\server.json" @{
    listen = '127.0.0.1'; port = $Port
    devices = @(
        @{id = 'device-a'; name = 'Desktop A'; token = $tokenA},
        @{id = 'device-b'; name = 'Desktop B'; token = $tokenB}
    )
}
Write-Json "$session\client-a.json" @{server = "ws://127.0.0.1:$Port"; device_id = 'device-a'; token = $tokenA}
Write-Json "$session\client-b.json" @{server = "ws://127.0.0.1:$Port"; device_id = 'device-b'; token = $tokenB}
$processes = @()
try {
    $server = Start-Process -FilePath "$dist\FSRemoteMessageServer.exe" -ArgumentList @('--config', "`"$session\server.json`"") -WorkingDirectory $dist -WindowStyle Hidden -RedirectStandardOutput "$session\server.log" -RedirectStandardError "$session\server-error.log" -PassThru
    $processes += $server
    $ready = $false
    for ($i = 0; $i -lt 50; $i++) {
        Start-Sleep -Milliseconds 100
        if ($server.HasExited) { throw "Server failed. See $session\server-error.log" }
        if ((Test-Path "$session\server.log") -and (Get-Content "$session\server.log" -Raw) -match 'Listening') { $ready = $true; break }
    }
    if (-not $ready) { throw 'Server startup timed out.' }
    foreach ($name in @('a', 'b')) {
        $processes += Start-Process -FilePath "$dist\FSRemoteMessages.exe" -ArgumentList @('--config', "`"$session\client-$name.json`"") -WorkingDirectory $dist -PassThru
    }
    Write-Json "$session\processes.json" @($processes | ForEach-Object { @{id = $_.Id; path = $_.Path; started = $_.StartTime.ToUniversalTime().ToString('o')} })
    Write-Host "Demo running on ws://127.0.0.1:$Port"
    Write-Host "Stop with: .\scripts\stop-demo.ps1 -Session '$session'"
} catch {
    foreach ($process in $processes) { if (-not $process.HasExited) { $process.Kill() } }
    throw
}

