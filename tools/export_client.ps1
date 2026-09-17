# Exports the Godot client as a standalone Windows game.
#
#   cmake --preset godot-export; cmake --build --preset godot-export
#   powershell -ExecutionPolicy Bypass -File tools\export_client.ps1 [-Output dist\Cinderbox] [-Debug]
#
# Needs the Godot 4.7.2 export templates, either installed from the editor or extracted to
# %LOCALAPPDATA%\cinderbox-build\tools\godot\templates (they are copied to Godot's template folder).
# Mod packs in mods\ are copied next to the executable.

param(
	[string]$Output = "",
	[string]$Godot = "",
	[switch]$Debug
)

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
if (-not $Output) { $Output = Join-Path $root "dist\Cinderbox" }
$Output = [System.IO.Path]::GetFullPath($Output)
$tools = Join-Path $env:LOCALAPPDATA "cinderbox-build\tools\godot"
if (-not $Godot) { $Godot = Join-Path $tools "Godot_v4.7.2-stable_win64_console.exe" }
if (-not (Test-Path $Godot)) { throw "Godot not found at $Godot; pass -Godot" }

$target = if ($Debug) { "template_debug" } else { "template_release" }
$dll = Join-Path $root "godot\bin\libcinderbox.windows.$target.x86_64.dll"
if (-not (Test-Path $dll)) {
	$preset = if ($Debug) { "clang-release" } else { "godot-export" }
	throw "$dll is missing; build it with: cmake --preset $preset; cmake --build --preset $preset"
}

$installed = Join-Path $env:APPDATA "Godot\export_templates\4.7.2.stable"
if (-not (Test-Path (Join-Path $installed "windows_release_x86_64.exe"))) {
	$extracted = Join-Path $tools "templates"
	if (-not (Test-Path (Join-Path $extracted "windows_release_x86_64.exe"))) {
		throw "export templates not found; install them from the editor or extract them to $extracted"
	}
	New-Item -ItemType Directory -Force $installed | Out-Null
	Copy-Item (Join-Path $extracted "*") $installed -Force
	Write-Host "installed export templates to $installed"
}

New-Item -ItemType Directory -Force $Output | Out-Null
$project = Join-Path $root "godot"
$exe = Join-Path $Output "Cinderbox.exe"
$mode = if ($Debug) { "--export-debug" } else { "--export-release" }
& $Godot --headless --path $project --import | Out-Null
& $Godot --headless --path $project $mode "Windows" $exe
if ($LASTEXITCODE -ne 0 -or -not (Test-Path $exe)) { throw "export failed" }

$mods = Join-Path $root "mods"
if (Test-Path $mods) {
	New-Item -ItemType Directory -Force (Join-Path $Output "mods") | Out-Null
	Copy-Item (Join-Path $mods "*.zip") (Join-Path $Output "mods") -Force
}
Write-Host "exported to ${Output}:"
Get-ChildItem -Recurse $Output | ForEach-Object { Write-Host "  $($_.FullName.Substring($Output.Length + 1))  $($_.Length)" }
