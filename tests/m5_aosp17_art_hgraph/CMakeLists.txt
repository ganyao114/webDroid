cmake_minimum_required(VERSION 3.20)

if(CMAKE_CURRENT_SOURCE_DIR STREQUAL CMAKE_SOURCE_DIR)
    project(WasmAndroidAosp17ArtHGraph LANGUAGES C CXX)
    enable_testing()
endif()

include("${CMAKE_CURRENT_LIST_DIR}/../../cmake/Aosp17ArtHGraph.cmake")
get_filename_component(wa_root "${CMAKE_CURRENT_LIST_DIR}/../.." ABSOLUTE)

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

set(object_java "${CMAKE_CURRENT_SOURCE_DIR}/HGraphObjectsProbe.java")
set(object_dir "${CMAKE_CURRENT_BINARY_DIR}/objects")
set(object_classes "${object_dir}/classes")
set(object_class_files "${object_classes}/HGraphObjectsProbe.class")
foreach(member HasArea Shape Rect Circle Node ProbeFailure)
    list(APPEND object_class_files "${object_classes}/HGraphObjectsProbe$${member}.class")
endforeach()
set(object_dex "${object_dir}/classes.dex")
set(object_wasm "${CMAKE_CURRENT_BINARY_DIR}/hgraph_objects.wasm")
add_custom_command(OUTPUT ${object_class_files}
    COMMAND "${CMAKE_COMMAND}" -E make_directory "${object_classes}"
    COMMAND "${WA_JAVAC_EXECUTABLE}" --release 8 -g -d "${object_classes}" "${object_java}"
    DEPENDS "${object_java}" "${WA_JAVAC_EXECUTABLE}"
    VERBATIM)
add_custom_command(OUTPUT "${object_dex}"
    COMMAND "${WA_D8_EXECUTABLE}" --debug --min-api 21 --lib "${WA_ANDROID_JAR}"
        --output "${object_dir}" ${object_class_files}
    DEPENDS ${object_class_files} "${WA_D8_EXECUTABLE}" "${WA_ANDROID_JAR}"
    COMMENT "Compiling HGraphObjectsProbe to DEX with SDK 36 d8"
    VERBATIM)
add_custom_target(wa_hgraph_objects_dex DEPENDS "${object_dex}")

add_executable(test_aosp17_art_hgraph test_aosp17_art_hgraph.cpp)
target_link_libraries(test_aosp17_art_hgraph PRIVATE aosp17_art_hgraph)
target_include_directories(test_aosp17_art_hgraph PRIVATE "${wa_root}/include")
add_dependencies(test_aosp17_art_hgraph wa_hgraph_probe_dex wa_hgraph_objects_dex)
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

# Keep the collector test independent of the Dalvik emitter, JNI and DBT.
_wa_art_static(wa_hgraph_shadow_gc "${wa_root}/runtime"
    art_wasm_backend/shadow_gc.cpp aosp17_shims/art/aosp17_dex_to_wasm.cpp)
target_sources(wa_hgraph_shadow_gc PRIVATE shadow_runtime_link.cpp)
target_include_directories(wa_hgraph_shadow_gc PUBLIC "${wa_root}/include"
    PRIVATE "${wa_root}/runtime/art_wasm_backend")
target_compile_options(wa_hgraph_shadow_gc PRIVATE -ffunction-sections -fdata-sections)
target_link_libraries(wa_hgraph_shadow_gc PRIVATE aosp17_libdexfile)

if(EMSCRIPTEN)
    set(shadow_runtime wa_hgraph_shadow_wasm)
    add_executable(${shadow_runtime} "${wa_root}/tests/m3_art_wasm/browser_entry.cpp")
    set_target_properties(${shadow_runtime} PROPERTIES SUFFIX ".mjs")
    set(shadow_exports
        _malloc _free _wa_art_browser_abi_version _wa_art_dex_open _wa_art_dex_close
        _wa_art_heap_create _wa_art_heap_destroy _wa_art_heap_bind_memory
        _wa_art_thread_attach _wa_art_gc_collect _wa_art_gc_get_stats
        _wa_art_rt_new_object _wa_art_rt_new_array _wa_art_rt_const_string
        _wa_art_rt_get_field _wa_art_rt_put_field _wa_art_rt_get_static _wa_art_rt_put_static
        _wa_art_rt_array_length _wa_art_rt_array_get _wa_art_rt_array_put _wa_art_rt_is_instance
        _wa_art_rt_virtual_method _wa_art_rt_raise _wa_art_rt_safepoint)
    list(JOIN shadow_exports "," shadow_exports)
    target_link_options(${shadow_runtime} PRIVATE
        -O3 --no-entry -sMODULARIZE=1 -sEXPORT_ES6=1 -sENVIRONMENT=node
        -sIMPORTED_MEMORY=1 -sINITIAL_MEMORY=33554432 "-sEXPORTED_FUNCTIONS=${shadow_exports}")
else()
    set(shadow_runtime wa_hgraph_shadow_node)
    find_path(WA_NODE_INCLUDE_DIR node_api.h HINTS /opt/homebrew/include/node
        /usr/local/include/node /usr/include/node REQUIRED)
    add_library(${shadow_runtime} MODULE node_shadow_runtime.cpp)
    target_include_directories(${shadow_runtime} SYSTEM PRIVATE "${WA_NODE_INCLUDE_DIR}")
    set_target_properties(${shadow_runtime} PROPERTIES PREFIX "" SUFFIX ".node")
    if(APPLE)
        target_link_options(${shadow_runtime} PRIVATE "-Wl,-undefined,dynamic_lookup" "-Wl,-dead_strip")
    endif()
endif()
target_link_libraries(${shadow_runtime} PRIVATE wa_hgraph_shadow_gc)

set(object_runner "${CMAKE_CURRENT_SOURCE_DIR}/run_hgraph_objects.mjs")
set(object_args "${mode}" "$<TARGET_FILE:test_aosp17_art_hgraph>" "${object_dex}" "${object_wasm}")
add_custom_command(OUTPUT "${object_wasm}"
    COMMAND "${WA_NODE_EXECUTABLE}" "${object_runner}" ${object_args} --compile-only
    DEPENDS test_aosp17_art_hgraph "${object_dex}" "${object_runner}"
    COMMENT "Compiling upstream ART object graphs to hgraph_objects.wasm"
    VERBATIM)
add_custom_target(wa_hgraph_objects ALL DEPENDS "${object_wasm}")
add_test(NAME test_aosp17_art_hgraph_objects
    COMMAND "${WA_NODE_EXECUTABLE}" "${object_runner}" ${object_args}
        "$<TARGET_FILE:${shadow_runtime}>")
