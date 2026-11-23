include_guard(GLOBAL)

get_filename_component(_wa_default_aosp17 "${CMAKE_CURRENT_LIST_DIR}/../third_party/aosp17" ABSOLUTE)
set(AOSP17_SOURCE_DIR "${_wa_default_aosp17}"
    CACHE PATH "Read-only android-17.0.0_r1 source checkouts")
set(WA_ANDROID_SDK "$ENV{HOME}/Library/Android/sdk" CACHE PATH "Android SDK directory")
set(_art "${AOSP17_SOURCE_DIR}/art")
set(_system "${AOSP17_SOURCE_DIR}/system")
set(_zlib "${AOSP17_SOURCE_DIR}/external/zlib")
set(_fmt "${AOSP17_SOURCE_DIR}/external/fmtlib")
set(_logging "${_system}/logging/liblog")
set(_base "${_system}/libbase")
set(_zip "${_system}/libziparchive")
set(_shims "${CMAKE_CURRENT_LIST_DIR}/../runtime/aosp17_shims/art")

if(NOT EXISTS "${_art}/libdexfile/dex/dex_file_verifier.cc")
    message(FATAL_ERROR "Set AOSP17_SOURCE_DIR to the read-only android-17.0.0_r1 checkouts")
endif()

function(_wa_art_static target directory)
    set(sources ${ARGN})
    list(TRANSFORM sources PREPEND "${directory}/")
    add_library(${target} STATIC ${sources})
    set_target_properties(${target} PROPERTIES POSITION_INDEPENDENT_CODE ON)
    target_compile_features(${target} PUBLIC cxx_std_23)
    if(EMSCRIPTEN)
        target_include_directories(${target} SYSTEM PUBLIC
            "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/../runtime/aosp17_shims/art/wasm")
        target_compile_options(${target} PRIVATE -O3 -msimd128 -matomics -mbulk-memory
            $<$<COMPILE_LANGUAGE:CXX>:-fwasm-exceptions>)
        target_link_options(${target} INTERFACE -msimd128 -matomics -mbulk-memory -fwasm-exceptions)
    endif()
endfunction()

function(_wa_art_edit_copy source destination)
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${source}")
    file(READ "${source}" contents)
    set(argument 2)
    while(argument LESS ARGC)
        set(original "${ARGV${argument}}")
        math(EXPR argument "${argument} + 1")
        string(FIND "${contents}" "${original}" found)
        if(found EQUAL -1)
            message(FATAL_ERROR "Source layout has changed in ${source}")
        endif()
        string(REPLACE "${original}" "${ARGV${argument}}" contents "${contents}")
        math(EXPR argument "${argument} + 1")
    endwhile()
    file(CONFIGURE OUTPUT "${destination}" CONTENT "${contents}" @ONLY)
endfunction()

# Standalone dependencies. Use the same targets as the core port when it is present.
if(NOT TARGET aosp17_zlib)
    _wa_art_static(aosp17_zlib "${_zlib}"
        adler32.c compress.c crc32.c deflate.c gzclose.c gzlib.c gzread.c gzwrite.c
        infback.c inffast.c inflate.c inftrees.c trees.c uncompr.c zutil.c)
    target_include_directories(aosp17_zlib SYSTEM PUBLIC "${_zlib}")
    target_compile_definitions(aosp17_zlib PRIVATE HAVE_UNISTD_H)
endif()

if(NOT TARGET aosp17_liblog)
    _wa_art_static(aosp17_liblog "${_logging}" logger_name.cpp logger_write.cpp properties.cpp)
    target_include_directories(aosp17_liblog SYSTEM PUBLIC "${_logging}/include"
        PRIVATE "${_base}/include" "${_system}/core/libcutils/include")
endif()

if(NOT TARGET aosp17_libbase)
    _wa_art_static(aosp17_libbase "${_base}"
        errors_unix.cpp file.cpp logging.cpp mapped_file.cpp parsebool.cpp posix_strerror_r.cpp
        properties.cpp stringprintf.cpp strings.cpp threads.cpp)
    target_include_directories(aosp17_libbase SYSTEM PUBLIC "${_base}/include")
    target_link_libraries(aosp17_libbase PUBLIC aosp17_liblog)
endif()

if(NOT TARGET aosp17_libziparchive)
    _wa_art_static(aosp17_libziparchive "${_zip}" zip_archive.cc zip_cd_entry_map.cc zip_error.cpp)
    target_include_directories(aosp17_libziparchive SYSTEM PUBLIC "${_zip}/include"
        PRIVATE "${_zip}/incfs_support/include")
    target_compile_definitions(aosp17_libziparchive PRIVATE INCFS_SUPPORT_DISABLED=1 ZLIB_CONST)
    target_link_libraries(aosp17_libziparchive PUBLIC aosp17_libbase aosp17_zlib)
