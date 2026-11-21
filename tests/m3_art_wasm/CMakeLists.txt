cmake_minimum_required(VERSION 3.20)

if(CMAKE_CURRENT_SOURCE_DIR STREQUAL CMAKE_SOURCE_DIR)
    project(WasmAndroidM3ArtWasm LANGUAGES C CXX)
    enable_testing()
    set(CMAKE_POSITION_INDEPENDENT_CODE ON)
endif()

get_filename_component(wa_root "${CMAKE_CURRENT_LIST_DIR}/../.." ABSOLUTE)
if(NOT TARGET wa_art_wasm)
    add_subdirectory("${wa_root}/runtime/art_wasm_backend" art_wasm)
endif()

set(WA_JAVAC_EXECUTABLE "/usr/bin/javac" CACHE FILEPATH "Java compiler")
set(WA_ANDROID_SDK "$ENV{HOME}/Library/Android/sdk" CACHE PATH "Android SDK directory")
set(WA_D8_EXECUTABLE "${WA_ANDROID_SDK}/build-tools/36.0.0/d8" CACHE FILEPATH "SDK 36 d8")
set(WA_ANDROID_JAR "${WA_ANDROID_SDK}/platforms/android-36/android.jar" CACHE FILEPATH "SDK 36 library")
set(WA_ANDROID_NDK_BIN
    "${WA_ANDROID_SDK}/ndk/29.0.13846066/toolchains/llvm/prebuilt/darwin-x86_64/bin"
    CACHE PATH "Directory containing the Android NDK 29 Clang drivers")
find_program(WA_NODE_EXECUTABLE NAMES node HINTS /opt/homebrew/bin REQUIRED)

if(EMSCRIPTEN)
    set(WA_ART_HOST_COMPILER "" CACHE FILEPATH "Path to the host dex2wasm")
    if(NOT EXISTS "${WA_ART_HOST_COMPILER}")
        message(FATAL_ERROR "Configure with -DWA_ART_HOST_COMPILER=/path/to/host/dex2wasm")
    endif()
    set(compiler "${WA_ART_HOST_COMPILER}")
else()
    set(compiler dex2wasm)
endif()

set(classes "${CMAKE_CURRENT_BINARY_DIR}/classes")
set(dex_dir "${CMAKE_CURRENT_BINARY_DIR}/dex")
set(dex "${dex_dir}/classes.dex")
set(class_files "${classes}/BenchmarkApp.class" "${classes}/BenchmarkApp$Node.class"
    "${classes}/BenchmarkApp$SpecialNode.class" "${classes}/BenchmarkApp$CustomException.class")
set(java "${CMAKE_CURRENT_SOURCE_DIR}/BenchmarkApp.java")
set(profile "${CMAKE_CURRENT_SOURCE_DIR}/boot_profile.txt")
set(full "${CMAKE_CURRENT_BINARY_DIR}/benchmark_app.wasm")
set(boot "${CMAKE_CURRENT_BINARY_DIR}/boot_core.wasm")

add_custom_command(OUTPUT ${class_files}
    COMMAND "${CMAKE_COMMAND}" -E make_directory "${classes}"
    COMMAND "${WA_JAVAC_EXECUTABLE}" --release 8 -g:none -d "${classes}" "${java}"
    DEPENDS "${java}" "${WA_JAVAC_EXECUTABLE}"
    COMMENT "Compiling BenchmarkApp.java with javac"
    VERBATIM)
add_custom_command(OUTPUT "${dex}"
    COMMAND "${CMAKE_COMMAND}" -E make_directory "${dex_dir}"
    COMMAND "${WA_D8_EXECUTABLE}" --min-api 21 --lib "${WA_ANDROID_JAR}" --output "${dex_dir}"
        ${class_files}
    DEPENDS ${class_files} "${WA_D8_EXECUTABLE}" "${WA_ANDROID_JAR}"
    COMMENT "Compiling Android DEX v035 with SDK 36 d8"
    VERBATIM)
add_custom_command(OUTPUT "${full}" "${boot}"
    COMMAND "${compiler}" "${dex}" -o "${full}"
    COMMAND "${compiler}" "${dex}" -o "${boot}" --profile "${profile}"
    DEPENDS "${compiler}" "${dex}" "${profile}"
    COMMENT "Compiling the full and profiled DEX Wasm chunks"
    VERBATIM)
add_custom_target(wa_m3_chunks DEPENDS "${full}" "${boot}")

