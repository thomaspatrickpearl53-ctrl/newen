# Installs the "newen" game configuration into TrenchBroom's user folder (%APPDATA%\TrenchBroom\games\newen).
# The entity definitions come from TrenchBroom's own Half-Life config, so pass the folder TrenchBroom.exe is in:
#   powershell -ExecutionPolicy Bypass -File tools\trenchbroom\install.ps1 -TrenchBroom "C:\path\to\TrenchBroom"
param(
	[string]$TrenchBroom = "$env:USERPROFILE\Downloads\TrenchBroom-Win64-AMD64-v2026.2-Release"
)
$ErrorActionPreference = 'Stop'
$src = Join-Path $PSScriptRoot 'newen'
$fgd = Join-Path $TrenchBroom 'games\Halflife\HalfLife.fgd'
if (-not (Test-Path $fgd)) {
	throw "Couldn't find $fgd; pass -TrenchBroom with the folder TrenchBroom.exe is in"
}
$dst = Join-Path $env:APPDATA 'TrenchBroom\games\newen'
New-Item -ItemType Directory -Force $dst | Out-Null
Copy-Item (Join-Path $src 'GameConfig.cfg'), (Join-Path $src 'Icon.png') $dst -Force
Copy-Item $fgd $dst -Force
Write-Output "Installed the newen game configuration in $dst"
