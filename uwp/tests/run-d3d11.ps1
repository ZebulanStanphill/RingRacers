param([string]$Engine = (Join-Path $PSScriptRoot "..\RingRacers"))
$ErrorActionPreference = "Stop"
$Engine = (Resolve-Path $Engine).Path
$Output = Join-Path $PSScriptRoot "..\build\shader-regressions"
New-Item -ItemType Directory -Force $Output | Out-Null
$Exe = Join-Path $Output "water-d3d11.exe"
& cl.exe /nologo /std:c++17 /O2 /EHsc /W3 /WX "/I$Engine\src" "/Fe:$Exe" "/Fo:$Output\water-d3d11.obj" (Join-Path $PSScriptRoot "water-d3d11.cpp") /link d3d11.lib d3dcompiler.lib dxguid.lib
if ($LASTEXITCODE -ne 0) { throw "Direct3D shader test compilation failed" }
& $Exe $Engine
if ($LASTEXITCODE -ne 0) { throw "Production Direct3D water shader regression failed" }
& $Exe $Engine --negative-control
if ($LASTEXITCODE -ne 0) { throw "Old reciprocal-depth shader was not rejected" }

# Generate the probe from the production driver; only the surrounding engine
# state is supplied by the harness. GPU copies/readback use real Direct3D.
python (Join-Path $PSScriptRoot "generate.py") --engine $Engine --output "$Output\generated"
if ($LASTEXITCODE -ne 0) { throw "Surface probe extraction failed" }
$Probe = Join-Path $Output "surface-probe-d3d11.exe"
& cl.exe /nologo /std:c++17 /O2 /EHsc /W3 /WX /D_UWP /D_CRT_SECURE_NO_WARNINGS /DNONX86 /DNORUSEASM "/I$Engine\src" "/Fe:$Probe" "/Fo:$Output\surface-probe-d3d11.obj" "$Output\generated\surface_probe_d3d11.cpp" /link d3d11.lib
if ($LASTEXITCODE -ne 0) { throw "Direct3D surface probe compilation failed" }
& $Probe
if ($LASTEXITCODE -ne 0) { throw "Production Direct3D surface probe regression failed" }

# Execute the production custom-program compiler/input-layout factory on SM4.
$Custom = Join-Path $Output "custom-shaders-d3d11.exe"
& cl.exe /nologo /std:c++17 /O2 /EHsc /W3 /WX /D_UWP /D_CRT_SECURE_NO_WARNINGS /DNONX86 /DNORUSEASM "/I$Engine\src" "/Fe:$Custom" "/Fo:$Output\custom-shaders-d3d11.obj" "$Output\generated\custom_d3d11.cpp" /link d3d11.lib d3dcompiler.lib
if ($LASTEXITCODE -ne 0) { throw "Direct3D custom-shader compilation failed" }
$Fixture = Get-ChildItem (Join-Path $PSScriptRoot "..\regression-results") -Recurse -Filter 'render-0.vert.hlsl' | Select-Object -First 1
if ($null -eq $Fixture) { throw "No translated custom-shader GPU fixtures" }
& $Custom $Fixture.DirectoryName
if ($LASTEXITCODE -ne 0) { throw "Custom shader rendering regression failed" }
