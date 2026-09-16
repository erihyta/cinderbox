# Builds the simulation with Clang, MinGW GCC and MSVC, then checks that all three produce the same
# per-tick state hashes and can load each other's portable snapshots.
#
#   powershell -ExecutionPolicy Bypass -File scripts\check_determinism.ps1 [-Reference hashes.txt]
#
# -Reference compares against a dump made on another platform (cb_tests --dump <file>).

param(
	[string]$Reference = ""
)

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
$buildRoot = Join-Path $env:LOCALAPPDATA "cinderbox-build"
$work = Join-Path $buildRoot "determinism-check"
New-Item -ItemType Directory -Force $work | Out-Null

$presets = @("clang-release", "gcc-release")

Push-Location $root
try {
	foreach ($p in $presets) {
		Write-Host "== building $p"
		cmake --preset $p | Out-Null
		if ($LASTEXITCODE -ne 0) { throw "configure $p failed" }
		cmake --build --preset $p | Out-Null
		if ($LASTEXITCODE -ne 0) { throw "build $p failed" }
	}

	# MSVC needs a developer environment.
	$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
	$vsPath = $null
	if (Test-Path $vswhere) {
		$vsPath = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
	}
	if ($vsPath) {
		Write-Host "== building msvc-release"
		$vcvars = Join-Path $vsPath "VC\Auxiliary\Build\vcvars64.bat"
		$installer = Split-Path -Parent $vswhere
		cmd /c "set `"PATH=%PATH%;$installer`" && `"$vcvars`" >nul && cmake --preset msvc-release >nul && cmake --build --preset msvc-release >nul"
		if ($LASTEXITCODE -ne 0) { throw "msvc build failed" }
		$presets += "msvc-release"
	}
	else {
		Write-Host "== MSVC not found, skipping"
	}
}
finally {
	Pop-Location
}

$failed = $false
$first = $presets[0]
$firstExe = Join-Path $buildRoot "$first\bin\cb_tests.exe"
$refFile = Join-Path $work "hashes-$first.txt"
& $firstExe --dump $refFile

foreach ($p in $presets) {
	$exe = Join-Path $buildRoot "$p\bin\cb_tests.exe"
	Write-Host "== $p vs $first"
	& $exe --compare $refFile
	if ($LASTEXITCODE -ne 0) { $failed = $true }
	if ($Reference) {
		Write-Host "== $p vs $Reference"
		& $exe --compare $Reference
		if ($LASTEXITCODE -ne 0) { $failed = $true }
	}
	& $exe --save-portable (Join-Path $work "portable-$p.bin") | Out-Null
}

foreach ($from in $presets) {
	foreach ($to in $presets) {
		$exe = Join-Path $buildRoot "$to\bin\cb_tests.exe"
		Write-Host -NoNewline "== portable $from -> ${to}: "
		& $exe --load-portable (Join-Path $work "portable-$from.bin")
		if ($LASTEXITCODE -ne 0) { $failed = $true }
	}
}

if ($failed) {
	Write-Host "DETERMINISM CHECK FAILED" -ForegroundColor Red
	exit 1
}
Write-Host "determinism check passed for: $($presets -join ', ')" -ForegroundColor Green
