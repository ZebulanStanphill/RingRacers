cmake_minimum_required(VERSION 3.24)
project(RRMetalDriverTests LANGUAGES C CXX OBJCXX)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_OBJCXX_STANDARD 17)
get_filename_component(RR_ENGINE "${CMAKE_CURRENT_LIST_DIR}/../.." ABSOLUTE)
# The Metal backend of the VibeRant driver (src/hardware/r_viberant/metal, "mtl12"), with the driver test in front of it:
# the counterpart of ../vulkan-driver. HAVE_VIBERANT_METAL is the only backend of a macOS build.
set(SRB2_CONFIG_VIBERANT_METAL ON)
include("${RR_ENGINE}/src/hardware/r_viberant/custom_shaders.cmake")
set(metal_backend_sources
    "${RR_ENGINE}/src/hardware/r_viberant/metal/mtl12_device.mm"
    "${RR_ENGINE}/src/hardware/r_viberant/metal/mtl12_commands.mm"
    "${RR_ENGINE}/src/hardware/r_viberant/metal/mtl12_resources.mm"
    "${RR_ENGINE}/src/hardware/r_viberant/metal/mtl12_pipeline.mm")
add_executable(metal-driver
    ../viberant-driver.cpp
    "${RR_ENGINE}/src/hardware/r_viberant/r_viberant.cpp"
    "${RR_ENGINE}/src/hardware/r_viberant/viberant_core.cpp"
    "${RR_ENGINE}/src/hardware/r_viberant/viberant_upload.cpp"
    "${RR_ENGINE}/src/hardware/r_viberant/viberant_pipeline.cpp"
    "${RR_ENGINE}/src/hardware/r_viberant/viberant_prewarm.cpp"
    "${RR_ENGINE}/src/hardware/r_viberant/custom_shader.cpp"
    "${RR_ENGINE}/src/hardware/r_viberant/spirv_shader.cpp"
    "${RR_ENGINE}/src/hardware/r_viberant/metal/mtl12_msl.cpp"
    ${metal_backend_sources}
    metal-objects.mm)
# mtl12 keeps Metal's objects in C++ classes, which automatic reference counting makes safe
set_source_files_properties(${metal_backend_sources} metal-objects.mm PROPERTIES COMPILE_OPTIONS -fobjc-arc)
# ENABLE_HLSL: glslang's header only declares its HLSL switches with it
target_compile_definitions(metal-driver PRIVATE HAVE_VIBERANT HAVE_VIBERANT_METAL HWRENDER HAVE_SDL HAVE_THREADS NONX86 NORUSEASM ENABLE_HLSL)
target_include_directories(metal-driver PRIVATE "${RR_ENGINE}/src" "${CMAKE_CURRENT_BINARY_DIR}" ../viberant-stubs)
file(GLOB thirdparty_includes "${RR_ENGINE}/thirdparty/*/include")
target_include_directories(metal-driver PRIVATE ${thirdparty_includes})
target_link_libraries(metal-driver PRIVATE glslang SPIRV glslang-default-resource-limits spirv-cross-hlsl spirv-cross-msl
    "-framework Metal" "-framework Foundation" "-framework CoreGraphics" "-framework QuartzCore")

# Direct3D 12's headers for platforms without the Windows SDK (MIT license)
include(FetchContent)
FetchContent_Declare(rr_directx_headers
    GIT_REPOSITORY https://github.com/microsoft/DirectX-Headers.git
    GIT_TAG 48a762973271c5a75869946bf1fdbc489a628a5c # v1.614.1
    SOURCE_SUBDIR does-not-exist) # only the headers are used
FetchContent_MakeAvailable(rr_directx_headers)
target_include_directories(metal-driver PRIVATE "${rr_directx_headers_SOURCE_DIR}/include"
    "${rr_directx_headers_SOURCE_DIR}/include/directx" "${rr_directx_headers_SOURCE_DIR}/include/wsl/stubs")

# The shaders are the SPIR-V the Vulkan backend is built with, by the same commands (src/hardware/CMakeLists.txt): the
# driver hands them to mtl12, which translates them to Metal Shading Language when a pipeline state first needs them.
set(viberant_shader_entries VSPoly VSPolyClip VSSky VSModel VSModelLight VSWipe VSFullscreen VSPostImage
    PSFixed PSDefault PSSoftware PSSoftwareModelLight PSWater PSWaterRefraction PSFog PSFogRemap PSSky PSWipe PSWipeFull PSPalettePostprocess PSFadeMap PSPostImage)
set(RR_VULKAN_SHADER_SOURCE "${RR_ENGINE}/src/hardware/r_viberant/shaders.hlsl" CACHE FILEPATH "HLSL source (negative controls only override this)")
set(viberant_shader_source "${RR_VULKAN_SHADER_SOURCE}")
set(vulkan_shader_dir "${CMAKE_CURRENT_BINARY_DIR}/r_vulkan_shaders")
set(vulkan_shader_headers "")
file(MAKE_DIRECTORY "${vulkan_shader_dir}")
foreach(entry ${viberant_shader_entries})
    string(SUBSTRING "${entry}" 0 2 stage)
    if(stage STREQUAL "VS")
        set(stage vert)
    else()
        set(stage frag)
    endif()
    set(header "${vulkan_shader_dir}/${entry}.h")
    add_custom_command(
        OUTPUT "${header}"
        COMMAND glslang-standalone -D -V -S ${stage} -e ${entry} --hlsl-dx-position-w -fhlsl_functionality1
            --shift-ubo-binding 0 --shift-texture-binding 16 --shift-sampler-binding 32
            -DRR_VULKAN --vn g_${entry}_spv -o "${header}" "${viberant_shader_source}"
        DEPENDS "${viberant_shader_source}" glslang-standalone
        COMMENT "Compiling the ${entry} shader to SPIR-V"
        VERBATIM
    )
    list(APPEND vulkan_shader_headers "${header}")
endforeach()
add_custom_target(SRB2VulkanShaders DEPENDS ${vulkan_shader_headers})
add_dependencies(metal-driver SRB2VulkanShaders)
