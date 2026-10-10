param([string]$Engine = (Join-Path $PSScriptRoot "..\RingRacers"))
$ErrorActionPreference = "Stop"
$Engine = (Resolve-Path $Engine).Path
$Output = Join-Path $PSScriptRoot "..\build\d3d12-regressions"
$Shaders = Join-Path $Output "r_d3d12_shaders"
New-Item -ItemType Directory -Force $Shaders | Out-Null
# Match src/hardware/CMakeLists.txt exactly, including embedded root signatures.
$Source = Join-Path $Engine "src\hardware\r_d3d11\shaders.hlsl"
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
$Exe = Join-Path $Output 'd3d12-driver.exe'
& cl.exe /nologo /std:c++17 /O2 /EHsc /W3 /D_UWP /DHWRENDER /DHAVE_SDL /DHAVE_THREADS /D_CRT_SECURE_NO_WARNINGS /DNONX86 /DNORUSEASM @Includes "/Fe:$Exe" "/Fo:$Output\" @Files /link d3d12.lib dxgi.lib dxguid.lib
if ($LASTEXITCODE -ne 0) { throw "Production D3D12 driver compilation failed" }
& $Exe
if ($LASTEXITCODE -eq 77) {
    Write-Host 'Debug layer and GPU-based validation are required; installing Windows Graphics Tools.'
    Add-WindowsCapability -Online -Name 'Tools.Graphics.DirectX~~~~0.0.1.0' | Out-Null
    & $Exe
}
if ($LASTEXITCODE -ne 0) { throw "Production D3D12 WARP/GBV regression failed (exit $LASTEXITCODE)" }
# Each oracle must prove it rejects incorrect pixels. Failure is the required outcome.
foreach ($Scene in @('clear','readrect','opaque','translucent','additive','subtract','reverse','multiply','environment','masked','invert','fog','depth','nodepth','noocclude','indexed','line','rings','growth','shutdown')) {
    $ErrorActionPreference = "Continue"
    $NegativeLog = (& $Exe --negative-control $Scene 2>&1 | Out-String)
    $NegativeExit = $LASTEXITCODE
    $ErrorActionPreference = "Stop"
    Write-Host $NegativeLog
    if ($NegativeExit -ne 1 -or $NegativeLog -notmatch "NEGATIVE_ORACLE_REJECTED: $Scene") { throw "D3D12 $Scene negative control was not rejected by its oracle (exit $NegativeExit)" }
}

# Expected failing native controls must not become the enclosing CI shell exit code.
$global:LASTEXITCODE = 0
