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

add_executable(${PROJECT_NAME} WIN32
	main.cpp
	Package.appxmanifest
)

# SDL2 (UWP), Mesa, and libuwp from worleydl/uwp-dep, fetched by the Ring Racers build
set(BinDeps
	${UWP_DEPS}/bin/dxil.dll
	${UWP_DEPS}/bin/libgallium_wgl.dll
	${UWP_DEPS}/bin/libuwp.dll
	${UWP_DEPS}/bin/opengl32.dll
	${UWP_DEPS}/bin/SDL2.dll
	${UWP_DEPS}/bin/z-1.dll
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

target_sources(${PROJECT_NAME} PRIVATE
	${BinDeps}
	${PkgAssets}
)

target_include_directories(${PROJECT_NAME} PRIVATE
	${UWP_DEPS}/include
)

set(VCPKG_LIB "${RR_DIR}/vcpkg_installed/x64-windows-static-md/lib")
set(BinLibs
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
	${UWP_DEPS}/lib/SDL2.lib

	# Sys libs
	WS2_32.lib
	OneCore.lib
	WindowsApp.lib
)

target_link_libraries(${PROJECT_NAME} PRIVATE ${BinLibs})
set_target_properties(${PROJECT_NAME} PROPERTIES VS_WINDOWS_TARGET_PLATFORM_MIN_VERSION 10.0.19041.0)
