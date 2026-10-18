# Installs the "newen" game configuration into TrenchBroom's user folder (%APPDATA%\TrenchBroom\games\newen):
# the game config, the compile profiles ("Compile and play", "Compile only") and newen as the engine to launch.
# The entity definitions come from TrenchBroom's own Half-Life config, so pass the folder TrenchBroom.exe is in:
#   powershell -ExecutionPolicy Bypass -File tools\trenchbroom\install.ps1 -TrenchBroom "C:\path\to\TrenchBroom"
# Close TrenchBroom first: it keeps the profiles in memory and writes them back when they're edited.
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
Copy-Item (Join-Path $src 'GameConfig.cfg'), (Join-Path $src 'Icon.png'), (Join-Path $src 'CompilationProfiles.cfg') $dst -Force
Copy-Item $fgd $dst -Force

# the engine profile the "Compile and play" profile launches (its id is fixed there)
$exe = (Resolve-Path (Join-Path $PSScriptRoot '..\..\bin')).Path + '\newen.exe'
$engines = [ordered]@{
	profiles = @([ordered]@{
		id = 'e515644c-8531-4c7a-ada6-b42bec7aaaf8'
		name = 'newen'
		parameters = '+map ${MAP_BASE_NAME}'
		path = $exe
	})
	version = 1
}
$json = $engines | ConvertTo-Json -Depth 4
[IO.File]::WriteAllText((Join-Path $dst 'GameEngineProfiles.cfg'), $json)   # no BOM
Write-Output "Installed the newen game configuration in $dst"
