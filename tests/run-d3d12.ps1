param([string]$Engine = (Join-Path $PSScriptRoot ".."))
$ErrorActionPreference = "Stop"
$Engine = (Resolve-Path $Engine).Path
$Output = Join-Path $PSScriptRoot "..\build\d3d12-regressions"
$Shaders = Join-Path $Output "r_d3d12_shaders"
New-Item -ItemType Directory -Force $Shaders | Out-Null
# Match src/hardware/CMakeLists.txt exactly, including embedded root signatures.
$Source = Join-Path $Engine "src\hardware\r_d3d12\shaders.hlsl"
$Entries = @('VSPoly','VSPolyClip','VSSky','VSModel','VSModelLight','VSWipe','VSFullscreen','VSPostImage',
    'PSFixed','PSDefault','PSSoftware','PSSoftwareModelLight','PSWater','PSWaterRefraction','PSFog','PSFogRemap','PSSky','PSWipe','PSWipeFull','PSPalettePostprocess','PSFadeMap','PSPostImage')
foreach ($Entry in $Entries) {
    $Stage = $Entry.Substring(0,2).ToLower() + '_5_1'
    & fxc.exe /nologo /O3 /D_UWP /DRR_D3D12 /T $Stage /E $Entry /Vn "g_$Entry" /Fh "$Shaders\$Entry.h" $Source
    if ($LASTEXITCODE -ne 0) { throw "Production SM5.1 $Entry compilation failed" }
}
& fxc.exe /nologo /D_UWP /DRR_D3D12 /T rootsig_1_0 /E RR_ROOT_SIGNATURE /Vn g_RootSignature /Fh "$Shaders\RootSignature.h" $Source
if ($LASTEXITCODE -ne 0) { throw "Production root signature compilation failed" }
$Driver = Join-Path $Engine "src\hardware\r_d3d12"
$Files = @((Join-Path $PSScriptRoot 'd3d12-driver.cpp'), "$Driver\r_d3d12.cpp", "$Driver\d3d12_core.cpp", "$Driver\d3d12_upload.cpp", "$Driver\d3d12_pipeline.cpp", "$Driver\d3d12_prewarm.cpp")
$Includes = @("/I$Engine\src", "/I$Output", "/I$PSScriptRoot\d3d12-stubs")
# The real engine headers use third-party headers even though the harness calls no engine services.
Get-ChildItem "$Engine\thirdparty" -Directory | ForEach-Object {
    $Include = Join-Path $_.FullName 'include'
    if (Test-Path $Include) { $Includes += "/I$Include" }
}
# Build the production runtime translator with its pinned source dependencies.
# WARP runs before the app build, so its native archives must be prepared here.
$LegacySource = Join-Path $Output 'legacy-source'
$LegacyBuild = Join-Path $Output 'legacy-build'
New-Item -ItemType Directory -Force $LegacySource | Out-Null
$EngineCMake = $Engine.Replace('\', '/')
@"
cmake_minimum_required(VERSION 3.20)
project(RRD3D12LegacyHarness LANGUAGES C CXX)
set(CMAKE_CXX_STANDARD 17)
include("$EngineCMake/src/hardware/r_d3d12/custom_shaders.cmake")
add_library(rr_d3d12_legacy STATIC "$EngineCMake/src/hardware/r_d3d12/custom_shader.cpp")
target_link_libraries(rr_d3d12_legacy PRIVATE glslang SPIRV glslang-default-resource-limits spirv-cross-hlsl)
"@ | Set-Content (Join-Path $LegacySource 'CMakeLists.txt')
& cmake -S $LegacySource -B $LegacyBuild -G Ninja -DCMAKE_BUILD_TYPE=Release
if ($LASTEXITCODE -ne 0) { throw 'D3D12 runtime translator configuration failed' }
& cmake --build $LegacyBuild --target rr_d3d12_legacy
if ($LASTEXITCODE -ne 0) { throw 'D3D12 runtime translator compilation failed' }
$LegacyLibraries = @((Join-Path $LegacyBuild 'rr_d3d12_legacy.lib'))
$LegacyLibraries += @(Get-ChildItem (Join-Path $LegacyBuild 'legacy-shader-libs') -Filter '*.lib' | ForEach-Object { $_.FullName })
$Exe = Join-Path $Output 'd3d12-driver.exe'
& cl.exe /nologo /std:c++17 /O2 /MD /EHsc /W3 /D_UWP /DHAVE_D3D12 /DHWRENDER /DHAVE_SDL /DHAVE_THREADS /D_CRT_SECURE_NO_WARNINGS /DNONX86 /DNORUSEASM @Includes "/Fe:$Exe" "/Fo:$Output\" @Files /link d3d12.lib dxgi.lib dxguid.lib d3dcompiler.lib @LegacyLibraries
if ($LASTEXITCODE -ne 0) { throw "Production D3D12 driver compilation failed" }
& $Exe
if ($LASTEXITCODE -eq 77) {
    Write-Host 'Debug layer and GPU-based validation are required; installing Windows Graphics Tools.'
    Add-WindowsCapability -Online -Name 'Tools.Graphics.DirectX~~~~0.0.1.0' | Out-Null
    & $Exe
}
if ($LASTEXITCODE -ne 0) { throw "Production D3D12 WARP/GBV regression failed (exit $LASTEXITCODE)" }
# The full run above checks its own oracles (a self-test of every kind of comparison, and a registry of the scenes whose
# oracle ran), so the negative controls left are the ones that change the code under test: the oracle stays correct and
# must reject the regression. Each is its own process that replays the scenes up to the one that rejects it, with the
# debug layer but without GPU-based validation (the full run validates the same work).
$Failures = @()
function Finish($Run) {
    $Run.Process.WaitForExit()
    $Log = $Run.Output.Result + $Run.Error.Result
    $ExitCode = $Run.Process.ExitCode
    $Run.Process.Dispose()
    Write-Host $Log
    if ($ExitCode -ne 1 -or $Log -notmatch "NEGATIVE_ORACLE_REJECTED: $($Run.Scene)") {
        $script:Failures += "D3D12 $($Run.Scene) negative control was not rejected by its oracle (exit $ExitCode)"
    }
}
function Run-Control([string]$ControlExe, [string]$Scene) {
    $Info = [System.Diagnostics.ProcessStartInfo]::new($ControlExe, "--negative-control $Scene")
    $Info.UseShellExecute = $false
    $Info.RedirectStandardOutput = $true
    $Info.RedirectStandardError = $true
    $Process = [System.Diagnostics.Process]::Start($Info)
    # Both streams are drained on the thread pool, so a chatty control can't fill a pipe and stall.
    Finish @{ Scene = $Scene; Process = $Process; Output = $Process.StandardOutput.ReadToEndAsync(); Error = $Process.StandardError.ReadToEndAsync() }
}
# Turning add-on shaders off must make the custom-shader oracle see built-in pixels.
Run-Control $Exe 'custom-shader-nocustom'

# Preserve the former SM4 harness's bug-injection coverage on the shipped SM5.1 path.
# These controls change GPU code while keeping the pixel oracle correct. Recompile only
# the pipeline object containing the shader bytecode, then link the production harness.
$Controls = @(
    @{ Scene = 'water-refraction'; Entry = 'PSWaterRefraction'; Good = 'WaterBackgroundOffset(i.pos.w, u_leveltime)'; Bad = 'WaterBackgroundOffset(1.0 / i.pos.w, u_leveltime)' },
    @{ Scene = 'fog-palette'; Entry = 'PSFogRemap'; Good = 'FogColormap(i.pos.w, u_fog_lightnum'; Bad = 'FogColormap(1.0 / i.pos.w, u_fog_lightnum' },
    @{ Scene = 'brightmap-palette'; Entry = 'PSSoftware'; Good = 'brightmap_mix > 0.0 ? 32 : '; Bad = '' }
)
$ProductionSource = Get-Content -Raw $Source
foreach ($Control in $Controls) {
    if (-not $ProductionSource.Contains($Control.Good)) { throw "Cannot inject $($Control.Scene) shader regression" }
    $ControlDir = Join-Path $Output $Control.Scene
    $ControlShaders = Join-Path $ControlDir 'r_d3d12_shaders'
    New-Item -ItemType Directory -Force $ControlShaders | Out-Null
    Copy-Item "$Shaders\*.h" $ControlShaders
    $ControlSource = Join-Path $ControlDir 'shaders.hlsl'
    $ProductionSource.Replace($Control.Good, $Control.Bad) | Set-Content $ControlSource
    $Entry = $Control.Entry
    & fxc.exe /nologo /O3 /D_UWP /DRR_D3D12 /T ps_5_1 /E $Entry /Vn "g_$Entry" /Fh "$ControlShaders\$Entry.h" $ControlSource
    if ($LASTEXITCODE -ne 0) { throw "Negative $($Control.Scene) SM5.1 compilation failed" }
    $PipelineObject = Join-Path $ControlDir 'd3d12_pipeline.obj'
    $ControlIncludes = @("/I$ControlDir") + $Includes
    & cl.exe /nologo /c /std:c++17 /O2 /MD /EHsc /W3 /D_UWP /DHAVE_D3D12 /DHWRENDER /DHAVE_SDL /DHAVE_THREADS /D_CRT_SECURE_NO_WARNINGS /DNONX86 /DNORUSEASM @ControlIncludes "/Fo:$PipelineObject" "$Driver\d3d12_pipeline.cpp"
    if ($LASTEXITCODE -ne 0) { throw "Negative $($Control.Scene) pipeline compilation failed" }
    $Objects = @($Files | Where-Object { [IO.Path]::GetFileName($_) -ne 'd3d12_pipeline.cpp' } | ForEach-Object { Join-Path $Output ([IO.Path]::GetFileNameWithoutExtension($_) + '.obj') })
    $ControlExe = Join-Path $ControlDir 'd3d12-driver.exe'
    & cl.exe /nologo "/Fe:$ControlExe" @Objects $PipelineObject /link d3d12.lib dxgi.lib dxguid.lib d3dcompiler.lib @LegacyLibraries
    if ($LASTEXITCODE -ne 0) { throw "Negative $($Control.Scene) harness link failed" }
    Run-Control $ControlExe $Control.Scene
}
if ($Failures.Count -gt 0) { throw ($Failures -join "`n") }

# Expected failing native controls must not become the enclosing CI shell exit code.
$global:LASTEXITCODE = 0
