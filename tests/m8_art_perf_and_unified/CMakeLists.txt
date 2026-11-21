set(fixture "${CMAKE_CURRENT_BINARY_DIR}/fixture")
set(dex "${fixture}/classes.dex")
set(source "${CMAKE_CURRENT_SOURCE_DIR}/PerfBench.java")

add_custom_command(OUTPUT "${dex}"
    COMMAND "${CMAKE_COMMAND}" -E make_directory "${fixture}/classes"
    COMMAND "${WA_JAVAC_EXECUTABLE}" --release 17 -encoding UTF-8 -d "${fixture}/classes" "${source}"
    COMMAND "${WA_JAR_EXECUTABLE}" --create --file "${fixture}/bench.jar" -C "${fixture}/classes" .
    COMMAND "${WA_D8_EXECUTABLE}" --min-api 26 --lib "${WA_ANDROID_JAR}"
        --output "${fixture}" "${fixture}/bench.jar"
    DEPENDS "${source}" "${WA_JAVAC_EXECUTABLE}" "${WA_JAR_EXECUTABLE}"
        "${WA_D8_EXECUTABLE}" "${WA_ANDROID_JAR}"
    COMMENT "Building the Java 17 ART view tree with SDK 36 d8"
    VERBATIM)

get_target_property(home_dir wa_home_launcher_apk BINARY_DIR)
set(resources "${CMAKE_BINARY_DIR}/tests/m7_aosp17_art_interp/fixtures")
set(fixtures "${dex}" "${resources}/pm/aosp17_pm_core.dex" "${resources}/pm_res_bridge.dex"
    "${home_dir}/wa_home_launcher.apk")
add_custom_target(m8_art_perf_fixtures ALL DEPENDS "${dex}")
add_dependencies(m8_art_perf_fixtures aosp17_art_interp_fixtures wa_home_launcher_apk)

if(EMSCRIPTEN)
    add_test(NAME test_m8_art_perf_wasm COMMAND "${WA_NODE_EXECUTABLE}"
        "${CMAKE_CURRENT_SOURCE_DIR}/verify_wasm.mjs" "$<TARGET_FILE:android_system>" ${fixtures})
    set_tests_properties(test_m8_art_perf_wasm PROPERTIES TIMEOUT 90 RUN_SERIAL TRUE)
else()
    add_executable(test_m8_art_perf_host verify_host.cpp)
    add_dependencies(test_m8_art_perf_host m8_art_perf_fixtures)
    target_link_libraries(test_m8_art_perf_host PRIVATE wa_android_system aosp17_art_interp)
    add_test(NAME test_m8_art_perf_host COMMAND test_m8_art_perf_host ${fixtures})
    set_tests_properties(test_m8_art_perf_host PROPERTIES TIMEOUT 60 RUN_SERIAL TRUE)
endif()