endif()

# ART's encoded value iterator uses the JNI jvalue header. No JNI library is needed.
file(GLOB _jni_candidates LIST_DIRECTORIES true
    "${WA_ANDROID_SDK}/ndk/*/toolchains/llvm/prebuilt/*/sysroot/usr/include")
list(SORT _jni_candidates COMPARE NATURAL ORDER DESCENDING)
find_path(WA_AOSP17_JNI_INCLUDE_DIR jni.h HINTS ${_jni_candidates}
    NO_CMAKE_FIND_ROOT_PATH REQUIRED)
set(_jni "${CMAKE_CURRENT_BINARY_DIR}/aosp17-art/jni")
# Import the single header so the NDK libc headers cannot shadow the host or Wasm libc.
configure_file("${WA_AOSP17_JNI_INCLUDE_DIR}/jni.h" "${_jni}/jni.h" COPYONLY)

if(NOT TARGET aosp17_fmt)
    _wa_art_static(aosp17_fmt "${_fmt}" src/format.cc)
    target_include_directories(aosp17_fmt PUBLIC "${_fmt}/include")
elseif(APPLE)
    # AppleClang searches /usr/local/include before the core port's -isystem path.
    # Prefer the pinned fmt headers for fmt itself and for its consumers.
    get_target_property(_fmt_system_includes aosp17_fmt INTERFACE_SYSTEM_INCLUDE_DIRECTORIES)
    if(_fmt_system_includes)
        list(REMOVE_ITEM _fmt_system_includes "${_fmt}/include")
        set_property(TARGET aosp17_fmt PROPERTY INTERFACE_SYSTEM_INCLUDE_DIRECTORIES
            "${_fmt_system_includes}")
    endif()
    get_target_property(_fmt_imported aosp17_fmt IMPORTED)
    if(NOT _fmt_imported)
        target_compile_options(aosp17_fmt PRIVATE "SHELL:-iquote \"${_fmt}/include\"")
    endif()
endif()

set(_overlay "${CMAKE_CURRENT_BINARY_DIR}/aosp17-art/upstream")
if(EMSCRIPTEN)
    _wa_art_edit_copy("${_art}/libartbase/base/utils.cc" "${_overlay}/base/utils.cc"
        "#elif defined(__BIONIC__)\n  return gettid();"
        "#elif defined(__BIONIC__) || defined(__EMSCRIPTEN__)\n  return gettid();"
        "#if defined(__linux__) || defined(_WIN32)\n  // pthread_setname_np"
        "#if defined(__linux__) || defined(_WIN32) || defined(__EMSCRIPTEN__)\n  // pthread_setname_np"
        "__builtin___clear_cache(reinterpret_cast<char*>(begin), reinterpret_cast<char*>(end));\n  return true;"
        "UNUSED(begin, end);\n  return false;  // Wasm has no native instruction cache API.")
    set(_art_utils "${_overlay}/base/utils.cc")
else()
    set(_art_utils "${_art}/libartbase/base/utils.cc")
endif()

_wa_art_static(aosp17_libartbase "${_art}/libartbase"
    base/allocator.cc
    base/arena_allocator.cc
    base/arena_bit_vector.cc
    base/bit_vector.cc
    base/calloc_arena_pool.cc
    base/compiler_filter.cc
    base/file_magic.cc
    base/file_utils.cc
    base/globals_unix.cc
    base/hex_dump.cc
    base/logging.cc
    base/membarrier.cc
    base/memfd.cc
    base/mem_map.cc
    base/mem_map_unix.cc
    base/memory_region.cc
    base/os_linux.cc
    base/pointer_size.cc
    base/runtime_debug.cc
    base/scoped_arena_allocator.cc
    base/scoped_flock.cc
    base/socket_peer_is_trusted.cc
    base/time_utils.cc
    base/unix_file/fd_file.cc
    base/unix_file/random_access_file_utils.cc
    base/zip_archive.cc)
