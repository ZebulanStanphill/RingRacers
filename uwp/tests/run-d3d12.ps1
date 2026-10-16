param([string]$Engine = (Join-Path $PSScriptRoot "..\RingRacers"))
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
& cl.exe /nologo /std:c++17 /O2 /MD /EHsc /W3 /D_UWP /DHWRENDER /DHAVE_SDL /DHAVE_THREADS /D_CRT_SECURE_NO_WARNINGS /DNONX86 /DNORUSEASM @Includes "/Fe:$Exe" "/Fo:$Output\" @Files /link d3d12.lib dxgi.lib dxguid.lib d3dcompiler.lib @LegacyLibraries
if ($LASTEXITCODE -ne 0) { throw "Production D3D12 driver compilation failed" }
& $Exe
if ($LASTEXITCODE -eq 77) {
    Write-Host 'Debug layer and GPU-based validation are required; installing Windows Graphics Tools.'
    Add-WindowsCapability -Online -Name 'Tools.Graphics.DirectX~~~~0.0.1.0' | Out-Null
    & $Exe
}
if ($LASTEXITCODE -ne 0) { throw "Production D3D12 WARP/GBV regression failed (exit $LASTEXITCODE)" }
# Each oracle must prove it rejects incorrect pixels. Failure is the required outcome.
# Every control is its own process that replays the scenes before its own with the debug layer but without
# GPU-based validation (the full run above validates the same work), and they run several at a time (one per
# core); the harness touches no files, so they don't collide.
$Scenes = @('startup-present','clear','readrect','surface-probe','opaque','translucent','additive','subtract','reverse','multiply','environment','masked','invert','fog','fog-remap','fog-palette','water-surface','water-refraction','water-group','water-group-counts','capture-mixed','capture-recreate','capture-skipped','depth','nodepth','noocclude','portal-supported','portal-aperture','portal-nested','portal-restore','portal-depth','portal-plane','indexed','line','rings','growth','shutdown','custom-shader','custom-shader-clean','custom-shader-invalid','custom-shader-nocustom','texture-guard','texture','texture-update','texture-reuse','mips','palette','palette-post','brightmap-palette','brightmap-smooth','light-table-update','light-table-update-post','light-table-clear','screen-palette','palette-off','sky','model-buffered','model-lerped','model-fallback','screen-copy','final-screen','screen-flush','wipe-blend','wipe-mode','wipe-wiggle','wipe-fallback','wipe-after','postimg-identity','postimg-shift','fade-map-off','fade-map','fade-map-row','fade-map-clamp','fade-map-remake','fade-map-after','fade-map-reuse','resize-fade-map','resize-screen','blend-invisible','blend-invisible-depth','blend-invisible-restore','blend-remap-mask','blend-remap-restore','custom-none-quiet','custom-none','custom-layout-poly','custom-layout-sky','custom-layout-model','custom-recompile','custom-replace-invalid','custom-replace-recover','custom-sprite-clip','custom-model-kept','custom-model-lighting','custom-water-refraction')
$Workers = [Math]::Max(1, [Environment]::ProcessorCount)
$Pending = [System.Collections.Generic.List[object]]::new()
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
foreach ($Scene in $Scenes) {
    while ($Pending.Count -ge $Workers) {
        $Done = $Pending | Where-Object { $_.Process.HasExited } | Select-Object -First 1
        if ($null -eq $Done) { Start-Sleep -Milliseconds 200; continue }
        $Pending.Remove($Done) | Out-Null
        Finish $Done
    }
    $Info = [System.Diagnostics.ProcessStartInfo]::new($Exe, "--negative-control $Scene")
    $Info.UseShellExecute = $false
    $Info.RedirectStandardOutput = $true
    $Info.RedirectStandardError = $true
    $Process = [System.Diagnostics.Process]::Start($Info)
    # Both streams are drained on the thread pool, so a chatty control can't fill a pipe and stall.
    $Pending.Add(@{ Scene = $Scene; Process = $Process; Output = $Process.StandardOutput.ReadToEndAsync(); Error = $Process.StandardError.ReadToEndAsync() })
}
foreach ($Run in $Pending) { Finish $Run }
if ($Failures.Count -gt 0) { throw ($Failures -join "`n") }

# Preserve the former SM4 harness's bug-injection coverage on the shipped SM5.1 path.
# These controls change GPU code while keeping the pixel oracle correct. Recompile only
# the pipeline object containing the shader bytecode, then link the production harness.
function Run-ShaderControl([string]$ControlExe, [string]$Scene) {
    $Info = [System.Diagnostics.ProcessStartInfo]::new($ControlExe, "--shader-negative-control $Scene")
    $Info.UseShellExecute = $false
    $Info.RedirectStandardOutput = $true
    $Info.RedirectStandardError = $true
    $Process = [System.Diagnostics.Process]::Start($Info)
    Finish @{ Scene = $Scene; Process = $Process; Output = $Process.StandardOutput.ReadToEndAsync(); Error = $Process.StandardError.ReadToEndAsync() }
}
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
    & cl.exe /nologo /c /std:c++17 /O2 /MD /EHsc /W3 /D_UWP /DHWRENDER /DHAVE_SDL /DHAVE_THREADS /D_CRT_SECURE_NO_WARNINGS /DNONX86 /DNORUSEASM @ControlIncludes "/Fo:$PipelineObject" "$Driver\d3d12_pipeline.cpp"
    if ($LASTEXITCODE -ne 0) { throw "Negative $($Control.Scene) pipeline compilation failed" }
    $Objects = @($Files | Where-Object { [IO.Path]::GetFileName($_) -ne 'd3d12_pipeline.cpp' } | ForEach-Object { Join-Path $Output ([IO.Path]::GetFileNameWithoutExtension($_) + '.obj') })
    $ControlExe = Join-Path $ControlDir 'd3d12-driver.exe'
    & cl.exe /nologo "/Fe:$ControlExe" @Objects $PipelineObject /link d3d12.lib dxgi.lib dxguid.lib d3dcompiler.lib @LegacyLibraries
    if ($LASTEXITCODE -ne 0) { throw "Negative $($Control.Scene) harness link failed" }
    Run-ShaderControl $ControlExe $Control.Scene
}
# These controls inject missing/repeated cache pretranslation for bright Encore texels.
# The four-cycle palette makes either error visible without changing normal/exempt texels.
Run-ShaderControl $Exe 'brightmap-encore-missing'
Run-ShaderControl $Exe 'brightmap-encore-double'
Run-ShaderControl $Exe 'brightmap-encore-missing-smooth'
Run-ShaderControl $Exe 'brightmap-encore-double-smooth'
if ($Failures.Count -gt 0) { throw ($Failures -join "`n") }

# Expected failing native controls must not become the enclosing CI shell exit code.
$global:LASTEXITCODE = 0
