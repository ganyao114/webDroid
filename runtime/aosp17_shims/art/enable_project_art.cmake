# Entry point for CMAKE_PROJECT_WasmAndroid_INCLUDE when the root CMakeLists
# has not yet been updated to include Aosp17Art.cmake.
function(_wa_aosp17_enable_project_art)
    if(NOT TARGET wa_art_wasm)
        message(FATAL_ERROR "The AOSP ART hook requires the wa_art_wasm backend target")
    endif()
    include("${CMAKE_CURRENT_FUNCTION_LIST_DIR}/../../../cmake/Aosp17Art.cmake")
endfunction()

cmake_language(DEFER CALL _wa_aosp17_enable_project_art)
