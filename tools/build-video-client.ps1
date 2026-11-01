param([string]$QtRoot = 'F:\Qt\Qt5.14.2')
$ErrorActionPreference = 'Stop'
$repo = Split-Path $PSScriptRoot -Parent
$qtBin = Join-Path $QtRoot '5.14.2\mingw73_64\bin'
$compilerBin = Join-Path $QtRoot 'Tools\mingw730_64\bin'
$qmake = Join-Path $qtBin 'qmake.exe'
$make = Join-Path $compilerBin 'mingw32-make.exe'
if (!(Test-Path -LiteralPath $qmake) -or !(Test-Path -LiteralPath $make)) {
    throw 'Qt 5.14.2 / MinGW 7.3 not found. Pass -QtRoot with the installation root.'
}
$env:PATH = "$qtBin;$compilerBin;$env:PATH"
$build = Join-Path $repo 'build-video-client'
New-Item -ItemType Directory -Force $build | Out-Null
Push-Location $build
try {
    & $qmake (Join-Path $repo 'TcpClient\TcpClient.pro') 'CONFIG+=release' 'CONFIG-=debug'
    if ($LASTEXITCODE -ne 0) { throw 'qmake failed' }
    & $make -s -j4
    if ($LASTEXITCODE -ne 0) { throw 'Client build failed' }
    Write-Host "Built: $build\release\TcpClient.exe (start with --monitor)"
} finally { Pop-Location }
