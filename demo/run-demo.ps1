param([ValidateSet('Server', 'Local', 'LAN', 'Public')][string]$Mode = 'Public')
$ErrorActionPreference = 'Stop'
try {
    $config = Get-Content -LiteralPath (Join-Path $PSScriptRoot 'endpoint.json') -Raw | ConvertFrom-Json
    $exe = Join-Path $PSScriptRoot 'FSRemoteNetworkDemo.exe'
    if (-not (Test-Path -LiteralPath $exe)) { throw 'The demo executable is missing. Copy the entire NetworkDemo folder.' }
    $logDir = Join-Path $PSScriptRoot 'logs'
    New-Item -ItemType Directory -Force -Path $logDir | Out-Null
    $logFile = Join-Path $logDir ("{0}-{1}-{2}.log" -f $Mode, (Get-Date -Format 'yyyyMMdd-HHmmss'), $PID)
    Write-Host "Log: $logFile"
    if ($Mode -eq 'Server') {
        Write-Host "Run this on server computer $($config.lan_host). Leave this window open."
        & $exe --server --port $config.listen_port --minutes 30 2>&1 | Tee-Object -FilePath $logFile
        $result = $LASTEXITCODE
    } else {
        $target = switch ($Mode) { 'Local' { '127.0.0.1' }; 'LAN' { $config.lan_host }; 'Public' { $config.public_host } }
        $port = if ($Mode -eq 'Public') { $config.public_port } else { $config.listen_port }
        $url = 'ws://{0}:{1}/probe' -f $target, $port
        $result = 0
        for ($round = 1; $round -le 3; $round++) {
            "Connection attempt $round/3" | Tee-Object -FilePath $logFile -Append
            & $exe --url $url 2>&1 | Tee-Object -FilePath $logFile -Append
            if ($LASTEXITCODE -ne 0) { $result = $LASTEXITCODE; break }
        }
        if ($result -eq 0) { Write-Host 'PASS: all three connections completed successfully.' -ForegroundColor Green }
        else { Write-Host 'FAIL: send the log file for diagnosis.' -ForegroundColor Red }
    }
    exit $result
} catch {
    Write-Host $_.Exception.Message -ForegroundColor Red
    exit 1
}
