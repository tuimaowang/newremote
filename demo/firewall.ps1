param([switch]$Remove)
$ErrorActionPreference = 'Stop'
try {
    $identity = [Security.Principal.WindowsIdentity]::GetCurrent()
    $principal = [Security.Principal.WindowsPrincipal]::new($identity)
    if (-not $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
        throw 'Right-click this .cmd file and choose Run as administrator on the SERVER computer.'
    }
    $exe = Join-Path $PSScriptRoot 'FSRemoteNetworkDemo.exe'
    $name = 'FSRemote-NetworkDemo-Temporary'
    if ($Remove) {
        Get-NetFirewallRule -Name $name -ErrorAction SilentlyContinue | Remove-NetFirewallRule
        Write-Host 'Temporary demo firewall rule removed.'
    } else {
        if (-not (Test-Path -LiteralPath $exe)) { throw 'Demo executable is missing.' }
        $config = Get-Content -LiteralPath (Join-Path $PSScriptRoot 'endpoint.json') -Raw | ConvertFrom-Json
        $port = [int]$config.listen_port
        if ($port -lt 1 -or $port -gt 65535) { throw 'Invalid port.' }
        Get-NetFirewallRule -Name $name -ErrorAction SilentlyContinue | Remove-NetFirewallRule
        New-NetFirewallRule -Name $name -DisplayName 'FSRemote temporary communication demo' `
            -Direction Inbound -Action Allow -Protocol TCP -LocalPort $port -Program $exe -Profile Any | Out-Null
        Write-Host "Allowed TCP $port for $exe only. Remove the rule after testing."
    }
} catch {
    Write-Host $_.Exception.Message -ForegroundColor Red
    exit 1
}
