cmake_minimum_required(VERSION 3.20)

if(CMAKE_CURRENT_SOURCE_DIR STREQUAL CMAKE_SOURCE_DIR)
    project(wa_m4_aosp17_binder LANGUAGES CXX)
    include(CTest)
endif()

get_filename_component(_wa_root "../.." ABSOLUTE BASE_DIR "${CMAKE_CURRENT_SOURCE_DIR}")
include("${_wa_root}/cmake/Aosp17Binder.cmake")

find_program(WA_AIDL_EXECUTABLE NAMES aidl
    HINTS "$ENV{ANDROID_HOME}/build-tools/36.0.0"
          "$ENV{ANDROID_SDK_ROOT}/build-tools/36.0.0"
          "$ENV{HOME}/Library/Android/sdk/build-tools/36.0.0"
    REQUIRED)
set(_wa_aidl_package org/wasmandroid/test)
set(_wa_aidl_input "${CMAKE_CURRENT_SOURCE_DIR}/${_wa_aidl_package}/ICalcService.aidl")
set(_wa_aidl_gen "${CMAKE_CURRENT_BINARY_DIR}/aidl")
set(_wa_aidl_raw "${_wa_aidl_gen}/src/${_wa_aidl_package}/ICalcService.cpp")
set(_wa_aidl_cpp "${_wa_aidl_gen}/aosp17/${_wa_aidl_package}/ICalcService.cpp")
set(_wa_aidl_headers "${_wa_aidl_gen}/include/${_wa_aidl_package}")

add_custom_command(
    OUTPUT "${_wa_aidl_cpp}"
           "${_wa_aidl_headers}/ICalcService.h"
           "${_wa_aidl_headers}/BnCalcService.h"
           "${_wa_aidl_headers}/BpCalcService.h"
    BYPRODUCTS "${_wa_aidl_raw}"
    COMMAND "${WA_AIDL_EXECUTABLE}" --lang=cpp --omit_invocation
            "-I${CMAKE_CURRENT_SOURCE_DIR}" "-o${_wa_aidl_gen}/src" "-h${_wa_aidl_gen}/include"
            "${_wa_aidl_input}"
    COMMAND "${CMAKE_COMMAND}" "-DINPUT=${_wa_aidl_raw}" "-DOUTPUT=${_wa_aidl_cpp}"
            -P "${CMAKE_CURRENT_SOURCE_DIR}/Aidl36Compatibility.cmake"
    DEPENDS "${_wa_aidl_input}" "${WA_AIDL_EXECUTABLE}"
            "${CMAKE_CURRENT_SOURCE_DIR}/Aidl36Compatibility.cmake"
    VERBATIM)

add_executable(test_aosp17_binder test_aosp17_binder.cpp "${_wa_aidl_cpp}")
target_include_directories(test_aosp17_binder PRIVATE "${_wa_aidl_gen}/include")
target_link_libraries(test_aosp17_binder PRIVATE aosp17_libbinder)

if(EMSCRIPTEN)
    set_target_properties(test_aosp17_binder PROPERTIES SUFFIX ".mjs")
    target_compile_options(test_aosp17_binder PRIVATE -O3)
    target_link_options(test_aosp17_binder PRIVATE
        -O3 --no-entry -sMODULARIZE=1 -sEXPORT_ES6=1 -sENVIRONMENT=web,worker,node
        -sPTHREAD_POOL_SIZE=3 -sINITIAL_MEMORY=33554432 -sMAXIMUM_MEMORY=33554432 -sSTACK_SIZE=262144
        -sASSERTIONS=1 -sINCOMING_MODULE_JS_API=wasmMemory,instantiateWasm
        --pre-js "${CMAKE_CURRENT_SOURCE_DIR}/binder_imports.js"
        --js-library "${CMAKE_CURRENT_SOURCE_DIR}/binder_library.js")
    set_property(TARGET test_aosp17_binder APPEND PROPERTY LINK_DEPENDS
        "${CMAKE_CURRENT_SOURCE_DIR}/binder_imports.js"
        "${CMAKE_CURRENT_SOURCE_DIR}/binder_library.js")

    # Run the same M0 ring implementation in a separate imported memory, as
    # web/workers/vproc_worker.mjs does for the vproc host bridge.
    file(GENERATE OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/ring_anchor.cpp"
        CONTENT "// The linker exports the M0 ring functions from wa_libkern.\n")
    add_executable(m4_binder_ring "${CMAKE_CURRENT_BINARY_DIR}/ring_anchor.cpp")
    set_target_properties(m4_binder_ring PROPERTIES SUFFIX ".wasm")
    target_link_libraries(m4_binder_ring PRIVATE wa_libkern)
    target_link_options(m4_binder_ring PRIVATE
        -O3 -matomics -mbulk-memory -msimd128 -sSHARED_MEMORY=1 -sIMPORTED_MEMORY=1
        -sINITIAL_MEMORY=16777216 -sMAXIMUM_MEMORY=16777216 -sSTACK_SIZE=65536 -sFILESYSTEM=0
        --no-entry)
    foreach(_wa_export __heap_base __stack_pointer wa_binder_ring_attach wa_binder_ring_reserve
            wa_binder_ring_commit wa_binder_ring_claim wa_binder_ring_release)
        target_link_options(m4_binder_ring PRIVATE "-Wl,--export=${_wa_export}")
    endforeach()
    add_dependencies(test_aosp17_binder m4_binder_ring)

    if(BUILD_TESTING)
        find_program(WA_BINDER_NODE_EXECUTABLE NAMES node HINTS /opt/homebrew/bin REQUIRED)
        add_test(NAME m4_aosp17_binder_wasm
            COMMAND "${WA_BINDER_NODE_EXECUTABLE}"
                "${CMAKE_CURRENT_SOURCE_DIR}/run_binder_e2e.mjs" --node
                --build-dir "$<TARGET_FILE_DIR:test_aosp17_binder>")
        set_tests_properties(m4_aosp17_binder_wasm PROPERTIES TIMEOUT 60)
    endif()
elseif(BUILD_TESTING)
    add_test(NAME m4_aosp17_binder_host COMMAND test_aosp17_binder)
    set_tests_properties(m4_aosp17_binder_host PROPERTIES TIMEOUT 30)
endif()
