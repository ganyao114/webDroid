include_guard(GLOBAL)

set(AOSP17_SOURCE_DIR "/Volumes/Data2/wasm-android/third_party/aosp17" CACHE PATH
    "Read-only AOSP android-17.0.0_r1 source tree")

set(_aosp17_shims "${CMAKE_CURRENT_LIST_DIR}/../runtime/aosp17_shims/core")
set(_aosp17_core "${AOSP17_SOURCE_DIR}/system/core")
set(_aosp17_base "${AOSP17_SOURCE_DIR}/system/libbase")
set(_aosp17_log "${AOSP17_SOURCE_DIR}/system/logging/liblog")
set(_aosp17_zip "${AOSP17_SOURCE_DIR}/system/libziparchive")
set(_aosp17_incfs "${AOSP17_SOURCE_DIR}/system/incremental_delivery/incfs")
set(_aosp17_androidfw "${AOSP17_SOURCE_DIR}/frameworks/base/libs/androidfw")

function(_aosp17_add_static target directory)
    set(sources "${ARGN}")
    list(TRANSFORM sources PREPEND "${directory}/")
    add_library(${target} STATIC ${sources})
    if(EMSCRIPTEN)
        target_compile_options(${target} PRIVATE -O3 -flto=thin -msimd128 -matomics -mbulk-memory
            $<$<COMPILE_LANGUAGE:CXX>:-fwasm-exceptions>)
        target_link_options(${target} INTERFACE -msimd128 -matomics -mbulk-memory
            $<$<LINK_LANGUAGE:CXX>:-fwasm-exceptions>)
    endif()
endfunction()

function(_aosp17_wasm_source_overlay target original before after)
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${original}")
    file(READ "${original}" contents)
    string(FIND "${contents}" "${before}" match)
    if(match EQUAL -1)
        message(FATAL_ERROR "AOSP 17 Wasm compatibility edit no longer matches ${original}")
    endif()
    string(REPLACE "${before}" "${after}" contents "${contents}")
    get_filename_component(name "${original}" NAME)
    get_filename_component(directory "${original}" DIRECTORY)
    set(copy "${CMAKE_CURRENT_BINARY_DIR}/aosp17-overlays/${target}/${name}")
    file(WRITE "${copy}.in" "${contents}")
    configure_file("${copy}.in" "${copy}" COPYONLY)
    get_target_property(sources ${target} SOURCES)
    list(REMOVE_ITEM sources "${original}")
    list(APPEND sources "${copy}")
    set_property(TARGET ${target} PROPERTY SOURCES "${sources}")
    target_include_directories(${target} PRIVATE "${directory}")
endfunction()

if(NOT TARGET aosp17_zlib)
    set(_aosp17_zlib "${AOSP17_SOURCE_DIR}/external/zlib")
    _aosp17_add_static(aosp17_zlib "${_aosp17_zlib}"
        adler32.c adler32_simd.c compress.c cpu_features.c crc32.c crc32_simd.c crc_folding.c
        deflate.c gzclose.c gzlib.c gzread.c gzwrite.c infback.c inffast.c inflate.c inftrees.c
        trees.c uncompr.c zutil.c)
    target_include_directories(aosp17_zlib SYSTEM PUBLIC "${_aosp17_zlib}")
    target_compile_definitions(aosp17_zlib PUBLIC ZLIB_CONST PRIVATE HAVE_HIDDEN
        CHROMIUM_ZLIB_NO_CASTAGNOLI)
    set_target_properties(aosp17_zlib PROPERTIES C_STANDARD 17 C_STANDARD_REQUIRED ON)
    target_compile_options(aosp17_zlib PRIVATE -Wno-deprecated-non-prototype)
endif()

if(NOT TARGET aosp17_fmt)
    set(_aosp17_fmt "${AOSP17_SOURCE_DIR}/external/fmtlib")
    _aosp17_add_static(aosp17_fmt "${_aosp17_fmt}" src/format.cc)
    target_include_directories(aosp17_fmt SYSTEM PUBLIC "${_aosp17_fmt}/include")
    target_compile_features(aosp17_fmt PRIVATE cxx_std_23)
    target_compile_options(aosp17_fmt PRIVATE -fno-exceptions -UNDEBUG)
endif()

