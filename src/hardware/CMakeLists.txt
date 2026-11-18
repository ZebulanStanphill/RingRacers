target_sources(SRB2SDL2 PRIVATE
	hw_bsp.c
	hw_draw.c
	hw_light.c
	hw_main.c
	hw_clip.c
	hw_md2.c
	hw_cache.c
	hw_md2load.c
	hw_md3load.c
	hw_model.c
	u_list.c
	hw_batching.c
	# [ZE]: Build the runtime driver query so shared rendering code can follow the active backend.
	hw_driver.c
	r_opengl/r_opengl.c
)

# [ZE]: Build the enabled hardware backends with shared driver sources and backend-specific shaders.
set(viberant_shader_entries VSPoly VSPolyClip VSSky VSModel VSModelLight VSWipe VSFullscreen VSPostImage
	PSFixed PSDefault PSSoftware PSSoftwareModelLight PSWater PSWaterRefraction PSFog PSFogRemap PSSky PSWipe PSWipeFull PSPalettePostprocess PSFadeMap PSPostImage)

if(SRB2_CONFIG_VIBERANT_D3D OR SRB2_CONFIG_VIBERANT_VULKAN OR SRB2_CONFIG_VIBERANT_METAL)
	# The VibeRant driver, the hardware renderer's driver. It is written against the Direct3D 12 interfaces: the Direct3D 12
	# backend (VibeRant D3D) runs on Direct3D 12 itself, the Vulkan backend (VibeRant Vulkan) on Vulkan, and the Metal
	# backend (VibeRant Metal) on Metal, by implementing those interfaces. HAVE_VIBERANT means the driver is built, with
	# any of the backends.
	target_compile_definitions(SRB2SDL2 PRIVATE HAVE_VIBERANT)
	set(viberant_shader_source "${CMAKE_CURRENT_SOURCE_DIR}/r_viberant/shaders.hlsl")
	include(r_viberant/custom_shaders.cmake)
	target_sources(SRB2SDL2 PRIVATE r_viberant/custom_shader.cpp r_viberant/viberant_pipeline.cpp r_viberant/viberant_prewarm.cpp
		r_viberant/viberant_core.cpp r_viberant/viberant_upload.cpp r_viberant/r_viberant.cpp)
	target_link_libraries(SRB2SDL2 PRIVATE glslang SPIRV glslang-default-resource-limits spirv-cross-hlsl)
	target_include_directories(SRB2SDL2 PRIVATE "${CMAKE_CURRENT_BINARY_DIR}")
endif()

if(SRB2_CONFIG_VIBERANT_D3D)
	# The Direct3D 12 backend (Windows only; beside OpenGL in desktop builds), whose shaders fxc compiles to headers
	target_compile_definitions(SRB2SDL2 PRIVATE HAVE_VIBERANT_D3D)
	find_program(SRB2_FXC fxc HINTS "$ENV{WindowsSdkVerBinPath}/x64" "$ENV{WindowsSdkBinPath}/x64")
	if(NOT SRB2_FXC)
		message(FATAL_ERROR "fxc (from the Windows SDK) wasn't found; it compiles the driver's Direct3D 12 shaders")
	endif()
	message(STATUS "Compiling the driver's Direct3D 12 shaders with ${SRB2_FXC}")

	set(viberant_shader_dir "${CMAKE_CURRENT_BINARY_DIR}/r_viberant_shaders")
	set(viberant_shader_headers "")
	file(MAKE_DIRECTORY "${viberant_shader_dir}")

	foreach(entry ${viberant_shader_entries})
		string(SUBSTRING "${entry}" 0 2 stage)
		string(TOLOWER "${stage}" stage)
		set(header "${viberant_shader_dir}/${entry}.h")
		add_custom_command(
			OUTPUT "${header}"
			COMMAND "${SRB2_FXC}" /nologo /O3 /T ${stage}_5_1 /E ${entry} /Vn g_${entry} /Fh "${header}" "${viberant_shader_source}"
			DEPENDS "${viberant_shader_source}"
			COMMENT "Compiling the ${entry} D3D12 shader"
			VERBATIM
		)
		list(APPEND viberant_shader_headers "${header}")
	endforeach()

	set(root_header "${viberant_shader_dir}/RootSignature.h")
	add_custom_command(
		OUTPUT "${root_header}"
		COMMAND "${SRB2_FXC}" /nologo /T rootsig_1_0 /E RR_ROOT_SIGNATURE /Vn g_RootSignature /Fh "${root_header}" "${viberant_shader_source}"
		DEPENDS "${viberant_shader_source}"
		COMMENT "Compiling the D3D12 root signature"
		VERBATIM
	)
	add_custom_target(SRB2VibeRantShaders DEPENDS ${viberant_shader_headers} "${root_header}")
	add_dependencies(SRB2SDL2 SRB2VibeRantShaders)

	target_link_libraries(SRB2SDL2 PRIVATE d3dcompiler)
	# The one platform-specific file: the swap chain on UWP's CoreWindow (taken from ANGLE), or on a child window of the SDL window
	if(SRB2_CONFIG_UWP)
		target_sources(SRB2SDL2 PRIVATE r_viberant/d3d_present_uwp.cpp)
	else()
		target_sources(SRB2SDL2 PRIVATE r_viberant/d3d_present_win32.cpp)
		# UWP gets these from its own dependency set
		target_link_libraries(SRB2SDL2 PRIVATE d3d12 dxgi dxguid user32)
	endif()