if(EMSCRIPTEN)
    add_executable(wa_art_browser_runtime browser_entry.cpp)
    target_link_libraries(wa_art_browser_runtime PRIVATE wa_art_wasm)
    set_target_properties(wa_art_browser_runtime PROPERTIES SUFFIX ".mjs")
    set(browser_exports
        _malloc _free _wa_art_browser_abi_version _wa_art_dex_open _wa_art_dex_close
        _wa_art_heap_create _wa_art_heap_destroy _wa_art_heap_bind_memory
        _wa_art_thread_attach _wa_art_gc_collect _wa_art_gc_get_stats
        _wa_art_rt_new_object _wa_art_rt_new_array _wa_art_rt_const_string
        _wa_art_rt_get_field _wa_art_rt_put_field _wa_art_rt_get_static _wa_art_rt_put_static
        _wa_art_rt_array_length _wa_art_rt_array_get _wa_art_rt_array_put
        _wa_art_rt_is_instance _wa_art_rt_virtual_method _wa_art_rt_raise _wa_art_rt_safepoint
        _wa_art_rt_native)
    list(JOIN browser_exports "," browser_exports)
    target_link_options(wa_art_browser_runtime PRIVATE
        -O3 --no-entry -sENVIRONMENT=web,node -sMODULARIZE=1 -sEXPORT_ES6=1
        -sIMPORTED_MEMORY=1 -sINITIAL_MEMORY=33554432 "-sEXPORTED_FUNCTIONS=${browser_exports}")
    add_dependencies(wa_art_browser_runtime wa_m3_chunks)
    add_test(NAME test_m3_art_wasm_emscripten
        COMMAND "${WA_NODE_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/run_m3_wasm_emscripten.mjs"
            "${dex}" "${full}" "${boot}" "$<TARGET_FILE:wa_art_browser_runtime>")

    set(WA_CHROME_EXECUTABLE "/Applications/Google Chrome.app/Contents/MacOS/Google Chrome"
        CACHE FILEPATH "Chrome for the browser execution test")
    if(EXISTS "${WA_CHROME_EXECUTABLE}")
        add_test(NAME test_m3_art_wasm_chrome
            COMMAND "${WA_NODE_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/run_m3_wasm_browser.mjs"
                "${WA_CHROME_EXECUTABLE}" "${CMAKE_CURRENT_BINARY_DIR}" "${dex}" "${full}" "${boot}"
                "$<TARGET_FILE:wa_art_browser_runtime>")
        set_tests_properties(test_m3_art_wasm_chrome PROPERTIES SKIP_RETURN_CODE 77)
    endif()
else()
    set(container_dir "${CMAKE_CURRENT_BINARY_DIR}/dex041")
    set(container "${container_dir}/classes.dex")
    set(container_full "${container_dir}/benchmark_app.wasm")
    set(container_boot "${container_dir}/boot_core.wasm")
    add_custom_command(OUTPUT "${container}" "${container_full}" "${container_boot}"
        COMMAND "${CMAKE_COMMAND}" -E make_directory "${container_dir}"
        COMMAND "${WA_D8_EXECUTABLE}" -JDcom.android.tools.r8.dexContainerExperiment=true
            --min-api 36 --lib "${WA_ANDROID_JAR}" --output "${container_dir}" ${class_files}
        COMMAND dex2wasm "${container}" -o "${container_full}"
        COMMAND dex2wasm "${container}" -o "${container_boot}" --profile "${profile}"
        DEPENDS dex2wasm ${class_files} "${profile}" "${WA_D8_EXECUTABLE}" "${WA_ANDROID_JAR}"
        COMMENT "Compiling Android DEX v041 with SDK 36 d8 and dex2wasm"
        VERBATIM)
    add_custom_target(wa_m3_container_chunks DEPENDS "${container_full}" "${container_boot}")

    set(elf "${CMAKE_CURRENT_BINARY_DIR}/libnative_boost.so")
    set(driver "${WA_ANDROID_NDK_BIN}/aarch64-linux-android21-clang")
    add_custom_command(OUTPUT "${elf}"
        COMMAND "${driver}" -std=c11 -O2 -Wall -Wextra -Werror -fPIC -fvisibility=hidden
            -fno-stack-protector -nostdlib -shared -Wl,--no-undefined -Wl,-soname,libnative_boost.so
            "${CMAKE_CURRENT_SOURCE_DIR}/native_boost.c" -o "${elf}"
        DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/native_boost.c" "${driver}"
            "${WA_ANDROID_NDK_BIN}/../sysroot/usr/include/jni.h"
        COMMENT "Compiling the JNI guest with Android NDK 29 for arm64-v8a"
        VERBATIM)
    add_custom_target(wa_m3_jni_guest DEPENDS "${elf}")

    find_path(WA_NODE_INCLUDE_DIR NAMES node_api.h HINTS /opt/homebrew/include/node
        /usr/local/include/node /usr/include/node REQUIRED)
    add_library(wa_art_node MODULE node_runtime.cpp)
    target_link_libraries(wa_art_node PRIVATE wa_art_wasm)
    target_include_directories(wa_art_node SYSTEM PRIVATE "${WA_NODE_INCLUDE_DIR}")
    set_target_properties(wa_art_node PROPERTIES PREFIX "" SUFFIX ".node")
    if(APPLE)
        target_link_options(wa_art_node PRIVATE "-Wl,-undefined,dynamic_lookup")
    endif()

    add_executable(test_m3_art_wasm test_m3_art_wasm.cpp)
    target_link_libraries(test_m3_art_wasm PRIVATE wa_art_wasm)
    add_dependencies(test_m3_art_wasm wa_m3_chunks wa_m3_container_chunks wa_m3_jni_guest wa_art_node)
    set(node_args "${CMAKE_CURRENT_SOURCE_DIR}/run_m3_wasm_node.mjs"
        "${dex}" "${full}" "${boot}" "$<TARGET_FILE:wa_art_node>")
    add_test(NAME test_m3_art_wasm COMMAND test_m3_art_wasm "${dex}"
        "${WA_NODE_EXECUTABLE}" ${node_args} "${elf}")
    add_test(NAME test_m3_art_wasm_node COMMAND "${WA_NODE_EXECUTABLE}" ${node_args})
    add_test(NAME test_m3_art_wasm_dex041 COMMAND "${WA_NODE_EXECUTABLE}"
        "${CMAKE_CURRENT_SOURCE_DIR}/run_m3_wasm_node.mjs" "${container}"
        "${container_full}" "${container_boot}" "$<TARGET_FILE:wa_art_node>")
endif()
