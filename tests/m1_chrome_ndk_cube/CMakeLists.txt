find_path(WA_GLES3_INCLUDE_DIR GLES3/gl3.h HINTS /opt/homebrew/opt/mesa PATH_SUFFIXES include
    REQUIRED)
find_path(WA_EGL_INCLUDE_DIR EGL/egl.h HINTS /opt/homebrew/opt/mesa PATH_SUFFIXES include REQUIRED)
find_library(WA_GLES3_LIBRARY NAMES GLESv2 GLESv3 HINTS /opt/homebrew/opt/mesa PATH_SUFFIXES lib
    REQUIRED)
find_library(WA_EGL_LIBRARY EGL HINTS /opt/homebrew/opt/mesa PATH_SUFFIXES lib REQUIRED)
target_compile_definitions(wa_hal_browser PRIVATE WA_HAL_NATIVE_GLES3=1)
target_include_directories(wa_hal_browser SYSTEM PRIVATE "${WA_GLES3_INCLUDE_DIR}")
target_link_libraries(wa_hal_browser PRIVATE "${WA_GLES3_LIBRARY}")

set(demo "${CMAKE_SOURCE_DIR}/demos/ndk_gles3_cube")
set(ndk_out "${CMAKE_BINARY_DIR}/ndk")
set(guests "${ndk_out}/libndk_gles3_cube_armv7a.so" "${ndk_out}/libndk_gles3_cube_arm64.so")
add_custom_command(OUTPUT ${guests}
    COMMAND "${CMAKE_COMMAND}" "-DDEMO=${demo}" "-DOUT=${ndk_out}"
            "-DNDK_BIN=${WA_ANDROID_NDK_BIN}" -P "${CMAKE_CURRENT_SOURCE_DIR}/build_ndk.cmake"
    DEPENDS "${demo}/build_ndk.sh" "${demo}/cube_demo.c" "${demo}/cube_demo.h"
            "${CMAKE_CURRENT_SOURCE_DIR}/build_ndk.cmake"
    VERBATIM)
add_custom_target(wa_m1_ndk_cube_guests DEPENDS ${guests})

add_executable(test_ndk_cube_host test_ndk_cube_host.cpp)
target_include_directories(test_ndk_cube_host SYSTEM PRIVATE "${WA_GLES3_INCLUDE_DIR}"
    "${WA_EGL_INCLUDE_DIR}")
target_link_libraries(test_ndk_cube_host PRIVATE wa_android_system "${WA_EGL_LIBRARY}"
    "${WA_GLES3_LIBRARY}")
add_dependencies(test_ndk_cube_host wa_m1_ndk_cube_guests)

set(units "${CMAKE_CURRENT_BINARY_DIR}/cube-armv7a.wasm" "${CMAKE_CURRENT_BINARY_DIR}/cube-arm64.wasm")
add_test(NAME test_ndk_cube_host COMMAND test_ndk_cube_host ${guests} ${units})
set_tests_properties(test_ndk_cube_host PROPERTIES FIXTURES_SETUP wa_cube_units TIMEOUT 60)
add_test(NAME test_ndk_cube_wasm COMMAND "${WA_NODE_EXECUTABLE}"
    "${CMAKE_CURRENT_SOURCE_DIR}/validate_cube_units.mjs" ${units})
set_tests_properties(test_ndk_cube_wasm PROPERTIES FIXTURES_REQUIRED wa_cube_units)
