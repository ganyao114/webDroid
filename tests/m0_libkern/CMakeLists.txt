cmake_minimum_required(VERSION 3.18)
project(wa_m0_libkern LANGUAGES CXX)

add_subdirectory(../../runtime/libkern-wasm libkern)

if(EMSCRIPTEN)
    add_executable(m0_selftest m0_selftest_wasm.cpp)
    set_target_properties(m0_selftest PROPERTIES SUFFIX ".wasm")
    target_link_libraries(m0_selftest PRIVATE wa_libkern)
    target_compile_options(m0_selftest PRIVATE -O3 -sSHARED_MEMORY=1 -matomics -mbulk-memory -msimd128)
    target_link_options(m0_selftest PRIVATE
        -O3 -sSHARED_MEMORY=1 -matomics -mbulk-memory -msimd128
        -sIMPORTED_MEMORY=1 -sINITIAL_MEMORY=16777216 -sMAXIMUM_MEMORY=16777216
        -sSTACK_SIZE=65536 -sFILESYSTEM=0 --no-entry
    )
    foreach(symbol IN ITEMS
        __heap_base __stack_pointer
        wa_syscall_set_arch wa_syscall wa_tier0
        wa_futex_wait wa_futex_wake wa_eventfd wa_pipe wa_epoll_create wa_epoll_ctl wa_epoll_wait
        wa_binder_init_ring wa_binder_send wa_binder_recv
        wa_binder_ring_attach wa_binder_ring_reserve wa_binder_ring_commit
        wa_binder_ring_claim wa_binder_ring_reply wa_binder_ring_release
        wa_vfs_open wa_vfs_read wa_vfs_write wa_vfs_pread wa_vfs_pwrite wa_vfs_ftruncate
        wa_vfs_fsync wa_vfs_fstat wa_vfs_flock wa_vfs_close
        wa_jit_registry_init wa_jit_registry_lookup wa_jit_registry_reserve wa_jit_registry_publish
    )
        target_link_options(m0_selftest PRIVATE "-Wl,--export=${symbol}")
    endforeach()
else()
    enable_testing()
    add_executable(test_libkern_host test_libkern_host.cpp)
    target_link_libraries(test_libkern_host PRIVATE wa_libkern)
    add_test(NAME m0_libkern_host COMMAND test_libkern_host)
    set_tests_properties(m0_libkern_host PROPERTIES TIMEOUT 30)
endif()
