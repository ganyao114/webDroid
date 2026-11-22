find_package(Python3 REQUIRED COMPONENTS Interpreter)

get_target_property(framework_dex aosp17_framework_core WA_FRAMEWORK_DEX)
if(TARGET aosp17_launcher3_apk)
    get_target_property(launcher_build aosp17_launcher3_apk BINARY_DIR)
else()
    # The root host project declares the M8 Launcher3 target after android_system.
    set(launcher_build "${CMAKE_BINARY_DIR}/tests/m8_aosp17_launcher3_apk")
endif()
get_target_property(cube_build wa_cube_app_apk BINARY_DIR)
set(apps "${cube_build}/wa_cube_app.apk")

if(TARGET aosp17_systemui_apk)
    get_target_property(systemui_build aosp17_systemui_apk BINARY_DIR)
    set(systemui_target aosp17_systemui_apk)
else()
    get_filename_component(repo "${CMAKE_CURRENT_LIST_DIR}/../.." ABSOLUTE)
    set(systemui_build "${CMAKE_CURRENT_BINARY_DIR}")
    set(systemui_target m9_launcher_systemui_apk)
    set(systemui_output "${systemui_build}/apk")
    set(builder "${repo}/tools/aosp/build_upstream_systemui.py")
    set(patches "${repo}/patches/aosp17/systemui")
    set(WA_SYSTEMUI_DEPS_DIR "${repo}/third_party/aosp17_deps" CACHE PATH "Offline SystemUI dependencies")
    set(WA_APKSIGNER_EXECUTABLE "${WA_ANDROID_SDK}/build-tools/36.0.0/apksigner"
        CACHE FILEPATH "SDK 36 apksigner")
    find_program(WA_JAVA_EXECUTABLE java NO_CMAKE_FIND_ROOT_PATH REQUIRED)
    file(GLOB systemui_patches CONFIGURE_DEPENDS "${patches}/*.patch")
    file(GLOB systemui_kotlinc CONFIGURE_DEPENDS "${WA_SYSTEMUI_DEPS_DIR}/kotlinc/*.jar")
    add_custom_command(OUTPUT "${systemui_output}/SystemUI-upstream.apk"
                              "${systemui_output}/systemui_provenance.json"
        COMMAND "${CMAKE_COMMAND}" -E env PYTHONDONTWRITEBYTECODE=1
            "${Python3_EXECUTABLE}" "${builder}" --aosp-root "${AOSP17_SOURCE_DIR}"
            --deps-dir "${WA_SYSTEMUI_DEPS_DIR}" --out-dir "${systemui_output}"
            --java "${WA_JAVA_EXECUTABLE}" --javac "${WA_JAVAC_EXECUTABLE}"
            --aapt2 "${WA_AAPT2_EXECUTABLE}" --d8 "${WA_D8_EXECUTABLE}"
            --zipalign "${WA_ZIPALIGN_EXECUTABLE}" --apksigner "${WA_APKSIGNER_EXECUTABLE}"
            --android-jar "${WA_ANDROID_JAR}" --depfile "${systemui_output}/systemui.d"
        DEPENDS "${builder}" "${patches}/sources.txt" "${patches}/resources.txt"
            ${systemui_patches} ${systemui_kotlinc}
        DEPFILE "${systemui_output}/systemui.d"
        COMMENT "Compiling and signing the upstream AOSP 17 SystemUI APK"
        VERBATIM)
    add_custom_target(${systemui_target} DEPENDS "${systemui_output}/SystemUI-upstream.apk"
        "${systemui_output}/systemui_provenance.json")
endif()

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
add_dependencies(m9_launcher3_fixtures aosp17_framework_core aosp17_launcher3_apk wa_cube_app_apk
    ${systemui_target})

set(fixtures "${framework_dex}" "${WA_ANDROID_JAR}" "${launcher_build}/apk/Launcher3.apk"
    "${systemui_build}/apk/SystemUI-upstream.apk" ${apps} ${demo_apks})
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
    set_tests_properties(test_m9_launcher3_boot_host PROPERTIES TIMEOUT 120)
endif()
