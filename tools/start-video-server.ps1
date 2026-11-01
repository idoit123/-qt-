param([ValidateRange(1,65535)][int]$Port = 18888)
$ErrorActionPreference = 'Stop'
$wsl = Join-Path $env:ProgramFiles 'WSL\wsl.exe'
if (!(Test-Path -LiteralPath $wsl)) { $wsl = 'wsl.exe' }
$script = (Join-Path $PSScriptRoot 'start-video-server.sh').Replace('\', '/')
$linuxScript = & $wsl -d Ubuntu --exec wslpath -u $script
if ($LASTEXITCODE -ne 0 -or !$linuxScript) { throw 'Cannot access Ubuntu WSL.' }
& $wsl -d Ubuntu -- bash $linuxScript.Trim() $Port
exit $LASTEXITCODE
