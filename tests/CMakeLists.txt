cmake_minimum_required(VERSION 3.24)
project(ringracers_regressions LANGUAGES CXX)

enable_testing()
find_package(Python3 3.9 REQUIRED COMPONENTS Interpreter)
set(RR_SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/.." CACHE PATH "Ring Racers source checkout to test")
option(RR_SANITIZERS "Run with AddressSanitizer and UndefinedBehaviorSanitizer" ON)

if(NOT EXISTS "${RR_SOURCE_DIR}/src/hardware/r_d3d12/shaders.hlsl")
    message(FATAL_ERROR "Set RR_SOURCE_DIR to a Ring Racers checkout with the Direct3D 12 driver; see tests/README.md")
endif()
if(NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang" OR
   CMAKE_CXX_COMPILER_FRONTEND_VARIANT STREQUAL "MSVC")
    message(FATAL_ERROR "Use clang++ (Linux/macOS/WSL); the wipe harness uses Clang vector types")
endif()

# Reconfigure/regenerate when either a tested implementation or harness changes.
file(GLOB templates CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/templates/*.in")
file(GLOB_RECURSE smoke_sources CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/addons/shader-smoke/source/*")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
    ${smoke_sources} addons/shader-smoke/build.py addons/shader-smoke/D_ShaderSmoke_v1.pk3
    ${templates} fixtures/sprite-rotation-baseline.c source.py generate.py title_replay.py title_fixture.py title-shifts.ignorelist fixtures/title-map.json fixtures/title-znodes.bin fixtures/rhi_glsl_fragment_postimg.glsl fixtures/water-palace-horizons.json fixtures/water-palace-water.json
    "${RR_SOURCE_DIR}/src/f_finale.c"
    "${RR_SOURCE_DIR}/src/sdl/i_video.cpp"
    "${RR_SOURCE_DIR}/src/i_tcp.c"
    "${RR_SOURCE_DIR}/src/i_tcp_detail.h"
    "${RR_SOURCE_DIR}/src/d_clisrv.c"
    "${RR_SOURCE_DIR}/src/g_game.c"
    "${RR_SOURCE_DIR}/src/r_debug.cpp"
    "${RR_SOURCE_DIR}/src/r_main.cpp"
    "${RR_SOURCE_DIR}/src/r_fps.cpp"
    "${RR_SOURCE_DIR}/src/v_video.cpp"
    "${RR_SOURCE_DIR}/src/hardware/hw_draw.c"
    "${RR_SOURCE_DIR}/src/hardware/hw_cache.c"
    "${RR_SOURCE_DIR}/src/hardware/hw_batching.c"
    "${RR_SOURCE_DIR}/src/hardware/r_d3d12/r_d3d12.cpp"
    "${RR_SOURCE_DIR}/src/hardware/r_d3d12/d3d12_pipeline.h"
    "${RR_SOURCE_DIR}/src/hardware/r_d3d12/d3d12_screens.inc"
    "${RR_SOURCE_DIR}/src/hardware/r_d3d12/d3d12_captures.inc"
    "${RR_SOURCE_DIR}/src/menus/options-1.c"
    "${RR_SOURCE_DIR}/src/d_main.cpp"
    "${RR_SOURCE_DIR}/src/d_frame_schedule.h"
    "${RR_SOURCE_DIR}/src/hardware/hw_main.c"
    "${RR_SOURCE_DIR}/src/r_bsp.cpp"
    "${RR_SOURCE_DIR}/src/r_sky.c"
    "${RR_SOURCE_DIR}/src/r_plane.cpp"
    "${RR_SOURCE_DIR}/src/r_textures.cpp"
    "${RR_SOURCE_DIR}/src/p_slopes.c"
    "${RR_SOURCE_DIR}/src/taglist.c"
    "${RR_SOURCE_DIR}/src/tables.c"
    "${RR_SOURCE_DIR}/src/t_fsin.c"
    "${RR_SOURCE_DIR}/src/hardware/hw_clip.c"
    "${RR_SOURCE_DIR}/src/m_fixed.c"
    "${RR_SOURCE_DIR}/src/r_things.cpp"
    "${RR_SOURCE_DIR}/src/r_patchrotation.c"
    "${RR_SOURCE_DIR}/src/r_patch.cpp"
    "${RR_SOURCE_DIR}/src/r_picformats.c"
    "${RR_SOURCE_DIR}/src/p_mobj.c"
    "${RR_SOURCE_DIR}/src/p_user.c"
    "${RR_SOURCE_DIR}/src/hardware/r_d3d12/shaders.hlsl")
execute_process(
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/generate.py"
        --engine "${RR_SOURCE_DIR}" --output "${CMAKE_CURRENT_BINARY_DIR}/generated"
    RESULT_VARIABLE generated)
if(NOT generated EQUAL 0)
    message(FATAL_ERROR "Could not extract production implementations for regression tests")
endif()

file(GLOB bundled_includes LIST_DIRECTORIES true "${RR_SOURCE_DIR}/thirdparty/*/include")
function(regression name source)
    add_executable(${name} "${CMAKE_CURRENT_BINARY_DIR}/generated/${source}.cpp")
    target_compile_features(${name} PRIVATE cxx_std_17)
    target_compile_definitions(${name} PRIVATE NONX86 NORUSEASM ${ARGN})
    if("_UWP" IN_LIST ARGN)
        # UWP builds have the Direct3D 12 driver (SRB2_CONFIG_D3D12) by default
        target_compile_definitions(${name} PRIVATE HAVE_D3D12)
    endif()
    target_include_directories(${name} SYSTEM PRIVATE "${RR_SOURCE_DIR}/src" ${bundled_includes})
    # Release builds must still execute every assertion. Do not recover from UB.
    target_compile_options(${name} PRIVATE -UNDEBUG -Wall -Wextra -Werror -Wno-unused-function
        -ffp-contract=off)
    if(RR_SANITIZERS)
        target_compile_options(${name} PRIVATE -fsanitize=address,undefined
            -fno-sanitize-recover=all -fno-omit-frame-pointer)
        target_link_options(${name} PRIVATE -fsanitize=address,undefined)
    endif()
    add_test(NAME ${name} COMMAND ${name})
    set_tests_properties(${name} PROPERTIES TIMEOUT 60
        ENVIRONMENT "UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1")
endfunction()

regression(overlay_blend overlay_blend)
regression(split_projection split_projection)
regression(intro_uwp intro _UWP)
regression(intro_desktop intro)
regression(startup_gamepads startup_gamepads _UWP)

regression(udp_send_posix udp_send)
regression(udp_send_winsock udp_send TEST_WINSOCK)

regression(online_uwp_hw online _UWP HWRENDER)
regression(online_desktop_hw online HWRENDER)
regression(menu_uwp menu _UWP)
regression(menu_desktop menu)
regression(trace_uwp_hw trace _UWP HWRENDER)
regression(wipe_math wipe)
regression(view_interpolation view_interp)
regression(point_subsector_cache point_subsector _UWP)
regression(picture_formats pictures HWRENDER)
regression(fill_alpha_uwp fills _UWP HWRENDER)
regression(ripples_uwp ripples _UWP HWRENDER)
regression(split_distortion distortion _UWP HWRENDER)
regression(postimg_math postimg)
regression(postimg_selection postimg_selection)
regression(water_math water _UWP HWRENDER)
regression(water_visibility water_visibility HWRENDER)
regression(fog_boundaries fog HWRENDER _UWP)
regression(fog_capture_d3d12 fog_capture_d3d12 _UWP)
regression(water_capture_d3d12 water_capture_d3d12 _UWP)
regression(sky_offsets sky)
regression(sky_claim sky_claim _UWP HWRENDER)
regression(midtexture_sky midtexture_sky _UWP HWRENDER)
regression(midtexture_fog midtexture_fog HWRENDER)
regression(level_darkness level_darkness)
regression(front_bbox_uwp front_bbox _UWP HWRENDER)
regression(front_bbox_desktop front_bbox HWRENDER)
regression(batch_sort_keys batch_sort HWRENDER)
regression(horizon_visibility horizon HWRENDER)
regression(void_wall void_wall _UWP HWRENDER)
regression(flat_bleed flat_bleed _UWP HWRENDER)
regression(sprite_brightmap sprite_brightmap _UWP HWRENDER HWPRECIP)
regression(encore_brightmap encore_brightmap _UWP HWRENDER)
regression(spring_clip spring_clip _UWP HWRENDER)
regression(title_lightlist title_lightlist _UWP HWRENDER)
if(RR_SANITIZERS)
    target_compile_options(title_lightlist PRIVATE
        "-fsanitize-ignorelist=${CMAKE_CURRENT_SOURCE_DIR}/title-shifts.ignorelist")
endif()
regression(wall_light_slices wall_light _UWP HWRENDER)
regression(custom_shader_dispatch custom_dispatch _UWP HWRENDER)
regression(transparency_uwp transparency _UWP HWRENDER HWPRECIP)
regression(transparency_desktop transparency HWRENDER HWPRECIP)
target_compile_options(transparency_uwp PRIVATE -Wno-missing-field-initializers)
target_compile_options(transparency_desktop PRIVATE -Wno-missing-field-initializers)
add_test(NAME source_extraction
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/test_source.py")
set_tests_properties(source_extraction PROPERTIES TIMEOUT 10)

include("${RR_SOURCE_DIR}/src/hardware/r_d3d12/custom_shaders.cmake")
regression(custom_shader_pk3 custom_pk3 HWRENDER _UWP)
target_sources(custom_shader_pk3 PRIVATE "${RR_SOURCE_DIR}/src/hardware/r_d3d12/custom_shader.cpp")
target_link_libraries(custom_shader_pk3 PRIVATE glslang SPIRV glslang-default-resource-limits spirv-cross-hlsl)
# Match the general translation suite's artifact directory for Windows fxc.
target_compile_definitions(custom_shader_pk3 PRIVATE SMOKE_OUTPUT="${CMAKE_CURRENT_BINARY_DIR}/translated-shaders")
add_executable(custom_shader_translation custom-shaders.cpp
    "${RR_SOURCE_DIR}/src/hardware/r_d3d12/custom_shader.cpp")
target_compile_features(custom_shader_translation PRIVATE cxx_std_17)
target_include_directories(custom_shader_translation PRIVATE "${RR_SOURCE_DIR}/src")
target_link_libraries(custom_shader_translation PRIVATE glslang SPIRV glslang-default-resource-limits spirv-cross-hlsl)
target_compile_options(custom_shader_translation PRIVATE -UNDEBUG -Wall -Wextra -Werror)
if(RR_SANITIZERS)
    target_compile_options(custom_shader_translation PRIVATE -fsanitize=address,undefined -fno-sanitize-recover=all)
    target_link_options(custom_shader_translation PRIVATE -fsanitize=address,undefined)
endif()
add_test(NAME custom_shader_translation COMMAND custom_shader_translation "${CMAKE_CURRENT_BINARY_DIR}/translated-shaders")
set_tests_properties(custom_shader_translation PROPERTIES TIMEOUT 60)

# Compile GPU-independent D3D12 bookkeeping directly from the production headers.
add_executable(d3d12_allocators d3d12-allocators.cpp)
target_compile_features(d3d12_allocators PRIVATE cxx_std_17)
target_include_directories(d3d12_allocators PRIVATE "${RR_SOURCE_DIR}/src")
target_compile_options(d3d12_allocators PRIVATE -UNDEBUG -Wall -Wextra -Werror)
if(RR_SANITIZERS)
    target_compile_options(d3d12_allocators PRIVATE -fsanitize=address,undefined
        -fno-sanitize-recover=all -fno-omit-frame-pointer)
    target_link_options(d3d12_allocators PRIVATE -fsanitize=address,undefined)
endif()
add_test(NAME d3d12_allocators COMMAND d3d12_allocators)
set_tests_properties(d3d12_allocators PROPERTIES TIMEOUT 60
    ENVIRONMENT "UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1")

# The placement Screen Scaling's renderers share is a header of its own, with no dependencies.
add_executable(screen_scaling screen-scaling.cpp)
target_compile_features(screen_scaling PRIVATE cxx_std_17)
target_include_directories(screen_scaling PRIVATE "${RR_SOURCE_DIR}/src")
target_compile_options(screen_scaling PRIVATE -UNDEBUG -Wall -Wextra -Werror)
if(RR_SANITIZERS)
    target_compile_options(screen_scaling PRIVATE -fsanitize=address,undefined
        -fno-sanitize-recover=all -fno-omit-frame-pointer)
    target_link_options(screen_scaling PRIVATE -fsanitize=address,undefined)
endif()
add_test(NAME screen_scaling COMMAND screen_scaling)
set_tests_properties(screen_scaling PROPERTIES TIMEOUT 60
    ENVIRONMENT "UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1")

# The startup map preview cache's file format and lookups are a header of their own, with no dependencies.
add_executable(map_preview_cache map-preview-cache.cpp)
target_compile_features(map_preview_cache PRIVATE cxx_std_17)
target_include_directories(map_preview_cache PRIVATE "${RR_SOURCE_DIR}/src")
target_compile_options(map_preview_cache PRIVATE -UNDEBUG -Wall -Wextra -Werror)
if(RR_SANITIZERS)
    target_compile_options(map_preview_cache PRIVATE -fsanitize=address,undefined
        -fno-sanitize-recover=all -fno-omit-frame-pointer)
    target_link_options(map_preview_cache PRIVATE -fsanitize=address,undefined)
endif()
add_test(NAME map_preview_cache COMMAND map_preview_cache)
set_tests_properties(map_preview_cache PROPERTIES TIMEOUT 60
    ENVIRONMENT "UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1")

# The startup lump header cache's file format and lookups are a header of their own, with no dependencies.
add_executable(lump_header_cache lump-header-cache.cpp)
target_compile_features(lump_header_cache PRIVATE cxx_std_17)
target_include_directories(lump_header_cache PRIVATE "${RR_SOURCE_DIR}/src")
target_compile_options(lump_header_cache PRIVATE -UNDEBUG -Wall -Wextra -Werror)
if(RR_SANITIZERS)
    target_compile_options(lump_header_cache PRIVATE -fsanitize=address,undefined
        -fno-sanitize-recover=all -fno-omit-frame-pointer)
    target_link_options(lump_header_cache PRIVATE -fsanitize=address,undefined)
endif()
add_test(NAME lump_header_cache COMMAND lump_header_cache)
set_tests_properties(lump_header_cache PROPERTIES TIMEOUT 60
    ENVIRONMENT "UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1")

# The record beside a copy of a game data file and the rule for using the copy are a header of their own, with no dependencies.
add_executable(uwp_data_copy uwp-data-copy.cpp)
target_compile_features(uwp_data_copy PRIVATE cxx_std_17)
target_include_directories(uwp_data_copy PRIVATE "${RR_SOURCE_DIR}/src")
target_compile_options(uwp_data_copy PRIVATE -UNDEBUG -Wall -Wextra -Werror)
if(RR_SANITIZERS)
    target_compile_options(uwp_data_copy PRIVATE -fsanitize=address,undefined
        -fno-sanitize-recover=all -fno-omit-frame-pointer)
    target_link_options(uwp_data_copy PRIVATE -fsanitize=address,undefined)
endif()
add_test(NAME uwp_data_copy COMMAND uwp_data_copy)
set_tests_properties(uwp_data_copy PROPERTIES TIMEOUT 60
    ENVIRONMENT "UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1")

# The hash indexes behind the startup name lookups are a header of their own, with no dependencies.
add_executable(name_index name-index.cpp)
target_compile_features(name_index PRIVATE cxx_std_17)
target_include_directories(name_index PRIVATE "${RR_SOURCE_DIR}/src")
target_compile_options(name_index PRIVATE -UNDEBUG -Wall -Wextra -Werror)
if(RR_SANITIZERS)
    target_compile_options(name_index PRIVATE -fsanitize=address,undefined
        -fno-sanitize-recover=all -fno-omit-frame-pointer)
    target_link_options(name_index PRIVATE -fsanitize=address,undefined)
endif()
add_test(NAME name_index COMMAND name_index)
set_tests_properties(name_index PROPERTIES TIMEOUT 60
    ENVIRONMENT "UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1")

regression(frame_schedule frame_schedule _UWP)

regression(horizon_cache horizon_cache _UWP HWRENDER)
