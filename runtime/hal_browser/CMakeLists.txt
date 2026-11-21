add_library(wa_hal_browser STATIC
    audio_ring.cpp
    egl_shim.cpp
    gfx_webgl2.cpp
    gles_host_bridge.cpp
    guest_log.cpp
)
target_compile_features(wa_hal_browser PRIVATE cxx_std_20)
target_include_directories(wa_hal_browser PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}"
    "${CMAKE_CURRENT_SOURCE_DIR}/../../include"
)

if(EMSCRIPTEN)
    target_compile_options(wa_hal_browser PUBLIC -pthread)
    target_link_options(wa_hal_browser INTERFACE
        -pthread
        -sMIN_WEBGL_VERSION=2
        -sMAX_WEBGL_VERSION=2
        -sOFFSCREENCANVAS_SUPPORT=1
        # In Emscripten 7, this enables the #canvas selector for a pre-transferred Module.canvas.
        -sDISABLE_DEPRECATED_FIND_EVENT_TARGET_BEHAVIOR=0
    )
endif()
