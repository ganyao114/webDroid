get_target_property(framework_dex aosp17_framework_core WA_FRAMEWORK_DEX)
get_target_property(cube_build wa_cube_app_apk BINARY_DIR)

set(inputs "${WA_LAUNCHER_FRAMEWORK_DEX}" "${framework_dex}" "${WA_LAUNCHER_FRAMEWORK_RES}"
    "${WA_LAUNCHER_APK}" "${WA_SYSTEMUI_APK}" "${cube_build}/wa_cube_app.apk" ${WA_SYSTEM_APP_APKS})
if(EMSCRIPTEN)
    add_dependencies(android_system wa_cube_app_apk)
    add_test(NAME test_m9_launcher3_boot_wasm COMMAND "${WA_NODE_EXECUTABLE}"
        "${CMAKE_CURRENT_SOURCE_DIR}/verify_wasm.mjs" "$<TARGET_FILE:android_system>" ${inputs})
    set_tests_properties(test_m9_launcher3_boot_wasm PROPERTIES RUN_SERIAL TRUE TIMEOUT 120)
else()
    add_executable(test_m9_launcher3_boot_host verify_host.cpp)
    target_link_libraries(test_m9_launcher3_boot_host PRIVATE wa_android_system)
    target_include_directories(test_m9_launcher3_boot_host PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/../../include"
        "${CMAKE_CURRENT_SOURCE_DIR}/../../runtime/android_system")
    target_compile_features(test_m9_launcher3_boot_host PRIVATE cxx_std_23)
    add_dependencies(test_m9_launcher3_boot_host wa_cube_app_apk)
    add_test(NAME test_m9_launcher3_boot_host COMMAND test_m9_launcher3_boot_host ${inputs})
    set_tests_properties(test_m9_launcher3_boot_host PROPERTIES TIMEOUT 120
        FAIL_REGULAR_EXPRESSION "LauncherActivityCachingLogic: loadIcon;Database bulk icon loading failed")
endif()