target_sources(aosp17_libartbase PRIVATE "${_art_utils}")
target_include_directories(aosp17_libartbase SYSTEM PUBLIC "${_art}/libartbase")
target_compile_definitions(aosp17_libartbase PUBLIC ART_PAGE_SIZE_AGNOSTIC
    ART_STACK_OVERFLOW_GAP_arm=8192 ART_STACK_OVERFLOW_GAP_arm64=8192
    ART_STACK_OVERFLOW_GAP_riscv64=8192 ART_STACK_OVERFLOW_GAP_x86=8192
    ART_STACK_OVERFLOW_GAP_x86_64=8192
    PRIVATE ART_STATIC_LIBARTBASE ART_FRAME_SIZE_LIMIT=1744)
target_link_libraries(aosp17_libartbase PUBLIC aosp17_libbase aosp17_libziparchive aosp17_fmt)

if(EMSCRIPTEN)
    # Use the existing 32-bit layout of this ABI utility on Wasm. kRuntimeISA stays kNone.
    _wa_art_edit_copy("${_art}/libartbase/arch/instruction_set.h" "${_overlay}/arch/instruction_set.h"
        "#if defined(__i386__) || defined(__arm__)\nusing TwoWordReturn"
        "#if defined(__i386__) || defined(__arm__) || defined(__wasm32__)\nusing TwoWordReturn")
    configure_file("${_art}/libartbase/arch/instruction_set.cc"
        "${_overlay}/arch/instruction_set.cc" COPYONLY)
    target_include_directories(aosp17_libartbase SYSTEM BEFORE PUBLIC "${_overlay}")
    target_include_directories(aosp17_libartbase SYSTEM PRIVATE "${_art}/libartbase/base")
    target_sources(aosp17_libartbase PRIVATE "${_overlay}/arch/instruction_set.cc")
    set_source_files_properties("${_art}/libartbase/base/time_utils.cc"
        PROPERTIES COMPILE_OPTIONS "-include;sys/time.h")
else()
    target_sources(aosp17_libartbase PRIVATE "${_art}/libartbase/arch/instruction_set.cc")
endif()

# ClassAccessor and the code-item accessors are inline in this ART release.
_wa_art_static(aosp17_libdexfile "${_art}/libdexfile"
    dex/art_dex_file_loader.cc
    dex/descriptors_names.cc
    dex/dex_file_exception_helpers.cc
    dex/dex_file_loader.cc
    dex/dex_file_tracking_registrar.cc
    dex/dex_file_verifier.cc
    dex/dex_instruction.cc
    dex/modifiers.cc
    dex/primitive.cc
    dex/signature.cc
    dex/standard_dex_file.cc
    dex/type_lookup_table.cc
    dex/utf.cc)
if(EMSCRIPTEN)
    # This libc++ uses an iterator wrapper for std::array::end().
    _wa_art_edit_copy("${_art}/libdexfile/dex/dex_file.cc" "${_overlay}/dex/dex_file.cc"
        "DCHECK_EQ(dst, result.end());" "DCHECK_EQ(dst, result.data() + result.size());")
    target_sources(aosp17_libdexfile PRIVATE "${_overlay}/dex/dex_file.cc")
    target_include_directories(aosp17_libdexfile SYSTEM PRIVATE "${_art}/libdexfile/dex")
else()
    target_sources(aosp17_libdexfile PRIVATE "${_art}/libdexfile/dex/dex_file.cc")
endif()
target_include_directories(aosp17_libdexfile SYSTEM PUBLIC
    "${_art}/libdexfile" "${_jni}")
target_include_directories(aosp17_libdexfile PRIVATE "${_shims}/static-include")
target_compile_definitions(aosp17_libdexfile PRIVATE STATIC_LIB)
target_link_libraries(aosp17_libdexfile PUBLIC aosp17_libartbase aosp17_zlib)

find_package(Python3 REQUIRED COMPONENTS Interpreter)
function(_wa_art_enum_streams library)
    set(headers ${ARGN})
    list(TRANSFORM headers PREPEND "${_art}/${library}/")
    set(generator "${_shims}/generate_enum_streams.py")
    set(output "${CMAKE_CURRENT_BINARY_DIR}/aosp17-art/${library}_operator_out.cc")
    add_custom_command(OUTPUT "${output}"
        COMMAND "${Python3_EXECUTABLE}" "${generator}" "${_art}" "${library}" "${output}"
        DEPENDS "${generator}" ${headers}
        VERBATIM)
    target_sources(aosp17_${library} PRIVATE "${output}")
endfunction()

_wa_art_enum_streams(libartbase arch/instruction_set.h base/allocator.h base/unix_file/fd_file.h)
_wa_art_enum_streams(libdexfile dex/dex_file.h dex/dex_instruction.h dex/dex_instruction_utils.h
    dex/invoke_type.h)

