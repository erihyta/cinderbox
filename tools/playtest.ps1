# Playtesting by hand on one machine: a server, a network simulator in front of it, and two games
# side by side that join through it, as two players far from the server would. The checklist is
# docs/playtest.md.
#
#   powershell -ExecutionPolicy Bypass -File tools\playtest.ps1                        the default mods on test_ground
#   powershell -ExecutionPolicy Bypass -File tools\playtest.ps1 -Add possess,waves     and mods that are off by default
#   powershell -ExecutionPolicy Bypass -File tools\playtest.ps1 -Character critter     everyone plays the critter
#   powershell -ExecutionPolicy Bypass -File tools\playtest.ps1 -Latency 0 -Loss 0     no network trouble
#   powershell -ExecutionPolicy Bypass -File tools\playtest.ps1 -NetModel state        the state network model (the
#                                                                                    server's states, only your own
#                                                                                    body predicted; ROADMAP step 3)
#   powershell -ExecutionPolicy Bypass -File tools\playtest.ps1 -Autoplay 90           nobody at the keys: the games
#                                                                                    play by themselves, take pictures
#                                                                                    and quit (non-zero: a desync)
#   powershell -ExecutionPolicy Bypass -File tools\playtest.ps1 -Autoplay 90 -Headless the same with no windows
#
# Everything stops when the games are closed. The session is recorded (a replay and a view file in
# -Out), and the replay is verified at the end: it re-simulates the session and checks every
# checksum, so a desync shows even when nothing looked wrong. The logs are there too.

param(
	[string]$Add = "",            # mods that are off by default, to run too: possess, waves, sneak, flight, cannon, grenade
	[string]$Mods = "",           # exactly these mods instead (cb_server --mods)
	[string]$Map = "test_ground", # a map in godot/maps, or "" for the built-in sandbox
	[string]$Character = "",      # the character everyone plays (cb_server --character)
	[string]$NetModel = "inputs", # inputs or state (cb_server --net-model)
	[string[]]$Option = @(),      # mod options, NAME=VALUE (waves.first=10)
	[int]$Clients = 2,
	[int]$Bots = 0,               # cb_bot players too
	[int]$Latency = 75,           # ms added each way: 150 ms round trip
	[int]$Jitter = 5,
	[double]$Loss = 1,            # % lost each way: about 2% of round trips
	[string]$Preset = "clang-release",
	[double]$Autoplay = 0,        # seconds the games play by themselves (0: by hand)
	[switch]$Headless,            # with -Autoplay: no windows, nothing drawn (crashes, desyncs and errors still show)
	[string[]]$Extra = @(),       # more for each game, after "--" (--spam-use=1)
	[string]$Out = "",            # where the recordings and logs go
	[string]$Godot = ""
)

