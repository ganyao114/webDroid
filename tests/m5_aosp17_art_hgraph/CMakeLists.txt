cmake_minimum_required(VERSION 3.20)

if(CMAKE_CURRENT_SOURCE_DIR STREQUAL CMAKE_SOURCE_DIR)
    project(WasmAndroidAosp17ArtHGraph LANGUAGES C CXX)
    enable_testing()
endif()

include("${CMAKE_CURRENT_LIST_DIR}/../../cmake/Aosp17ArtHGraph.cmake")

set(WA_JAVAC_EXECUTABLE "/usr/bin/javac" CACHE FILEPATH "Java compiler")
set(WA_D8_EXECUTABLE "${WA_ANDROID_SDK}/build-tools/36.0.0/d8" CACHE FILEPATH "SDK 36 d8")
set(WA_ANDROID_JAR "${WA_ANDROID_SDK}/platforms/android-36/android.jar"
    CACHE FILEPATH "SDK 36 Android API")
find_program(WA_NODE_EXECUTABLE node HINTS /opt/homebrew/bin NO_CMAKE_FIND_ROOT_PATH REQUIRED)

set(java "${CMAKE_CURRENT_SOURCE_DIR}/HGraphProbe.java")
set(classes "${CMAKE_CURRENT_BINARY_DIR}/classes")
set(class_file "${classes}/HGraphProbe.class")
set(dex "${CMAKE_CURRENT_BINARY_DIR}/classes.dex")
add_custom_command(OUTPUT "${class_file}"
    COMMAND "${CMAKE_COMMAND}" -E make_directory "${classes}"
    COMMAND "${WA_JAVAC_EXECUTABLE}" --release 8 -g -d "${classes}" "${java}"
    DEPENDS "${java}" "${WA_JAVAC_EXECUTABLE}"
    VERBATIM)
add_custom_command(OUTPUT "${dex}"
    COMMAND "${WA_D8_EXECUTABLE}" --debug --min-api 21 --lib "${WA_ANDROID_JAR}"
        --output "${CMAKE_CURRENT_BINARY_DIR}" "${class_file}"
    DEPENDS "${class_file}" "${WA_D8_EXECUTABLE}" "${WA_ANDROID_JAR}"
    COMMENT "Compiling HGraphProbe to DEX with SDK 36 d8"
    VERBATIM)
add_custom_target(wa_hgraph_probe_dex DEPENDS "${dex}")

add_executable(test_aosp17_art_hgraph test_aosp17_art_hgraph.cpp)
target_link_libraries(test_aosp17_art_hgraph PRIVATE aosp17_art_hgraph)
add_dependencies(test_aosp17_art_hgraph wa_hgraph_probe_dex)
if(EMSCRIPTEN)
    set_target_properties(test_aosp17_art_hgraph PROPERTIES SUFFIX ".mjs")
    target_compile_options(test_aosp17_art_hgraph PRIVATE
        -O3 -msimd128 -matomics -mbulk-memory -fwasm-exceptions)
    target_link_options(test_aosp17_art_hgraph PRIVATE
        -O3 -sMODULARIZE=1 -sEXPORT_ES6=1 -sENVIRONMENT=node -sINVOKE_RUN=0
        -sFORCE_FILESYSTEM=1 -sALLOW_MEMORY_GROWTH=1 -sSTACK_SIZE=1048576
        "-sEXPORTED_RUNTIME_METHODS=FS,callMain")
    set(mode wasm)
else()
    set(mode native)
endif()

add_test(NAME test_aosp17_art_hgraph
    COMMAND "${WA_NODE_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/run_hgraph_probe.mjs"
        "${mode}" "$<TARGET_FILE:test_aosp17_art_hgraph>" "${dex}"
        "${CMAKE_CURRENT_BINARY_DIR}/hgraph_probe.wasm")
