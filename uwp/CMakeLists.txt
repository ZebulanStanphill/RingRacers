cmake_minimum_required(VERSION 3.14)

# Setup UWP platform
set(CMAKE_SYSTEM_NAME WindowsStore)
set(CMAKE_SYSTEM_VERSION 10.0.19041.0)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(NOT CMAKE_BUILD_TYPE)
	set(CMAKE_BUILD_TYPE "Release" CACHE STRING "Build type (Debug, Release, RelWithDebInfo, MinSizeRel)" FORCE)
endif()

project(ringracers-uwp LANGUAGES CXX)
set_directory_properties(PROPERTIES VS_STARTUP_PROJECT ${PROJECT_NAME})

# Build directory of the patched Ring Racers static lib (ninja-x64_windows_uwp_vcpkg-release preset)
set(RR_DIR "" CACHE PATH "Ring Racers UWP static lib build directory")
if(NOT RR_DIR)
	message(FATAL_ERROR "RR_DIR must be set to the Ring Racers UWP static lib build directory")
endif()
if(NOT EXISTS "${RR_DIR}/src/ringracers.lib")
	message(FATAL_ERROR "${RR_DIR}/src/ringracers.lib not found; build Ring Racers with the ninja-x64_windows_uwp_vcpkg-release preset first")
endif()

set(UWP_DEPS "${RR_DIR}/_deps/uwplibs-src/x64")

# SDL2 for UWP built with OpenGL ES through ANGLE, and the ANGLE.WindowsStore package (both from build.ps1)
set(SDL_DIR "" CACHE PATH "SDL2 UWP build output directory (SDL2.dll and SDL2.lib)")
set(ANGLE_DIR "" CACHE PATH "Extracted ANGLE.WindowsStore package")
if(NOT EXISTS "${SDL_DIR}/SDL2.lib")
	message(FATAL_ERROR "SDL_DIR must point to the SDL2 UWP build output; build.ps1 builds it")
endif()
if(NOT EXISTS "${ANGLE_DIR}/bin/UAP/x64/libGLESv2.dll")
	message(FATAL_ERROR "ANGLE_DIR must point to the extracted ANGLE.WindowsStore package; build.ps1 downloads it")
endif()

# The package version is Ring Racers' version (the first line of src/version.h, "2.4" or "2.4.1") padded to three parts,
# then RR_PACKAGE_REVISION: the Build workflow's run number (build.ps1 passes GITHUB_RUN_NUMBER), 0 for a local build.
# Each part is a whole number up to 65535, which is all a package version allows.
set(RR_PACKAGE_REVISION "0" CACHE STRING "Fourth part of the package version")
set(RR_VERSION_HEADER "${CMAKE_CURRENT_SOURCE_DIR}/../src/version.h")
# Configure again when the version changes
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${RR_VERSION_HEADER}")
file(STRINGS "${RR_VERSION_HEADER}" _rr_version_line LIMIT_COUNT 1)
if(NOT _rr_version_line MATCHES "^#define SRB2VERSION \"([^\"]*)\"")
	message(FATAL_ERROR "The first line of ${RR_VERSION_HEADER} isn't #define SRB2VERSION \"...\": ${_rr_version_line}")
endif()
set(RR_SRB2VERSION "${CMAKE_MATCH_1}")
string(REPLACE "." ";" _rr_version_parts "${RR_SRB2VERSION}")
list(LENGTH _rr_version_parts _rr_version_count)
if(_rr_version_count LESS 1 OR _rr_version_count GREATER 3)
	message(FATAL_ERROR "SRB2VERSION \"${RR_SRB2VERSION}\" must have one to three parts to make a package version, which has four with the revision")
endif()
while(_rr_version_count LESS 3)
	list(APPEND _rr_version_parts 0)
	math(EXPR _rr_version_count "${_rr_version_count} + 1")
endwhile()
list(APPEND _rr_version_parts "${RR_PACKAGE_REVISION}")
foreach(_rr_part IN LISTS _rr_version_parts)
	if(NOT _rr_part MATCHES "^(0|[1-9][0-9]?[0-9]?[0-9]?[0-9]?)$" OR _rr_part GREATER 65535)
		message(FATAL_ERROR "Package version part \"${_rr_part}\" (from SRB2VERSION \"${RR_SRB2VERSION}\" and RR_PACKAGE_REVISION \"${RR_PACKAGE_REVISION}\") isn't a whole number from 0 to 65535")
	endif()
endforeach()
list(JOIN _rr_version_parts "." RR_PACKAGE_VERSION)
message(STATUS "Package version: ${RR_PACKAGE_VERSION}")

# The Visual Studio generator takes a source with the .appxmanifest extension as the package manifest, so the
# configured copy keeps it
configure_file(Package.appxmanifest.in "${CMAKE_CURRENT_BINARY_DIR}/Package.appxmanifest" @ONLY)

add_executable(${PROJECT_NAME} WIN32
	main.cpp
	"${CMAKE_CURRENT_BINARY_DIR}/Package.appxmanifest"
)

# libuwp from worleydl/uwp-dep (fetched by the Ring Racers build), SDL2, and ANGLE
set(BinDeps
	${UWP_DEPS}/bin/libuwp.dll
	${SDL_DIR}/SDL2.dll
	${ANGLE_DIR}/bin/UAP/x64/libEGL.dll
	${ANGLE_DIR}/bin/UAP/x64/libGLESv2.dll
)
source_group("BinDeps" FILES ${BinDeps})
set_source_files_properties(${BinDeps} PROPERTIES
	VS_COPY_TO_OUT_DIR Always
	VS_DEPLOYMENT_CONTENT TRUE
	VS_DEPLOYMENT_LOCATION "."
)