if(NOT TARGET aosp17_liblog)
    _aosp17_add_static(aosp17_liblog "${_aosp17_log}"
        log_event_list.cpp log_event_write.cpp logger_name.cpp logger_read.cpp logger_write.cpp
        logprint.cpp properties.cpp event_tag_map.cpp)
    target_include_directories(aosp17_liblog SYSTEM PUBLIC "${_aosp17_log}/include"
        PRIVATE "${_aosp17_base}/include" "${_aosp17_core}/libcutils/include"
        "${_aosp17_core}/libutils/include" "${_aosp17_core}/libsystem/include")
    if(EMSCRIPTEN)
        target_include_directories(aosp17_liblog SYSTEM PUBLIC "${_aosp17_shims}/wasm")
        target_compile_options(aosp17_liblog PRIVATE -Wno-c99-designator)
        # Match libbase::GetThreadId(), which already uses Emscripten's gettid().
        _aosp17_wasm_source_overlay(aosp17_liblog "${_aosp17_log}/logger_write.cpp"
            "#if defined(__BIONIC__)\n  return gettid();"
            "#if defined(__BIONIC__) || defined(__EMSCRIPTEN__)\n  return gettid();")
    endif()
    target_compile_features(aosp17_liblog PRIVATE cxx_std_23)
    target_compile_definitions(aosp17_liblog PRIVATE LIBLOG_LOG_TAG=1006
        SNET_EVENT_LOG_TAG=1397638484 ANDROID_DEBUGGABLE=0)
endif()

if(NOT TARGET aosp17_libbase)
    _aosp17_add_static(aosp17_libbase "${_aosp17_base}"
        chrono_utils.cpp cmsg.cpp errors_unix.cpp file.cpp hex.cpp logging.cpp mapped_file.cpp
        parsebool.cpp parsenetaddress.cpp posix_strerror_r.cpp process.cpp properties.cpp
        result.cpp stringprintf.cpp strings.cpp threads.cpp test_utils.cpp)
    target_include_directories(aosp17_libbase SYSTEM PUBLIC "${_aosp17_base}/include")
    target_compile_features(aosp17_libbase PUBLIC cxx_std_23)
    target_link_libraries(aosp17_libbase PUBLIC aosp17_fmt aosp17_liblog)
endif()

if(NOT TARGET aosp17_libcutils)
    set(_aosp17_cutils "${_aosp17_core}/libcutils")
    _aosp17_add_static(aosp17_libcutils "${_aosp17_cutils}"
        ashmem-host.cpp config_utils.cpp fs.cpp hashmap.cpp iosched_policy.cpp load_file.cpp
        multiuser.cpp native_handle.cpp properties.cpp record_stream.cpp str_parms.cpp strlcpy.c
        trace-host.cpp sockets.cpp socket_inaddr_any_server_unix.cpp socket_local_client_unix.cpp
        socket_local_server_unix.cpp socket_network_client_unix.cpp sockets_utils.cpp
        sockets_unix.cpp)
    if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
        target_sources(aosp17_libcutils PRIVATE
            "${_aosp17_cutils}/canned_fs_config.cpp" "${_aosp17_cutils}/fs_config.cpp")
    endif()
    target_include_directories(aosp17_libcutils SYSTEM PUBLIC
        "${_aosp17_cutils}/include" "${_aosp17_core}/libprocessgroup/include"
        PRIVATE "${_aosp17_core}/libutils/include")
    target_link_libraries(aosp17_libcutils PUBLIC aosp17_libbase aosp17_liblog)
endif()

if(NOT TARGET aosp17_libutils)
    set(_aosp17_utils "${_aosp17_core}/libutils")
    _aosp17_add_static(aosp17_libutils "${_aosp17_utils}"
        FileMap.cpp JenkinsHash.cpp LightRefBase.cpp NativeHandle.cpp Printer.cpp StopWatch.cpp
        SystemClock.cpp Threads.cpp Timers.cpp Tokenizer.cpp misc.cpp
        binder/Errors.cpp binder/RefBase.cpp binder/SharedBuffer.cpp binder/String16.cpp
        binder/String8.cpp binder/StrongPointer.cpp binder/Unicode.cpp binder/VectorImpl.cpp)
    if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
        target_sources(aosp17_libutils PRIVATE "${_aosp17_utils}/Looper.cpp")
    endif()
    target_include_directories(aosp17_libutils SYSTEM PUBLIC "${_aosp17_utils}/include"
        "${_aosp17_utils}/binder/include" "${_aosp17_core}/libsystem/include")
    target_compile_definitions(aosp17_libutils PRIVATE ANDROID_UTILS_REF_BASE_DISABLE_IMPLICIT_CONSTRUCTION)
    target_link_libraries(aosp17_libutils PUBLIC aosp17_libcutils aosp17_liblog)
endif()

