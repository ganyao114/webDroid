cmake_minimum_required(VERSION 3.24)

if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    project(Aosp17UpstreamLauncher3Runtime LANGUAGES C CXX)
    enable_testing()
    option(AOSP17_ART_INTERP_HGRAPH "Enable ART HGraph promotion" OFF)
endif()

get_filename_component(m11_repo "${CMAKE_CURRENT_LIST_DIR}/../.." ABSOLUTE)
include("${m11_repo}/cmake/Aosp17UiFramework.cmake")
find_package(Python3 3.11 REQUIRED COMPONENTS Interpreter)

set(WA_UPSTREAM_LAUNCHER3_APK "" CACHE FILEPATH "Use an existing upstream Launcher3 APK")
set(WA_UPSTREAM_FRAMEWORK_DEX "" CACHE FILEPATH "Use an existing upstream AOSP 17 framework DEX")
set(WA_M11_FRAMEWORK_CORE_DEX "" CACHE FILEPATH "Use an existing AOSP 17 core UI framework DEX")
set(WA_M11_FRAMEWORK_RES_APK "${WA_ANDROID_JAR}" CACHE FILEPATH "Framework resource archive for the Launcher APK")

set(m11_dependencies)
if(WA_UPSTREAM_LAUNCHER3_APK)
    set(m11_apk "${WA_UPSTREAM_LAUNCHER3_APK}")
else()
    set(m11_apk_dir "${CMAKE_CURRENT_BINARY_DIR}/apk")
    set(m11_apk "${m11_apk_dir}/Launcher3-upstream.apk")
    set(m11_apk_driver "${m11_repo}/tools/aosp/build_upstream_launcher3.py")
    file(GLOB_RECURSE m11_launcher_patches CONFIGURE_DEPENDS "${m11_repo}/patches/aosp17/launcher3/*")
    add_custom_command(OUTPUT "${m11_apk}" "${m11_apk_dir}/upstream_provenance.json"
        COMMAND "${Python3_EXECUTABLE}" "${m11_apk_driver}"
            --aosp-root "${AOSP17_SOURCE_DIR}" --sdk "${WA_ANDROID_SDK}"
            --out-dir "${m11_apk_dir}" --depfile "${m11_apk_dir}/launcher3.d"
        DEPENDS "${m11_apk_driver}" ${m11_launcher_patches}
        DEPFILE "${m11_apk_dir}/launcher3.d"
        VERBATIM)
    add_custom_target(m11_upstream_launcher3_apk DEPENDS "${m11_apk}")
    list(APPEND m11_dependencies m11_upstream_launcher3_apk)
endif()

if(WA_UPSTREAM_FRAMEWORK_DEX)
    set(m11_upstream_dex "${WA_UPSTREAM_FRAMEWORK_DEX}")
else()
    set(m11_upstream_dir "${CMAKE_CURRENT_BINARY_DIR}/upstream-dex")
    set(m11_upstream_dex "${m11_upstream_dir}/aosp17_upstream_framework.dex")
    set(m11_framework_driver "${m11_repo}/tools/aosp/build_upstream_framework_dex.py")
    set(m11_framework_profile "${m11_repo}/patches/aosp17/frameworks_base")
    file(GLOB m11_framework_patches CONFIGURE_DEPENDS "${m11_framework_profile}/*.patch")
    add_custom_command(OUTPUT "${m11_upstream_dex}"
        COMMAND "${Python3_EXECUTABLE}" "${m11_framework_driver}"
            --aosp-root "${AOSP17_SOURCE_DIR}" --android-sdk "${WA_ANDROID_SDK}"
            --javac "${WA_JAVAC_EXECUTABLE}" --android-jar "${WA_ANDROID_JAR}"
            --d8 "${WA_D8_EXECUTABLE}" --out-dir "${m11_upstream_dir}"
            --depfile "${m11_upstream_dir}/framework.d"
        DEPENDS "${m11_framework_driver}" "${m11_framework_profile}/sources.txt" ${m11_framework_patches}
        DEPFILE "${m11_upstream_dir}/framework.d"
        VERBATIM)
    add_custom_target(m11_upstream_framework_dex DEPENDS "${m11_upstream_dex}")
    list(APPEND m11_dependencies m11_upstream_framework_dex)
