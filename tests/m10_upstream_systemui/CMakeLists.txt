cmake_minimum_required(VERSION 3.24)

if(CMAKE_CURRENT_SOURCE_DIR STREQUAL CMAKE_SOURCE_DIR)
    project(WasmAndroidAosp17SystemUI LANGUAGES C CXX)
    enable_testing()
    option(AOSP17_ART_INTERP_HGRAPH "Build the optional ART HGraph compiler" OFF)
endif()

get_filename_component(wa_root "${CMAKE_CURRENT_LIST_DIR}/../.." ABSOLUTE)
include("${wa_root}/cmake/Aosp17UiFramework.cmake")
find_package(Python3 3.10 REQUIRED COMPONENTS Interpreter)
find_program(WA_JAVA_EXECUTABLE java NO_CMAKE_FIND_ROOT_PATH REQUIRED)
find_program(WA_JAR_EXECUTABLE jar NO_CMAKE_FIND_ROOT_PATH REQUIRED)

set(WA_SYSTEMUI_DEPS_DIR "${wa_root}/third_party/aosp17_deps" CACHE PATH "Offline SystemUI dependencies")
foreach(tool IN ITEMS aapt2 zipalign apksigner)
    string(TOUPPER "${tool}" tool_upper)
    set(WA_${tool_upper}_EXECUTABLE "${WA_ANDROID_SDK}/build-tools/36.0.0/${tool}"
        CACHE FILEPATH "SDK 36 ${tool}")
endforeach()

get_target_property(framework_dex aosp17_framework_core WA_FRAMEWORK_DEX)
get_target_property(framework_jar aosp17_framework_core WA_FRAMEWORK_CLASSES)
if(NOT TARGET wa_upstream_systemui_apk)
    set(wa_launcher_output "${CMAKE_CURRENT_BINARY_DIR}/apk")
    include("${wa_root}/runtime/android_system/UpstreamLauncher.cmake")
endif()
set(apk "${WA_SYSTEMUI_APK}")

cmake_path(CONVERT "${framework_jar};${WA_ANDROID_JAR}" TO_NATIVE_PATH_LIST fixture_classpath)
set(fixture "${CMAKE_CURRENT_BINARY_DIR}/render-fixture")
set(fixture_apk "${fixture}/RenderProbe.apk")
set(fixture_dex "${fixture}/classes.dex")
add_custom_command(OUTPUT "${fixture_apk}" "${fixture_dex}"
    COMMAND "${CMAKE_COMMAND}" -E rm -rf "${fixture}/classes"
    COMMAND "${CMAKE_COMMAND}" -E make_directory "${fixture}/classes"
    COMMAND "${WA_AAPT2_EXECUTABLE}" link -I "${WA_ANDROID_JAR}"
        --manifest "${CMAKE_CURRENT_LIST_DIR}/AndroidManifest.xml"
        --min-sdk-version 26 --target-sdk-version 36 -o "${fixture_apk}"
    COMMAND "${WA_JAVAC_EXECUTABLE}" --release 17 -encoding UTF-8 -proc:none -XDstringConcat=inline
        -classpath "${fixture_classpath}" -d "${fixture}/classes"
        "${CMAKE_CURRENT_LIST_DIR}/RenderActivity.java"
    COMMAND "${WA_JAR_EXECUTABLE}" cf "${fixture}/classes.jar" -C "${fixture}/classes" .
    COMMAND "${WA_D8_EXECUTABLE}" --release --min-api 26 --lib "${WA_ANDROID_JAR}"
        --classpath "${framework_jar}" --output "${fixture}" "${fixture}/classes.jar"
    DEPENDS aosp17_framework_core "${framework_jar}" "${WA_ANDROID_JAR}"
        "${WA_AAPT2_EXECUTABLE}" "${WA_JAVAC_EXECUTABLE}" "${WA_D8_EXECUTABLE}"
        "${CMAKE_CURRENT_LIST_DIR}/RenderActivity.java" "${CMAKE_CURRENT_LIST_DIR}/AndroidManifest.xml"
    VERBATIM)
add_custom_target(m10_systemui_render_fixture DEPENDS "${fixture_apk}" "${fixture_dex}")

add_executable(test_m10_upstream_systemui test_systemui.cpp)
add_dependencies(test_m10_upstream_systemui wa_upstream_systemui_apk m10_systemui_render_fixture)
target_link_libraries(test_m10_upstream_systemui PRIVATE aosp17_ui_jni aosp17_libandroidfw)

if(EMSCRIPTEN)
    find_program(WA_NODE_EXECUTABLE node HINTS /opt/homebrew/bin NO_CMAKE_FIND_ROOT_PATH REQUIRED)
    execute_process(COMMAND "${WA_NODE_EXECUTABLE}" --version
        OUTPUT_VARIABLE node_version OUTPUT_STRIP_TRAILING_WHITESPACE COMMAND_ERROR_IS_FATAL ANY)
    if(NOT node_version MATCHES "^v26\\.")
        message(FATAL_ERROR "Set WA_NODE_EXECUTABLE to Node 26 (found ${node_version})")
    endif()
    set_target_properties(test_m10_upstream_systemui PROPERTIES SUFFIX ".mjs")
    target_compile_options(test_m10_upstream_systemui PRIVATE -fwasm-exceptions)
    target_link_options(test_m10_upstream_systemui PRIVATE -sMODULARIZE=1 -sEXPORT_ES6=1
        -sENVIRONMENT=node -sINVOKE_RUN=0 -sFORCE_FILESYSTEM=1 -sALLOW_MEMORY_GROWTH=1
        -sSTACK_SIZE=2097152 "-sEXPORTED_RUNTIME_METHODS=FS,callMain")
    set(run_systemui_test "${WA_NODE_EXECUTABLE}" "${CMAKE_CURRENT_LIST_DIR}/run_wasm.mjs"
        "$<TARGET_FILE:test_m10_upstream_systemui>")
    set(runtime_test test_m10_upstream_systemui_wasm)
else()
    set(run_systemui_test "$<TARGET_FILE:test_m10_upstream_systemui>")
    set(runtime_test m10_systemui_runtime)
endif()

add_test(NAME m10_systemui_reproducible COMMAND ${wa_build_systemui} --check)
add_test(NAME m10_systemui_zipaligned COMMAND "${WA_ZIPALIGN_EXECUTABLE}" -c -P 16 4 "${apk}")
add_test(NAME m10_systemui_signed COMMAND "${WA_APKSIGNER_EXECUTABLE}" verify "${apk}")
add_test(NAME m10_systemui_resources COMMAND ${run_systemui_test} resources "${apk}")
add_test(NAME m10_systemui_dex COMMAND ${run_systemui_test} dex "${apk}")
add_test(NAME ${runtime_test} COMMAND ${run_systemui_test} runtime "${apk}" "${framework_dex}"
    "${WA_ANDROID_JAR}" "${fixture_apk}" "${fixture_dex}")
set_tests_properties(${runtime_test} PROPERTIES RUN_SERIAL TRUE)
