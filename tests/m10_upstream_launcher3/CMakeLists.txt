cmake_minimum_required(VERSION 3.24)

if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    project(Aosp17UpstreamLauncher3 LANGUAGES C CXX)
    enable_testing()
endif()

get_filename_component(wa_root "${CMAKE_CURRENT_LIST_DIR}/../.." ABSOLUTE)
set(AOSP17_SOURCE_DIR "${wa_root}/third_party/aosp17" CACHE PATH "Read-only AOSP 17 tree")
include("${wa_root}/cmake/Aosp17ArtInterp.cmake")
find_package(Python3 3.11 REQUIRED COMPONENTS Interpreter)

if(NOT "$ENV{ANDROID_SDK_ROOT}" STREQUAL "")
    set(sdk_default "$ENV{ANDROID_SDK_ROOT}")
elseif(NOT "$ENV{ANDROID_HOME}" STREQUAL "")
    set(sdk_default "$ENV{ANDROID_HOME}")
elseif(CMAKE_HOST_APPLE)
    set(sdk_default "$ENV{HOME}/Library/Android/sdk")
else()
    set(sdk_default "$ENV{HOME}/Android/Sdk")
endif()
set(WA_ANDROID_SDK "${sdk_default}" CACHE PATH "Android SDK directory")
set(build_tools "${WA_ANDROID_SDK}/build-tools/36.0.0")

set(WA_UPSTREAM_LAUNCHER3_APK "" CACHE FILEPATH "Use an existing upstream Launcher3 APK")
if(WA_UPSTREAM_LAUNCHER3_APK)
    set(apk "${WA_UPSTREAM_LAUNCHER3_APK}")
else()
    set(apk_dir "${CMAKE_CURRENT_BINARY_DIR}/apk")
    set(apk "${apk_dir}/Launcher3-upstream.apk")
    set(driver "${wa_root}/tools/aosp/build_upstream_launcher3.py")
    file(GLOB_RECURSE local_inputs CONFIGURE_DEPENDS "${wa_root}/patches/aosp17/launcher3/*")
    add_custom_command(
        OUTPUT "${apk}" "${apk_dir}/upstream_provenance.json"
        COMMAND "${Python3_EXECUTABLE}" "${driver}" --aosp-root "${AOSP17_SOURCE_DIR}"
            --sdk "${WA_ANDROID_SDK}" --out-dir "${apk_dir}" --depfile "${apk_dir}/launcher3.d"
        DEPENDS "${driver}" ${local_inputs}
        DEPFILE "${apk_dir}/launcher3.d"
        VERBATIM)
    add_custom_target(aosp17_upstream_launcher3_apk DEPENDS "${apk}")
endif()

add_executable(test_m10_upstream_launcher3 test_upstream_launcher3.cpp)
target_compile_features(test_m10_upstream_launcher3 PRIVATE cxx_std_23)
target_link_libraries(test_m10_upstream_launcher3 PRIVATE
    aosp17_art_interp aosp17_libandroidfw aosp17_libdexfile)
if(TARGET aosp17_upstream_launcher3_apk AND NOT WA_UPSTREAM_LAUNCHER3_APK)
    add_dependencies(test_m10_upstream_launcher3 aosp17_upstream_launcher3_apk)
endif()

if(EMSCRIPTEN)
    find_program(WA_NODE_EXECUTABLE node HINTS /opt/homebrew/bin NO_CMAKE_FIND_ROOT_PATH REQUIRED)
    execute_process(COMMAND "${WA_NODE_EXECUTABLE}" --version
        OUTPUT_VARIABLE node_version OUTPUT_STRIP_TRAILING_WHITESPACE
        COMMAND_ERROR_IS_FATAL ANY)
    if(NOT node_version MATCHES "^v26\\.")
        message(FATAL_ERROR "Set WA_NODE_EXECUTABLE to Node 26 (found ${node_version})")
    endif()
    set_target_properties(test_m10_upstream_launcher3 PROPERTIES SUFFIX ".mjs")
    target_compile_options(test_m10_upstream_launcher3 PRIVATE -fwasm-exceptions)
    target_link_options(test_m10_upstream_launcher3 PRIVATE -fwasm-exceptions
        -sMODULARIZE=1 -sEXPORT_ES6=1 -sENVIRONMENT=node -sINVOKE_RUN=0
        -sFORCE_FILESYSTEM=1 -sALLOW_MEMORY_GROWTH=1 -sSTACK_SIZE=1048576
        "-sEXPORTED_RUNTIME_METHODS=FS,callMain")
    set(run_verifier "${WA_NODE_EXECUTABLE}" "${CMAKE_CURRENT_LIST_DIR}/run_wasm.mjs"
        "$<TARGET_FILE:test_m10_upstream_launcher3>" "${apk}")
else()
    set(run_verifier "$<TARGET_FILE:test_m10_upstream_launcher3>" "${apk}")
endif()

add_test(NAME m10_upstream_launcher3_zipalign COMMAND "${build_tools}/zipalign" -c -P 16 4 "${apk}")
add_test(NAME m10_upstream_launcher3_signature COMMAND "${build_tools}/apksigner" verify "${apk}")
foreach(mode resources dex interpreter)
    add_test(NAME m10_upstream_launcher3_${mode} COMMAND ${run_verifier} ${mode})
endforeach()