$ErrorActionPreference = "Stop"
if ($Headless -and $Autoplay -le 0) { throw "-Headless needs -Autoplay: nobody can play a game with no window" }
$root = Split-Path -Parent $PSScriptRoot
$build = Join-Path (Join-Path $env:LOCALAPPDATA "cinderbox-build") (Split-Path -Leaf $root)
$bin = Join-Path $build "$Preset\bin"
$server = Join-Path $bin "cb_server.exe"
$netsim = Join-Path $bin "cb_netsim.exe"
$replay = Join-Path $bin "cb_replay.exe"
$bot = Join-Path $bin "cb_bot.exe"
foreach ($exe in @($server, $netsim, $replay)) {
	if (-not (Test-Path $exe)) { throw "no $exe (cmake --build --preset $Preset)" }
}
if (-not $Godot) {
	# The one without a console: each game's output goes to its log.
	$Godot = Join-Path $env:LOCALAPPDATA "cinderbox-build\tools\godot\Godot_v4.7.2-stable_win64.exe"
}
if (-not (Test-Path $Godot)) { throw "Godot not found at $Godot; pass -Godot" }
if (-not $Out) { $Out = Join-Path $build ("playtest\" + (Get-Date -Format "yyyy-MM-dd_HH-mm-ss")) }
New-Item -ItemType Directory -Force $Out | Out-Null

# Which mods: the server's usual ones and -Add, or exactly -Mods.
if (-not $Mods -and $Add) {
	$default = @(& $server --list-mods | Where-Object { $_ -notmatch "disabled" } | ForEach-Object { $_.Trim() })
	$Mods = (@($default) + ($Add -split ",") | Where-Object { $_ } | Select-Object -Unique) -join ","
}

# Two ports nobody is likely to have: the server's, and the simulator's in front of it.
$port = Get-Random -Minimum 27900 -Maximum 28900
$relay = $port + 1

$serverArgs = @("--port", $port, "--exit-when-empty", 30, "--record", (Join-Path $Out "session.cbr"),
	"--record-view", (Join-Path $Out "session.cbview"))
if ($Map) { $serverArgs += @("--map", $Map) }
if ($Mods) { $serverArgs += @("--mods", $Mods) }
if ($Character) { $serverArgs += @("--character", $Character) }
if ($NetModel) { $serverArgs += @("--net-model", $NetModel) }
foreach ($o in $Option) { $serverArgs += @("--mod-option", $o) }

Write-Host "== server on $port$(if ($Mods) { " (mods: $Mods)" }), simulator on $relay (+$Latency ms each way, $Loss% lost each way)"
Write-Host "   recordings and logs: $Out"
$serverProc = Start-Process -FilePath $server -ArgumentList $serverArgs -PassThru -WindowStyle Hidden `
	-RedirectStandardOutput (Join-Path $Out "server.log") -RedirectStandardError (Join-Path $Out "server.err.log")
$netsimProc = Start-Process -FilePath $netsim -PassThru -WindowStyle Hidden `
	-ArgumentList @("--listen", $relay, "--target", "127.0.0.1:$port", "--latency", $Latency, "--jitter", $Jitter, "--loss", $Loss) `
	-RedirectStandardOutput (Join-Path $Out "netsim.log") -RedirectStandardError (Join-Path $Out "netsim.err.log")
Start-Sleep -Milliseconds 500
if ($serverProc.HasExited) { throw "the server stopped at once: see $Out\server.log and server.err.log" }

$botProc = $null
if ($Bots -gt 0) {
	if (-not (Test-Path $bot)) { throw "no $bot" }
	$botProc = Start-Process -FilePath $bot -PassThru -WindowStyle Hidden `
		-ArgumentList @("--port", $relay, "--count", $Bots, "--full", 0, "--duration", 3600) `
		-RedirectStandardOutput (Join-Path $Out "bots.log") -RedirectStandardError (Join-Path $Out "bots.err.log")
}

# The games side by side, two to a row, each with a config of its own (its name, its keys).
Add-Type -AssemblyName System.Windows.Forms
$area = [System.Windows.Forms.Screen]::PrimaryScreen.WorkingArea
$columns = [Math]::Min($Clients, 2)
$width = [int]($area.Width / $columns) - 16
$height = [int]($width * 9 / 16)
$games = @()
for ($i = 0; $i -lt $Clients; $i++) {
	$name = [string][char](65 + $i)
	$x = $area.X + ($i % $columns) * ($width + 16) + 8
	$y = $area.Y + [Math]::Floor($i / $columns) * ($height + 40) + 32
	$gameArgs = @("--path", (Join-Path $root "godot"), "--log-file", (Join-Path $Out "game_$name.log"))
	$gameArgs += if ($Headless) { @("--headless") } else { @("--position", "$x,$y", "--resolution", "${width}x${height}") }
	$gameArgs += @("--", "--host=127.0.0.1", "--port=$relay", "--name=$name", "--config=user://playtest_$name.cfg")
	$gameArgs += $Extra
	if ($Autoplay -gt 0) {
		$gameArgs += @("--autoplay=$Autoplay")
		if (-not $Headless) {
			$gameArgs += @("--screenshot=$(Join-Path $Out "shot_$name.png")", "--screenshot-every=$([Math]::Max(5, [int]($Autoplay / 6)))")
		}
	}
	$game = Start-Process -FilePath $Godot -ArgumentList $gameArgs -PassThru
	$null = $game.Handle # (so its exit code is kept)
	$games += $game
	Start-Sleep -Milliseconds 300
}
Write-Host "== $Clients games started ($(@($games | ForEach-Object { "pid $($_.Id)" }) -join ', ')). Close them to finish."

try {
	$games | Wait-Process
} finally {
	if ($botProc -and -not $botProc.HasExited) { Stop-Process -Id $botProc.Id -Force }
	# The server stops by itself once nobody is connected (and writes its recording out); give it
	# that time, then stop what is left.
	if (-not $serverProc.WaitForExit(40000)) { Stop-Process -Id $serverProc.Id -Force }
	# (Always: a relay left behind by a run cut short holds the build's cb_netsim.exe open.)
	Stop-Process -Id $netsimProc.Id -Force -ErrorAction SilentlyContinue
}

$failed = @($games | Where-Object { $_.ExitCode -ne 0 } | ForEach-Object { "pid $($_.Id): exit $($_.ExitCode)" })
if ($failed) { Write-Host "== games that did not exit cleanly: $($failed -join ', ')" }

Write-Host "== the replay, re-simulated"
& $replay verify (Join-Path $Out "session.cbr")
$verified = $LASTEXITCODE
$problems = @()
foreach ($f in Get-ChildItem $Out -Filter "*.log") {
	$hits = Select-String -Path $f.FullName -Pattern "ERROR|desyncs [1-9]|desync detected|crash|Assertion|unknown name" -CaseSensitive
	if ($hits) { $problems += "$($f.Name): $($hits.Count) line(s), first: $($hits[0].Line.Trim())" }
}
if ($problems) {
	Write-Host "== in the logs:"
	$problems | ForEach-Object { Write-Host "   $_" }
}
Write-Host "== watch it again:"
Write-Host "   $Godot --path $(Join-Path $root 'godot') -- --replay=$(Join-Path $Out 'session.cbr')"
Write-Host "   $Godot --path $(Join-Path $root 'godot') -- --view=$(Join-Path $Out 'session.cbview')"
if ($failed -and $verified -eq 0) { $verified = 1 }
exit $verified