if(NOT TARGET aosp17_libziparchive)
    _aosp17_add_static(aosp17_libziparchive "${_aosp17_zip}"
        zip_archive.cc zip_archive_stream_entry.cc zip_cd_entry_map.cc zip_error.cpp zip_writer.cc)
    target_include_directories(aosp17_libziparchive SYSTEM PUBLIC "${_aosp17_zip}/include"
        "${_aosp17_shims}/gtest_prod" PRIVATE "${_aosp17_zip}/incfs_support/include")
    target_compile_definitions(aosp17_libziparchive PRIVATE INCFS_SUPPORT_DISABLED=1)
    target_link_libraries(aosp17_libziparchive PUBLIC aosp17_libbase aosp17_zlib PRIVATE aosp17_liblog)
endif()

if(NOT TARGET aosp17_libandroidfw)
    # libandroidfw's host srcs and libandroidfw_pathutils from Android.bp.
    _aosp17_add_static(aosp17_libandroidfw "${_aosp17_androidfw}"
        ApkAssets.cpp ApkParsing.cpp Asset.cpp AssetDir.cpp AssetManager.cpp AssetManager2.cpp
        AssetsProvider.cpp AttributeResolution.cpp BigBuffer.cpp BigBufferStream.cpp ChunkIterator.cpp
        ConfigDescription.cpp CursorWindow.cpp FileStream.cpp Idmap.cpp LoadedArsc.cpp Locale.cpp
        LocaleData.cpp LocaleDataLookup.cpp misc.cpp NinePatch.cpp ObbFile.cpp PosixUtils.cpp Png.cpp
        PngChunkFilter.cpp PngCrunch.cpp ResourceTimer.cpp ResourceTypes.cpp ResourceUtils.cpp
        StreamingZipInflater.cpp StringPool.cpp TypeWrappers.cpp Util.cpp ZipFileRO.cpp ZipUtils.cpp
        PathUtils.cpp)
    target_sources(aosp17_libandroidfw PRIVATE "${_aosp17_incfs}/util/map_ptr.cpp")
    target_include_directories(aosp17_libandroidfw SYSTEM PUBLIC "${_aosp17_androidfw}/include"
        "${_aosp17_androidfw}/include_pathutils" "${_aosp17_incfs}/util/include"
        "${AOSP17_SOURCE_DIR}/frameworks/native/include"
        PRIVATE "${_aosp17_incfs}/include" "${_aosp17_incfs}/kernel-headers"
        "${_aosp17_shims}/androidfw")
    target_compile_definitions(aosp17_libandroidfw PRIVATE STATIC_ANDROIDFW_FOR_TOOLS)
    target_link_libraries(aosp17_libandroidfw PUBLIC
        aosp17_libziparchive aosp17_libutils aosp17_libbase aosp17_zlib)
    set_property(SOURCE "${_aosp17_androidfw}/ResourceTypes.cpp" APPEND PROPERTY
        COMPILE_OPTIONS -Wno-deprecated-anon-enum-enum-conversion)
    if(EMSCRIPTEN)
        # libc++ can wrap string_view iterators. The iterator/sentinel overload supports both ABIs.
        _aosp17_wasm_source_overlay(aosp17_libandroidfw "${_aosp17_androidfw}/Util.cpp"
            "StringPiece(start, current - start)" "StringPiece(start, current)")
    endif()

    # These public libpng headers are also usable by the Wasm compiler.
    find_path(AOSP17_PNG_INCLUDE_DIR NAMES png.h HINTS /opt/homebrew/opt/libpng/include
        NO_CMAKE_FIND_ROOT_PATH REQUIRED)
    target_include_directories(aosp17_libandroidfw SYSTEM PRIVATE "${AOSP17_PNG_INCLUDE_DIR}")
    if(EMSCRIPTEN)
        set(AOSP17_WASM_PNG_LIBRARY "" CACHE FILEPATH "Optional wasm32 libpng static archive")
        if(AOSP17_WASM_PNG_LIBRARY)
            target_link_libraries(aosp17_libandroidfw PRIVATE "${AOSP17_WASM_PNG_LIBRARY}")
        endif()
    else()
        # Prefer a static libpng so both libandroidfw and libpng use the AOSP zlib.
        find_library(AOSP17_HOST_PNG_LIBRARY NAMES libpng16.a libpng.a png16 png
            HINTS /opt/homebrew/opt/libpng/lib REQUIRED)
        target_link_libraries(aosp17_libandroidfw PRIVATE "${AOSP17_HOST_PNG_LIBRARY}")
    endif()
endif()
