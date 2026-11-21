get_filename_component(wa_default_aosp17 "${CMAKE_CURRENT_SOURCE_DIR}/../../third_party/aosp17"
    ABSOLUTE)
set(AOSP17_SOURCE_DIR "${wa_default_aosp17}" CACHE PATH "Read-only AOSP android-17.0.0_r1 source tree")

if(IS_DIRECTORY "${AOSP17_SOURCE_DIR}")
    include(../../cmake/Aosp17Core.cmake)
    include(../../cmake/Aosp17ArtHGraph.cmake)
    # Compile these translation units with AOSP's headers, independently of SwiftVM's fmt.
    add_library(wa_android_system_apk OBJECT apk_loader.cpp apk_dex.cpp)
    target_include_directories(wa_android_system_apk PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/../../include")
    target_link_libraries(wa_android_system_apk PRIVATE aosp17_libandroidfw aosp17_art_hgraph)
endif()

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
    if(TARGET wa_android_system_apk)
        target_link_libraries(android_system PRIVATE wa_android_system_apk)
        # Android's liblog supplies these C symbols in the unified runtime. The
        # HAL's NDK routes still use wa_android_log for guest console output.
        target_compile_definitions(wa_hal_browser PRIVATE
            __android_log_write=wa_hal_log_write __android_log_print=wa_hal_log_print)
        list(APPEND exports
            _wa_system_open_apk _wa_system_close_apk _wa_system_apk_info
            _wa_system_apk_compile_dex _wa_system_apk_dex_stats
            _wa_system_apk_load_native _wa_system_apk_set_backing_file)
    endif()
    list(JOIN exports "," exports)
    target_link_options(android_system PRIVATE "-sEXPORTED_FUNCTIONS=${exports}")
else()
    add_library(wa_android_system STATIC system_entry.cpp)
    target_include_directories(wa_android_system PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
    target_link_libraries(wa_android_system PUBLIC wa_libkern wa_linker_ffi
        PRIVATE wa_hal_browser svm_wasm svm_arm32_numeric svm_arm64_numeric)
    if(TARGET wa_android_system_apk)
        target_link_libraries(wa_android_system PRIVATE wa_android_system_apk)
    endif()
endif()

if(BUILD_TESTING AND TARGET wa_android_system_apk)
    add_subdirectory(../../tests/m6_apk_chrome_e2e "${CMAKE_BINARY_DIR}/tests/m6_apk_chrome_e2e")
endif()
