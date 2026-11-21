set(fixture "${CMAKE_CURRENT_SOURCE_DIR}/fixture")
set(home_apk "${CMAKE_CURRENT_BINARY_DIR}/wa_home_launcher.apk")
set(home_build "${CMAKE_CURRENT_BINARY_DIR}/home")
get_target_property(cube_build wa_cube_app_apk BINARY_DIR)
set(cube_apk "${cube_build}/wa_cube_app.apk")

add_custom_command(OUTPUT "${home_apk}"
    COMMAND "${CMAKE_COMMAND}" -E make_directory
        "${home_build}/java" "${home_build}/classes" "${home_build}/dex"
    COMMAND "${WA_AAPT2_EXECUTABLE}" compile --dir "${fixture}/res"
        -o "${home_build}/resources.zip"
    COMMAND "${WA_AAPT2_EXECUTABLE}" link -I "${WA_ANDROID_JAR}"
        --manifest "${fixture}/AndroidManifest.xml" --min-sdk-version 23 --target-sdk-version 36
        --java "${home_build}/java" -o "${home_build}/unaligned.apk" "${home_build}/resources.zip"
    COMMAND "${WA_JAVAC_EXECUTABLE}" --release 8 -classpath "${WA_ANDROID_JAR}"
        -d "${home_build}/classes" "${home_build}/java/org/wasmandroid/home/R.java"
        "${fixture}/HomeActivity.java"
    COMMAND "${WA_JAR_EXECUTABLE}" --create --file "${home_build}/home.jar"
        -C "${home_build}/classes" .
    COMMAND "${WA_D8_EXECUTABLE}" --min-api 23 --lib "${WA_ANDROID_JAR}"
        --output "${home_build}/dex" "${home_build}/home.jar"
    COMMAND "${CMAKE_COMMAND}" -E chdir "${home_build}/dex" "${WA_ZIP_EXECUTABLE}"
        -q -0 "${home_build}/unaligned.apk" classes.dex
    COMMAND "${WA_ZIPALIGN_EXECUTABLE}" -f 4 "${home_build}/unaligned.apk" "${home_apk}"
    DEPENDS "${fixture}/AndroidManifest.xml" "${fixture}/HomeActivity.java"
        "${fixture}/res/values/strings.xml" "${WA_ANDROID_JAR}"
        "${WA_AAPT2_EXECUTABLE}" "${WA_JAVAC_EXECUTABLE}" "${WA_JAR_EXECUTABLE}"
        "${WA_D8_EXECUTABLE}" "${WA_ZIP_EXECUTABLE}" "${WA_ZIPALIGN_EXECUTABLE}"
    COMMENT "Building the SDK 36 Home launcher fixture APK"
    VERBATIM)
add_custom_target(wa_home_launcher_apk ALL DEPENDS "${home_apk}")

if(EMSCRIPTEN)
    # The existing M0 ring code runs against its own shared memory.
    file(GENERATE OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/ring_anchor.cpp"
        CONTENT "// Ring operations are exported from wa_libkern.\n")
    add_executable(wa_system_binder_ring "${CMAKE_CURRENT_BINARY_DIR}/ring_anchor.cpp")
    set_target_properties(wa_system_binder_ring PROPERTIES SUFFIX ".wasm"
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
    target_link_libraries(wa_system_binder_ring PRIVATE wa_libkern)
    target_link_options(wa_system_binder_ring PRIVATE --no-entry -O3 -flto=thin -fwasm-exceptions
        -sSHARED_MEMORY=1 -sIMPORTED_MEMORY=1 -sFILESYSTEM=0 -sSTACK_SIZE=65536
        -sINITIAL_MEMORY=16777216 -sMAXIMUM_MEMORY=16777216)
    foreach(symbol __heap_base __stack_pointer wa_binder_ring_attach wa_binder_ring_reserve
            wa_binder_ring_commit wa_binder_ring_claim wa_binder_ring_release)
        target_link_options(wa_system_binder_ring PRIVATE "-Wl,--export=${symbol}")
    endforeach()
    add_test(NAME test_m7_system_binder_node COMMAND "${WA_NODE_EXECUTABLE}"
        "${CMAKE_CURRENT_SOURCE_DIR}/verify_m7_system_binder_node.mjs"
        --wasm-build "${CMAKE_BINARY_DIR}" --apk "${cube_apk}" --home-apk "${home_apk}")
    set_tests_properties(test_m7_system_binder_node PROPERTIES TIMEOUT 60)
else()
    add_executable(test_m7_system_binder_host test_m7_system_binder_host.cpp)
    target_link_libraries(test_m7_system_binder_host PRIVATE wa_android_system aosp17_servicemanager)
    add_dependencies(test_m7_system_binder_host wa_cube_app_apk wa_home_launcher_apk)
    add_test(NAME test_m7_system_binder_host COMMAND test_m7_system_binder_host
        "${cube_apk}" "${home_apk}")
    set_tests_properties(test_m7_system_binder_host PROPERTIES TIMEOUT 45)
endif()
