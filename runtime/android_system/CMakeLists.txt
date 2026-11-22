get_filename_component(wa_default_aosp17 "${CMAKE_CURRENT_SOURCE_DIR}/../../third_party/aosp17"
    ABSOLUTE)
set(AOSP17_SOURCE_DIR "${wa_default_aosp17}" CACHE PATH "Read-only AOSP android-17.0.0_r1 source tree")

if(IS_DIRECTORY "${AOSP17_SOURCE_DIR}")
    include(../../cmake/Aosp17Core.cmake)
    include(../../cmake/Aosp17ArtHGraph.cmake)
    include(../../cmake/Aosp17UiFramework.cmake)
    include(BinderRuntime.cmake)
    include(UpstreamLauncher.cmake)
    option(WA_VERIFY_LAUNCHER_DEX "Verify the system APK DEX files during Launcher boot" ON)
    # Compile AOSP translation units independently of SwiftVM's fmt.
    add_library(wa_android_system_apk OBJECT apk_loader.cpp apk_dex.cpp)
    target_include_directories(wa_android_system_apk PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/../../include")
    target_link_libraries(wa_android_system_apk PRIVATE aosp17_libandroidfw aosp17_art_hgraph)
    add_library(wa_android_system_binder OBJECT binder_service_entry.cpp)
    target_link_libraries(wa_android_system_binder PRIVATE
        aosp17_servicemanager aosp17_libbinder aosp17_libandroidfw)
    aosp17_servicemanager_generate_aidl(wa_android_system_binder PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/aidl" android/wasm
        PackageRecord IPackageManagerLite ActivityState IActivityManagerLite)

    find_program(WA_JAR_EXECUTABLE jar NO_CMAKE_FIND_ROOT_PATH REQUIRED)
    get_target_property(wa_framework_classes aosp17_framework_core WA_FRAMEWORK_CLASSES)
    cmake_path(CONVERT "${wa_framework_classes};${WA_ANDROID_JAR}" TO_NATIVE_PATH_LIST wa_systemui_classpath)
    set(wa_systemui_host "${CMAKE_CURRENT_BINARY_DIR}/systemui-host")
    set(wa_systemui_host_dex "${wa_systemui_host}/systemui_host_dex.inc")
    add_custom_command(OUTPUT "${wa_systemui_host_dex}"
        COMMAND "${CMAKE_COMMAND}" -E make_directory "${wa_systemui_host}/classes"
        COMMAND "${WA_JAVAC_EXECUTABLE}" --release 17 -proc:none -encoding UTF-8
            -classpath "${wa_systemui_classpath}" -d "${wa_systemui_host}/classes"
            "${CMAKE_CURRENT_SOURCE_DIR}/LauncherSystemUi.java"
        COMMAND "${WA_JAR_EXECUTABLE}" cf "${wa_systemui_host}/host.jar"
            -C "${wa_systemui_host}/classes" .
        COMMAND "${WA_D8_EXECUTABLE}" --release --min-api 26 --lib "${WA_ANDROID_JAR}"
            --classpath "${wa_framework_classes}" --output "${wa_systemui_host}" "${wa_systemui_host}/host.jar"
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/embed_systemui_host.py"
            "${wa_systemui_host}/classes.dex" "${wa_systemui_host_dex}"
        DEPENDS aosp17_framework_core "${wa_framework_classes}" "${WA_ANDROID_JAR}"
            LauncherSystemUi.java embed_systemui_host.py
            "${WA_JAVAC_EXECUTABLE}" "${WA_JAR_EXECUTABLE}" "${WA_D8_EXECUTABLE}"
        COMMENT "Compiling the window adapter for the upstream SystemUI APK"
        VERBATIM)
    set(wa_launcher_runtime_sources art_sf_entry.cpp launcher_session_entry.cpp "${wa_systemui_host_dex}")
    add_library(wa_android_system_foreground STATIC foreground_activity_entry.cpp)
    target_include_directories(wa_android_system_foreground PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/../../include")
    target_link_libraries(wa_android_system_foreground PUBLIC wa_hal_surfaceflinger
        PRIVATE wa_android_system_apk aosp17_ui_jni aosp17_libandroidfw)

    set(WA_BUILTIN_APPS_DIR "${CMAKE_CURRENT_BINARY_DIR}/upstream-apps")
    set(wa_builtin_outputs "${WA_BUILTIN_APPS_DIR}/upstream_apps_provenance.json")
    foreach(name IN ITEMS DeskClock Browser2 Music Gallery2)
        list(APPEND wa_builtin_outputs "${WA_BUILTIN_APPS_DIR}/${name}-upstream.apk")
    endforeach()
    file(GLOB wa_builtin_patches CONFIGURE_DEPENDS
        "../../patches/aosp17/deskclock/*.patch" "../../patches/aosp17/browser2/*.patch"
        "../../patches/aosp17/music/*.patch" "../../patches/aosp17/gallery2/*.patch")
    set(wa_builtin_driver "${CMAKE_CURRENT_SOURCE_DIR}/../../tools/aosp/build_upstream_apps.py")
    add_custom_command(OUTPUT ${wa_builtin_outputs}
        COMMAND "${Python3_EXECUTABLE}" -B "${wa_builtin_driver}"
            --aosp-root "${AOSP17_SOURCE_DIR}" --deps "${WA_SYSTEMUI_DEPS_DIR}"
            --sdk "${WA_ANDROID_SDK}" --java "${WA_JAVA_EXECUTABLE}" --javac "${WA_JAVAC_EXECUTABLE}"
            --out-dir "${WA_BUILTIN_APPS_DIR}" --depfile "${WA_BUILTIN_APPS_DIR}/apps.d"
        DEPENDS "${wa_builtin_driver}" ${wa_builtin_patches}
        DEPFILE "${WA_BUILTIN_APPS_DIR}/apps.d"
        COMMENT "Compiling and signing the AOSP 17 built-in APKs"
        VERBATIM)
    add_custom_target(wa_upstream_builtin_apps DEPENDS ${wa_builtin_outputs})
    add_custom_target(wa_foreground_framework_assets
        DEPENDS "${WA_LAUNCHER_FRAMEWORK_DEX}" "${WA_LAUNCHER_FRAMEWORK_RES}")
    add_dependencies(wa_foreground_framework_assets aosp17_framework_core)
endif()

if(EMSCRIPTEN)
    add_executable(android_system system_entry.cpp)
    set_target_properties(android_system PROPERTIES SUFFIX ".mjs"
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
    target_link_libraries(android_system PRIVATE wa_libkern wa_linker_ffi wa_hal_browser svm_wasm
        svm_arm32_numeric svm_arm64_numeric)
    target_link_options(android_system PRIVATE
        --no-entry -O3 -flto=thin
        -sMODULARIZE=1 -sEXPORT_ES6=1 -sENVIRONMENT=web,worker,node
        -sUSE_WEBGL2=1 -sFULL_ES3=1 -sOFFSCREENCANVAS_SUPPORT=1 -sSHARED_MEMORY=1
        -sALLOW_MEMORY_GROWTH=0 -sINITIAL_MEMORY=268435456 -sSTACK_SIZE=1048576
        -sABORTING_MALLOC=0
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
        target_link_libraries(android_system PRIVATE wa_android_system_apk wa_android_system_binder
            wa_android_system_foreground)
        add_dependencies(android_system wa_upstream_launcher_assets)
        target_sources(android_system PRIVATE ${wa_launcher_runtime_sources})
        target_include_directories(android_system PRIVATE "${wa_systemui_host}")
        target_link_libraries(android_system PRIVATE aosp17_ui_jni aosp17_libandroidfw)
        target_compile_definitions(android_system PRIVATE WA_VERIFY_LAUNCHER_DEX=$<BOOL:${WA_VERIFY_LAUNCHER_DEX}>)
        target_compile_options(android_system PRIVATE -ffunction-sections -fdata-sections)
        target_compile_options(aosp17_art_interp PRIVATE -ffunction-sections -fdata-sections)
        # Accept caller-owned vproc memory through the Emscripten loader.
        # The ApkAssets path loader uses MEMFS for APKs mounted through the ART entry point.
        target_link_options(android_system PRIVATE -sIMPORTED_MEMORY=1)
        # Android's liblog supplies these C symbols in the unified runtime. The
        # HAL's NDK routes still use wa_android_log for guest console output.
        target_compile_definitions(wa_hal_browser PRIVATE
            __android_log_write=wa_hal_log_write __android_log_print=wa_hal_log_print)
        list(APPEND exports
            _wa_system_open_apk _wa_system_close_apk _wa_system_apk_info
            _wa_system_apk_compile_dex _wa_system_apk_dex_stats
            _wa_system_apk_load_native _wa_system_apk_set_backing_file)
        list(APPEND exports
            _wa_system_binder_attach _wa_system_servicemanager_start _wa_system_servicemanager_pump
            _wa_system_server_start _wa_system_server_pump
            _wa_system_client_install_apk _wa_system_client_query_launcher
            _wa_system_client_resolve_home_activity _wa_system_client_get_package_info
            _wa_system_client_start_activity _wa_system_client_start_home_activity
            _wa_system_client_get_top_activity _wa_system_client_finish_top_activity)
        list(APPEND exports
            _wa_system_art_create _wa_system_art_destroy _wa_system_art_load_dex
            _wa_system_art_register_resource_natives _wa_system_art_mount_apk
            _wa_system_art_call_static_int _wa_system_art_call_static_string _wa_system_art_call_static_void
            _wa_system_art_last_error _wa_system_art_stats_json _wa_system_art_gc_collect
            _wa_system_art_set_gc_paused _wa_system_art_set_inline_caches_enabled
            _wa_system_launcher_create _wa_system_launcher_destroy _wa_system_launcher_sf_ctx
            _wa_system_launcher_load_framework_dex _wa_system_launcher_load_framework_fallback_dex
            _wa_system_launcher_set_framework_res_apk
            _wa_system_launcher_install_apk _wa_system_launcher_load_systemui_apk
            _wa_system_launcher_boot_apk _wa_system_launcher_tick
            _wa_system_launcher_resize _wa_system_launcher_pause _wa_system_launcher_resume
            _wa_system_launcher_set_gesture_navigation _wa_system_launcher_consume_recents
            _wa_system_launcher_touch_navigation
            _wa_system_launcher_send_home _wa_system_launcher_send_back _wa_system_launcher_state_json
            _wa_system_launcher_open_all_apps
            _wa_system_launcher_consume_launch _wa_system_launcher_last_error
            _wa_system_app_create _wa_system_app_destroy _wa_system_app_sf_ctx
            _wa_system_app_load_framework_dex _wa_system_app_load_framework_fallback_dex
            _wa_system_app_set_framework_res_apk _wa_system_app_install_package_catalog_apk
            _wa_system_app_boot_apk _wa_system_app_tick _wa_system_app_resize
            _wa_system_app_send_back _wa_system_app_state_json _wa_system_app_last_error
            _wa_system_sf_init _wa_system_sf_ctx _wa_system_sf_destroy
            _wa_sf_create _wa_sf_destroy _wa_sf_begin _wa_sf_set_layer _wa_sf_remove_layer
            _wa_sf_commit _wa_sf_abort _wa_sf_frame_acquire _wa_sf_frame_release
            _wa_sf_frame_layers _wa_sf_frame_list _wa_sf_bitmaps
            _wa_bitmap_put _wa_bitmap_remove _wa_bitmap_acquire _wa_bitmap_release _wa_bitmap_pixels
            _wa_sf_input _wa_sf_vsync _wa_sf_vsync_read _wa_sf_vsync_write _wa_sf_font_metrics
            _wa_input_push _wa_input_poll
            _wa_sf_codec_requests _wa_sf_codec_events _wa_sf_webview_requests _wa_sf_webview_events
            _wa_canvas_begin _wa_canvas_draw_color _wa_canvas_end)
    else()
        target_link_options(android_system PRIVATE -sFILESYSTEM=0)
    endif()
    list(JOIN exports "," exports)
    target_link_options(android_system PRIVATE "-sEXPORTED_FUNCTIONS=${exports}")
else()
    add_library(wa_android_system STATIC system_entry.cpp)
    target_include_directories(wa_android_system PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
    target_link_libraries(wa_android_system PUBLIC wa_libkern wa_linker_ffi
        PRIVATE wa_hal_browser svm_wasm svm_arm32_numeric svm_arm64_numeric)
    if(TARGET wa_android_system_apk)
        target_link_libraries(wa_android_system PRIVATE wa_android_system_apk wa_android_system_binder
            wa_android_system_foreground)
        target_sources(wa_android_system PRIVATE ${wa_launcher_runtime_sources})
        target_include_directories(wa_android_system PRIVATE "${wa_systemui_host}")
        target_compile_definitions(wa_android_system PRIVATE WA_VERIFY_LAUNCHER_DEX=$<BOOL:${WA_VERIFY_LAUNCHER_DEX}>)
        target_link_libraries(wa_android_system PRIVATE aosp17_ui_jni aosp17_libandroidfw
            PUBLIC wa_hal_surfaceflinger)
    endif()
endif()

if(BUILD_TESTING AND TARGET wa_android_system_apk)
    add_subdirectory(../../tests/m6_apk_chrome_e2e "${CMAKE_BINARY_DIR}/tests/m6_apk_chrome_e2e")
    add_subdirectory(../../tests/m7_system_binder_e2e "${CMAKE_BINARY_DIR}/tests/m7_system_binder_e2e")
    if(EMSCRIPTEN OR NOT EXISTS "${CMAKE_SOURCE_DIR}/third_party/aosp17/frameworks/base/libs/androidfw")
        add_subdirectory(../../tests/m7_aosp17_art_interp "${CMAKE_BINARY_DIR}/tests/m7_aosp17_art_interp")
    endif()
    add_subdirectory(../../tests/m8_art_perf_and_unified "${CMAKE_BINARY_DIR}/tests/m8_art_perf_and_unified")
    if(EMSCRIPTEN OR NOT EXISTS "${CMAKE_SOURCE_DIR}/third_party/aosp17/frameworks/base/libs/androidfw")
        add_subdirectory(../../tests/m8_aosp17_ui_framework "${CMAKE_BINARY_DIR}/tests/m8_aosp17_ui_framework")
        add_subdirectory(../../tests/m8_aosp17_launcher3_apk "${CMAKE_BINARY_DIR}/tests/m8_aosp17_launcher3_apk")
    endif()
    add_subdirectory(../../tests/m9_aosp17_launcher3_boot "${CMAKE_BINARY_DIR}/tests/m9_aosp17_launcher3_boot")
    add_subdirectory(../../tests/m14_upstream_apps "${CMAKE_BINARY_DIR}/tests/m14_upstream_apps")
endif()
