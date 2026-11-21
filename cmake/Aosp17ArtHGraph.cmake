include_guard(GLOBAL)

include("${CMAKE_CURRENT_LIST_DIR}/Aosp17Art.cmake")
find_package(Python3 REQUIRED COMPONENTS Interpreter)

set(_hgraph_art "${AOSP17_SOURCE_DIR}/art")
set(_hgraph_shims "${CMAKE_CURRENT_LIST_DIR}/../runtime/aosp17_shims/art_hgraph")
set(_hgraph_overlay "${CMAKE_CURRENT_BINARY_DIR}/aosp17-art-hgraph")
set(_hgraph_upstream "${_hgraph_overlay}/upstream")

configure_file("${_hgraph_shims}/com_android_art_flags.h.in"
               "${_hgraph_overlay}/com_android_art_flags.h" @ONLY)
configure_file("${_hgraph_shims}/com_android_art_rw_flags.h.in"
               "${_hgraph_overlay}/com_android_art_rw_flags.h" @ONLY)

# Runtime declares XML reporting through this header. The standalone compiler
# does not instantiate the formatter or link any metrics backend.
_wa_art_edit_copy("${_hgraph_art}/libartbase/base/metrics/metrics.h"
                  "${_hgraph_overlay}/base/metrics/metrics.h"
    "#include \"tinyxml2.h\"" ""
    "class XmlFormatter : public MetricsFormatter {" "class XmlFormatter;\n#if 0\nclass XmlFormatter : public MetricsFormatter {"
    "  tinyxml2::XMLDocument document_;\n};" "  tinyxml2::XMLDocument document_;\n};\n#endif")
_wa_art_edit_copy("${_hgraph_art}/runtime/base/mutex-inl.h"
                  "${_hgraph_overlay}/base/mutex-inl.h"
    "#include \"mutex.h\"" "#include \"base/mutex.h\""
    "  state_and_contenders_.store(kHeldMask, std::memory_order_relaxed);"
    "#if ART_USE_FUTEXES\n  state_and_contenders_.store(kHeldMask, std::memory_order_relaxed);\n#else\n  LOG(FATAL) << \"ART monitor inflation requires the runtime\";\n#endif")
if(EMSCRIPTEN)
    _wa_art_edit_copy("${_hgraph_art}/runtime/entrypoints/quick/callee_save_frame.h"
                      "${_hgraph_overlay}/entrypoints/quick/callee_save_frame.h"
        "using RuntimeCalleeSaveFrame = detail::CSFSelector<kRuntimeQuickCodeISA>::type;"
        "struct RuntimeCalleeSaveFrame {\n  static QuickMethodFrameInfo GetMethodFrameInfo(CalleeSaveType) {\n    LOG(FATAL) << \"Wasm has no ART quick frames\";\n    UNREACHABLE();\n  }\n};")
endif()

set(_hgraph_prepared
    driver/compiler_options.cc
    driver/dex_compilation_unit.cc
    optimizing/builder.cc
    optimizing/constant_folding.cc
    optimizing/control_flow_simplifier.cc
    optimizing/graph.cc
    optimizing/graph_checker.cc
    optimizing/instruction_builder.cc
    optimizing/instruction_simplifier.cc
    optimizing/intrinsics.cc
    optimizing/locations.cc
    optimizing/nodes.cc
    optimizing/reference_type_info.cc
    optimizing/ssa_builder.cc)
set(_hgraph_inputs ${_hgraph_prepared})
list(TRANSFORM _hgraph_inputs PREPEND "${_hgraph_art}/compiler/")
set(_hgraph_prepare "${_hgraph_shims}/prepare_upstream.py")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
    "${_hgraph_prepare}" ${_hgraph_inputs}
    "${_hgraph_art}/compiler/optimizing/intrinsics.h")
execute_process(COMMAND "${Python3_EXECUTABLE}" "${_hgraph_prepare}"
    "${_hgraph_art}" "${_hgraph_upstream}" COMMAND_ERROR_IS_FATAL ANY)
list(TRANSFORM _hgraph_prepared PREPEND "${_hgraph_upstream}/")

_wa_art_static(aosp17_art_hgraph "${_hgraph_art}/compiler"
    optimizing/block_builder.cc
    optimizing/block_namer.cc
    optimizing/data_type.cc
    optimizing/dead_code_elimination.cc
    optimizing/instruction_list.cc
    optimizing/loop_information.cc
    optimizing/nodes_vector.cc
    optimizing/ssa_phi_elimination.cc)
target_sources(aosp17_art_hgraph PRIVATE ${_hgraph_prepared}
    "${_hgraph_shims}/code_generator_wasm_hgraph.cpp"
    "${_hgraph_shims}/standalone_runtime.cpp")
target_include_directories(aosp17_art_hgraph PUBLIC "${_hgraph_shims}")
target_include_directories(aosp17_art_hgraph SYSTEM PUBLIC
    "${_hgraph_overlay}" "${_hgraph_art}/compiler" "${_hgraph_art}/runtime"
    "${AOSP17_SOURCE_DIR}/libnativehelper/header_only_include")
target_include_directories(aosp17_art_hgraph SYSTEM PRIVATE
    "${_hgraph_art}/compiler/optimizing" "${_hgraph_art}/compiler/driver")
target_compile_definitions(aosp17_art_hgraph PUBLIC ART_DEFAULT_GC_TYPE_IS_MS=1)
target_compile_options(aosp17_art_hgraph PRIVATE
    "SHELL:-include \"${_hgraph_overlay}/base/mutex-inl.h\"")
if(EMSCRIPTEN)
    # runtime-inl.h would otherwise pick its sibling entrypoints directory first.
    target_compile_options(aosp17_art_hgraph PRIVATE
        "SHELL:-include \"${_hgraph_overlay}/entrypoints/quick/callee_save_frame.h\"")
endif()
target_link_libraries(aosp17_art_hgraph PUBLIC aosp17_libdexfile aosp17_libartbase)
target_compile_options(aosp17_art_hgraph PRIVATE -ffunction-sections -fdata-sections)
