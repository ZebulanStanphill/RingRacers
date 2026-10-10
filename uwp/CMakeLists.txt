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

add_executable(${PROJECT_NAME} WIN32
	main.cpp
	Package.appxmanifest
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

target_link_libraries(${PROJECT_NAME} PRIVATE ${BinLibs})
set_target_properties(${PROJECT_NAME} PROPERTIES VS_WINDOWS_TARGET_PLATFORM_MIN_VERSION 10.0.19041.0)

# Write ringracers-uwp.map next to the exe, which maps the addresses in the profile the game writes while playing
# (E:\ringracers\uwp-profile.txt) to functions
target_link_options(${PROJECT_NAME} PRIVATE /MAP)
