if(EMSCRIPTEN)
    add_executable(android_system system_entry.cpp)
    set_target_properties(android_system PROPERTIES SUFFIX ".mjs"
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
    target_link_libraries(android_system PRIVATE wa_libkern wa_linker_ffi wa_hal_browser svm_wasm
        svm_arm32_numeric svm_arm64_numeric)
    target_link_options(android_system PRIVATE
        --no-entry -O3 -flto=thin
        -sMODULARIZE=1 -sEXPORT_ES6=1 -sENVIRONMENT=web,worker
        -sUSE_WEBGL2=1 -sFULL_ES3=1 -sOFFSCREENCANVAS_SUPPORT=1 -sSHARED_MEMORY=1
        -sALLOW_MEMORY_GROWTH=0 -sINITIAL_MEMORY=67108864 -sSTACK_SIZE=1048576
        -sABORTING_MALLOC=0 -sFILESYSTEM=0
        -sEXPORTED_RUNTIME_METHODS=HEAPU8,wasmMemory
    )
    set(exports
        _malloc _free _wa_dbt_free_buffer
        _wa_system_load _wa_system_destroy _wa_system_demo_init _wa_system_demo_render_frame
        _wa_system_demo_generate_audio _wa_system_bind_audio _wa_system_map_memory
        _wa_system_read_memory _wa_system_read_pixels _wa_system_compile _wa_system_get_stats
        _wa_system_jit_begin_render _wa_system_jit_handle_exit _wa_system_jit_cancel_render
        _wa_system_jit_compile_pc _wa_system_jit_numeric_lambda
        _wa_system_mmu_read8 _wa_system_mmu_read16 _wa_system_mmu_read32 _wa_system_mmu_read64
        _wa_system_mmu_write8 _wa_system_mmu_write16 _wa_system_mmu_write32 _wa_system_mmu_write64
        _wa_jit_registry_init _wa_jit_registry_lookup _wa_jit_registry_reserve
        _wa_jit_registry_publish
    )
    list(JOIN exports "," exports)
    target_link_options(android_system PRIVATE "-sEXPORTED_FUNCTIONS=${exports}")
else()
    add_library(wa_android_system STATIC system_entry.cpp)
    target_include_directories(wa_android_system PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
    target_link_libraries(wa_android_system PUBLIC wa_libkern wa_linker_ffi
        PRIVATE wa_hal_browser svm_wasm svm_arm32_numeric svm_arm64_numeric)
endif()
