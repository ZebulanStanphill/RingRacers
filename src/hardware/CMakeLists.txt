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
	r_opengl/r_opengl.c
)

if(SRB2_CONFIG_D3D12)
	# VibeRant D3D: Legacy GL's Direct3D 12 driver (Windows only), whose shaders fxc compiles to headers
	target_compile_definitions(SRB2SDL2 PRIVATE HAVE_D3D12)
	find_program(SRB2_FXC fxc HINTS "$ENV{WindowsSdkVerBinPath}/x64" "$ENV{WindowsSdkBinPath}/x64")
	if(NOT SRB2_FXC)
		message(FATAL_ERROR "fxc (from the Windows SDK) wasn't found; it compiles Legacy GL's Direct3D 12 shaders")
	endif()
	message(STATUS "Compiling Legacy GL's Direct3D 12 shaders with ${SRB2_FXC}")

	set(d3d12_shader_source "${CMAKE_CURRENT_SOURCE_DIR}/r_d3d12/shaders.hlsl")
	set(d3d12_shader_dir "${CMAKE_CURRENT_BINARY_DIR}/r_d3d12_shaders")
	set(d3d12_shader_headers "")
	file(MAKE_DIRECTORY "${d3d12_shader_dir}")

	foreach(entry VSPoly VSPolyClip VSSky VSModel VSModelLight VSWipe VSFullscreen VSPostImage
		PSFixed PSDefault PSSoftware PSSoftwareModelLight PSWater PSWaterRefraction PSFog PSFogRemap PSSky PSWipe PSWipeFull PSPalettePostprocess PSFadeMap PSPostImage)
		string(SUBSTRING "${entry}" 0 2 stage)
		string(TOLOWER "${stage}" stage)
		set(header "${d3d12_shader_dir}/${entry}.h")
		add_custom_command(
			OUTPUT "${header}"
			COMMAND "${SRB2_FXC}" /nologo /O3 /D_UWP /DRR_D3D12 /T ${stage}_5_1 /E ${entry} /Vn g_${entry} /Fh "${header}" "${d3d12_shader_source}"
			DEPENDS "${d3d12_shader_source}"
			COMMENT "Compiling the ${entry} D3D12 shader"
			VERBATIM
		)
		list(APPEND d3d12_shader_headers "${header}")
	endforeach()

	set(root_header "${d3d12_shader_dir}/RootSignature.h")
	add_custom_command(
		OUTPUT "${root_header}"
		COMMAND "${SRB2_FXC}" /nologo /D_UWP /DRR_D3D12 /T rootsig_1_0 /E RR_ROOT_SIGNATURE /Vn g_RootSignature /Fh "${root_header}" "${d3d12_shader_source}"
		DEPENDS "${d3d12_shader_source}"
		COMMENT "Compiling the D3D12 root signature"
		VERBATIM
	)
	add_custom_target(SRB2D3D12Shaders DEPENDS ${d3d12_shader_headers} "${root_header}")
	add_dependencies(SRB2SDL2 SRB2D3D12Shaders)

	include(r_d3d12/custom_shaders.cmake)
	target_sources(SRB2SDL2 PRIVATE r_d3d12/custom_shader.cpp r_d3d12/d3d12_pipeline.cpp r_d3d12/d3d12_prewarm.cpp
		r_d3d12/d3d12_core.cpp r_d3d12/d3d12_upload.cpp r_d3d12/d3d12_present.cpp r_d3d12/r_d3d12.cpp)
	target_link_libraries(SRB2SDL2 PRIVATE glslang SPIRV glslang-default-resource-limits spirv-cross-hlsl d3dcompiler)
	target_include_directories(SRB2SDL2 PRIVATE "${CMAKE_CURRENT_BINARY_DIR}")
endif()