set(PkgAssets
	Assets/LockScreenLogo.scale-200.png
	Assets/Square44x44Logo.targetsize-24_altform-unplated.png
	Assets/Square44x44Logo.scale-200.png
	Assets/Square150x150Logo.scale-200.png
	Assets/StoreLogo.png
)
source_group("PkgAssets" FILES ${PkgAssets})
set_source_files_properties(${PkgAssets} PROPERTIES
	VS_DEPLOYMENT_CONTENT TRUE
	VS_DEPLOYMENT_LOCATION "Assets"
)

set(ShaderLicenses
    "${RR_DIR}/legacy-shader-licenses/glslang.txt"
    "${RR_DIR}/legacy-shader-licenses/SPIRV-Cross.txt")
set_source_files_properties(${ShaderLicenses} PROPERTIES
    VS_COPY_TO_OUT_DIR Always
    VS_DEPLOYMENT_CONTENT 1 VS_DEPLOYMENT_LOCATION "Licenses")

target_sources(${PROJECT_NAME} PRIVATE
	${BinDeps}
	${PkgAssets}
	${ShaderLicenses}
)

target_include_directories(${PROJECT_NAME} PRIVATE
	${UWP_DEPS}/include
)

set(VCPKG_LIB "${RR_DIR}/vcpkg_installed/x64-windows-static-md/lib")
file(GLOB LegacyShaderLibs "${RR_DIR}/legacy-shader-libs/*.lib")
if(NOT LegacyShaderLibs)
	message(FATAL_ERROR "Runtime shader translator archives missing; rebuild the patched engine")
endif()
set(BinLibs
	${LegacyShaderLibs}
	d3dcompiler.lib
	d3d12.lib
	dxgi.lib
	${RR_DIR}/src/ringracers.lib

	${RR_DIR}/src/acs/vm/ACSVM/acsvm.lib
	${RR_DIR}/thirdparty/discord-rpc/discord-rpc.lib
	${RR_DIR}/thirdparty/glad/glad.lib
	${RR_DIR}/thirdparty/imgui/imgui.lib
	${RR_DIR}/thirdparty/libwebm/libwebm_mkvmuxer.lib
	${RR_DIR}/thirdparty/renamenoise/renamenoise.lib
	${RR_DIR}/thirdparty/stb/Stb.lib
	${RR_DIR}/thirdparty/tracy/TracyClient.lib
	${RR_DIR}/thirdparty/vma/VulkanMemoryAllocator.lib
	${RR_DIR}/thirdparty/xmp-lite/xmp-lite.lib

	# vcpkg
	${VCPKG_LIB}/jpeg.lib
	${VCPKG_LIB}/libcurl.lib
	${VCPKG_LIB}/libpng16.lib
	${VCPKG_LIB}/ogg.lib
	${VCPKG_LIB}/opus.lib
	${VCPKG_LIB}/turbojpeg.lib
	${VCPKG_LIB}/vorbis.lib
	${VCPKG_LIB}/vorbisenc.lib
	${VCPKG_LIB}/vorbisfile.lib
	${VCPKG_LIB}/vpx.lib
	${VCPKG_LIB}/yuv.lib
	${VCPKG_LIB}/zlib.lib

	# UWP specific stuff
	${UWP_DEPS}/lib/libuwp.lib
	${SDL_DIR}/SDL2.lib

	# Sys libs
	WS2_32.lib
	OneCore.lib
	WindowsApp.lib
)

# The engine is compiled with clang-cl, while this executable uses MSVC link.exe.
# Resolve compiler-rt from that exact compiler, rather than another LLVM on PATH.
option(RR_PGO_GENERATE "Link the engine's instrumentation runtime" OFF)
set(RR_PGO_CLANG_CL "" CACHE FILEPATH "clang-cl used to build the engine")
if(RR_PGO_GENERATE)
	if(NOT EXISTS "${RR_PGO_CLANG_CL}")
		message(FATAL_ERROR "PGO generation requires RR_PGO_CLANG_CL (build.ps1 sets it)")
	endif()
	execute_process(COMMAND "${RR_PGO_CLANG_CL}" /clang:-print-resource-dir
		OUTPUT_VARIABLE _clang_resources OUTPUT_STRIP_TRAILING_WHITESPACE
		RESULT_VARIABLE _clang_result)
	if(NOT _clang_result EQUAL 0)
		message(FATAL_ERROR "Cannot query the engine clang-cl resource directory")
	endif()
	file(GLOB_RECURSE _profile_runtimes "${_clang_resources}/lib/*clang_rt.profile-x86_64.lib")
	list(LENGTH _profile_runtimes _runtime_count)
	if(NOT _runtime_count EQUAL 1)
		message(FATAL_ERROR "Expected one clang_rt.profile-x86_64.lib under ${_clang_resources}/lib; install compiler-rt for the engine's clang-cl")
	endif()
	message(STATUS "PGO runtime: ${_profile_runtimes}")
	list(APPEND BinLibs ${_profile_runtimes})
	# LLVM ships the runtime built against the static CRT, so it asks for libcmt.lib; its CRT calls resolve
	# against the app CRT this executable already links.
	target_link_options(${PROJECT_NAME} PRIVATE /NODEFAULTLIB:libcmt.lib)
endif()

target_link_libraries(${PROJECT_NAME} PRIVATE ${BinLibs})
set_target_properties(${PROJECT_NAME} PROPERTIES VS_WINDOWS_TARGET_PLATFORM_MIN_VERSION 10.0.19041.0)

# Write ringracers-uwp.map next to the exe, which maps the addresses in the profile the game writes while playing
# (E:\ringracers\uwp-profile.txt) to functions
target_link_options(${PROJECT_NAME} PRIVATE /MAP)
