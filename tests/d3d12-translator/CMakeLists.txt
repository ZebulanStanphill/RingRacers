# The production runtime shader translator and its pinned glslang/SPIRV-Cross sources, for the Direct3D 12 WARP
# harness (tests/run-d3d12.ps1). CI caches this build; its key hashes this file and custom_shaders.cmake.
cmake_minimum_required(VERSION 3.20)
project(RRD3D12LegacyHarness LANGUAGES C CXX)
set(CMAKE_CXX_STANDARD 17)
set(RR_ENGINE "" CACHE PATH "Ring Racers checkout whose translator is built")
include("${RR_ENGINE}/src/hardware/r_d3d12/custom_shaders.cmake")
add_library(rr_d3d12_legacy STATIC "${RR_ENGINE}/src/hardware/r_d3d12/custom_shader.cpp")
target_link_libraries(rr_d3d12_legacy PRIVATE glslang SPIRV glslang-default-resource-limits spirv-cross-hlsl)
