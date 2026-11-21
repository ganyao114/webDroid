cmake_minimum_required(VERSION 3.18)

if(CMAKE_CURRENT_SOURCE_DIR STREQUAL CMAKE_SOURCE_DIR)
    project(wa_m2_binder_posix LANGUAGES CXX)
    if(NOT EMSCRIPTEN)
        enable_testing()
        add_subdirectory(../m0_libkern m0_libkern)
    endif()
endif()

if(NOT TARGET wa_binder_ipc)
    add_subdirectory(../../runtime/binder_ipc binder_ipc)
endif()

if(EMSCRIPTEN)
    add_executable(m2_binder m2_binder_wasm.cpp)
    set_target_properties(m2_binder PROPERTIES SUFFIX ".wasm")
    target_link_libraries(m2_binder PRIVATE wa_binder_ipc)
    target_compile_options(m2_binder PRIVATE -O3 -matomics -mbulk-memory -msimd128)
    target_link_options(m2_binder PRIVATE
        -O3 -matomics -mbulk-memory -msimd128
        -sSHARED_MEMORY=1 -sIMPORTED_MEMORY=1 -sFILESYSTEM=0 -sABORTING_MALLOC=0
        -sINITIAL_MEMORY=33554432 -sMAXIMUM_MEMORY=33554432 -sSTACK_SIZE=262144
        --no-entry
    )
    foreach(symbol IN ITEMS
        __heap_base __stack_pointer
        wa_binder_ring_attach wa_binder_ring_reserve wa_binder_ring_commit
        wa_binder_ring_claim wa_binder_ring_release
        m2_init m2_set_marker m2_get_marker m2_manager_run m2_service_register m2_service_run
        m2_client_queue m2_client_finish m2_posix_smoke
    )
        target_link_options(m2_binder PRIVATE "-Wl,--export=${symbol}")
    endforeach()
else()
    add_executable(test_m2_binder_posix test_m2_binder_posix.cpp)
    target_link_libraries(test_m2_binder_posix PRIVATE wa_binder_ipc)
    add_test(NAME test_m2_binder_posix COMMAND test_m2_binder_posix)
    set_tests_properties(test_m2_binder_posix PROPERTIES TIMEOUT 45)
endif()
