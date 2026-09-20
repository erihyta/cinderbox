# Bakes a map scene into the .cbmap the server loads.
#
#   powershell -ExecutionPolicy Bypass -File tools\bake_map.ps1 -Scene res://maps/example_arena.tscn
#
# The same thing the editor's "Bake Map" button does, for scripted rebuilds. The Godot extension
# must be built first (cmake --build --preset clang-release), because the marker nodes live in it.
#
# The output is <project>/maps/<scene name>.cbmap; run the server with:
#   cb_server --map godot\maps\<scene name>.cbmap

param(
	[Parameter(Mandatory = $true)][string]$Scene,
	[string]$Out = "",
	[string]$Godot = ""
)

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
$project = Join-Path $root "godot"

if (-not $Godot) {
	$Godot = Join-Path $env:LOCALAPPDATA "cinderbox-build\tools\godot\Godot_v4.7.2-stable_win64_console.exe"
}
if (-not (Test-Path $Godot)) { throw "Godot not found at $Godot; pass -Godot" }
if (-not (Test-Path (Join-Path $project "bin"))) {
	throw "godot\bin is empty; build the extension with: cmake --build --preset clang-release"
}

$args = @("--scene=$Scene")
if ($Out) { $args += "--out=$Out" }

& $Godot --headless --path $project --script "res://addons/cinderbox_maps/bake_cli.gd" -- @args
if ($LASTEXITCODE -ne 0) { throw "bake failed" }
