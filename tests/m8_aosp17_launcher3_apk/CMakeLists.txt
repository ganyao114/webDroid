cmake_minimum_required(VERSION 3.24)

if(CMAKE_CURRENT_SOURCE_DIR STREQUAL CMAKE_SOURCE_DIR)
    project(WasmAndroidAosp17Launcher3 LANGUAGES C CXX)
    enable_testing()
endif()

get_filename_component(wa_root "${CMAKE_CURRENT_LIST_DIR}/../.." ABSOLUTE)
include("${wa_root}/cmake/Aosp17ArtInterp.cmake")
find_package(Python3 3.9 REQUIRED COMPONENTS Interpreter)

if(DEFINED ENV{ANDROID_SDK_ROOT} AND NOT "$ENV{ANDROID_SDK_ROOT}" STREQUAL "")
    set(sdk_default "$ENV{ANDROID_SDK_ROOT}")
elseif(DEFINED ENV{ANDROID_HOME} AND NOT "$ENV{ANDROID_HOME}" STREQUAL "")
    set(sdk_default "$ENV{ANDROID_HOME}")
elseif(CMAKE_HOST_APPLE)
    set(sdk_default "$ENV{HOME}/Library/Android/sdk")
else()
    set(sdk_default "$ENV{HOME}/Android/Sdk")
endif()
set(WA_ANDROID_SDK "${sdk_default}" CACHE PATH "Android SDK directory")
foreach(tool IN ITEMS aapt2 d8 zipalign apksigner)
    string(TOUPPER "${tool}" tool_upper)
    set(WA_${tool_upper}_EXECUTABLE "${WA_ANDROID_SDK}/build-tools/36.0.0/${tool}"
        CACHE FILEPATH "SDK 36 ${tool}")
endforeach()
set(WA_ANDROID_JAR "${WA_ANDROID_SDK}/platforms/android-36/android.jar"
    CACHE FILEPATH "SDK 36 Android API")
if(NOT WA_JAVAC_EXECUTABLE)
    find_program(WA_JAVAC_EXECUTABLE javac NO_CMAKE_FIND_ROOT_PATH REQUIRED)
endif()
foreach(input IN ITEMS WA_ANDROID_JAR WA_AAPT2_EXECUTABLE WA_D8_EXECUTABLE
        WA_ZIPALIGN_EXECUTABLE WA_APKSIGNER_EXECUTABLE WA_JAVAC_EXECUTABLE)
    if(NOT EXISTS "${${input}}")
        message(FATAL_ERROR "Set ${input} to its SDK 36 or JDK 17 path (currently ${${input}})")
    endif()
endforeach()

set(builder "${wa_root}/tools/aosp/build_launcher3_apk.py")
set(compat "${wa_root}/tools/aosp/launcher3_build")
file(GLOB_RECURSE local_inputs CONFIGURE_DEPENDS
    "${compat}/java/*.java" "${compat}/res_compat/*.xml" "${compat}/*.py")
set(launcher "${AOSP17_SOURCE_DIR}/packages/apps/Launcher3")
file(GLOB_RECURSE resource_inputs CONFIGURE_DEPENDS "${launcher}/res/*" "${launcher}/aconfig/launcher*.aconfig")
set(output "${CMAKE_CURRENT_BINARY_DIR}/apk")
set(apk "${output}/Launcher3.apk")
set(report "${output}/launcher3_manifest_report.json")
set(build_command
    "${Python3_EXECUTABLE}" "${builder}" --aosp-root "${AOSP17_SOURCE_DIR}" --out-dir "${output}"
    --aapt2 "${WA_AAPT2_EXECUTABLE}" --javac "${WA_JAVAC_EXECUTABLE}" --d8 "${WA_D8_EXECUTABLE}"
    --zipalign "${WA_ZIPALIGN_EXECUTABLE}" --apksigner "${WA_APKSIGNER_EXECUTABLE}"
    --android-jar "${WA_ANDROID_JAR}")
add_custom_command(OUTPUT "${apk}" "${report}"
    COMMAND ${build_command} --depfile "${output}/launcher3.d"
    DEPENDS "${builder}" "${compat}/upstream_java.txt" ${local_inputs} ${resource_inputs}
    DEPFILE "${output}/launcher3.d"
    COMMENT "Compiling and signing the AOSP 17 Launcher3 home APK"
    VERBATIM)
add_custom_target(aosp17_launcher3_apk ALL DEPENDS "${apk}" "${report}")

add_executable(test_aosp17_launcher3_apk test_launcher3.cpp)
add_dependencies(test_aosp17_launcher3_apk aosp17_launcher3_apk)
target_link_libraries(test_aosp17_launcher3_apk PRIVATE
    aosp17_art_interp aosp17_libandroidfw aosp17_libdexfile)
target_include_directories(test_aosp17_launcher3_apk SYSTEM PRIVATE "${AOSP17_SOURCE_DIR}/art/runtime")
if(EMSCRIPTEN)
    find_program(WA_NODE_EXECUTABLE node HINTS /opt/homebrew/bin NO_CMAKE_FIND_ROOT_PATH REQUIRED)
    execute_process(COMMAND "${WA_NODE_EXECUTABLE}" --version
        OUTPUT_VARIABLE node_version OUTPUT_STRIP_TRAILING_WHITESPACE
        COMMAND_ERROR_IS_FATAL ANY)
    if(NOT node_version MATCHES "^v26\\.")
        message(FATAL_ERROR "Set WA_NODE_EXECUTABLE to Node 26 (found ${node_version})")
    endif()
    set_target_properties(test_aosp17_launcher3_apk PROPERTIES SUFFIX ".mjs")
    target_compile_options(test_aosp17_launcher3_apk PRIVATE -O3 -fwasm-exceptions)
    target_link_options(test_aosp17_launcher3_apk PRIVATE -O3 -sMODULARIZE=1 -sEXPORT_ES6=1
        -sENVIRONMENT=node -sINVOKE_RUN=0 -sFORCE_FILESYSTEM=1 -sALLOW_MEMORY_GROWTH=1
        -sSTACK_SIZE=1048576 "-sEXPORTED_RUNTIME_METHODS=FS,callMain")
    set(run_launcher3_test "${WA_NODE_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/run_wasm.mjs"
        "$<TARGET_FILE:test_aosp17_launcher3_apk>" "${apk}")
else()
    set(run_launcher3_test "$<TARGET_FILE:test_aosp17_launcher3_apk>" "${apk}")
endif()

add_test(NAME aosp17_launcher3_reproducible COMMAND ${build_command} --check)
add_test(NAME aosp17_launcher3_zipaligned COMMAND "${WA_ZIPALIGN_EXECUTABLE}" -c -P 16 4 "${apk}")
add_test(NAME aosp17_launcher3_androidfw COMMAND ${run_launcher3_test} resources)
set(WA_LAUNCHER3_FRAMEWORK_DEX "" CACHE FILEPATH "Optional AOSP 17 UI framework DEX for class linkage")
set(framework_arg)
if(WA_LAUNCHER3_FRAMEWORK_DEX)
    if(NOT EXISTS "${WA_LAUNCHER3_FRAMEWORK_DEX}")
        message(FATAL_ERROR "Framework DEX does not exist at ${WA_LAUNCHER3_FRAMEWORK_DEX}")
    endif()
    list(APPEND framework_arg "${WA_LAUNCHER3_FRAMEWORK_DEX}")
endif()
add_test(NAME aosp17_launcher3_art COMMAND ${run_launcher3_test} dex ${framework_arg})
