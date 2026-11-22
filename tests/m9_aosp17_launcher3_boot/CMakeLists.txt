find_package(Python3 REQUIRED COMPONENTS Interpreter)

get_target_property(framework_dex aosp17_framework_core WA_FRAMEWORK_DEX)
get_target_property(cube_build wa_cube_app_apk BINARY_DIR)
set(apps "${cube_build}/wa_cube_app.apk")

set(demos "${CMAKE_CURRENT_BINARY_DIR}/apps")
set(demo_apks)
foreach(app IN ITEMS settings browser gallery calculator clock)
    list(APPEND demo_apks "${demos}/${app}.apk")
endforeach()
add_custom_command(OUTPUT ${demo_apks}
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/build_apps.py"
        --out "${demos}" --aapt2 "${WA_AAPT2_EXECUTABLE}" --android-jar "${WA_ANDROID_JAR}"
        --javac "${WA_JAVAC_EXECUTABLE}" --d8 "${WA_D8_EXECUTABLE}"
    DEPENDS build_apps.py "${WA_AAPT2_EXECUTABLE}" "${WA_ANDROID_JAR}"
        "${WA_JAVAC_EXECUTABLE}" "${WA_D8_EXECUTABLE}"
    COMMENT "Building localized Android APKs for Launcher3"
    VERBATIM)
add_custom_target(m9_launcher3_fixtures ALL DEPENDS ${demo_apks})
add_dependencies(m9_launcher3_fixtures wa_upstream_launcher_assets wa_cube_app_apk)

set(fixtures "${framework_dex}" "${WA_LAUNCHER_FRAMEWORK_DEX}" "${WA_LAUNCHER_FRAMEWORK_RES}"
    "${WA_LAUNCHER_APK}" "${WA_SYSTEMUI_APK}" ${apps} ${demo_apks})
if(EMSCRIPTEN)
    add_dependencies(android_system m9_launcher3_fixtures)
    add_test(NAME test_m9_launcher3_boot_wasm COMMAND "${WA_NODE_EXECUTABLE}"
        "${CMAKE_CURRENT_SOURCE_DIR}/verify_wasm.mjs" "$<TARGET_FILE:android_system>" ${fixtures})
    set_tests_properties(test_m9_launcher3_boot_wasm PROPERTIES RUN_SERIAL TRUE TIMEOUT 120)
else()
    add_executable(test_m9_launcher3_boot_host verify_host.cpp)
    target_link_libraries(test_m9_launcher3_boot_host PRIVATE wa_android_system)
    target_include_directories(test_m9_launcher3_boot_host PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/../../include"
        "${CMAKE_CURRENT_SOURCE_DIR}/../../runtime/android_system")
    target_compile_features(test_m9_launcher3_boot_host PRIVATE cxx_std_23)
    add_dependencies(test_m9_launcher3_boot_host m9_launcher3_fixtures)
    add_test(NAME test_m9_launcher3_boot_host COMMAND test_m9_launcher3_boot_host ${fixtures})
    set_tests_properties(test_m9_launcher3_boot_host PROPERTIES TIMEOUT 120
        FAIL_REGULAR_EXPRESSION "LauncherActivityCachingLogic: loadIcon")
endif()