endif()

if(WA_M11_FRAMEWORK_CORE_DEX)
    set(m11_core_dex "${WA_M11_FRAMEWORK_CORE_DEX}")
else()
    get_target_property(m11_core_dex aosp17_framework_core WA_FRAMEWORK_DEX)
    list(APPEND m11_dependencies aosp17_framework_core)
endif()

add_executable(test_m11_upstream_launcher3_runtime test_runtime.cpp)
target_link_libraries(test_m11_upstream_launcher3_runtime PRIVATE aosp17_ui_jni)
set(m11_compressed "${CMAKE_CURRENT_BINARY_DIR}/compressed-dex.zip")
set(m11_invalid "${CMAKE_CURRENT_BINARY_DIR}/invalid-secondary-dex.zip")
set(m11_archive_script "${CMAKE_CURRENT_LIST_DIR}/make_archive_fixtures.py")
add_custom_command(OUTPUT "${m11_compressed}" "${m11_invalid}"
    COMMAND "${Python3_EXECUTABLE}" "${m11_archive_script}" "${m11_upstream_dex}"
        "${m11_compressed}" "${m11_invalid}"
    DEPENDS "${m11_archive_script}" "${m11_upstream_dex}"
    VERBATIM)
add_custom_target(m11_dex_archives DEPENDS "${m11_compressed}" "${m11_invalid}")
add_dependencies(test_m11_upstream_launcher3_runtime m11_dex_archives)
if(m11_dependencies)
    add_dependencies(test_m11_upstream_launcher3_runtime ${m11_dependencies})
endif()

set(m11_test_command "$<TARGET_FILE:test_m11_upstream_launcher3_runtime>")
if(EMSCRIPTEN)
    find_program(WA_M11_NODE_EXECUTABLE node HINTS /opt/homebrew/bin NO_CMAKE_FIND_ROOT_PATH REQUIRED)
    execute_process(COMMAND "${WA_M11_NODE_EXECUTABLE}" --version
        OUTPUT_VARIABLE m11_node_version OUTPUT_STRIP_TRAILING_WHITESPACE
        COMMAND_ERROR_IS_FATAL ANY)
    if(NOT m11_node_version MATCHES "^v26\\.")
        message(FATAL_ERROR "Set WA_M11_NODE_EXECUTABLE to Node 26 (found ${m11_node_version})")
    endif()
    set_target_properties(test_m11_upstream_launcher3_runtime PROPERTIES SUFFIX ".mjs")
    target_compile_options(test_m11_upstream_launcher3_runtime PRIVATE -fwasm-exceptions)
    target_link_options(test_m11_upstream_launcher3_runtime PRIVATE
        -lnodefs.js -sMODULARIZE=1 -sEXPORT_ES6=1 -sENVIRONMENT=node -sINVOKE_RUN=0
        -sFORCE_FILESYSTEM=1 -sALLOW_MEMORY_GROWTH=1 -sSTACK_SIZE=2097152
        "-sEXPORTED_RUNTIME_METHODS=FS,NODEFS,callMain")
    set(m11_test_command "${WA_M11_NODE_EXECUTABLE}" "${CMAKE_CURRENT_LIST_DIR}/run_wasm.mjs"
        "$<TARGET_FILE:test_m11_upstream_launcher3_runtime>")
endif()

foreach(m11_mode IN ITEMS load-verified load-trusted execute activity)
    add_test(NAME m11_upstream_launcher3_${m11_mode} COMMAND ${m11_test_command}
        "${m11_mode}" --apk "${m11_apk}" --core "${m11_core_dex}"
        --framework "${m11_upstream_dex}" --framework-resources "${WA_M11_FRAMEWORK_RES_APK}")
    set_tests_properties(m11_upstream_launcher3_${m11_mode} PROPERTIES TIMEOUT 120)
endforeach()
add_test(NAME m11_upstream_launcher3_archives COMMAND ${m11_test_command} archives
    --compressed "${m11_compressed}" --invalid "${m11_invalid}")
set_tests_properties(m11_upstream_launcher3_archives PROPERTIES TIMEOUT 120)
