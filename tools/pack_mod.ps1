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

# A mod that uses Cinderbox resources (effect bindings, map nodes) needs the extension present
# while Godot imports and packs it, or those files cannot be loaded. The extension itself is a
# developer file and is never packed: the preset lists the files to ship.
$extension = Join-Path $root "godot\cinderbox.gdextension"
$bin = Join-Path $root "godot\bin"
$copiedExtension = Join-Path $Project "cinderbox.gdextension"
$copiedBin = Join-Path $Project "bin"
if ((Test-Path $extension) -and (Test-Path $bin)) {
	Copy-Item $extension $copiedExtension -Force
	New-Item -ItemType Directory -Force $copiedBin | Out-Null
	Copy-Item (Join-Path $bin "*.dll") $copiedBin -Force -ErrorAction SilentlyContinue
	Copy-Item (Join-Path $bin "*.so") $copiedBin -Force -ErrorAction SilentlyContinue
	Copy-Item (Join-Path $bin "*.dylib") $copiedBin -Force -ErrorAction SilentlyContinue
} else {
	Write-Warning "godot\bin is empty; a mod using Cinderbox resources will not pack correctly"
}

& $Godot --headless --path $Project --import | Out-Null
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
