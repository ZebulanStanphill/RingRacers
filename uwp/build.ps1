<#
.SYNOPSIS
Builds Ring Racers for UWP (Xbox Dev Mode).

.DESCRIPTION
Clones Ring Racers at the pinned release, applies patches/ringracers-uwp.patch,
builds it as a static library with the ninja-x64_windows_uwp_vcpkg-release
preset, builds SDL2 for UWP with OpenGL ES through ANGLE
(patches/sdl-angle.patch) and a fix for controllers showing up twice
(patches/sdl-controller-duplicates.patch), downloads ANGLE, then generates and
builds the UWP launcher solution in build/.

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

# worleydl's SDL2 for UWP, and our patches to it: switching it from Mesa to ANGLE, and keeping controllers from
# showing up twice
$SdlCommit = "c5f2ce7c4f792d4e42c81551fcb385dae96d98ce"
$SdlPatches = "sdl-angle.patch", "sdl-controller-duplicates.patch"
$AngleVersion = "2.1.14"
$AngleSha256 = "566F78D4FAB2086E694DC8F1EDCDB498EE549DADE8A198CA95246CFDD0632E98"

$Deps = Join-Path $Root "deps"
$SdlSource = Join-Path $Deps "SDL"
$SdlBuild = Join-Path $Deps "SDL-build"
# msbuild puts UWP project output in a subfolder named after the project
$SdlOut = Join-Path $SdlBuild "SDL2-UWP"
$Angle = Join-Path $Deps "ANGLE"

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
foreach ($Tool in "git", "cmake", "ninja", "clang-cl", "msbuild") {
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

New-Item -ItemType Directory -Force $Deps | Out-Null

if (-not (Test-Path $SdlSource)) {
	Exec { git init -q $SdlSource }
	Exec { git -C $SdlSource config core.autocrlf false }
	Exec { git -C $SdlSource config core.eol lf }
	Exec { git -C $SdlSource fetch -q --depth 1 https://github.com/worleydl/SDL-uwp-gl.git $SdlCommit }
	Exec { git -C $SdlSource checkout -q FETCH_HEAD }
	foreach ($SdlPatch in $SdlPatches) {
		Exec { git -C $SdlSource apply (Join-Path $Root "patches/$SdlPatch") }
	}
}
# Forward slashes so a trailing separator doesn't escape the closing quote when the path has spaces
Exec { msbuild (Join-Path $SdlSource "VisualC-WinRT/SDL-UWP.vcxproj") -m -nologo -p:Configuration=Release -p:Platform=x64 "-p:OutDir=$($SdlBuild.Replace('\', '/'))/" }

if (-not (Test-Path (Join-Path $Angle "bin/UAP/x64/libGLESv2.dll"))) {
	$AnglePackage = Join-Path $Deps "angle.zip"
	$ProgressPreference = "SilentlyContinue"
	Invoke-WebRequest -UseBasicParsing "https://api.nuget.org/v3-flatcontainer/angle.windowsstore/$AngleVersion/angle.windowsstore.$AngleVersion.nupkg" -OutFile $AnglePackage
	if ((Get-FileHash -Algorithm SHA256 $AnglePackage).Hash -ne $AngleSha256) {
		throw "ANGLE.WindowsStore $AngleVersion download doesn't match the expected SHA-256."
	}
	Expand-Archive -Force $AnglePackage $Angle
}

Exec { cmake -S (Join-Path $Root "uwp") -B $UwpBuild -G "Visual Studio 17 2022" -A x64 "-DRR_DIR=$($LibBuild.Replace('\', '/'))" "-DSDL_DIR=$($SdlOut.Replace('\', '/'))" "-DANGLE_DIR=$($Angle.Replace('\', '/'))" }
Exec { cmake --build $UwpBuild --config Release }

Write-Host ""
Write-Host "Built. Open build\ringracers-uwp.sln in Visual Studio 2022 to deploy to your Xbox or create an app package."
