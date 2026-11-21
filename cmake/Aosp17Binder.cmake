include_guard(GLOBAL)

get_filename_component(_wa_binder_project "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)
set(_wa_binder_default_root "${_wa_binder_project}/third_party/aosp17")
if(DEFINED AOSP17_SOURCE_DIR)
    set(_wa_binder_default_root "${AOSP17_SOURCE_DIR}")
endif()
set(WA_AOSP17_ROOT "${_wa_binder_default_root}" CACHE PATH
    "Path to the upstream android-17.0.0_r1 sources")
set(_wa_binder_shims "${_wa_binder_project}/runtime/aosp17_shims/binder")
set(_wa_binder_native "${WA_AOSP17_ROOT}/frameworks/native")
set(_wa_binder_src "${_wa_binder_native}/libs/binder")
set(_wa_binder_core "${WA_AOSP17_ROOT}/system/core")
set(_wa_binder_base "${WA_AOSP17_ROOT}/system/libbase")
set(_wa_binder_log "${WA_AOSP17_ROOT}/system/logging/liblog")
set(_wa_binder_uapi "${WA_AOSP17_ROOT}/bionic/libc/kernel/uapi")

if(NOT EXISTS "${_wa_binder_src}/Parcel.cpp")
    message(FATAL_ERROR "Set WA_AOSP17_ROOT to the android-17.0.0_r1 checkout")
endif()

if(NOT TARGET wa_libkern)
    add_subdirectory("${_wa_binder_project}/runtime/libkern-wasm" libkern)
endif()

function(_wa_binder_port_source source output original replacement)
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${source}")
    file(READ "${source}" contents)
    string(FIND "${contents}" "${original}" position)
    if(position EQUAL -1)
        message(FATAL_ERROR "Expected Android 17 source declaration in ${source}")
    endif()
    string(REPLACE "${original}" "${replacement}" contents "${contents}")
    file(CONFIGURE OUTPUT "${output}" CONTENT "#line 1 \"${source}\"\n${contents}" @ONLY)
endfunction()

# Worker 1 provides these targets when Aosp17Core.cmake is included first.
if(NOT TARGET aosp17_libutils)
    if(NOT TARGET aosp17_liblog)
        set(_wa_logger "${_wa_binder_log}/logger_write.cpp")
        if(EMSCRIPTEN)
            set(_wa_logger "${CMAKE_CURRENT_BINARY_DIR}/aosp17-binder/logger_write.cpp")
            _wa_binder_port_source("${_wa_binder_log}/logger_write.cpp" "${_wa_logger}"
                "#if defined(__BIONIC__)\n  return gettid();"
                "#if defined(__BIONIC__) || defined(__EMSCRIPTEN__)\n  return gettid();")
        endif()
        add_library(aosp17_liblog STATIC
            "${_wa_binder_log}/log_event_list.cpp"
            "${_wa_binder_log}/log_event_write.cpp"
            "${_wa_logger}"
            "${_wa_binder_log}/properties.cpp"
        )
        target_compile_features(aosp17_liblog PUBLIC cxx_std_20)
        target_include_directories(aosp17_liblog PUBLIC "${_wa_binder_log}/include"
            PRIVATE "${_wa_binder_base}/include" "${_wa_binder_core}/libcutils/include"
                "${_wa_binder_log}")
        if(EMSCRIPTEN)
            target_include_directories(aosp17_liblog PUBLIC "${_wa_binder_shims}/include/wasm")
        endif()
        target_compile_definitions(aosp17_liblog PRIVATE
            LIBLOG_LOG_TAG=1006 SNET_EVENT_LOG_TAG=1397638484 ANDROID_DEBUGGABLE=0)
    endif()

    if(NOT TARGET aosp17_libbase)
        add_library(aosp17_libbase STATIC
            "${_wa_binder_base}/errors_unix.cpp"
            "${_wa_binder_base}/file.cpp"
            "${_wa_binder_base}/logging.cpp"
            "${_wa_binder_base}/posix_strerror_r.cpp"
            "${_wa_binder_base}/stringprintf.cpp"
            "${_wa_binder_base}/strings.cpp"
            "${_wa_binder_base}/threads.cpp"
            "${WA_AOSP17_ROOT}/external/fmtlib/src/format.cc"
        )
        target_compile_features(aosp17_libbase PRIVATE cxx_std_23)
        target_include_directories(aosp17_libbase PUBLIC
            "${_wa_binder_base}/include" "${WA_AOSP17_ROOT}/external/fmtlib/include")
        target_link_libraries(aosp17_libbase PUBLIC aosp17_liblog)
    endif()

    if(NOT TARGET aosp17_libcutils)
        add_library(aosp17_libcutils STATIC
            "${_wa_binder_core}/libcutils/ashmem-host.cpp"
            "${_wa_binder_core}/libcutils/native_handle.cpp"
            "${_wa_binder_core}/libcutils/trace-host.cpp"
        )
        target_include_directories(aosp17_libcutils PUBLIC "${_wa_binder_core}/libcutils/include"
            PRIVATE "${_wa_binder_core}/libutils/include" "${_wa_binder_shims}/include")
        target_link_libraries(aosp17_libcutils PUBLIC aosp17_libbase)
    endif()

    add_library(aosp17_libutils STATIC
        "${_wa_binder_core}/libutils/SystemClock.cpp"
        "${_wa_binder_core}/libutils/Threads.cpp"
        "${_wa_binder_core}/libutils/Timers.cpp"
        "${_wa_binder_core}/libutils/misc.cpp"
    )
    foreach(_wa_util Errors RefBase SharedBuffer String8 String16 StrongPointer Unicode VectorImpl)
        target_sources(aosp17_libutils PRIVATE "${_wa_binder_core}/libutils/binder/${_wa_util}.cpp")
    endforeach()
    target_include_directories(aosp17_libutils PUBLIC
        "${_wa_binder_core}/libutils/include" "${_wa_binder_core}/libutils/binder/include"
        "${_wa_binder_core}/libsystem/include"
        PRIVATE "${_wa_binder_shims}/include")
    target_link_libraries(aosp17_libutils PUBLIC aosp17_libcutils)

    if(EMSCRIPTEN)
        foreach(_wa_dep aosp17_libutils aosp17_libcutils aosp17_libbase aosp17_liblog)
            target_compile_options(${_wa_dep} PRIVATE
                -O3 -pthread -matomics -mbulk-memory -msimd128)
        endforeach()
    endif()
endif()

if(EMSCRIPTEN)
    # Apply the stack check to the existing libutils target when Core supplies it.
    # Wasm heap objects and pthread stacks can occupy the same page.
    set(_wa_refbase "${_wa_binder_core}/libutils/binder/RefBase.cpp")
    get_target_property(_wa_utils_sources aosp17_libutils SOURCES)
    if(_wa_refbase IN_LIST _wa_utils_sources)
        set(_wa_refbase_local "${CMAKE_CURRENT_BINARY_DIR}/aosp17-binder/RefBase.cpp")
        _wa_binder_port_source("${_wa_refbase}" "${_wa_refbase_local}"
            [=[    static constexpr int MIN_PAGE_SIZE = 0x1000;  // 4K. Safer than including sys/user.h.
    static constexpr uintptr_t MIN_PAGE_MASK = ~static_cast<uintptr_t>(MIN_PAGE_SIZE - 1);
    uintptr_t my_frame_address =
            reinterpret_cast<uintptr_t>(__builtin_frame_address(0 /* this frame */));
    if (((reinterpret_cast<uintptr_t>(ptr) ^ my_frame_address) & MIN_PAGE_MASK) == 0) {]=]
            [=[    const auto address = reinterpret_cast<uintptr_t>(ptr);
    if (address >= emscripten_stack_get_end() && address < emscripten_stack_get_base()) {]=])
        list(REMOVE_ITEM _wa_utils_sources "${_wa_refbase}")
        list(APPEND _wa_utils_sources "${_wa_refbase_local}")
        set_property(TARGET aosp17_libutils PROPERTY SOURCES "${_wa_utils_sources}")
        set_property(SOURCE "${_wa_refbase_local}" TARGET_DIRECTORY aosp17_libutils
            APPEND PROPERTY COMPILE_OPTIONS -include emscripten/stack.h)
    endif()
endif()

add_library(aosp17_libbinder STATIC
    "${_wa_binder_shims}/wa_binder_driver.cpp"
)
foreach(_wa_source Binder BpBinder BufferedTextOutput Debug FdTrigger IInterface
        IResultReceiver IShellCallback JavaBBinderBase OS_android OS_unix_base Parcel
        ParcelFileDescriptor RecordedTransaction RpcServer RpcSession RpcState
        RpcTransportRaw Stability Static Status TextOutput Utils file)
    target_sources(aosp17_libbinder PRIVATE "${_wa_binder_src}/${_wa_source}.cpp")
endforeach()

target_compile_features(aosp17_libbinder PUBLIC cxx_std_20 PRIVATE cxx_std_23)
target_compile_definitions(aosp17_libbinder PRIVATE BINDER_WITH_KERNEL_IPC BUILDING_LIBBINDER)
target_include_directories(aosp17_libbinder PUBLIC
    "${_wa_binder_src}/include" "${_wa_binder_native}/headers"
    "${_wa_binder_shims}"
    PRIVATE "${_wa_binder_src}" "${_wa_binder_src}/include_private"
        "${_wa_binder_shims}/include" "${_wa_binder_uapi}" "${_wa_binder_core}/libsystem/include")
target_link_libraries(aosp17_libbinder PUBLIC
    aosp17_libutils aosp17_libcutils aosp17_libbase aosp17_liblog wa_libkern)

# The optional observer includes platform-generated stats AIDL even when
# disabled. Guard only that include in build-local copies of these two units.
foreach(_wa_source ProcessState IPCThreadState)
    set(_wa_upstream "${_wa_binder_src}/${_wa_source}.cpp")
    set(_wa_local "${CMAKE_CURRENT_BINARY_DIR}/aosp17-binder/${_wa_source}.cpp")
    _wa_binder_port_source("${_wa_upstream}" "${_wa_local}" "#include \"observer/BinderObserver.h\""
        "#ifdef BINDER_WITH_OBSERVERS\n#include \"observer/BinderObserver.h\"\n#endif")
    target_sources(aosp17_libbinder PRIVATE "${_wa_local}")
endforeach()
set_property(SOURCE "${CMAKE_CURRENT_BINARY_DIR}/aosp17-binder/ProcessState.cpp"
    APPEND PROPERTY COMPILE_OPTIONS
    -include "${_wa_binder_shims}/redirect_process_state.h")
set_property(SOURCE "${CMAKE_CURRENT_BINARY_DIR}/aosp17-binder/IPCThreadState.cpp"
    APPEND PROPERTY COMPILE_OPTIONS
    -include "${_wa_binder_shims}/redirect_ipc_thread_state.h")
set_property(SOURCE "${_wa_binder_src}/RpcTransportRaw.cpp" APPEND PROPERTY COMPILE_OPTIONS
    -include cerrno -include android-base/macros.h)
if(APPLE)
    target_compile_options(aosp17_libbinder PRIVATE -include "${_wa_binder_shims}/darwin.h")
    target_sources(aosp17_libbinder PRIVATE "${_wa_binder_shims}/darwin.cpp")
endif()
if(EMSCRIPTEN)
    target_include_directories(aosp17_libbinder PUBLIC "${_wa_binder_shims}/include/wasm")
    target_compile_options(aosp17_libbinder PUBLIC -pthread -matomics -mbulk-memory -msimd128
        PRIVATE -O3)
    target_link_options(aosp17_libbinder INTERFACE -pthread -matomics -mbulk-memory -msimd128)
endif()
