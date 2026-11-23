cmake_minimum_required(VERSION 3.20)

if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    project(WasmAndroidM2FfiThunks LANGUAGES C CXX)
    enable_testing()
endif()

get_filename_component(wa_root "${CMAKE_CURRENT_LIST_DIR}/../.." ABSOLUTE)
option(WA_M2_FFI_TEST_DBT "Also run an NDK JNI guest through wa_linker_ffi" OFF)
if(NOT TARGET wa_softmmu)
    add_subdirectory("${wa_root}/runtime/softmmu_opfs" softmmu)
endif()
if(WA_M2_FFI_TEST_DBT AND NOT TARGET wa_linker_ffi)
    add_subdirectory("${wa_root}/runtime/linker_ffi" linker_ffi)
endif()
if(NOT TARGET wa_ffi_thunks)
    add_subdirectory("${wa_root}/tools/ffi_gen" ffi_gen)
endif()

find_package(Python3 REQUIRED COMPONENTS Interpreter)
set(WA_ANDROID_NDK_BIN
    "$ENV{HOME}/Library/Android/sdk/ndk/29.0.13846066/toolchains/llvm/prebuilt/darwin-x86_64/bin"
    CACHE PATH "Directory containing the Android NDK 29 Clang drivers")
set(ndk_jni_header "${WA_ANDROID_NDK_BIN}/../sysroot/usr/include/jni.h")
set(oracle_source "${CMAKE_CURRENT_LIST_DIR}/ndk_layout_oracle.c")
set(ffi_include "${wa_root}/tools/ffi_gen")

set(oracle_drivers aarch64-linux-android21-clang armv7a-linux-androideabi21-clang
    x86_64-linux-android21-clang)
set(oracle_profiles AARCH64 ARMV7 X86_64)
foreach(driver profile IN ZIP_LISTS oracle_drivers oracle_profiles)
    set(cc "${WA_ANDROID_NDK_BIN}/${driver}")
    if(NOT EXISTS "${cc}")
        message(FATAL_ERROR "Missing NDK 29 driver ${cc}. Set WA_ANDROID_NDK_BIN.")
    endif()
    set(object "${CMAKE_CURRENT_BINARY_DIR}/ndk_layout_${profile}.o")
    set(depfile "${object}.d")
    add_custom_command(OUTPUT "${object}"
        COMMAND "${cc}" -std=c11 -Wall -Wextra -Werror
            "-DWA_FFI_ORACLE_${profile}=1" "-I${ffi_include}"
            -MD -MF "${depfile}" -c "${oracle_source}" -o "${object}"
        DEPENDS "${oracle_source}" "${ffi_include}/generated/abi_layouts.h"
            "${ffi_include}/generated/abi_layout_oracle.inc"
        DEPFILE "${depfile}"
        COMMENT "Checking NDK 29 ABI with ${driver}"
        VERBATIM)
    list(APPEND oracle_objects "${object}")
endforeach()
add_custom_target(wa_m2_ndk_layout_oracles DEPENDS ${oracle_objects})

add_executable(test_m2_ffi_thunks test_m2_ffi_thunks.cpp)
target_link_libraries(test_m2_ffi_thunks PRIVATE wa_ffi_thunks)
add_dependencies(test_m2_ffi_thunks wa_m2_ndk_layout_oracles)

if(EMSCRIPTEN)
    add_library(wa_m2_wasm_layout_oracle OBJECT "${oracle_source}")
    target_include_directories(wa_m2_wasm_layout_oracle PRIVATE "${ffi_include}")
    target_compile_features(wa_m2_wasm_layout_oracle PRIVATE c_std_11)
    target_compile_definitions(wa_m2_wasm_layout_oracle PRIVATE WA_FFI_ORACLE_WASM32=1
        "WA_NDK_JNI_HEADER=\"${ndk_jni_header}\"")
    target_compile_options(wa_m2_wasm_layout_oracle PRIVATE -Wall -Wextra -Werror)
    add_dependencies(test_m2_ffi_thunks wa_m2_wasm_layout_oracle)

    find_program(WA_NODE_EXECUTABLE NAMES node REQUIRED)
    set_property(TARGET test_m2_ffi_thunks PROPERTY CROSSCOMPILING_EMULATOR
        "${WA_NODE_EXECUTABLE}")
    target_link_options(test_m2_ffi_thunks PRIVATE -sENVIRONMENT=node
        -sALLOW_MEMORY_GROWTH=1 -sEXIT_RUNTIME=1)
endif()

add_test(NAME m2_ffi_thunks COMMAND test_m2_ffi_thunks)
add_test(NAME m2_ffi_generated_headers COMMAND "${Python3_EXECUTABLE}"
    "${ffi_include}/generate_thunks.py" --check)

if(WA_M2_FFI_TEST_DBT)
    set(guest_source "${CMAKE_CURRENT_LIST_DIR}/jni_guest.c")
    set(guest_library "${CMAKE_CURRENT_BINARY_DIR}/libwa_jni_probe.so")
    add_custom_command(OUTPUT "${guest_library}"
        COMMAND "${WA_ANDROID_NDK_BIN}/aarch64-linux-android21-clang" -std=c11
            -O2 -fPIC -fvisibility=hidden -fno-stack-protector -nostdlib -shared
            -Wl,--no-undefined "-Wl,-soname,libwa_jni_probe.so"
            "${guest_source}" -o "${guest_library}"
        DEPENDS "${guest_source}" "${ndk_jni_header}"
        COMMENT "Building the NDK 29 ARM64 JNI guest"
        VERBATIM)
    add_custom_target(wa_m2_jni_guest DEPENDS "${guest_library}")
    add_executable(test_m2_jni_guest test_jni_guest.cpp)
    target_link_libraries(test_m2_jni_guest PRIVATE wa_jni_dbt)
    add_dependencies(test_m2_jni_guest wa_m2_jni_guest)
    if(EMSCRIPTEN)
        set_property(TARGET test_m2_jni_guest PROPERTY CROSSCOMPILING_EMULATOR
            "${WA_NODE_EXECUTABLE}")
        target_link_options(test_m2_jni_guest PRIVATE -sENVIRONMENT=node -sNODERAWFS=1
            -sALLOW_MEMORY_GROWTH=1 -sEXIT_RUNTIME=1)
    endif()
    add_test(NAME m2_ffi_jni_guest COMMAND test_m2_jni_guest "${guest_library}")
endif()
