$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$exe = Join-Path $root 'dist/NetworkDemo/FSRemoteNetworkDemo.exe'
$run = Join-Path $root ('build/network-demo/verification-' + [guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $run | Out-Null
$stdout = Join-Path $run 'server.log'
$stderr = Join-Path $run 'server-error.log'
$server = $null

# Exercise malformed traffic independently of the Qt demo client.
Add-Type -TypeDefinition @'
using System;
using System.Net.WebSockets;
using System.Text;
using System.Threading;
public static class DemoNegativeProbe {
    public static bool Rejected(string url, string mode) {
        using (var ws = new ClientWebSocket())
        using (var deadline = new CancellationTokenSource(5000)) {
            ws.Options.Proxy = null;
            ws.ConnectAsync(new Uri(url), deadline.Token).GetAwaiter().GetResult();
            var buffer = new ArraySegment<byte>(new byte[1024]);
            var hello = ws.ReceiveAsync(buffer, deadline.Token).GetAwaiter().GetResult();
            if (hello.MessageType != WebSocketMessageType.Text ||
                Encoding.UTF8.GetString(buffer.Array, 0, hello.Count) != "FSREMOTE-DEMO/1 READY")
                throw new Exception("Expected a valid greeting before negative probe.");
            byte[] data = Encoding.UTF8.GetBytes(mode == "oversize" ? new string('x', 1024) : "WRONG");
            try {
                ws.SendAsync(new ArraySegment<byte>(data),
                    mode == "binary" ? WebSocketMessageType.Binary : WebSocketMessageType.Text,
                    true, deadline.Token).GetAwaiter().GetResult();
                var reply = ws.ReceiveAsync(buffer, deadline.Token).GetAwaiter().GetResult();
                return reply.MessageType == WebSocketMessageType.Close;
            } catch (WebSocketException) { return !deadline.IsCancellationRequested; }
        }
    }
}
'@

try {
    $server = Start-Process -FilePath $exe -ArgumentList '--server --listen 127.0.0.1 --port 0 --minutes 1' `
        -WindowStyle Hidden -RedirectStandardOutput $stdout -RedirectStandardError $stderr -PassThru
    $port = 0
    for ($attempt = 0; $attempt -lt 50; $attempt++) {
        Start-Sleep -Milliseconds 100
        if ($server.HasExited) { throw 'Test server exited before readiness.' }
        $content = Get-Content -LiteralPath $stdout -Raw -ErrorAction SilentlyContinue
        if ($content -match 'LISTENING ws://127\.0\.0\.1:(\d+)/probe') { $port = [int]$Matches[1]; break }
    }
    if ($port -eq 0) { throw 'No listening endpoint was reported.' }
    $url = "ws://127.0.0.1:$port/probe"
    for ($round = 1; $round -le 3; $round++) {
        $output = & $exe --url $url
        if ($LASTEXITCODE -ne 0 -or -not ($output -match 'PASS:')) { throw "Round trip $round failed: $output" }
    }
    Write-Host 'PASS: three independent connections, 15/15 replies.'
    foreach ($mode in @('text', 'binary', 'oversize')) {
        if (-not [DemoNegativeProbe]::Rejected($url, $mode)) { throw "$mode traffic was not rejected." }
        Write-Host "PASS: rejected $mode traffic."
    }
    $output = & $exe --url $url
    if ($LASTEXITCODE -ne 0) { throw 'Server unhealthy after rejected traffic.' }
    Write-Host 'PASS: healthy after invalid traffic.'
    $output = & $exe --server --listen 127.0.0.1 --port $port --minutes 1
    if ($LASTEXITCODE -ne 1 -or -not ($output -match 'FAIL: listen')) { throw 'Occupied port did not fail clearly.' }
    Write-Host 'PASS: occupied port reported.'
    $output = & $exe --url "ws://127.0.0.1:$port/wrong"
    if ($LASTEXITCODE -ne 2) { throw 'Malformed endpoint not rejected.' }
    $server.Kill()
    $server.WaitForExit()
    $output = & $exe --url $url
    if ($LASTEXITCODE -ne 1 -or -not ($output -match 'FAIL:')) { throw 'Offline server not reported.' }
    Write-Host 'PASS: malformed endpoint and offline server reported.'
    $errors = Get-Content -LiteralPath $stderr -Raw
    if ($errors -match 'Failed to create a timer') { throw 'Qt rejected-frame timer regression.' }
    Write-Host "Verification logs: $run"
} finally {
    if ($null -ne $server -and -not $server.HasExited) { $server.Kill(); $server.WaitForExit() }
}
