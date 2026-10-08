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
