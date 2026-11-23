cmake_minimum_required(VERSION 3.24)

if(CMAKE_CURRENT_SOURCE_DIR STREQUAL CMAKE_SOURCE_DIR)
    project(Aosp17UpstreamSettings LANGUAGES C CXX)
    enable_testing()
    option(AOSP17_ART_INTERP_HGRAPH "Enable optional ART HGraph promotion" OFF)
endif()

get_filename_component(wa_root "${CMAKE_CURRENT_LIST_DIR}/../.." ABSOLUTE)
include("${wa_root}/cmake/Aosp17UiFramework.cmake")
find_package(Python3 3.10 REQUIRED COMPONENTS Interpreter)
if(NOT TARGET wa_upstream_settings_apk)
    set(wa_settings_output "${CMAKE_CURRENT_BINARY_DIR}/apk")
    include("${wa_root}/runtime/android_system/UpstreamLauncher.cmake")
endif()

get_target_property(framework aosp17_framework_core WA_FRAMEWORK_DEX)
add_executable(test_m14_upstream_settings test_upstream_settings.cpp)
add_dependencies(test_m14_upstream_settings wa_upstream_settings_apk wa_foreground_framework_assets)
target_link_libraries(test_m14_upstream_settings PRIVATE aosp17_ui_jni aosp17_libandroidfw)
target_compile_features(test_m14_upstream_settings PRIVATE cxx_std_20)
if(TARGET wa_android_system_foreground)
    target_link_libraries(test_m14_upstream_settings PRIVATE wa_android_system_foreground)
    if(TARGET wa_android_system)
        target_link_libraries(test_m14_upstream_settings PRIVATE wa_android_system)
    endif()
    target_compile_definitions(test_m14_upstream_settings PRIVATE WA_TEST_FOREGROUND_ABI)
endif()

if(EMSCRIPTEN)
    find_program(WA_SETTINGS_NODE node HINTS /opt/homebrew/bin NO_CMAKE_FIND_ROOT_PATH REQUIRED)
    execute_process(COMMAND "${WA_SETTINGS_NODE}" --version
        OUTPUT_VARIABLE node_version OUTPUT_STRIP_TRAILING_WHITESPACE COMMAND_ERROR_IS_FATAL ANY)
    if(NOT node_version MATCHES "^v26\\.")
        message(FATAL_ERROR "Set WA_SETTINGS_NODE to Node 26 (found ${node_version})")
    endif()
    set_target_properties(test_m14_upstream_settings PROPERTIES SUFFIX ".mjs")
    target_compile_options(test_m14_upstream_settings PRIVATE -fwasm-exceptions)
    target_link_options(test_m14_upstream_settings PRIVATE -O3 -fwasm-exceptions -lnodefs.js
        -sMODULARIZE=1 -sEXPORT_ES6=1 -sENVIRONMENT=node -sINVOKE_RUN=0 -sFORCE_FILESYSTEM=1
        -sALLOW_MEMORY_GROWTH=1 -sMAXIMUM_MEMORY=268435456 -sSTACK_SIZE=2097152
        "-sEXPORTED_RUNTIME_METHODS=FS,NODEFS,callMain")
    set(test_command "${WA_SETTINGS_NODE}" "${CMAKE_CURRENT_LIST_DIR}/verify_wasm.mjs"
        "$<TARGET_FILE:test_m14_upstream_settings>")
    set(test_name test_m14_upstream_settings_wasm)
else()
    set(test_command "$<TARGET_FILE:test_m14_upstream_settings>")
    set(test_name test_m14_upstream_settings)
endif()
add_test(NAME ${test_name} COMMAND ${test_command} "${WA_SETTINGS_APK}" "${WA_LAUNCHER_FRAMEWORK_DEX}"
    "${framework}" "${WA_LAUNCHER_FRAMEWORK_RES}")
set_tests_properties(${test_name} PROPERTIES TIMEOUT 300)
