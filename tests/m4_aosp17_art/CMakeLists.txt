cmake_minimum_required(VERSION 3.20)

if(CMAKE_CURRENT_SOURCE_DIR STREQUAL CMAKE_SOURCE_DIR)
    project(WasmAndroidAosp17Art LANGUAGES C CXX)
    enable_testing()
endif()

get_filename_component(wa_root "${CMAKE_CURRENT_LIST_DIR}/../.." ABSOLUTE)
include("${wa_root}/cmake/Aosp17Art.cmake")

if(TARGET wa_art_wasm)
    wa_use_aosp17_art(wa_art_wasm)
    set(compiler wa_art_wasm)
else()
    wa_add_aosp17_dex_compiler(wa_aosp17_dex_to_wasm)
    set(compiler wa_aosp17_dex_to_wasm)
endif()

set(WA_JAVAC_EXECUTABLE "/usr/bin/javac" CACHE FILEPATH "Java compiler")
set(WA_D8_EXECUTABLE "${WA_ANDROID_SDK}/build-tools/36.0.0/d8" CACHE FILEPATH "SDK 36 d8")
set(WA_ANDROID_JAR "${WA_ANDROID_SDK}/platforms/android-36/android.jar"
    CACHE FILEPATH "SDK 36 Android API")
find_program(WA_NODE_EXECUTABLE node HINTS /opt/homebrew/bin NO_CMAKE_FIND_ROOT_PATH REQUIRED)
find_program(WA_ZIP_EXECUTABLE zip NO_CMAKE_FIND_ROOT_PATH REQUIRED)

set(java "${CMAKE_CURRENT_SOURCE_DIR}/AospArtProbe.java")
set(class_dir "${CMAKE_CURRENT_BINARY_DIR}/classes")
set(class_file "${class_dir}/AospArtProbe.class")
add_custom_command(OUTPUT "${class_file}"
    COMMAND "${CMAKE_COMMAND}" -E make_directory "${class_dir}"
    COMMAND "${WA_JAVAC_EXECUTABLE}" --release 8 -g:none -d "${class_dir}" "${java}"
    DEPENDS "${java}" "${WA_JAVAC_EXECUTABLE}"
    VERBATIM)

function(wa_art_probe_fixture version api compression)
    set(dex_dir "${CMAKE_CURRENT_BINARY_DIR}/dex${version}")
    set(dex "${CMAKE_CURRENT_BINARY_DIR}/classes_v${version}.dex")
    set(apk "${CMAKE_CURRENT_BINARY_DIR}/probe_v${version}.apk")
    if(version STREQUAL "041")
        set(container -JDcom.android.tools.r8.dexContainerExperiment=true)
    endif()
    add_custom_command(OUTPUT "${dex}" "${apk}"
        COMMAND "${CMAKE_COMMAND}" -E make_directory "${dex_dir}"
        COMMAND "${WA_D8_EXECUTABLE}" ${container} --min-api "${api}" --lib "${WA_ANDROID_JAR}"
            --output "${dex_dir}" "${class_file}"
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different "${dex_dir}/classes.dex" "${dex}"
        COMMAND "${CMAKE_COMMAND}" -E rm -f "${apk}"
        COMMAND "${WA_ZIP_EXECUTABLE}" -q -X -j "${compression}" "${apk}" "${dex_dir}/classes.dex"
        DEPENDS "${class_file}" "${WA_D8_EXECUTABLE}" "${WA_ANDROID_JAR}" "${WA_ZIP_EXECUTABLE}"
        COMMENT "Building DEX v${version} and its APK with SDK 36 d8"
        VERBATIM)
    add_custom_target(wa_art_probe_${version} DEPENDS "${dex}" "${apk}")
endfunction()

wa_art_probe_fixture(035 21 -9)
wa_art_probe_fixture(041 36 -0)

add_executable(test_aosp17_art test_aosp17_art.cpp)
target_link_libraries(test_aosp17_art PRIVATE ${compiler} aosp17_libdexfile aosp17_libartbase)
add_dependencies(test_aosp17_art wa_art_probe_035 wa_art_probe_041)
if(EMSCRIPTEN)
    set_target_properties(test_aosp17_art PROPERTIES SUFFIX ".mjs")
    target_compile_options(test_aosp17_art PRIVATE
        -O3 -msimd128 -matomics -mbulk-memory -fwasm-exceptions)
    target_link_options(test_aosp17_art PRIVATE -O3 -sMODULARIZE=1 -sEXPORT_ES6=1 -sENVIRONMENT=node
        -sINVOKE_RUN=0 -sFORCE_FILESYSTEM=1 -sALLOW_MEMORY_GROWTH=1 -sSTACK_SIZE=1048576
        "-sEXPORTED_RUNTIME_METHODS=FS,callMain")
    set(mode wasm)
else()
    set(mode native)
endif()

function(wa_art_probe_test name fixture version format)
    add_test(NAME "test_aosp17_art_${name}"
        COMMAND "${WA_NODE_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/run_probe.mjs"
            "${mode}" "$<TARGET_FILE:test_aosp17_art>" "${CMAKE_CURRENT_BINARY_DIR}/${fixture}"
            "${version}" "${format}" "${CMAKE_CURRENT_BINARY_DIR}/probe_${name}.wasm")
endfunction()

wa_art_probe_test(dex035 classes_v035.dex 35 dex)
wa_art_probe_test(dex041 classes_v041.dex 41 dex)
wa_art_probe_test(apk035 probe_v035.apk 35 apk)
wa_art_probe_test(apk041 probe_v041.apk 41 apk)
