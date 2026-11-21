cmake_minimum_required(VERSION 3.20)
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    project(M7SurfaceFlinger LANGUAGES C)
    include(CTest)
endif()
if(NOT TARGET wa_hal_surfaceflinger)
    add_subdirectory("${CMAKE_CURRENT_LIST_DIR}/../../runtime/hal_surfaceflinger" hal_surfaceflinger)
endif()

if(EMSCRIPTEN)
    find_program(WA_M7_NODE node HINTS /opt/homebrew/bin NO_CMAKE_FIND_ROOT_PATH REQUIRED)
    add_executable(m7_sf_demo m7_sf_demo.c)
    set_target_properties(m7_sf_demo PROPERTIES SUFFIX ".mjs")
    target_link_libraries(m7_sf_demo PRIVATE wa_hal_surfaceflinger)
    target_compile_options(m7_sf_demo PRIVATE -O3 -pthread)
    target_link_options(m7_sf_demo PRIVATE -O3 -pthread --no-entry
        -sMODULARIZE=1 -sEXPORT_ES6=1 -sENVIRONMENT=web,worker,node
        -sINITIAL_MEMORY=67108864 -sMALLOC=emmalloc -sFILESYSTEM=0
        "-sEXPORTED_RUNTIME_METHODS=['HEAPU8']"
        "-sEXPORTED_FUNCTIONS=['_m7_demo_create','_m7_demo_tick','_m7_demo_state','_m7_demo_destroy','_wa_sf_frame_acquire','_wa_sf_frame_release','_wa_sf_bitmaps','_wa_bitmap_acquire','_wa_bitmap_release','_wa_sf_font_metrics','_wa_sf_input','_wa_sf_vsync','_wa_sf_codec_requests','_wa_sf_codec_events','_wa_sf_webview_requests','_wa_sf_webview_events']")
    if(BUILD_TESTING)
        add_test(NAME test_m7_sf_node COMMAND "${WA_M7_NODE}"
            "${CMAKE_CURRENT_LIST_DIR}/verify_m7_sf_node.mjs" "$<TARGET_FILE:m7_sf_demo>")
        set_tests_properties(test_m7_sf_node PROPERTIES TIMEOUT 60)
    endif()
elseif(BUILD_TESTING)
    find_package(Threads REQUIRED)
    add_executable(test_hal_surfaceflinger test_hal_surfaceflinger.c)
    target_link_libraries(test_hal_surfaceflinger PRIVATE wa_hal_surfaceflinger Threads::Threads)
    add_test(NAME test_hal_surfaceflinger COMMAND test_hal_surfaceflinger)
    set_tests_properties(test_hal_surfaceflinger PROPERTIES TIMEOUT 60)
endif()
