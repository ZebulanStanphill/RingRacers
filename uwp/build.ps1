<#
.SYNOPSIS
Builds Ring Racers for UWP (Xbox Dev Mode).

.DESCRIPTION
Clones Ring Racers at the pinned release, applies patches/ringracers-uwp.patch,
builds it as a static library with the ninja-x64_windows_uwp_vcpkg-release
preset, then generates and builds the UWP launcher solution in build/.

Run from an x64 Visual Studio 2022 developer shell with VCPKG_ROOT set.
#>
param(
	[string]$RingRacersTag = "v2.4"
)

$ErrorActionPreference = "Stop"

$Root = $PSScriptRoot
$Source = Join-Path $Root "RingRacers"
$Patch = Join-Path $Root "patches/ringracers-uwp.patch"
$Preset = "ninja-x64_windows_uwp_vcpkg-release"
$LibBuild = Join-Path $Source "build/$Preset"
$UwpBuild = Join-Path $Root "build"

function Exec([scriptblock]$Command) {
	& $Command
	if ($LASTEXITCODE -ne 0) {
		throw "Command failed with exit code ${LASTEXITCODE}: $Command"
	}
}

if ($env:VSCMD_ARG_TGT_ARCH -ne "x64") {
	throw "Run this from an x64 Visual Studio 2022 developer shell (e.g. 'x64 Native Tools Command Prompt for VS 2022', then 'powershell')."
}
# The developer shell doesn't always put Visual Studio's bundled clang-cl on PATH
$VsLlvm = Join-Path $env:VCINSTALLDIR "Tools\Llvm\x64\bin"
if (-not (Get-Command "clang-cl" -ErrorAction SilentlyContinue) -and (Test-Path (Join-Path $VsLlvm "clang-cl.exe"))) {
	$env:PATH = "$VsLlvm;$env:PATH"
}
foreach ($Tool in "git", "cmake", "ninja", "clang-cl") {
	if (-not (Get-Command $Tool -ErrorAction SilentlyContinue)) {
		throw "$Tool was not found in PATH."
	}
}
if (-not $env:VCPKG_ROOT -or -not (Test-Path (Join-Path $env:VCPKG_ROOT "scripts/buildsystems/vcpkg.cmake"))) {
	throw "VCPKG_ROOT must point to a vcpkg checkout."
}

if (-not (Test-Path $Source)) {
	# Force LF line endings so the patch applies regardless of the user's core.autocrlf
	Exec { git clone --depth 1 --branch $RingRacersTag --config core.autocrlf=false --config core.eol=lf https://github.com/KartKrewDev/RingRacers.git $Source }
}

Push-Location $Source
try {
	if (-not (Select-String -Quiet -Path "CMakeLists.txt" -Pattern "SRB2_CONFIG_UWP")) {
		Exec { git apply $Patch }
	}

	Exec { cmake --preset $Preset }
	Exec { cmake --build --preset $Preset }
}
finally {
	Pop-Location
}

Exec { cmake -S (Join-Path $Root "uwp") -B $UwpBuild -G "Visual Studio 17 2022" -A x64 "-DRR_DIR=$($LibBuild.Replace('\', '/'))" }
Exec { cmake --build $UwpBuild --config Release }

Write-Host ""
Write-Host "Built. Open build\ringracers-uwp.sln in Visual Studio 2022 to deploy to your Xbox or create an app package."
