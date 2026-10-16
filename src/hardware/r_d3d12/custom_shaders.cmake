# Pinned source dependencies for UWP's runtime Legacy GL shader translation.
include(FetchContent)
set(ENABLE_GLSLANG_BINARIES OFF CACHE BOOL "" FORCE)
set(GLSLANG_TESTS OFF CACHE BOOL "" FORCE)
set(ENABLE_RTTI ON CACHE BOOL "" FORCE)
set(ENABLE_SPVREMAPPER OFF CACHE BOOL "" FORCE)
set(ENABLE_HLSL OFF CACHE BOOL "" FORCE)
set(ENABLE_OPT OFF CACHE BOOL "" FORCE)
set(BUILD_EXTERNAL OFF CACHE BOOL "" FORCE)
set(SKIP_GLSLANG_INSTALL ON CACHE BOOL "" FORCE)
set(SPIRV_CROSS_CLI OFF CACHE BOOL "" FORCE)
set(SPIRV_CROSS_ENABLE_TESTS OFF CACHE BOOL "" FORCE)
set(SPIRV_CROSS_ENABLE_HLSL ON CACHE BOOL "" FORCE)
set(SPIRV_CROSS_ENABLE_GLSL ON CACHE BOOL "" FORCE)
set(SPIRV_CROSS_ENABLE_MSL OFF CACHE BOOL "" FORCE)
set(SPIRV_CROSS_ENABLE_CPP OFF CACHE BOOL "" FORCE)
set(SPIRV_CROSS_ENABLE_REFLECT OFF CACHE BOOL "" FORCE)
set(SPIRV_CROSS_ENABLE_C_API OFF CACHE BOOL "" FORCE)
FetchContent_Declare(rr_glslang
    GIT_REPOSITORY https://github.com/KhronosGroup/glslang.git
    GIT_TAG 1062752a891c95b2bfeed9e356562d88f9df84ac)
FetchContent_Declare(rr_spirv_cross
    GIT_REPOSITORY https://github.com/KhronosGroup/SPIRV-Cross.git
    GIT_TAG 2275d0efc4f2fa46851035d9d3c67c105bc8b99e)
FetchContent_MakeAvailable(rr_glslang rr_spirv_cross)
# The launcher is configured separately and links these engine-build archives.
foreach(shader_library glslang MachineIndependent GenericCodeGen OSDependent SPIRV
        glslang-default-resource-limits spirv-cross-core spirv-cross-glsl spirv-cross-hlsl)
    if(TARGET ${shader_library})
        set_target_properties(${shader_library} PROPERTIES
            ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/legacy-shader-libs")
    endif()
endforeach()

# Ship the upstream license texts and concrete copyright notices in the app.
# SPIRV-Cross offers Apache-2.0 OR MIT; this GPL-2.0 build uses the MIT option.
function(shader_notices name source)
    file(GLOB_RECURSE notice_sources "${source}/glslang/*.cpp" "${source}/glslang/*.h"
        "${source}/SPIRV/*.cpp" "${source}/SPIRV/*.h")
    file(GLOB top_sources "${source}/*.cpp" "${source}/*.hpp" "${source}/*.h")
    list(APPEND notice_sources ${top_sources})
    set(copyrights "")
    foreach(input IN LISTS notice_sources)
        file(STRINGS "${input}" lines REGEX "[Cc]opyright")
        list(APPEND copyrights ${lines})
    endforeach()
    list(REMOVE_DUPLICATES copyrights)
    list(JOIN copyrights "\n" notices)
    file(WRITE "${CMAKE_BINARY_DIR}/legacy-shader-licenses/${name}.txt" "${notices}\n\n")
endfunction()
shader_notices(glslang "${rr_glslang_SOURCE_DIR}")
file(READ "${rr_glslang_SOURCE_DIR}/LICENSE.txt" glslang_license)
file(APPEND "${CMAKE_BINARY_DIR}/legacy-shader-licenses/glslang.txt" "${glslang_license}")
shader_notices(SPIRV-Cross "${rr_spirv_cross_SOURCE_DIR}")
file(READ "${rr_spirv_cross_SOURCE_DIR}/LICENSES/MIT.txt" cross_license)
file(READ "${rr_spirv_cross_SOURCE_DIR}/LICENSES/LicenseRef-KhronosFreeUse.txt" khronos_license)
file(APPEND "${CMAKE_BINARY_DIR}/legacy-shader-licenses/SPIRV-Cross.txt"
    "SPIRV-Cross: MIT license option.\n${cross_license}\nSPIR-V headers:\n${khronos_license}")
