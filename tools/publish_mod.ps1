# Publishes a server mod's look as a workshop item.
#
#   powershell -ExecutionPolicy Bypass -File tools\publish_mod.ps1 -Mod pistol [-Workshop DIR]
#
# 1. Packs server_mods\<mod>\client (a Godot project, see its "Mod" export preset) with pack_mod.ps1.
# 2. Names the item by the SHA-256 of that pack: the identity servers announce and clients check.
# 3. Installs it into the local workshop (what subscribing will do once there is a real workshop):
#    <workshop>\<mod>\<sha256>.zip, by default the game's user folder, user://workshop.
# 4. Records the hash in server_mods\<mod>\client_item.cfg (commit it with the mod), and copies it
#    to bin\items\<mod>.item in this project's existing builds, so a server knows it at once.
#
# The game server never sends items: players need the exact item the server names.

param(
	[Parameter(Mandatory = $true)][string]$Mod,
	[string]$Workshop = "",
	[string]$Godot = ""
)

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
$project = Join-Path $root "server_mods\$Mod\client"
if (-not (Test-Path (Join-Path $project "project.godot"))) { throw "no client project at $project" }
if (-not $Workshop) {
	# user:// of the game project ("Cinderbox" in godot\project.godot).
	$Workshop = Join-Path $env:APPDATA "Godot\app_userdata\Cinderbox\workshop"
}

$temp = Join-Path ([System.IO.Path]::GetTempPath()) "cinderbox-item-$Mod.zip"
$packArgs = @{ Project = $project; Output = $temp }
if ($Godot) { $packArgs.Godot = $Godot }
& (Join-Path $PSScriptRoot "pack_mod.ps1") @packArgs | Out-Host

$sha = (Get-FileHash -Algorithm SHA256 $temp).Hash.ToLowerInvariant()
$itemDir = Join-Path $Workshop $Mod
New-Item -ItemType Directory -Force $itemDir | Out-Null
$item = Join-Path $itemDir "$sha.zip"
Move-Item -Force $temp $item

$manifest = Join-Path $root "server_mods\$Mod\client_item.cfg"
$text = "# The workshop item players need for the $Mod mod's look (written by tools\publish_mod.ps1).`nsha256=$sha`n"
[System.IO.File]::WriteAllText($manifest, $text)

$builds = Join-Path (Join-Path $env:LOCALAPPDATA "cinderbox-build") (Split-Path -Leaf $root)
if (Test-Path $builds) {
	Get-ChildItem $builds -Directory | ForEach-Object {
		$bin = Join-Path $_.FullName "bin"
		if (Test-Path (Join-Path $bin "cb_server.exe")) {
			New-Item -ItemType Directory -Force (Join-Path $bin "items") | Out-Null
			[System.IO.File]::WriteAllText((Join-Path $bin "items\$Mod.item"), $text)
			Write-Host "  server in $bin now announces it"
		}
	}
}

Write-Host "published ${Mod}: $sha"
Write-Host "  workshop: $item"
Write-Host "  manifest: $manifest"
