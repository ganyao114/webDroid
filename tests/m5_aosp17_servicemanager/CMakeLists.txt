cmake_minimum_required(VERSION 3.20)

if(CMAKE_CURRENT_SOURCE_DIR STREQUAL CMAKE_SOURCE_DIR)
    project(wa_m5_aosp17_servicemanager LANGUAGES C CXX)
    include(CTest)
endif()

include("${CMAKE_CURRENT_SOURCE_DIR}/../../cmake/Aosp17ServiceManager.cmake")

set(m4_binder_fixture "${CMAKE_CURRENT_SOURCE_DIR}/../m4_aosp17_binder")
add_executable(test_aosp17_servicemanager test_aosp17_servicemanager.cpp)
target_link_libraries(test_aosp17_servicemanager PRIVATE aosp17_servicemanager)
aosp17_servicemanager_generate_aidl(test_aosp17_servicemanager PRIVATE
    "${m4_binder_fixture}" org/wasmandroid/test ICalcService)

if(EMSCRIPTEN)
    set_target_properties(test_aosp17_servicemanager PROPERTIES SUFFIX ".mjs")
    target_compile_options(test_aosp17_servicemanager PRIVATE -O3)
    target_link_options(test_aosp17_servicemanager PRIVATE
        -O3 --no-entry -sMODULARIZE=1 -sEXPORT_ES6=1 -sENVIRONMENT=web,worker,node
        -sINITIAL_MEMORY=16777216 -sMAXIMUM_MEMORY=16777216 -sSTACK_SIZE=262144
        -sASSERTIONS=1 -sINCOMING_MODULE_JS_API=wasmMemory,instantiateWasm
        --pre-js "${m4_binder_fixture}/binder_imports.js"
        --js-library "${m4_binder_fixture}/binder_library.js")
    set_property(TARGET test_aosp17_servicemanager APPEND PROPERTY LINK_DEPENDS
        "${m4_binder_fixture}/binder_imports.js" "${m4_binder_fixture}/binder_library.js")

    # The same module also supplies the M0 ring operations in the ring's own memory.
    foreach(symbol __heap_base __stack_pointer wa_binder_ring_attach wa_binder_ring_reserve
            wa_binder_ring_commit wa_binder_ring_claim wa_binder_ring_release)
        target_link_options(test_aosp17_servicemanager PRIVATE "-Wl,--export=${symbol}")
    endforeach()
endif()

if(BUILD_TESTING)
    if(EMSCRIPTEN)
        find_program(WA_BINDER_NODE_EXECUTABLE NAMES node HINTS /opt/homebrew/bin
            NO_CMAKE_FIND_ROOT_PATH REQUIRED)
        add_test(NAME test_aosp17_servicemanager
            COMMAND "${WA_BINDER_NODE_EXECUTABLE}"
                "${CMAKE_CURRENT_SOURCE_DIR}/run_m5_sm_e2e.mjs" --node
                --build-dir "$<TARGET_FILE_DIR:test_aosp17_servicemanager>")
    else()
        add_test(NAME test_aosp17_servicemanager COMMAND test_aosp17_servicemanager)
    endif()
    set_tests_properties(test_aosp17_servicemanager PROPERTIES TIMEOUT 60)
endif()