endif()

if(SRB2_CONFIG_VIBERANT_VULKAN OR SRB2_CONFIG_VIBERANT_METAL)
	# What the Vulkan and the Metal backend share. Their shaders are the same shaders.hlsl, compiled to SPIR-V headers
	# by glslang's HLSL front end: registers become bindings of one descriptor set (b0.. at 0, t0.. at 16, s0.. at 32),
	# SV_Position.w stays Direct3D's, and the semantics are kept so that the backend can give every stage the same
	# locations. The Metal backend turns the SPIR-V into Metal Shading Language when a pipeline state first needs it.
	# ENABLE_HLSL: glslang's header only declares its HLSL switches with it
	target_compile_definitions(SRB2SDL2 PRIVATE ENABLE_HLSL)
	include(FetchContent)
	if(NOT WIN32)
		# Direct3D 12's headers for platforms without the Windows SDK (MIT license)
		FetchContent_Declare(rr_directx_headers
			GIT_REPOSITORY https://github.com/microsoft/DirectX-Headers.git
			GIT_TAG 48a762973271c5a75869946bf1fdbc489a628a5c # v1.614.1
			SOURCE_SUBDIR does-not-exist) # only the headers are used
		FetchContent_MakeAvailable(rr_directx_headers)
		target_include_directories(SRB2SDL2 PRIVATE "${rr_directx_headers_SOURCE_DIR}/include"
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
				-DRR_VULKAN --vn g_${entry}_spv -o "${header}" "${viberant_shader_source}"
			DEPENDS "${viberant_shader_source}" glslang-standalone
			COMMENT "Compiling the ${entry} shader to SPIR-V"
			VERBATIM
		)
		list(APPEND vulkan_shader_headers "${header}")
	endforeach()
	add_custom_target(SRB2VulkanShaders DEPENDS ${vulkan_shader_headers})
	add_dependencies(SRB2SDL2 SRB2VulkanShaders)

	target_sources(SRB2SDL2 PRIVATE r_viberant/spirv_shader.cpp)
endif()

if(SRB2_CONFIG_VIBERANT_VULKAN)
	# The Vulkan backend
	target_compile_definitions(SRB2SDL2 PRIVATE HAVE_VIBERANT_VULKAN VK_NO_PROTOTYPES)
	FetchContent_Declare(rr_vulkan_headers
		GIT_REPOSITORY https://github.com/KhronosGroup/Vulkan-Headers.git
		GIT_TAG 29f979ee5aa58b7b005f805ea8df7a855c39ff37 # v1.3.296
		SOURCE_SUBDIR does-not-exist) # only the headers are used
	FetchContent_Declare(rr_volk
		GIT_REPOSITORY https://github.com/zeux/volk.git
		GIT_TAG b465507a1720ea32d65163153a53ffa136ca835f # 1.3.295
		SOURCE_SUBDIR does-not-exist) # only volk.c and volk.h are used
	FetchContent_MakeAvailable(rr_vulkan_headers rr_volk)
	target_include_directories(SRB2SDL2 PRIVATE "${rr_vulkan_headers_SOURCE_DIR}/include" "${rr_volk_SOURCE_DIR}")

	target_sources(SRB2SDL2 PRIVATE "${rr_volk_SOURCE_DIR}/volk.c"
		r_viberant/vulkan/vk12_device.cpp r_viberant/vulkan/vk12_commands.cpp r_viberant/vulkan/vk12_resources.cpp
		r_viberant/vulkan/vk12_pipeline.cpp r_viberant/vulkan/vk_present.cpp)
endif()

if(SRB2_CONFIG_VIBERANT_METAL)
	# The Metal backend (macOS), "mtl12", and its presenter: Objective-C++, except the SPIR-V to Metal Shading Language
	# step (SPIRV-Cross) and the presenter's way to the game's own headers, which Apple's can't share a file with
	target_compile_definitions(SRB2SDL2 PRIVATE HAVE_VIBERANT_METAL)
	set(metal_objcxx_sources r_viberant/metal/mtl12_device.mm r_viberant/metal/mtl12_commands.mm
		r_viberant/metal/mtl12_resources.mm r_viberant/metal/mtl12_pipeline.mm r_viberant/metal/mtl_present.mm)
	target_sources(SRB2SDL2 PRIVATE ${metal_objcxx_sources} r_viberant/metal/mtl12_msl.cpp r_viberant/metal/mtl_present_game.cpp)
	# mtl12 keeps Metal's objects in C++ classes, which automatic reference counting makes safe: without it nothing
	# retains them. The option is the target's, for its Objective-C++ files, which are these five and no others: a
	# source file's own properties only reach targets of the directory that sets them, the target is made in the
	# directory above, and naming that directory (TARGET_DIRECTORY) takes CMake 3.18, where older versions read the
	# word as one more file and build without the option, with no word of it.
	target_compile_options(SRB2SDL2 PRIVATE $<$<COMPILE_LANGUAGE:OBJCXX>:-fobjc-arc>)
	target_link_libraries(SRB2SDL2 PRIVATE spirv-cross-msl
		"-framework Metal" "-framework QuartzCore" "-framework Foundation" "-framework CoreGraphics" "-framework AppKit")
endif()
