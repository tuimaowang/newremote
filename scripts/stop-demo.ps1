param([Parameter(Mandatory = $true)][string]$Session)
$ErrorActionPreference = 'Stop'
$records = Get-Content -LiteralPath (Join-Path $Session 'processes.json') -Raw | ConvertFrom-Json
foreach ($record in $records) {
    $process = Get-Process -Id $record.id -ErrorAction SilentlyContinue
    $started = ([datetime]$record.started).ToUniversalTime()
    if ($process -and $process.Path -eq $record.path -and
        $process.StartTime.ToUniversalTime().Ticks -eq $started.Ticks) {
        Stop-Process -InputObject $process
        $process.WaitForExit()
    }
}
