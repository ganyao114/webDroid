set(via_apk "${CMAKE_SOURCE_DIR}/third_party/commercial_apks/via-release-cn.apk")
get_target_property(via_fallback aosp17_framework_core WA_FRAMEWORK_DEX)
set(via_inputs "${WA_LAUNCHER_FRAMEWORK_DEX}" "${via_fallback}" "${WA_LAUNCHER_FRAMEWORK_RES}" "${via_apk}")

if(EMSCRIPTEN)
    find_program(via_node node HINTS /opt/homebrew/bin NO_CMAKE_FIND_ROOT_PATH REQUIRED)
    add_test(NAME test_m16_commercial_apk_compat_wasm COMMAND "${via_node}"
        "${CMAKE_CURRENT_LIST_DIR}/test_via.mjs" "$<TARGET_FILE:android_system>" ${via_inputs})
    set_tests_properties(test_m16_commercial_apk_compat_wasm
        PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 180)
else()
    add_executable(test_m16_commercial_apk_compat test_via.cpp)
    target_compile_features(test_m16_commercial_apk_compat PRIVATE cxx_std_23)
    target_link_libraries(test_m16_commercial_apk_compat PRIVATE wa_android_system wa_android_system_foreground)
    add_dependencies(test_m16_commercial_apk_compat wa_foreground_framework_assets)
    add_test(NAME test_m16_commercial_apk_compat COMMAND test_m16_commercial_apk_compat ${via_inputs})
    set_tests_properties(test_m16_commercial_apk_compat PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 180)
endif()
