set(vlc_apk "${CMAKE_SOURCE_DIR}/third_party/commercial_apks/VLC-Android-3.6.3-arm64-v8a.apk")
get_target_property(vlc_fallback aosp17_framework_core WA_FRAMEWORK_DEX)
set(vlc_inputs "${WA_LAUNCHER_FRAMEWORK_DEX}" "${vlc_fallback}"
    "${WA_LAUNCHER_FRAMEWORK_RES}" "${vlc_apk}")

if(EMSCRIPTEN)
    find_program(WA_NODE_EXECUTABLE node HINTS /opt/homebrew/bin NO_CMAKE_FIND_ROOT_PATH REQUIRED)
    add_test(NAME test_m18_vlc_framework_compat_wasm COMMAND "${WA_NODE_EXECUTABLE}"
        "${CMAKE_CURRENT_SOURCE_DIR}/test_vlc_boot.mjs" "$<TARGET_FILE:android_system>" ${vlc_inputs})
    set(vlc_test test_m18_vlc_framework_compat_wasm)
else()
    add_executable(test_m18_vlc_framework_compat test_vlc_boot.cpp)
    target_compile_features(test_m18_vlc_framework_compat PRIVATE cxx_std_23)
    target_link_libraries(test_m18_vlc_framework_compat PRIVATE wa_android_system wa_android_system_foreground)
    add_dependencies(test_m18_vlc_framework_compat wa_foreground_framework_assets)
    add_test(NAME test_m18_vlc_framework_compat COMMAND test_m18_vlc_framework_compat ${vlc_inputs})
    set(vlc_test test_m18_vlc_framework_compat)
endif()
set_tests_properties(${vlc_test} PROPERTIES SKIP_REGULAR_EXPRESSION "VLC APK is absent" TIMEOUT 180)
