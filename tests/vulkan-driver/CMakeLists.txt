cmake_minimum_required(VERSION 3.24)
project(RRVulkanDriverTests LANGUAGES C CXX)
set(CMAKE_CXX_STANDARD 17)
get_filename_component(RR_ENGINE "${CMAKE_CURRENT_LIST_DIR}/../.." ABSOLUTE)
set(SRB2_CONFIG_VIBERANT_VULKAN ON)
include("${RR_ENGINE}/src/hardware/r_viberant/custom_shaders.cmake")
add_executable(vulkan-driver
    ../viberant-driver.cpp
    "${RR_ENGINE}/src/hardware/r_viberant/r_viberant.cpp"
    "${RR_ENGINE}/src/hardware/r_viberant/viberant_core.cpp"
    "${RR_ENGINE}/src/hardware/r_viberant/viberant_upload.cpp"
    "${RR_ENGINE}/src/hardware/r_viberant/viberant_pipeline.cpp"
    "${RR_ENGINE}/src/hardware/r_viberant/viberant_prewarm.cpp"
    "${RR_ENGINE}/src/hardware/r_viberant/custom_shader.cpp")
target_compile_definitions(vulkan-driver PRIVATE HAVE_VIBERANT HWRENDER HAVE_SDL HAVE_THREADS NONX86 NORUSEASM)
target_include_directories(vulkan-driver PRIVATE "${RR_ENGINE}/src" "${CMAKE_CURRENT_BINARY_DIR}" ../viberant-stubs)
file(GLOB thirdparty_includes "${RR_ENGINE}/thirdparty/*/include")
target_include_directories(vulkan-driver PRIVATE ${thirdparty_includes})
target_link_libraries(vulkan-driver PRIVATE glslang SPIRV glslang-default-resource-limits spirv-cross-hlsl ${CMAKE_DL_LIBS})
set(viberant_shader_entries VSPoly VSPolyClip VSSky VSModel VSModelLight VSWipe VSFullscreen VSPostImage
	PSFixed PSDefault PSSoftware PSSoftwareModelLight PSWater PSWaterRefraction PSFog PSFogRemap PSSky PSWipe PSWipeFull PSPalettePostprocess PSFadeMap PSPostImage)
set(RR_VULKAN_SHADER_SOURCE "${RR_ENGINE}/src/hardware/r_viberant/shaders.hlsl" CACHE FILEPATH "HLSL source (negative controls only override this)")
set(viberant_shader_source "${RR_VULKAN_SHADER_SOURCE}")
if(SRB2_CONFIG_VIBERANT_VULKAN)
	# The Vulkan backend. Its shaders are the same shaders.hlsl, compiled to SPIR-V headers by glslang's HLSL front end:
	# registers become bindings of one descriptor set (b0.. at 0, t0.. at 16, s0.. at 32), SV_Position.w stays
	# Direct3D's, and the semantics are kept so that the backend can give every stage the same locations.
	# ENABLE_HLSL: glslang's header only declares its HLSL switches with it
	target_compile_definitions(vulkan-driver PRIVATE HAVE_VIBERANT_VULKAN VK_NO_PROTOTYPES ENABLE_HLSL)
	include(FetchContent)
	FetchContent_Declare(rr_vulkan_headers
		GIT_REPOSITORY https://github.com/KhronosGroup/Vulkan-Headers.git
		GIT_TAG 29f979ee5aa58b7b005f805ea8df7a855c39ff37 # v1.3.296
		SOURCE_SUBDIR does-not-exist) # only the headers are used
	FetchContent_Declare(rr_volk
		GIT_REPOSITORY https://github.com/zeux/volk.git
		GIT_TAG b465507a1720ea32d65163153a53ffa136ca835f # 1.3.295
		SOURCE_SUBDIR does-not-exist) # only volk.c and volk.h are used
	FetchContent_MakeAvailable(rr_vulkan_headers rr_volk)
	target_include_directories(vulkan-driver PRIVATE "${rr_vulkan_headers_SOURCE_DIR}/include" "${rr_volk_SOURCE_DIR}")
	if(NOT WIN32)
		# Direct3D 12's headers for platforms without the Windows SDK (MIT license)
		FetchContent_Declare(rr_directx_headers
			GIT_REPOSITORY https://github.com/microsoft/DirectX-Headers.git
			GIT_TAG 48a762973271c5a75869946bf1fdbc489a628a5c # v1.614.1
			SOURCE_SUBDIR does-not-exist) # only the headers are used
		FetchContent_MakeAvailable(rr_directx_headers)
		target_include_directories(vulkan-driver PRIVATE "${rr_directx_headers_SOURCE_DIR}/include"
			"${rr_directx_headers_SOURCE_DIR}/include/directx" "${rr_directx_headers_SOURCE_DIR}/include/wsl/stubs")
	endif()

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
				-D_UWP -DRR_D3D12 -DRR_VULKAN --vn g_${entry}_spv -o "${header}" "${viberant_shader_source}"
			DEPENDS "${viberant_shader_source}" glslang-standalone
			COMMENT "Compiling the ${entry} Vulkan shader"
			VERBATIM
		)
		list(APPEND vulkan_shader_headers "${header}")
	endforeach()
	add_custom_target(SRB2VulkanShaders DEPENDS ${vulkan_shader_headers})
	add_dependencies(vulkan-driver SRB2VulkanShaders)

	target_sources(vulkan-driver PRIVATE "${rr_volk_SOURCE_DIR}/volk.c"
		${RR_ENGINE}/src/hardware/r_viberant/vulkan/vk12_device.cpp ${RR_ENGINE}/src/hardware/r_viberant/vulkan/vk12_commands.cpp ${RR_ENGINE}/src/hardware/r_viberant/vulkan/vk12_resources.cpp
		${RR_ENGINE}/src/hardware/r_viberant/vulkan/vk12_pipeline.cpp ${RR_ENGINE}/src/hardware/r_viberant/spirv_shader.cpp)
endif()
