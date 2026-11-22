cmake_minimum_required(VERSION 3.24)

if(CMAKE_CURRENT_SOURCE_DIR STREQUAL CMAKE_SOURCE_DIR)
    project(Aosp17UpstreamSettings LANGUAGES C CXX)
    enable_testing()
    option(AOSP17_ART_INTERP_HGRAPH "Enable optional ART HGraph promotion" OFF)
endif()

get_filename_component(wa_root "${CMAKE_CURRENT_LIST_DIR}/../.." ABSOLUTE)
include("${wa_root}/cmake/Aosp17UiFramework.cmake")
find_package(Python3 3.10 REQUIRED COMPONENTS Interpreter)
set(WA_SETTINGS_DEPS_DIR "${wa_root}/third_party/aosp17_deps" CACHE PATH "Offline Settings dependencies")

set(builder "${wa_root}/tools/aosp/build_upstream_settings.py")
set(profile "${wa_root}/patches/aosp17/settings")
file(GLOB patches CONFIGURE_DEPENDS "${profile}/*.patch")
set(output "${CMAKE_CURRENT_BINARY_DIR}/apk")
set(apk "${output}/Settings-upstream.apk")
set(provenance "${output}/upstream_provenance.json")
set(deps "${output}/settings.d")
set(build_settings "${Python3_EXECUTABLE}" "${builder}"
    --aosp-root "${AOSP17_SOURCE_DIR}" --deps-dir "${WA_SETTINGS_DEPS_DIR}"
    --sdk "${WA_ANDROID_SDK}" --android-jar "${WA_ANDROID_JAR}"
    --javac "${WA_JAVAC_EXECUTABLE}" --d8 "${WA_D8_EXECUTABLE}" --out-dir "${output}")
add_custom_command(OUTPUT "${apk}" "${provenance}"
    COMMAND ${build_settings} --depfile "${deps}"
    DEPENDS "${builder}" "${profile}/sources.txt" "${profile}/resources.txt"
        "${profile}/PreferenceLink.java" ${patches}
    DEPFILE "${deps}"
    COMMENT "Building the signed upstream Settings APK"
    VERBATIM)
add_custom_target(m14_upstream_settings_apk DEPENDS "${apk}" "${provenance}")

get_target_property(framework aosp17_framework_core WA_FRAMEWORK_DEX)
add_executable(test_m14_upstream_settings test_upstream_settings.cpp)
add_dependencies(test_m14_upstream_settings m14_upstream_settings_apk aosp17_framework_core)
target_link_libraries(test_m14_upstream_settings PRIVATE aosp17_ui_jni aosp17_libandroidfw)
target_compile_features(test_m14_upstream_settings PRIVATE cxx_std_20)

if(EMSCRIPTEN)
    find_program(WA_SETTINGS_NODE node HINTS /opt/homebrew/bin NO_CMAKE_FIND_ROOT_PATH REQUIRED)
    execute_process(COMMAND "${WA_SETTINGS_NODE}" --version
        OUTPUT_VARIABLE node_version OUTPUT_STRIP_TRAILING_WHITESPACE COMMAND_ERROR_IS_FATAL ANY)
    if(NOT node_version MATCHES "^v26\\.")
        message(FATAL_ERROR "Set WA_SETTINGS_NODE to Node 26 (found ${node_version})")
    endif()
    set_target_properties(test_m14_upstream_settings PROPERTIES SUFFIX ".mjs")
    target_compile_options(test_m14_upstream_settings PRIVATE -fwasm-exceptions)
    target_link_options(test_m14_upstream_settings PRIVATE -fwasm-exceptions -lnodefs.js
        -sMODULARIZE=1 -sEXPORT_ES6=1 -sENVIRONMENT=node -sINVOKE_RUN=0 -sFORCE_FILESYSTEM=1
        -sALLOW_MEMORY_GROWTH=1 -sMAXIMUM_MEMORY=268435456 -sSTACK_SIZE=2097152
        "-sEXPORTED_RUNTIME_METHODS=FS,NODEFS,callMain")
    set(test_command "${WA_SETTINGS_NODE}" "${CMAKE_CURRENT_LIST_DIR}/verify_wasm.mjs"
        "$<TARGET_FILE:test_m14_upstream_settings>")
else()
    set(test_command "$<TARGET_FILE:test_m14_upstream_settings>")
endif()
add_test(NAME test_m14_upstream_settings
    COMMAND ${test_command} "${apk}" "${framework}" "${WA_ANDROID_JAR}")
set_tests_properties(test_m14_upstream_settings PROPERTIES TIMEOUT 120)
