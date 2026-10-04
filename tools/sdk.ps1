# The Cinderbox SDK: the Godot project a mod's look is made in (sdk\), and the projects made from it.
#
#   powershell -ExecutionPolicy Bypass -File tools\sdk.ps1 -Setup             put the extension into sdk\, to open it in Godot
#   powershell -ExecutionPolicy Bypass -File tools\sdk.ps1 -New <mod>         make server_mods\<mod>\client from it
#   powershell -ExecutionPolicy Bypass -File tools\sdk.ps1 -Update <mod>      refresh an existing project's SDK parts
#   powershell -ExecutionPolicy Bypass -File tools\sdk.ps1 -Starters          rebuild sdk\starters' animation packs
#
# An SDK project's own parts (copied in, git-ignored in a mod's project, never packed into the mod):
#   cinderbox.gdextension, bin\    the Cinderbox viewer extension (build it first: cmake --build --preset godot-export)
#   placeholder\                   the placeholder model: the CC0 mannequin with its locomotion clips
#
# -New gives the project its own files, made from sdk\starters with the mod's name in them:
#   prefabs\<mod>.tscn                             the item's scene (body, grips, a muzzle)
#   vfx\reactions_<mod>.tscn                       its look (CbItemLook), a prediction, a reaction
#   ui\hud_<mod>.tscn                              its HUD
#   animation_packs\<mod>_animations.tscn          an animation pack: the default tree in full, replacing the upper body
# Files that are already there are kept: -New on an existing project only adds what is missing.
#
# Then: open the project in Godot, author, and publish with tools\publish_mod.ps1 -Mod <mod>.

param(
	[switch]$Setup,
	[string]$New = "",
	[string]$Update = "",
	[switch]$Starters,
	[string]$Godot = ""
)

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
$sdk = Join-Path $root "sdk"
$game = Join-Path $root "godot"

if (-not $Godot) {
	$Godot = Join-Path $env:LOCALAPPDATA "cinderbox-build\tools\godot\Godot_v4.7.2-stable_win64_console.exe"
}

# The extension, into a project. Only the viewer: the peer extension (simulation, networking) is
# the game's alone.
function Extension([string]$project) {
	$bin = Join-Path $game "bin"
	$extension = Join-Path $game "cinderbox.gdextension"
	if (-not (Test-Path $extension) -or -not (Get-ChildItem $bin -Filter "libcinderbox.*" -ErrorAction SilentlyContinue)) {
		throw "the extension is not built (godot\bin is empty): cmake --build --preset godot-export"
	}
	Copy-Item $extension (Join-Path $project "cinderbox.gdextension") -Force
	$projectBin = Join-Path $project "bin"
	New-Item -ItemType Directory -Force $projectBin | Out-Null
	foreach ($pattern in @("libcinderbox.*.dll", "libcinderbox.*.so", "libcinderbox.*.dylib")) {
		# A library the open editor has loaded cannot be replaced: close the project to update it.
		try { Copy-Item (Join-Path $bin $pattern) $projectBin -Force -ErrorAction Stop } catch { Write-Warning "bin was not updated (the project is open in Godot?)" }
	}
	Remove-Item (Join-Path $projectBin "libcinderbox_peer.*") -Force -ErrorAction SilentlyContinue
}

# The placeholder model, into a mod's project (sdk\ has it already: it is the template's).
function Placeholder([string]$project) {
	$to = Join-Path $project "placeholder"
	New-Item -ItemType Directory -Force $to | Out-Null
	Copy-Item (Join-Path $sdk "placeholder\*") $to -Force
}

function Import([string]$project) {
	if (-not (Test-Path $Godot)) { throw "Godot not found at $Godot; pass -Godot" }
	& $Godot --headless --path $project --import | Out-Null
}

# A starter, with the mod's name in it. Kept if the mod already has that file.
function Starter([string]$name, [string]$target, [string]$mod) {
	if (Test-Path $target) {
		Write-Host "  kept $target"
		return
	}
	$source = Join-Path $sdk "starters\$name"
	if (-not (Test-Path $source)) { throw "no starter $source (run tools\sdk.ps1 -Starters)" }
	New-Item -ItemType Directory -Force (Split-Path -Parent $target) | Out-Null
	$text = [System.IO.File]::ReadAllText($source).Replace("MODNAME", $mod)
	# A copy is a file of its own: Godot gives it a new id.
	$text = [regex]::Replace($text, '^(\[gd_scene[^\]]*?) uid="uid://[a-z0-9]+"', '$1')
	[System.IO.File]::WriteAllText($target, $text)
	Write-Host "  made $target"
}

if ($Setup -or $Starters) {
	Extension $sdk
	Import $sdk
	if ($Starters) {
		& $Godot --headless --path $sdk --script (Join-Path $game "addons\cinderbox_maps\make_sdk_starters.gd")
		if ($LASTEXITCODE -ne 0) { throw "the starter packs could not be built (see above)" }
	}
	Write-Host "the SDK is ready: open $sdk in Godot"
}

if ($New) {
	if ($New -notmatch '^[a-z][a-z0-9_]*$') { throw "a mod's name is lower case letters, digits and _ (it is its folder and the start of its names)" }
	$project = Join-Path $root "server_mods\$New\client"
	New-Item -ItemType Directory -Force $project | Out-Null
	$text = [System.IO.File]::ReadAllText((Join-Path $sdk "project.godot")).Replace('"Cinderbox SDK"', "`"Cinderbox item: $New`"")
	[System.IO.File]::WriteAllText((Join-Path $project "project.godot"), $text)
	Copy-Item (Join-Path $sdk "export_presets.cfg") $project -Force
	Starter "item.tscn" (Join-Path $project "prefabs\$New.tscn") $New
	Starter "reactions.tscn" (Join-Path $project "vfx\reactions_$New.tscn") $New
	Starter "hud.tscn" (Join-Path $project "ui\hud_$New.tscn") $New
	# The starter pack is for a mod that has none of its own yet.
	if (-not (Get-ChildItem (Join-Path $project "animation_packs") -Filter "*.tscn" -ErrorAction SilentlyContinue)) {
		Starter "animation_pack.tscn" (Join-Path $project "animation_packs\${New}_animations.tscn") $New
	}
	New-Item -ItemType Directory -Force (Join-Path $project "assets") | Out-Null
	Extension $project
	Placeholder $project
	Import $project
	Write-Host "made ${project}: open it in Godot. The server's half is server_mods\$New\$New.cpp (see README: Writing a server mod)."
}

if ($Update) {
	$project = if (Test-Path (Join-Path $Update "project.godot")) { (Resolve-Path $Update).Path } else { Join-Path $root "server_mods\$Update\client" }
	if (-not (Test-Path (Join-Path $project "project.godot"))) { throw "no project at $project" }
	Extension $project
	Placeholder $project
	Import $project
	Write-Host "updated $project"
}

if (-not ($Setup -or $Starters -or $New -or $Update)) {
	Write-Host "pass -Setup, -New <mod>, -Update <mod> or -Starters (see the top of this file)"
}
