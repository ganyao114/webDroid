get_target_property(m14_core aosp17_framework_core WA_FRAMEWORK_DEX)
set(m14_inputs "${m14_core}" "${WA_LAUNCHER_FRAMEWORK_DEX}" "${WA_LAUNCHER_FRAMEWORK_RES}"
    "${WA_BUILTIN_APPS_DIR}")

if(EMSCRIPTEN)
    find_program(WA_M14_NODE_EXECUTABLE node HINTS /opt/homebrew/bin NO_CMAKE_FIND_ROOT_PATH REQUIRED)
    get_target_property(cube_build wa_cube_app_apk BINARY_DIR)
    add_test(NAME test_m14_upstream_apps_wasm COMMAND "${WA_M14_NODE_EXECUTABLE}"
        "${CMAKE_CURRENT_LIST_DIR}/verify_wasm.mjs" "$<TARGET_FILE:android_system>" ${m14_inputs}
        --settings "${WA_SETTINGS_APK}" --cube "${cube_build}/wa_cube_app.apk")
    set_tests_properties(test_m14_upstream_apps_wasm PROPERTIES TIMEOUT 180)
    add_custom_target(m14_upstream_apps_test_inputs DEPENDS android_system)
else()
    add_executable(test_m14_upstream_apps test_upstream_apps.cpp)
    target_compile_features(test_m14_upstream_apps PRIVATE cxx_std_23)
    target_link_libraries(test_m14_upstream_apps PRIVATE wa_android_system wa_android_system_foreground)
    add_test(NAME test_m14_upstream_apps COMMAND test_m14_upstream_apps ${m14_inputs})
    set_tests_properties(test_m14_upstream_apps PROPERTIES TIMEOUT 180)
    add_custom_target(m14_upstream_apps_test_inputs DEPENDS test_m14_upstream_apps)
endif()
add_dependencies(m14_upstream_apps_test_inputs wa_upstream_builtin_apps wa_foreground_framework_assets)
