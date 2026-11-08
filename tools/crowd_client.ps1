# The game in front of a crowd (M259): how long its frames take, and where that time goes.
#
#   powershell -ExecutionPolicy Bypass -File tools\crowd_client.ps1 -Players 255             a fight of 255 recorded
#                                                                                         (cb_server --fake-players
#                                                                                         --bench --record-view), then
#                                                                                         played in the game
#   powershell -ExecutionPolicy Bypass -File tools\crowd_client.ps1 -Players 255 -Live       the game joins a server of
#                                                                                         255 fake players instead (the
#                                                                                         server must keep up)
#   powershell -ExecutionPolicy Bypass -File tools\crowd_client.ps1 -Players 1023 -Radius 25 -Headless
#
# The game plays by itself (--autoplay) and says, measured from a few seconds in: its frames (on
# average, at the 95th, at most), its own thread's parts (taking the frame, the mirror and the poses
# within it, the nodes, the events...), the HUD's nodes, the events a frame, and, live, its source's
# thread (the frames it fills, the physics, the others placed, the prediction). A recording is kept
# in -Out and played again when it is there (-Again: made anew).

param(
	[int]$Players = 255,
	[double]$Radius = 15,          # how far from the spawn the fake players walk back (a crowd)
	[double]$Seconds = 20,         # how long the game plays
	[double]$MeasureFrom = 5,      # from how far in its frames are measured
	[switch]$Live,
	[switch]$Headless,             # nothing drawn: what is left is the game's own work
	[switch]$Again,
	[string]$Resolution = "1280x720",
	[string]$Preset = "clang-release",
	[string]$Out = "",
	[string]$Godot = ""
)

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
$build = Join-Path (Join-Path $env:LOCALAPPDATA "cinderbox-build") (Split-Path -Leaf $root)
$server = Join-Path $build "$Preset\bin\cb_server.exe"
if (-not (Test-Path $server)) { throw "no $server (cmake --build --preset $Preset)" }
if (-not $Godot) { $Godot = Join-Path $env:LOCALAPPDATA "cinderbox-build\tools\godot\Godot_v4.7.2-stable_win64_console.exe" }
if (-not (Test-Path $Godot)) { throw "Godot not found at $Godot; pass -Godot" }
if (-not $Out) { $Out = Join-Path $build "crowd_client" }
New-Item -ItemType Directory -Force $Out | Out-Null
$fight = @("--fake-players", $Players, "--fake-radius", $Radius, "--fake-fight")
$game = @("--path", (Join-Path $root "godot"))
$game += if ($Headless) { @("--headless") } else { @("--resolution", $Resolution) }
$game += @("--", "--autoplay=$Seconds", "--measure-from=$MeasureFrom", "--config=user://crowd_client.cfg")

if ($Live) {
	$port = Get-Random -Minimum 27900 -Maximum 28900
	Write-Host "== a server of $Players fake players on $port"
	$serverProc = Start-Process -FilePath $server -PassThru -WindowStyle Hidden `
		-ArgumentList (@("--port", $port, "--exit-when-empty", 10) + $fight) `
		-RedirectStandardOutput (Join-Path $Out "server.log") -RedirectStandardError (Join-Path $Out "server.err.log")
	try {
		Start-Sleep -Seconds 3
		& $Godot @game "--host=127.0.0.1" "--port=$port" "--name=measured" 2>&1 | Select-String -Pattern "^(frames|the client|the game|events a|its source|autoplay done|nodes)"
	} finally {
		if (-not $serverProc.HasExited) { Stop-Process -Id $serverProc.Id -Force }
	}
	exit 0
}

$view = Join-Path $Out "crowd_$Players.cbview"
if ($Again -or -not (Test-Path $view)) {
	Write-Host "== recording $Players fake players fighting (the bench's clock: as fast as the machine can)"
	& $server --port (Get-Random -Minimum 27900 -Maximum 28900) @fight --bench ($Seconds + 5) --record-view $view | Select-String -Pattern "game in"
}
Write-Host "== the game, playing $view"
& $Godot @game "--view=$view" 2>&1 | Select-String -Pattern "^(frames|the client|the game|events a|its source|autoplay done|nodes)"
