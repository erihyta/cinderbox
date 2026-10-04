# The Cinderbox SDK: the Godot project a mod's look is made in (sdk\), and the projects made from it.
#
#   powershell -ExecutionPolicy Bypass -File tools\sdk.ps1 -Setup             fill sdk\ itself, to open in Godot
#   powershell -ExecutionPolicy Bypass -File tools\sdk.ps1 -New <mod>         make server_mods\<mod>\client from it
#   powershell -ExecutionPolicy Bypass -File tools\sdk.ps1 -Update <mod>      refresh an existing project's SDK parts
#   powershell -ExecutionPolicy Bypass -File tools\sdk.ps1 -Starters          rebuild sdk\starters' animation packs
#
# What every SDK project gets (copied, git-ignored, never packed into the mod):
#   cinderbox.gdextension, bin\        the Cinderbox viewer extension (build it first: cmake --build --preset godot-export)
#   addons\cinderbox_maps\             the editor addon (the Bake Map button)
#   characters\mannequin\              the placeholder character: the CC0 mannequin with its locomotion,
#                                      at the path the game has it
#
# -New also gives the project its own files, made from sdk\starters with the mod's name in them:
#   prefabs\<mod>.tscn                 the item's scene (body, grips, a muzzle)
#   vfx\reactions_<mod>.tscn           its look (CbItemLook), a prediction, a reaction
#   ui\hud_<mod>.tscn                  its HUD
#   anim_src\<mod>_hold.tscn           an animation pack for the upper body ("<mod>.hold")
#   anim_src\<mod>_walk.tscn           an animation pack for the base layer ("<mod>.walk")
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

# The SDK's parts of a project: the extension, the addon, the placeholder character.
function Fill([string]$project) {
	$bin = Join-Path $game "bin"
	$extension = Join-Path $game "cinderbox.gdextension"
	if (-not (Test-Path $extension) -or -not (Get-ChildItem $bin -Filter "libcinderbox.*" -ErrorAction SilentlyContinue)) {
		throw "the extension is not built (godot\bin is empty): cmake --build --preset godot-export"
	}
	Copy-Item $extension (Join-Path $project "cinderbox.gdextension") -Force
	$projectBin = Join-Path $project "bin"
	New-Item -ItemType Directory -Force $projectBin | Out-Null
	# Only the viewer: the peer extension (simulation, networking) is the game's alone.
	foreach ($pattern in @("libcinderbox.*.dll", "libcinderbox.*.so", "libcinderbox.*.dylib")) {
		Copy-Item (Join-Path $bin $pattern) $projectBin -Force -ErrorAction SilentlyContinue
	}
	Remove-Item (Join-Path $projectBin "libcinderbox_peer.*") -Force -ErrorAction SilentlyContinue

	$addon = Join-Path $project "addons\cinderbox_maps"
	New-Item -ItemType Directory -Force $addon | Out-Null
	foreach ($file in @("plugin.cfg", "plugin.gd")) {
		Copy-Item (Join-Path $game "addons\cinderbox_maps\$file") $addon -Force
	}

	# The placeholder: the scene, its source model and what it bakes to. A file in use (the project
	# open in the editor) is left as it is.
	$from = Join-Path $game "characters\mannequin"
	$to = Join-Path $project "characters\mannequin"
	New-Item -ItemType Directory -Force $to | Out-Null
	Get-ChildItem $from -Recurse -File | Where-Object { $_.Name -notlike "*.uid" } | ForEach-Object {
		$target = Join-Path $to $_.FullName.Substring($from.Length + 1)
		New-Item -ItemType Directory -Force (Split-Path -Parent $target) | Out-Null
		try { Copy-Item $_.FullName $target -Force } catch { Write-Warning "kept $target (in use)" }
	}
	Write-Host "  SDK parts in $project"
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
	if (-not (Test-Path $source)) { throw "no starter $source (run tools\sdk.ps1 -Setup -Starters)" }
	New-Item -ItemType Directory -Force (Split-Path -Parent $target) | Out-Null
	$text = [System.IO.File]::ReadAllText($source).Replace("MODNAME", $mod)
	# A copy is a file of its own: Godot gives it a new id.
	$text = [regex]::Replace($text, '^(\[gd_scene[^\]]*?) uid="uid://[a-z0-9]+"', '$1')
	[System.IO.File]::WriteAllText($target, $text)
	Write-Host "  made $target"
}

if ($Setup -or $Starters) {
	Fill $sdk
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
	foreach ($file in @("project.godot", "export_presets.cfg")) {
		$target = Join-Path $project $file
		$text = [System.IO.File]::ReadAllText((Join-Path $sdk $file))
		if ($file -eq "project.godot") {
			$text = $text.Replace('config/name="Cinderbox SDK"', "config/name=`"Cinderbox item: $New`"")
			$text = $text.Replace("; Cinderbox SDK: the Godot project a mod's look is made in.", "; The $New mod's look: a Cinderbox SDK project (tools\sdk.ps1 -New $New).")
		}
		[System.IO.File]::WriteAllText($target, $text)
	}
	Starter "item.tscn" (Join-Path $project "prefabs\$New.tscn") $New
	Starter "reactions.tscn" (Join-Path $project "vfx\reactions_$New.tscn") $New
	Starter "hud.tscn" (Join-Path $project "ui\hud_$New.tscn") $New
	# One pack scene is enough to say a mod has its own: the starters are for a mod that has none.
	if (-not (Get-ChildItem (Join-Path $project "anim_src") -Filter "*.tscn" -ErrorAction SilentlyContinue)) {
		Starter "anim_upper.tscn" (Join-Path $project "anim_src\${New}_hold.tscn") $New
		Starter "anim_base.tscn" (Join-Path $project "anim_src\${New}_walk.tscn") $New
	}
	New-Item -ItemType Directory -Force (Join-Path $project "assets") | Out-Null
	Fill $project
	Import $project
	Write-Host "made ${project}: open it in Godot. The server's half is server_mods\$New\$New.cpp (see README: Writing a server mod)."
}

if ($Update) {
	$project = if (Test-Path (Join-Path $Update "project.godot")) { (Resolve-Path $Update).Path } else { Join-Path $root "server_mods\$Update\client" }
	if (-not (Test-Path (Join-Path $project "project.godot"))) { throw "no project at $project" }
	Fill $project
	Import $project
}

if (-not ($Setup -or $Starters -or $New -or $Update)) {
	Write-Host "pass -Setup, -New <mod>, -Update <mod> or -Starters (see the top of this file)"
}
