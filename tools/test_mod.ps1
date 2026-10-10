# One step from an edit to playing it: builds the server, publishes a mod's look, and starts the
# game hosting a server of its own (godot\host.gd) with it.
#
#   powershell -ExecutionPolicy Bypass -File tools\test_mod.ps1 -Mod grenade
#   powershell -ExecutionPolicy Bypass -File tools\test_mod.ps1 -Mod grenade -Bots 4
#   powershell -ExecutionPolicy Bypass -File tools\test_mod.ps1 -Mod grenade -Only       only this mod and what it cannot do without
#
# The editor's "Test mod" button (in a mod's client project) runs this for the project that is open.
#
#   1. cmake --build --preset <Preset>         the mod's C++ (nothing to do when nothing changed); it
#                                              also writes the names the editor completes and checks
#   2. tools\publish_mod.ps1 -Mod <mod>        bakes and packs the look; a name nobody declares stops it
#   3. the game, with --host-local             it starts that build's cb_server, joins it, and stops
#                                              it when the game is closed
#
# -NoBuild skips step 1 (only the look changed). -Autoplay SEC plays by itself for that long and
# exits (0: everything agreed), for a check without hands.

param(
	[Parameter(Mandatory = $true)][string]$Mod,
	[string]$Preset = "clang-release",
	[int]$Bots = 0,
	[switch]$Only,
	[switch]$NoBuild,
	[double]$Autoplay = 0,
	[string]$Godot = ""
)

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
if (-not (Test-Path (Join-Path $root "server_mods\$Mod\$Mod.cpp"))) { throw "no server mod at server_mods\$Mod" }
if (-not $Godot) {
	$Godot = Join-Path $env:LOCALAPPDATA "cinderbox-build\tools\godot\Godot_v4.7.2-stable_win64_console.exe"
}
if (-not (Test-Path $Godot)) { throw "Godot not found at $Godot; pass -Godot" }
$bin = Join-Path (Join-Path (Join-Path $env:LOCALAPPDATA "cinderbox-build") (Split-Path -Leaf $root)) "$Preset\bin"

if (-not $NoBuild) {
	Write-Host "== building ($Preset)"
	Push-Location $root
	try {
		& cmake --build --preset $Preset
		if ($LASTEXITCODE -ne 0) {
			throw "the build failed (see above). If it could not write cb_server.exe, a server from this build is still running: stop it."
		}
	} finally {
		Pop-Location
	}
}
$server = Join-Path $bin "cb_server.exe"
if (-not (Test-Path $server)) { throw "no $server (build first, or pass -Preset)" }

if (Test-Path (Join-Path $root "server_mods\$Mod\client\project.godot")) {
	Write-Host "== publishing $Mod"
	& (Join-Path $PSScriptRoot "publish_mod.ps1") -Mod $Mod -Godot $Godot
}

# Which mods the server runs: its usual ones (and this one, if it is off by default), or with
# -Only just this one beside the ones nearly everything is written against.
$mods = ""
$listed = & $server --list-mods
$default = @($listed | Where-Object { $_ -notmatch "disabled" } | ForEach-Object { $_.Trim() })
if ($Only) {
	$mods = (@("combat", "inventory", $Mod) | Select-Object -Unique) -join ","
} elseif ($default -notcontains $Mod) {
	$mods = (@($default) + $Mod) -join ","
}

Write-Host "== starting the game$(if ($mods) { " (mods: $mods)" })"
$gameArgs = @("--path", (Join-Path $root "godot"), "--", "--host-local$(if ($mods) { "=$mods" })", "--server-exe=$server")
if ($Bots -gt 0) { $gameArgs += "--bots=$Bots" }
if ($Autoplay -gt 0) { $gameArgs += "--autoplay=$Autoplay" }
& $Godot @gameArgs
exit $LASTEXITCODE
