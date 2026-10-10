# Packs a mod project into a .zip resource pack the client loads from its mods folders.
#
#   powershell -ExecutionPolicy Bypass -File tools\pack_mod.ps1 -Project mods_src\example_neon [-Output mods\example_neon.zip]
#
# The mod project needs an export preset named "Mod" (see mods_src/example_neon/export_presets.cfg)
# listing the files to pack. Godot converts and imports everything the same way as for the game.
# The game refuses packs with scripts or files outside prefabs/, vfx/, ui/, maps/, assets/.

param(
	[Parameter(Mandatory = $true)][string]$Project,
	[string]$Output = "",
	[string]$Godot = ""
)

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
$Project = (Resolve-Path $Project).Path
$name = Split-Path -Leaf $Project
if (-not $Output) { $Output = Join-Path $root "mods\$name.zip" }
New-Item -ItemType Directory -Force (Split-Path -Parent $Output) | Out-Null
$Output = [System.IO.Path]::GetFullPath($Output)

if (-not $Godot) {
	$Godot = Join-Path $env:LOCALAPPDATA "cinderbox-build\tools\godot\Godot_v4.7.2-stable_win64_console.exe"
}
if (-not (Test-Path $Godot)) { throw "Godot not found at $Godot; pass -Godot" }

# A mod that uses Cinderbox resources (reactions, map nodes) needs the viewer extension present
# while Godot imports and packs it, or those files cannot be loaded. Only the viewer: the peer
# extension (simulation, networking) is the game's alone. The extension itself is a developer
# file and is never packed: the preset lists the files to ship.
$extension = Join-Path $root "godot\cinderbox.gdextension"
$bin = Join-Path $root "godot\bin"
$copiedExtension = Join-Path $Project "cinderbox.gdextension"
$copiedBin = Join-Path $Project "bin"
if ((Test-Path $extension) -and (Test-Path $bin)) {
	Copy-Item $extension $copiedExtension -Force
	New-Item -ItemType Directory -Force $copiedBin | Out-Null
	Remove-Item (Join-Path $copiedBin "libcinderbox_peer.*") -Force -ErrorAction SilentlyContinue
	Copy-Item (Join-Path $bin "libcinderbox.*.dll") $copiedBin -Force -ErrorAction SilentlyContinue
	Copy-Item (Join-Path $bin "libcinderbox.*.so") $copiedBin -Force -ErrorAction SilentlyContinue
	Copy-Item (Join-Path $bin "libcinderbox.*.dylib") $copiedBin -Force -ErrorAction SilentlyContinue
} else {
	Write-Warning "godot\bin is empty; a mod using Cinderbox resources will not pack correctly"
}

# A project with animation packs plays them on the SDK's placeholder skeleton, which is not in the
# repository with the mod (tools\sdk.ps1 copies it in): a fresh checkout gets it here.
$placeholder = Join-Path $root "sdk\placeholder"
if ((Test-Path (Join-Path $Project "animation_packs")) -and -not (Test-Path (Join-Path $Project "placeholder\skeleton.tscn")) -and (Test-Path $placeholder)) {
	New-Item -ItemType Directory -Force (Join-Path $Project "placeholder") | Out-Null
	Copy-Item (Join-Path $placeholder "*") (Join-Path $Project "placeholder") -Force -Recurse
}

& $Godot --headless --path $Project --import | Out-Null

# Items: every scene whose root is a CbItem becomes items/<kind>.cfg, which the server reads from
# the pack and the game finds the scene by (the preset's include_filter ships them).
$bakeItems = Join-Path $root "godot\addons\cinderbox_maps\bake_items.gd"
if (Test-Path $bakeItems) {
	& $Godot --headless --path $Project --script $bakeItems
	if ($LASTEXITCODE -ne 0) { throw "an item could not be baked (see above)" }
}
# Characters: baked again from the scene that ships (saving it in the editor already did; a bake
# that changes nothing writes nothing).
$bakeCharacters = Join-Path $root "godot\addons\cinderbox_maps\bake_characters.gd"
if ((Test-Path (Join-Path $Project "characters")) -and (Test-Path $bakeCharacters)) {
	& $Godot --headless --path $Project --script $bakeCharacters
	if ($LASTEXITCODE -ne 0) { throw "a character could not be baked (see above)" }
}
# Animation packs: baked again from their scenes (animation_packs/*.tscn), like the characters.
$bakePacks = Join-Path $root "godot\addons\cinderbox_maps\bake_packs.gd"
if ((Test-Path (Join-Path $Project "animation_packs")) -and (Test-Path $bakePacks)) {
	& $Godot --headless --path $Project --script $bakePacks
	if ($LASTEXITCODE -ne 0) { throw "an animation pack could not be baked (see above)" }
}
# Motions: baked again from their scenes (motion_sets/*.tscn) into motions/<set>.cfg.
$bakeMotions = Join-Path $root "godot\addons\cinderbox_maps\bake_motions.gd"
if ((Test-Path (Join-Path $Project "motion_sets")) -and (Test-Path $bakeMotions)) {
	& $Godot --headless --path $Project --script $bakeMotions
	if ($LASTEXITCODE -ne 0) { throw "a motion set could not be baked (see above)" }
}
# Names: what the project's scenes name (an event, a field in a condition, an action, an item kind)
# against what the server's mods declare (cinderbox_names.cfg, written by the server's build). A name
# nobody declares stops the pack: in the game it would read as 0 and say nothing.
$checkNames = Join-Path $root "godot\addons\cinderbox_maps\check_names.gd"
if (Test-Path $checkNames) {
	& $Godot --headless --path $Project --script $checkNames
	if ($LASTEXITCODE -ne 0) { throw "a scene names something no mod declares (see above)" }
}
& $Godot --headless --path $Project --export-pack "Mod" $Output
if ($LASTEXITCODE -ne 0 -or -not (Test-Path $Output)) { throw "export failed" }

# Godot adds the mod project's own settings and caches; loaded over the game they would replace
# the game's files, so they are removed.
$projectFiles = @("project.binary", ".godot/uid_cache.bin", ".godot/global_script_class_cache.cfg",
	".godot/extension_list.cfg", "cinderbox.gdextension")
Add-Type -AssemblyName System.IO.Compression
Add-Type -AssemblyName System.IO.Compression.FileSystem
$zip = [System.IO.Compression.ZipFile]::Open($Output, [System.IO.Compression.ZipArchiveMode]::Update)
foreach ($entry in @($zip.Entries)) {
	if ($projectFiles -contains $entry.FullName) { $entry.Delete() }
}
Write-Host "packed ${Output}:"
$zip.Entries | ForEach-Object { Write-Host "  $($_.FullName)" }
$zip.Dispose()

# The game's own check of a pack, now rather than when a player joins.
$checkPack = Join-Path $root "godot\addons\cinderbox_maps\check_pack.gd"
if (Test-Path $checkPack) {
	& $Godot --headless --path (Join-Path $root "godot") --script $checkPack -- $Output
	if ($LASTEXITCODE -ne 0) { throw "the game would refuse this pack (see above)" }
}
