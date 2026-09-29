param([string]$QtRoot = 'C:\Qt\6.11.1\msvc2022_64')
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$build = Join-Path $root 'build/network-demo'
$package = Join-Path $root 'dist/NetworkDemo'
& cmake -S $root -B $build -G 'Visual Studio 17 2022' -A x64 "-DCMAKE_PREFIX_PATH=$QtRoot"
if ($LASTEXITCODE -ne 0) { throw 'CMake configuration failed.' }
& cmake --build $build --config Release --target FSRemoteNetworkDemo --parallel 4
if ($LASTEXITCODE -ne 0) { throw 'Demo build failed.' }
New-Item -ItemType Directory -Force -Path $package | Out-Null
Copy-Item -LiteralPath "$build/Release/FSRemoteNetworkDemo.exe" -Destination $package
Get-ChildItem -LiteralPath "$root/demo" -File | Where-Object { $_.Extension -in @('.cmd', '.ps1', '.json', '.txt') } |
    Copy-Item -Destination $package
& "$QtRoot/bin/windeployqt.exe" --release --no-translations --no-system-d3d-compiler --no-opengl-sw "$package/FSRemoteNetworkDemo.exe"
if ($LASTEXITCODE -ne 0) { throw 'Runtime deployment failed.' }
# Include redistributable CRT DLLs so another Windows PC needs no development tools.
$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio/Installer/vswhere.exe'
if (-not (Test-Path -LiteralPath $vswhere)) { throw 'vswhere is required to locate the redistributable runtime.' }
$vsInstall = & $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
$crt = Get-ChildItem -LiteralPath "$vsInstall/VC/Redist/MSVC" -Directory |
    Where-Object { $_.Name -match '^\d+\.\d+\.\d+$' } |
    Sort-Object { [version]$_.Name } -Descending |
    ForEach-Object { Join-Path $_.FullName 'x64/Microsoft.VC143.CRT' } |
    Where-Object { Test-Path -LiteralPath $_ } | Select-Object -First 1
if (-not $crt) { throw 'x64 redistributable CRT was not found.' }
Get-ChildItem -LiteralPath $crt -Filter '*.dll' | Copy-Item -Destination $package
$archive = Join-Path $root 'dist/NetworkDemo.zip'
# Archive only deployment files, excluding logs left by previous user tests.
$zipStage = Join-Path $build ('package-' + [guid]::NewGuid().ToString('N'))
$zipFolder = Join-Path $zipStage 'NetworkDemo'
New-Item -ItemType Directory -Force -Path $zipFolder | Out-Null
Get-ChildItem -LiteralPath $package | Where-Object { $_.Name -ne 'logs' -and $_.Extension -ne '.log' } |
    Copy-Item -Destination $zipFolder -Recurse
Compress-Archive -LiteralPath $zipFolder -DestinationPath $archive -Force
Write-Host "Ready: $package"
Write-Host "Portable archive: $archive"
