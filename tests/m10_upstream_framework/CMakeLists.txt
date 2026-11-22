cmake_minimum_required(VERSION 3.24)

if(CMAKE_CURRENT_SOURCE_DIR STREQUAL CMAKE_SOURCE_DIR)
    project(WasmAndroidUpstreamFramework LANGUAGES C CXX)
    enable_testing()
    option(AOSP17_ART_INTERP_HGRAPH "Enable ART HGraph promotion" OFF)
endif()

get_filename_component(repo "${CMAKE_CURRENT_LIST_DIR}/../.." ABSOLUTE)
set(AOSP17_SOURCE_DIR "${repo}/third_party/aosp17" CACHE PATH "Read-only AOSP 17 checkout")
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
set(WA_JAVAC_EXECUTABLE "/usr/bin/javac" CACHE FILEPATH "JDK 17 javac")
set(WA_D8_EXECUTABLE "${WA_ANDROID_SDK}/build-tools/36.0.0/d8" CACHE FILEPATH "SDK 36 d8")
set(WA_AIDL_EXECUTABLE "${WA_ANDROID_SDK}/build-tools/36.0.0/aidl" CACHE FILEPATH "SDK 36 aidl")
set(WA_ANDROID_JAR "${WA_ANDROID_SDK}/platforms/android-36/android.jar" CACHE FILEPATH "SDK 36 API")
include("${repo}/cmake/Aosp17ArtInterp.cmake")
find_package(Python3 REQUIRED COMPONENTS Interpreter)

if(NOT TARGET wa_hal_surfaceflinger)
    add_subdirectory("${repo}/runtime/hal_surfaceflinger" "${CMAKE_CURRENT_BINARY_DIR}/surfaceflinger")
endif()
if(TARGET aosp17_ui_jni)
    set(ui_jni aosp17_ui_jni)
else()
    set(ui_jni aosp17_upstream_ui_jni)
    set(ui_native "${repo}/runtime/aosp17_shims/ui_jni")
    _wa_art_static(${ui_jni} "${ui_native}" ui_bridge.cpp canvas.cpp path_data.cpp bitmap.cpp)
    target_include_directories(${ui_jni} PUBLIC "${ui_native}")
    target_link_libraries(${ui_jni} PUBLIC aosp17_art_interp wa_hal_surfaceflinger
        PRIVATE aosp17_core_jni aosp17_zlib)
endif()

set(builder "${repo}/tools/aosp/build_upstream_framework_dex.py")
set(profile "${repo}/patches/aosp17/frameworks_base")
set(output "${CMAKE_CURRENT_BINARY_DIR}/upstream-dex")
set(framework "${output}/aosp17_upstream_framework.dex")
set(services "${output}/aosp17_upstream_services.dex")
set(framework_classes "${output}/aosp17_upstream_framework_classes.jar")
set(services_classes "${output}/aosp17_upstream_services_classes.jar")
set(provenance "${output}/framework_provenance.json")
set(artifacts "${framework}" "${framework_classes}" "${services}" "${services_classes}" "${provenance}")
set(build_command "${Python3_EXECUTABLE}" "${builder}"
    --aosp-root "${AOSP17_SOURCE_DIR}" --out-dir "${output}"
    --android-sdk "${WA_ANDROID_SDK}" --javac "${WA_JAVAC_EXECUTABLE}"
    --android-jar "${WA_ANDROID_JAR}" --d8 "${WA_D8_EXECUTABLE}" --aidl "${WA_AIDL_EXECUTABLE}"
    --services-classes-jar)
file(GLOB patches CONFIGURE_DEPENDS "${profile}/*.patch")
add_custom_command(OUTPUT ${artifacts}
    COMMAND ${build_command} --depfile "${output}/framework.d"
    DEPENDS "${builder}" "${profile}/sources.txt" ${patches}
    DEPFILE "${output}/framework.d"
    COMMENT "Building the upstream AOSP 17 framework and services DEX files"
    VERBATIM)
add_custom_target(aosp17_upstream_dex DEPENDS ${artifacts})

set(fixture_source "${CMAKE_CURRENT_SOURCE_DIR}/FrameworkProbe.java")
set(fixture_dir "${CMAKE_CURRENT_BINARY_DIR}/fixture")
set(fixture "${fixture_dir}/classes.dex")
add_custom_command(OUTPUT "${fixture}"
    COMMAND "${CMAKE_COMMAND}" -E rm -rf "${fixture_dir}/classes"
    COMMAND "${CMAKE_COMMAND}" -E make_directory "${fixture_dir}/classes"
    COMMAND "${WA_JAVAC_EXECUTABLE}" --release 17 -encoding UTF-8 -proc:none
        -classpath "${framework_classes}:${services_classes}:${WA_ANDROID_JAR}"
        -d "${fixture_dir}/classes" "${fixture_source}"
    COMMAND "${CMAKE_COMMAND}" -E chdir "${fixture_dir}/classes"
        "${CMAKE_COMMAND}" -E tar cf "${fixture_dir}/probe.jar" --format=zip org
    COMMAND "${WA_D8_EXECUTABLE}" --release --min-api 26 --lib "${WA_ANDROID_JAR}"
        --classpath "${framework_classes}" --classpath "${services_classes}"
        --output "${fixture_dir}" "${fixture_dir}/probe.jar"
    DEPENDS "${fixture_source}" "${framework_classes}" "${services_classes}"
    COMMENT "Compiling the interpreter test fixture"
    VERBATIM)
add_custom_target(aosp17_upstream_probe DEPENDS "${fixture}")

add_executable(test_aosp17_upstream_framework test_upstream.cpp)
target_link_libraries(test_aosp17_upstream_framework PRIVATE ${ui_jni})
add_dependencies(test_aosp17_upstream_framework aosp17_upstream_dex aosp17_upstream_probe)
if(EMSCRIPTEN)
    find_program(WA_NODE_EXECUTABLE node HINTS /opt/homebrew/bin NO_CMAKE_FIND_ROOT_PATH REQUIRED)
    set_target_properties(test_aosp17_upstream_framework PROPERTIES SUFFIX ".mjs")
    target_compile_options(test_aosp17_upstream_framework PRIVATE -fwasm-exceptions)
    target_link_options(test_aosp17_upstream_framework PRIVATE
        -sMODULARIZE=1 -sEXPORT_ES6=1 -sENVIRONMENT=node -sINVOKE_RUN=0
        -sFORCE_FILESYSTEM=1 -sALLOW_MEMORY_GROWTH=1 -sSTACK_SIZE=2097152
        "-sEXPORTED_RUNTIME_METHODS=FS,callMain")
    set(test_command "${WA_NODE_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/run_wasm.mjs"
        "$<TARGET_FILE:test_aosp17_upstream_framework>")
else()
    set(test_command "$<TARGET_FILE:test_aosp17_upstream_framework>")
endif()
add_test(NAME m10_upstream_reproducible COMMAND ${build_command} --check)
add_test(NAME m10_upstream_strict_patches
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/test_patches.py")
add_test(NAME m10_upstream_verify COMMAND ${test_command} verify "${framework}" "${services}")
add_test(NAME m10_upstream_execute COMMAND ${test_command} execute "${framework}" "${services}" "${fixture}")
set_tests_properties(m10_upstream_reproducible m10_upstream_strict_patches m10_upstream_verify m10_upstream_execute
    PROPERTIES TIMEOUT 120)