function(_wa_aosp17_add_frontend target)
    get_filename_component(root "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/.." ABSOLUTE)
    set(backend "${root}/runtime/art_wasm_backend")
    set(shims "${root}/runtime/aosp17_shims/art")
    set(original "${backend}/dalvik_ir.cpp")
    set(adapted "${CMAKE_CURRENT_BINARY_DIR}/aosp17-art/dalvik_ir.cpp")
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${original}")
    file(READ "${original}" source)
    # Swap only the backend's decoder. Keep the upstream-independent IR analysis and emitter.
    string(FIND "${source}" "\nKind field_variant(" begin)
    string(FIND "${source}" "\nKind merge_kind(" end)
    if(begin LESS 0 OR end LESS_EQUAL begin)
        message(FATAL_ERROR "Cannot locate the Dalvik decoder in ${original}")
    endif()
    string(SUBSTRING "${source}" 0 ${begin} prefix)
    string(SUBSTRING "${source}" ${end} -1 suffix)
    string(CONCAT source
        "#include \"dex/dex_instruction-inl.h\"\n#include \"dex/dex_instruction_iterator.h\"\n"
        "#include \"dex/dex_instruction_utils.h\"\n"
        "${prefix}\n#include \"aosp17_dalvik_lowering.inc\"\n${suffix}")
    file(CONFIGURE OUTPUT "${adapted}" CONTENT "${source}" @ONLY)

    target_sources(${target} PRIVATE "${shims}/aosp17_dex_to_wasm.cpp" "${adapted}")
    target_include_directories(${target} PUBLIC "${root}/include" "${shims}" PRIVATE "${backend}")
    target_link_libraries(${target} PUBLIC aosp17_libdexfile)
endfunction()

# Apply after declaring wa_art_wasm. Its C ABI and runtime targets stay in use.
function(wa_use_aosp17_art target)
    get_target_property(enabled ${target} WA_AOSP17_ART)
    if(enabled)
        return()
    endif()
    get_target_property(sources ${target} SOURCES)
    list(LENGTH sources before)
    list(FILTER sources EXCLUDE REGEX "(^|/)(dex_reader|dalvik_ir)\\.cpp$")
    list(LENGTH sources after)
    math(EXPR removed "${before} - ${after}")
    if(NOT removed EQUAL 2)
        message(FATAL_ERROR "Cannot replace the DEX reader and Dalvik decoder in ${target}")
    endif()
    set_target_properties(${target} PROPERTIES SOURCES "${sources}" WA_AOSP17_ART ON)
    _wa_aosp17_add_frontend(${target})
endfunction()

# Build the same CodeGeneratorWasm for tooling without the application runtime.
function(wa_add_aosp17_dex_compiler target)
    set(backend "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/../runtime/art_wasm_backend")
    _wa_art_static(${target} "${backend}" codegen_wasm.cpp)
    # The emitter shares this predicate with the GC. Copy its implementation and type
    # constants so the standalone compiler does not require the heap, JNI, or DBT.
    set(gc "${backend}/shadow_gc.cpp")
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${gc}")
    file(READ "${gc}" source)
    string(FIND "${source}" "constexpr std::string_view kObject" constants_begin)
    string(FIND "${source}" "constexpr uint32_t kHeaderSize" constants_end)
    string(FIND "${source}" "bool wa::art::builtin_constructor(" predicate_begin)
    string(FIND "${source}" "wa_art_heap::wa_art_heap(" predicate_end)
    if(constants_begin LESS 0 OR constants_end LESS_EQUAL constants_begin OR
       predicate_begin LESS 0 OR predicate_end LESS_EQUAL predicate_begin)
        message(FATAL_ERROR "Cannot locate the runtime constructor predicate in ${gc}")
    endif()
    math(EXPR constants_length "${constants_end} - ${constants_begin}")
    math(EXPR predicate_length "${predicate_end} - ${predicate_begin}")
    string(SUBSTRING "${source}" ${constants_begin} ${constants_length} constants)
    string(SUBSTRING "${source}" ${predicate_begin} ${predicate_length} predicate)
    set(constructor "${CMAKE_CURRENT_BINARY_DIR}/aosp17-art/builtin_constructor.cpp")
    file(CONFIGURE OUTPUT "${constructor}" @ONLY CONTENT
        "#include \"runtime_internal.h\"\n#include <algorithm>\nnamespace {\n${constants}}\n${predicate}")
    target_sources(${target} PRIVATE "${constructor}")
    _wa_aosp17_add_frontend(${target})
endfunction()

if(TARGET wa_art_wasm)
    wa_use_aosp17_art(wa_art_wasm)
endif()
