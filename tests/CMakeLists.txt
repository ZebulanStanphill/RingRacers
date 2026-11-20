cmake_minimum_required(VERSION 3.24)
project(ringracers_regressions LANGUAGES CXX)

enable_testing()
find_package(Python3 3.9 REQUIRED COMPONENTS Interpreter)
set(RR_SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/.." CACHE PATH "Ring Racers source checkout to test")
option(RR_SANITIZERS "Run with AddressSanitizer and UndefinedBehaviorSanitizer" ON)

if(NOT EXISTS "${RR_SOURCE_DIR}/src/hardware/r_viberant/shaders.hlsl")
    message(FATAL_ERROR "Set RR_SOURCE_DIR to a Ring Racers checkout with the VibeRant driver; see tests/README.md")
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
    ${templates} fixtures/sprite-rotation-baseline.c fixtures/spring-clip-reference.inc source.py generate.py title_replay.py title_fixture.py title-shifts.ignorelist fixtures/title-map.json fixtures/title-znodes.bin fixtures/rhi_glsl_fragment_postimg.glsl fixtures/water-palace-horizons.json fixtures/water-palace-water.json
    fixtures/block-lines-baseline.c fixtures/batch-sort-baseline.c fixtures/pathfind-baseline.c
    fixtures/thing-check-order-baseline.inc fixtures/thing-walk-baseline.c
    "${RR_SOURCE_DIR}/src/k_pathfind.c"
    "${RR_SOURCE_DIR}/src/k_bheap.c"
    "${RR_SOURCE_DIR}/src/p_map.c"
    "${RR_SOURCE_DIR}/src/p_maputl.c"
    "${RR_SOURCE_DIR}/src/p_maputl.h"
    "${RR_SOURCE_DIR}/src/p_polyobj.c"
    "${RR_SOURCE_DIR}/src/r_state.h"
    "${RR_SOURCE_DIR}/src/f_finale.c"
    "${RR_SOURCE_DIR}/src/sdl/i_video.cpp"
    "${RR_SOURCE_DIR}/src/sdl/i_system.cpp"
    "${RR_SOURCE_DIR}/src/screen.c"
    "${RR_SOURCE_DIR}/src/cvars.cpp"
    "${RR_SOURCE_DIR}/src/i_tcp.c"
    "${RR_SOURCE_DIR}/src/i_tcp_detail.h"
    "${RR_SOURCE_DIR}/src/d_clisrv.c"
    "${RR_SOURCE_DIR}/src/g_game.c"
    "${RR_SOURCE_DIR}/src/r_debug.cpp"
    "${RR_SOURCE_DIR}/src/r_main.cpp"
    "${RR_SOURCE_DIR}/src/r_fps.cpp"
    "${RR_SOURCE_DIR}/src/v_video.cpp"
    "${RR_SOURCE_DIR}/src/hardware/hw_bsp.c"
    "${RR_SOURCE_DIR}/src/hardware/hw_glob.h"
    "${RR_SOURCE_DIR}/src/hardware/hw_draw.c"
    "${RR_SOURCE_DIR}/src/hardware/r_opengl/r_opengl.c"
    "${RR_SOURCE_DIR}/src/hardware/hw_cache.c"
    "${RR_SOURCE_DIR}/src/hardware/hw_batching.c"
    "${RR_SOURCE_DIR}/src/hardware/r_viberant/r_viberant.cpp"
    "${RR_SOURCE_DIR}/src/hardware/r_viberant/viberant_pipeline.h"
    "${RR_SOURCE_DIR}/src/hardware/r_viberant/viberant_screens.inc"
    "${RR_SOURCE_DIR}/src/hardware/r_viberant/viberant_captures.inc"
    "${RR_SOURCE_DIR}/src/menus/options-1.c"
    "${RR_SOURCE_DIR}/src/menus/options-video-1.c"
    "${RR_SOURCE_DIR}/src/menus/options-video-advanced.c"
    "${RR_SOURCE_DIR}/src/k_menu.h"
    "${RR_SOURCE_DIR}/src/d_main.cpp"
    "${RR_SOURCE_DIR}/src/d_frame_schedule.h"
    "${RR_SOURCE_DIR}/src/hardware/hw_main.c"
    "${RR_SOURCE_DIR}/src/p_spec.c"
    "${RR_SOURCE_DIR}/src/r_segs.cpp"
    "${RR_SOURCE_DIR}/src/r_bsp.cpp"
    "${RR_SOURCE_DIR}/src/r_sky.c"
    "${RR_SOURCE_DIR}/src/r_plane.cpp"
    "${RR_SOURCE_DIR}/src/r_textures.cpp"
    "${RR_SOURCE_DIR}/src/p_slopes.c"
    "${RR_SOURCE_DIR}/src/taglist.c"
    "${RR_SOURCE_DIR}/src/tables.c"
    "${RR_SOURCE_DIR}/src/t_fsin.c"
    "${RR_SOURCE_DIR}/src/hardware/hw_clip.c"
    "${RR_SOURCE_DIR}/src/doomdef.h"
    "${RR_SOURCE_DIR}/src/m_fixed.h"
    "${RR_SOURCE_DIR}/src/m_fixed.c"
    "${RR_SOURCE_DIR}/src/r_things.cpp"
    "${RR_SOURCE_DIR}/src/r_draw_column.cpp"
    "${RR_SOURCE_DIR}/src/r_patchrotation.c"
    "${RR_SOURCE_DIR}/src/r_patch.cpp"
    "${RR_SOURCE_DIR}/src/r_picformats.c"
    "${RR_SOURCE_DIR}/src/p_mobj.c"
    "${RR_SOURCE_DIR}/src/p_user.c"
    "${RR_SOURCE_DIR}/src/p_tick.c"
    "${RR_SOURCE_DIR}/src/hardware/r_viberant/shaders.hlsl")
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
        # UWP builds have the VibeRant driver, with its Direct3D 12 backend (SRB2_CONFIG_VIBERANT_D3D), by default
        target_compile_definitions(${name} PRIVATE HAVE_VIBERANT)
    endif()
    target_include_directories(${name} SYSTEM PRIVATE "${RR_SOURCE_DIR}/src" ${bundled_includes})
    # Production code asks HWR_VibeRant() which hardware driver draws: always the VibeRant one with _UWP, never without
    # HAVE_VIBERANT, and the setting hwr_viberant (defined by the engine's hw_driver.c) with only HAVE_VIBERANT.
    if("HAVE_VIBERANT" IN_LIST ARGN AND NOT "_UWP" IN_LIST ARGN)
        target_sources(${name} PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/hw-driver.cpp")
    endif()
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

regression(shared_brightmap_gl shared_brightmap_gl HWRENDER)
add_test(NAME shared_brightmap_gl_update COMMAND shared_brightmap_gl update)
regression(shared_brightmap_batches shared_brightmap HWRENDER)
# Separate invocations keep each collection entry point independently observable.
foreach(scenario texture flat patch)
    add_test(NAME shared_brightmap_${scenario} COMMAND shared_brightmap_batches ${scenario})
endforeach()
set_tests_properties(shared_brightmap_gl_update shared_brightmap_texture
    shared_brightmap_flat shared_brightmap_patch PROPERTIES TIMEOUT 60
    ENVIRONMENT "UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1")
regression(sprite_fade sprite_fade)
regression(sprite_height sprite_height)
regression(shadow_draw shadow_draw)
regression(drop_shadow_color drop_shadow_color)
regression(overlay_blend overlay_blend)
regression(split_projection split_projection)
regression(software_shear software_shear)
regression(renderer_setting renderer_setting HWRENDER)
regression(renderer_setting_uwp renderer_setting HWRENDER _UWP)
# Desktop builds with the VibeRant driver: Direct3D 12 only, Vulkan only (Linux), and both (Windows)
regression(renderer_setting_both renderer_setting HWRENDER HAVE_VIBERANT HAVE_VIBERANT_D3D)
regression(renderer_setting_vulkan renderer_setting HWRENDER HAVE_VIBERANT HAVE_VIBERANT_VULKAN)
regression(renderer_setting_metal renderer_setting HWRENDER HAVE_VIBERANT HAVE_VIBERANT_METAL)
regression(renderer_setting_all renderer_setting HWRENDER HAVE_VIBERANT HAVE_VIBERANT_D3D HAVE_VIBERANT_VULKAN)
# What Video Options > Advanced says about the renderer, in each kind of desktop build with the VibeRant driver
regression(renderer_description_both renderer_description HWRENDER HAVE_VIBERANT HAVE_VIBERANT_D3D)
regression(renderer_description_vulkan renderer_description HWRENDER HAVE_VIBERANT HAVE_VIBERANT_VULKAN)
regression(renderer_description_metal renderer_description HWRENDER HAVE_VIBERANT HAVE_VIBERANT_METAL)
regression(renderer_description_all renderer_description HWRENDER HAVE_VIBERANT HAVE_VIBERANT_D3D HAVE_VIBERANT_VULKAN)
regression(event_queue event_queue)
regression(quit_signal quit_signal)
regression(upright_wall upright_wall HWRENDER)
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
regression(pathfind pathfind)
regression(block_lines block_lines)
# Exercise invalid pointers through the production linedef box accessor.
regression(block_lines_paranoia block_lines PARANOIA TEST_LINE_PARANOIA)
regression(thing_check_order thing_check_order)
regression(thing_walk thing_walk)
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
regression(surface_light surface_light HWRENDER _UWP)
regression(semibright_light_uwp semibright_light HWRENDER _UWP)
regression(semibright_light_desktop semibright_light HWRENDER)
regression(semibright_light_both semibright_light HWRENDER HAVE_VIBERANT)
regression(fog_capture_viberant fog_capture_viberant _UWP)
regression(water_capture_viberant water_capture_viberant _UWP)
regression(sky_offsets sky)
regression(sky_claim sky_claim _UWP HWRENDER)
regression(sky_claim_both sky_claim HWRENDER HAVE_VIBERANT)
regression(midtexture_sky midtexture_sky _UWP HWRENDER)
regression(midtexture_fog midtexture_fog HWRENDER)
regression(level_darkness level_darkness)
regression(front_bbox_uwp front_bbox _UWP HWRENDER)
regression(front_bbox_desktop front_bbox HWRENDER)
regression(front_bbox_both front_bbox HWRENDER HAVE_VIBERANT)
regression(batch_sort_keys batch_sort HWRENDER)
regression(batch_sort_groups batch_sort_groups)
regression(horizon_visibility horizon HWRENDER)
regression(void_wall void_wall _UWP HWRENDER)
regression(flat_bleed flat_bleed _UWP HWRENDER)
regression(slope_light slope_light HWRENDER)
regression(wall_light_steps wall_light_steps HWRENDER)
regression(sprite_brightmap sprite_brightmap _UWP HWRENDER HWPRECIP)
regression(encore_brightmap encore_brightmap _UWP HWRENDER)
regression(spring_clip spring_clip _UWP HWRENDER)
regression(splat_clear splat_clear HWRENDER)
regression(title_lightlist title_lightlist _UWP HWRENDER)
if(RR_SANITIZERS)
    target_compile_options(title_lightlist PRIVATE
        "-fsanitize-ignorelist=${CMAKE_CURRENT_SOURCE_DIR}/title-shifts.ignorelist")
endif()
regression(wall_light_slices wall_light _UWP HWRENDER)
regression(translucent_wall_light translucent_wall_light _UWP HWRENDER)
regression(video_fullscreen video_fullscreen HAVE_SDL)
regression(video_fullscreen_uwp video_fullscreen HAVE_SDL _UWP)
regression(first_person_angle first_person_angle)
regression(camera_position camera_position)
regression(first_person_title first_person_title)
regression(custom_shader_dispatch custom_dispatch _UWP HWRENDER)
regression(transparency_uwp transparency _UWP HWRENDER HWPRECIP)
regression(transparency_desktop transparency HWRENDER HWPRECIP)
regression(transparency_both transparency HWRENDER HWPRECIP HAVE_VIBERANT)
target_compile_options(transparency_uwp PRIVATE -Wno-missing-field-initializers)
target_compile_options(transparency_desktop PRIVATE -Wno-missing-field-initializers)
target_compile_options(transparency_both PRIVATE -Wno-missing-field-initializers)
add_test(NAME source_extraction
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/test_source.py")
set_tests_properties(source_extraction PROPERTIES TIMEOUT 10)

include("${RR_SOURCE_DIR}/src/hardware/r_viberant/custom_shaders.cmake")
regression(custom_shader_pk3 custom_pk3 HWRENDER _UWP)
target_sources(custom_shader_pk3 PRIVATE "${RR_SOURCE_DIR}/src/hardware/r_viberant/custom_shader.cpp")
target_link_libraries(custom_shader_pk3 PRIVATE glslang SPIRV glslang-default-resource-limits spirv-cross-hlsl)
# Match the general translation suite's artifact directory for Windows fxc.
target_compile_definitions(custom_shader_pk3 PRIVATE SMOKE_OUTPUT="${CMAKE_CURRENT_BINARY_DIR}/translated-shaders")
add_executable(custom_shader_translation custom-shaders.cpp
    "${RR_SOURCE_DIR}/src/hardware/r_viberant/custom_shader.cpp")
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

# Compile GPU-independent VibeRant bookkeeping directly from the production headers.
add_executable(viberant_allocators viberant-allocators.cpp)
target_compile_features(viberant_allocators PRIVATE cxx_std_17)
target_include_directories(viberant_allocators PRIVATE "${RR_SOURCE_DIR}/src")
target_compile_options(viberant_allocators PRIVATE -UNDEBUG -Wall -Wextra -Werror)
if(RR_SANITIZERS)
    target_compile_options(viberant_allocators PRIVATE -fsanitize=address,undefined
        -fno-sanitize-recover=all -fno-omit-frame-pointer)
    target_link_options(viberant_allocators PRIVATE -fsanitize=address,undefined)
endif()
add_test(NAME viberant_allocators COMMAND viberant_allocators)
set_tests_properties(viberant_allocators PROPERTIES TIMEOUT 60
    ENVIRONMENT "UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1")

add_executable(viberant_texture_atlas viberant-texture-atlas.cpp)
target_compile_features(viberant_texture_atlas PRIVATE cxx_std_17)
target_include_directories(viberant_texture_atlas PRIVATE "${RR_SOURCE_DIR}/src")
target_compile_options(viberant_texture_atlas PRIVATE -UNDEBUG -Wall -Wextra -Werror)
if(RR_SANITIZERS)
    target_compile_options(viberant_texture_atlas PRIVATE -fsanitize=address,undefined
        -fno-sanitize-recover=all -fno-omit-frame-pointer)
    target_link_options(viberant_texture_atlas PRIVATE -fsanitize=address,undefined)
endif()
add_test(NAME viberant_texture_atlas COMMAND viberant_texture_atlas)
set_tests_properties(viberant_texture_atlas PROPERTIES TIMEOUT 60
    ENVIRONMENT "UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1")

# What VibeRant Metal's presenter does with a frame when its window may be out of sight: a header with no dependencies,
# since the presenter itself is Objective-C++ and only builds on macOS.
add_executable(metal_present_seen metal-present-seen.cpp)
target_compile_features(metal_present_seen PRIVATE cxx_std_17)
target_include_directories(metal_present_seen PRIVATE "${RR_SOURCE_DIR}/src")
target_compile_options(metal_present_seen PRIVATE -UNDEBUG -Wall -Wextra -Werror)
if(RR_SANITIZERS)
    target_compile_options(metal_present_seen PRIVATE -fsanitize=address,undefined
        -fno-sanitize-recover=all -fno-omit-frame-pointer)
    target_link_options(metal_present_seen PRIVATE -fsanitize=address,undefined)
endif()
add_test(NAME metal_present_seen COMMAND metal_present_seen)
set_tests_properties(metal_present_seen PROPERTIES TIMEOUT 60
    ENVIRONMENT "UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1")

# Whether a drawable holds the frame VibeRant Metal's presenter placed in it (-mtlcheck): the comparison is a header
# with no dependencies too.
add_executable(metal_present_check metal-present-check.cpp)
target_compile_features(metal_present_check PRIVATE cxx_std_17)
target_include_directories(metal_present_check PRIVATE "${RR_SOURCE_DIR}/src")
target_compile_options(metal_present_check PRIVATE -UNDEBUG -Wall -Wextra -Werror -O2)
if(RR_SANITIZERS)
    target_compile_options(metal_present_check PRIVATE -fsanitize=address,undefined
        -fno-sanitize-recover=all -fno-omit-frame-pointer)
    target_link_options(metal_present_check PRIVATE -fsanitize=address,undefined)
endif()
add_test(NAME metal_present_check COMMAND metal_present_check)
set_tests_properties(metal_present_check PROPERTIES TIMEOUT 60
    ENVIRONMENT "UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1")

# How tests/metal-frames/capture.py judges a run of the macOS game by its log, against logs made up from the
# presenter's own lines: no game is started.
add_test(NAME metal_capture_judging COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/metal-frames/test_capture.py")
set_tests_properties(metal_capture_judging PROPERTIES TIMEOUT 60)

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

# The frame capture mode's tic list and screenshot names are a header of their own, with no dependencies.
add_executable(frame_capture frame-capture.cpp)
target_compile_features(frame_capture PRIVATE cxx_std_17)
target_include_directories(frame_capture PRIVATE "${RR_SOURCE_DIR}/src")
target_compile_options(frame_capture PRIVATE -UNDEBUG -Wall -Wextra -Werror)
if(RR_SANITIZERS)
    target_compile_options(frame_capture PRIVATE -fsanitize=address,undefined
        -fno-sanitize-recover=all -fno-omit-frame-pointer)
    target_link_options(frame_capture PRIVATE -fsanitize=address,undefined)
endif()
add_test(NAME frame_capture COMMAND frame_capture)
set_tests_properties(frame_capture PROPERTIES TIMEOUT 60
    ENVIRONMENT "UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1")

# The fork's version text is a header of its own, with no dependencies beyond the C library.
add_executable(fork_version fork-version.cpp)
target_compile_features(fork_version PRIVATE cxx_std_17)
target_include_directories(fork_version PRIVATE "${RR_SOURCE_DIR}/src")
target_compile_options(fork_version PRIVATE -UNDEBUG -Wall -Wextra -Werror)
if(RR_SANITIZERS)
    target_compile_options(fork_version PRIVATE -fsanitize=address,undefined
        -fno-sanitize-recover=all -fno-omit-frame-pointer)
    target_link_options(fork_version PRIVATE -fsanitize=address,undefined)
endif()
add_test(NAME fork_version COMMAND fork_version)
set_tests_properties(fork_version PROPERTIES TIMEOUT 60
    ENVIRONMENT "UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1")

# Whether a build is a release of the fork, decided by cmake/Modules/ForkVersion.cmake in temporary git repositories.
find_package(Git)
if(GIT_FOUND)
    add_test(NAME fork_version_release COMMAND "${CMAKE_COMMAND}"
        -DGIT_EXECUTABLE=${GIT_EXECUTABLE} -DRR_SOURCE_DIR=${RR_SOURCE_DIR}
        -DWORK_DIR=${CMAKE_CURRENT_BINARY_DIR}/fork-version-release
        -P "${CMAKE_CURRENT_SOURCE_DIR}/fork-version-release.cmake")
    set_tests_properties(fork_version_release PROPERTIES TIMEOUT 120)
endif()

regression(frame_schedule frame_schedule _UWP)

regression(horizon_cache horizon_cache _UWP HWRENDER)
regression(horizon_cache_both horizon_cache HWRENDER HAVE_VIBERANT)

regression(plane_polygon_loops plane_polygon_loops HWRENDER)

# Image comparison tooling needs NumPy and Pillow; CI installs the pinned requirements.
execute_process(COMMAND "${Python3_EXECUTABLE}" -c "import numpy; import PIL"
    RESULT_VARIABLE map_sweep_python)
if(map_sweep_python EQUAL 0)
    add_test(NAME map_sweep COMMAND "${Python3_EXECUTABLE}" -m unittest discover
        -s "${CMAKE_CURRENT_SOURCE_DIR}/map-sweep" -p "test_*.py")
else()
    message(STATUS "Map sweep Python tests disabled: install tests/map-sweep/requirements.txt")
endif()
