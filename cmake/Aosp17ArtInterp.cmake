include_guard(GLOBAL)

include("${CMAKE_CURRENT_LIST_DIR}/Aosp17CoreJni.cmake")
option(AOSP17_ART_INTERP_HGRAPH "Enable ART HGraph promotion for the DEX interpreter" ON)
if(AOSP17_ART_INTERP_HGRAPH)
    include("${CMAKE_CURRENT_LIST_DIR}/Aosp17ArtHGraph.cmake")
else()
    include("${CMAKE_CURRENT_LIST_DIR}/Aosp17Art.cmake")
endif()
find_package(Python3 REQUIRED COMPONENTS Interpreter)

set(_interp "${CMAKE_CURRENT_LIST_DIR}/../runtime/aosp17_shims/art_interp")
set(_framework_jni "${AOSP17_SOURCE_DIR}/frameworks/base/core/jni")
set(_thunk_dir "${CMAKE_CURRENT_BINARY_DIR}/aosp17-interp/jni")

# Keep the AOSP definitions and their registration tables in the core JNI target.
# The generated translation units add a typed callable for each registered pointer.
get_target_property(_core_jni_sources aosp17_core_jni SOURCES)
set(_jni_wrappers)
foreach(source android_content_res_ApkAssets android_util_XmlBlock android_util_StringBlock
               android_util_AssetManager)
    set(original "${_framework_jni}/${source}.cpp")
    set(wrapper "${_thunk_dir}/${source}.cpp")
    add_custom_command(OUTPUT "${wrapper}"
        COMMAND "${Python3_EXECUTABLE}" "${_interp}/generate_jni.py"
            --source "${original}" --wrapper "${wrapper}"
        DEPENDS "${original}" "${_interp}/generate_jni.py"
        VERBATIM)
    list(REMOVE_ITEM _core_jni_sources "${original}")
    list(APPEND _core_jni_sources "${wrapper}")
    list(APPEND _jni_wrappers "${wrapper}")
endforeach()
add_custom_target(aosp17_interp_jni_thunks DEPENDS ${_jni_wrappers})
add_dependencies(aosp17_core_jni aosp17_interp_jni_thunks)
set_property(TARGET aosp17_core_jni PROPERTY SOURCES "${_core_jni_sources}")
target_include_directories(aosp17_core_jni PRIVATE "${_interp}")

_wa_art_static(aosp17_art_interp "${_interp}"
    class_linker.cpp gc_heap.cpp interpreter.cpp runtime.cpp boot.cpp boot_collections.cpp
    boot_platform.cpp boot_files.cpp deflater.cpp jni.cpp tier1.cpp vm_runtime.cpp)
target_include_directories(aosp17_art_interp PUBLIC "${_interp}"
    PRIVATE "${AOSP17_SOURCE_DIR}/art/runtime")
target_link_libraries(aosp17_art_interp PUBLIC aosp17_libdexfile aosp17_libartbase
    PRIVATE aosp17_core_jni aosp17_zlib)
if(AOSP17_ART_INTERP_HGRAPH)
    get_target_property(_hgraph_includes aosp17_art_hgraph INTERFACE_SYSTEM_INCLUDE_DIRECTORIES)
    target_include_directories(aosp17_art_interp SYSTEM BEFORE PRIVATE ${_hgraph_includes})
    target_compile_definitions(aosp17_art_interp PRIVATE WA_INTERP_HGRAPH=1)
    target_link_libraries(aosp17_art_interp PRIVATE aosp17_art_hgraph)
endif()
