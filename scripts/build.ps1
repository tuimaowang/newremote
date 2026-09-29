param(
    [string]$QtRoot = 'C:\Qt\6.11.1\msvc2022_64',
    [switch]$SkipTests
)
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$build = Join-Path $root 'build'
$dist = Join-Path $root 'dist'
& cmake -S $root -B $build -G 'Visual Studio 17 2022' -A x64 "-DCMAKE_PREFIX_PATH=$QtRoot"
if ($LASTEXITCODE -ne 0) { throw 'CMake configuration failed.' }
& cmake --build $build --config Release --parallel 4
if ($LASTEXITCODE -ne 0) { throw 'Compilation failed.' }
$oldPath = $env:PATH
try {
    $env:PATH = "$QtRoot\bin;$oldPath"
    if (-not $SkipTests) {
        & ctest --test-dir $build -C Release --output-on-failure
        if ($LASTEXITCODE -ne 0) { throw 'Tests failed.' }
    }
    New-Item -ItemType Directory -Force -Path $dist | Out-Null
    Copy-Item -LiteralPath "$build\Release\FSRemoteMessages.exe" -Destination $dist
    Copy-Item -LiteralPath "$build\Release\FSRemoteMessageServer.exe" -Destination $dist
    & "$QtRoot\bin\windeployqt.exe" --release --no-translations --no-system-d3d-compiler --no-opengl-sw "$dist\FSRemoteMessages.exe" "$dist\FSRemoteMessageServer.exe"
    if ($LASTEXITCODE -ne 0) { throw 'Qt runtime deployment failed.' }
} finally {
    $env:PATH = $oldPath
}
Write-Host "Ready: $dist"

