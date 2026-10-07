cmake_minimum_required(VERSION 3.24)
project(ringracers_regressions LANGUAGES CXX)

enable_testing()
find_package(Python3 3.9 REQUIRED COMPONENTS Interpreter)
set(RR_SOURCE_DIR "" CACHE PATH "Patched Ring Racers v2.4 source checkout")
option(RR_SANITIZERS "Run with AddressSanitizer and UndefinedBehaviorSanitizer" ON)

if(NOT EXISTS "${RR_SOURCE_DIR}/src/hardware/r_d3d11/shaders.hlsl")
    message(FATAL_ERROR "Set RR_SOURCE_DIR to patched v2.4; see tests/README.md")
endif()
if(NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang" OR
   CMAKE_CXX_COMPILER_FRONTEND_VARIANT STREQUAL "MSVC")
    message(FATAL_ERROR "Use clang++ (Linux/macOS/WSL); the wipe harness uses Clang vector types")
endif()

# Reconfigure/regenerate when either a tested implementation or harness changes.
file(GLOB templates CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/templates/*.in")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
    ${templates} source.py generate.py
    "${RR_SOURCE_DIR}/src/d_clisrv.c"
    "${RR_SOURCE_DIR}/src/g_game.c"
    "${RR_SOURCE_DIR}/src/menus/options-1.c"
    "${RR_SOURCE_DIR}/src/d_main.cpp"
    "${RR_SOURCE_DIR}/src/hardware/hw_main.c"
    "${RR_SOURCE_DIR}/src/p_slopes.c"
    "${RR_SOURCE_DIR}/src/m_fixed.c"
    "${RR_SOURCE_DIR}/src/r_things.cpp"
    "${RR_SOURCE_DIR}/src/p_mobj.c"
    "${RR_SOURCE_DIR}/src/hardware/r_d3d11/shaders.hlsl")
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

regression(online_uwp_hw online _UWP HWRENDER)
regression(online_desktop_hw online HWRENDER)
regression(online_uwp_soft online _UWP)
regression(menu_uwp menu _UWP)
regression(menu_desktop menu)
regression(trace_uwp_hw trace _UWP HWRENDER)
regression(trace_uwp_soft trace _UWP)
regression(wipe_math wipe)
regression(transparency_uwp transparency _UWP HWRENDER HWPRECIP)
regression(transparency_desktop transparency HWRENDER HWPRECIP)
target_compile_options(transparency_uwp PRIVATE -Wno-missing-field-initializers)
target_compile_options(transparency_desktop PRIVATE -Wno-missing-field-initializers)
add_test(NAME source_extraction
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/test_source.py")
set_tests_properties(source_extraction PROPERTIES TIMEOUT 10)
