get_target_property(wechat_fallback aosp17_framework_core WA_FRAMEWORK_DEX)
add_executable(test_m20_wechat_compat test_wechat_boot.cpp)
target_compile_features(test_m20_wechat_compat PRIVATE cxx_std_23)
target_include_directories(test_m20_wechat_compat PRIVATE
    ../../runtime/android_system ../m17_hybrid_art_ndk)
target_link_libraries(test_m20_wechat_compat PRIVATE
    wa_android_system wa_android_system_foreground aosp17_art_interp)
add_dependencies(test_m20_wechat_compat wa_foreground_framework_assets)
add_test(NAME test_m20_wechat_compat COMMAND test_m20_wechat_compat
    "${WA_LAUNCHER_FRAMEWORK_DEX}" "${wechat_fallback}" "${WA_LAUNCHER_FRAMEWORK_RES}"
    "${CMAKE_SOURCE_DIR}/third_party/commercial_apks/weixin8080_arm64.apk")
set_tests_properties(test_m20_wechat_compat PROPERTIES
    SKIP_REGULAR_EXPRESSION "WeChat APK is absent" TIMEOUT 180)
