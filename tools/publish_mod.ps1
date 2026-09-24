# Publishes a server mod's look, or a character, as a workshop item.
#
#   powershell -ExecutionPolicy Bypass -File tools\publish_mod.ps1 -Mod pistol [-Workshop DIR]
#   powershell -ExecutionPolicy Bypass -File tools\publish_mod.ps1 -Character robot [-Workshop DIR]
#
# 1. Packs server_mods\<mod>\client, or characters\<name>\client (a Godot project, see its "Mod"
#    export preset), with pack_mod.ps1. A character is baked in the editor first (Bake on its
#    CbCharacter): the pack ships the baked files, nothing is converted on the way.
# 2. Names the item by the SHA-256 of that pack: the identity servers announce and clients check.
# 3. Installs it into the local workshop (what subscribing will do once there is a real workshop):
#    <workshop>\<mod>\<sha256>.zip, by default the game's user folder, user://workshop.
# 4. Records the hash in server_mods\<mod>\client_item.cfg (commit it with the mod), and copies it
#    to bin\items\<mod>.item in this project's existing builds, so a server knows it at once.
#
# The game server never sends items: players need the exact item the server names.

param(
	[string]$Mod = "",
	[string]$Character = "",
	[string]$Workshop = "",
	[string]$Godot = ""
)

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
if ($Character) {
	$Mod = $Character
	$home_ = Join-Path $root "characters\$Character"
} elseif ($Mod) {
	$home_ = Join-Path $root "server_mods\$Mod"
} else {
	throw "pass -Mod <server mod> or -Character <character>"
}
$project = Join-Path $home_ "client"
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

$manifest = Join-Path $home_ "client_item.cfg"
$what = if ($Character) { "the $Mod character" } else { "the $Mod mod's look" }
$text = "# The workshop item players need for $what (written by tools\publish_mod.ps1).`nsha256=$sha`n"
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
