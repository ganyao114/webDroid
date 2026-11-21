cmake_minimum_required(VERSION 3.20)
project(wa_hal_selftest LANGUAGES C CXX)

add_subdirectory(../../runtime/hal_browser hal_browser)

if(EMSCRIPTEN)
    add_executable(hal_selftest hal_selftest.cpp cube_demo.c)
    target_compile_features(hal_selftest PRIVATE c_std_11 cxx_std_20)
    target_include_directories(hal_selftest PRIVATE wasm_compat)
    target_compile_options(hal_selftest PRIVATE -msimd128 -Wall -Wextra)
    target_link_libraries(hal_selftest PRIVATE wa_hal_browser)
    set_target_properties(hal_selftest PROPERTIES SUFFIX ".mjs")
    target_link_options(hal_selftest PRIVATE
        --no-entry
        -msimd128
        -sMODULARIZE=1
        -sEXPORT_ES6=1
        -sENVIRONMENT=worker
        -sABORTING_MALLOC=0
        -sEXPORTED_RUNTIME_METHODS=HEAPU8,HEAPF32
        "-sEXPORTED_FUNCTIONS=_malloc,_free,_wa_gfx_resize,_wa_gfx_read_pixels_rgba,_wa_audio_ring_write,_demo_generate_audio,_demo_render_frame"
    )
else()
    enable_testing()
    find_package(Threads REQUIRED)
    add_executable(wa_audio_ring_test audio_ring_test.cpp)
    target_compile_features(wa_audio_ring_test PRIVATE cxx_std_20)
    target_link_libraries(wa_audio_ring_test PRIVATE wa_hal_browser Threads::Threads)
    add_test(NAME wa_audio_ring COMMAND wa_audio_ring_test)
    set_tests_properties(wa_audio_ring PROPERTIES TIMEOUT 15)

    add_executable(wa_host_bridge_test host_bridge_test.cpp)
    target_compile_features(wa_host_bridge_test PRIVATE cxx_std_20)
    target_link_libraries(wa_host_bridge_test PRIVATE wa_hal_browser)
    add_test(NAME wa_host_bridge COMMAND wa_host_bridge_test)
endif()
