# Choose the newest installed NDK when a valid bin directory was not supplied.
set(ndk_bin "${WA_ANDROID_NDK_BIN}")
if(NOT EXISTS "${ndk_bin}/aarch64-linux-android24-clang" OR
   NOT EXISTS "${ndk_bin}/armv7a-linux-androideabi24-clang")
    file(GLOB ndk_candidates "${WA_ANDROID_SDK}/ndk/*/toolchains/llvm/prebuilt/*/bin")
    list(SORT ndk_candidates COMPARE NATURAL ORDER DESCENDING)
    foreach(candidate IN LISTS ndk_candidates)
        if(EXISTS "${candidate}/aarch64-linux-android24-clang" AND
           EXISTS "${candidate}/armv7a-linux-androideabi24-clang")
            set(ndk_bin "${candidate}")
            break()
        endif()
    endforeach()
endif()
if(NOT EXISTS "${ndk_bin}/aarch64-linux-android24-clang" OR
   NOT EXISTS "${ndk_bin}/armv7a-linux-androideabi24-clang")
    message(FATAL_ERROR "The hybrid JNI fixtures require an Android NDK with ARM64 and ARMv7 clang drivers")
endif()

set(fixture "${CMAKE_CURRENT_SOURCE_DIR}/fixture")
set(packaging "${CMAKE_CURRENT_BINARY_DIR}/package")
set(ndk_flags -O2 -fPIC -shared -ffp-contract=off -Wl,--no-undefined)
foreach(abi arm64-v8a armeabi-v7a)
    if(abi STREQUAL arm64-v8a)
        set(clang "${ndk_bin}/aarch64-linux-android24-clang")
    else()
        set(clang "${ndk_bin}/armv7a-linux-androideabi24-clang")
    endif()
    set(directory "${packaging}/lib/${abi}")
    set(math "${directory}/libhybrid_math.so")
    set(jni "${directory}/libhybrid_jni.so")
    add_custom_command(OUTPUT "${math}"
        COMMAND "${CMAKE_COMMAND}" -E make_directory "${directory}"
        COMMAND "${clang}" ${ndk_flags} -Wl,-soname,libhybrid_math.so
            "${fixture}/hybrid_math.c" -o "${math}"
        DEPENDS "${clang}" "${fixture}/hybrid_math.c" "${fixture}/hybrid_math.h"
        VERBATIM)
    add_custom_command(OUTPUT "${jni}"
        COMMAND "${clang}" ${ndk_flags} -Wl,-soname,libhybrid_jni.so
            "${fixture}/hybrid_jni.c" "-L${directory}" -lhybrid_math -llog -ldl -ljnigraphics -landroid -o "${jni}"
        DEPENDS "${clang}" "${math}" "${fixture}/hybrid_jni.c" "${fixture}/hybrid_math.h"
        VERBATIM)
    set(libraries_${abi} "lib/${abi}/libhybrid_math.so" "lib/${abi}/libhybrid_jni.so")
endforeach()

set(classes "${CMAKE_CURRENT_BINARY_DIR}/classes")
set(class_path "com/wasmandroid/hybrid")
set(primary "${CMAKE_CURRENT_BINARY_DIR}/activity.jar")
set(secondary "${CMAKE_CURRENT_BINARY_DIR}/bridge.jar")
set(secondary_dex "${CMAKE_CURRENT_BINARY_DIR}/secondary")
set(dex "${packaging}/classes.dex")
set(dex2 "${packaging}/classes2.dex")
add_custom_command(OUTPUT "${dex}" "${dex2}"
    COMMAND "${CMAKE_COMMAND}" -E make_directory "${classes}" "${packaging}" "${secondary_dex}"
    COMMAND "${WA_JAVAC_EXECUTABLE}" --release 8 -encoding UTF-8 -classpath "${WA_ANDROID_JAR}"
        -d "${classes}" "${fixture}/HybridBridge.java" "${fixture}/HybridActivity.java"
    COMMAND "${WA_JAR_EXECUTABLE}" cf "${primary}"
        -C "${classes}" "${class_path}/HybridActivity.class"
        -C "${classes}" "${class_path}/HybridActivity$1.class"
    COMMAND "${WA_JAR_EXECUTABLE}" cf "${secondary}" -C "${classes}" "${class_path}/HybridBridge.class"
    COMMAND "${WA_D8_EXECUTABLE}" --release --min-api 24 --lib "${WA_ANDROID_JAR}"
        --classpath "${secondary}" --output "${packaging}" "${primary}"
    COMMAND "${WA_D8_EXECUTABLE}" --release --min-api 24 --lib "${WA_ANDROID_JAR}"
        --classpath "${primary}" --output "${secondary_dex}" "${secondary}"
    COMMAND "${CMAKE_COMMAND}" -E copy "${secondary_dex}/classes.dex" "${dex2}"
    DEPENDS "${fixture}/HybridActivity.java" "${fixture}/HybridBridge.java"
        "${WA_JAVAC_EXECUTABLE}" "${WA_JAR_EXECUTABLE}" "${WA_D8_EXECUTABLE}" "${WA_ANDROID_JAR}"
    VERBATIM)

set(manifest_apk "${CMAKE_CURRENT_BINARY_DIR}/manifest.apk")
add_custom_command(OUTPUT "${manifest_apk}"
    COMMAND "${WA_AAPT2_EXECUTABLE}" link -I "${WA_ANDROID_JAR}"
        --manifest "${fixture}/AndroidManifest.xml" --min-sdk-version 24 --target-sdk-version 36
        -A "${fixture}/assets"
        -o "${manifest_apk}"
    DEPENDS "${fixture}/AndroidManifest.xml" "${fixture}/assets/native-probe.txt"
        "${WA_AAPT2_EXECUTABLE}" "${WA_ANDROID_JAR}"
    VERBATIM)

set(signing "${PROJECT_SOURCE_DIR}/tools/aosp/launcher3_build/signing")
set(test_key "${signing}/offline-test.pk8")
set(test_cert "${signing}/offline-test.x509.pem")

function(package_hybrid name)
    set(entries ${ARGN})
    set(dependencies ${entries})
    list(TRANSFORM dependencies PREPEND "${packaging}/")
    set(apk "${CMAKE_CURRENT_BINARY_DIR}/${name}.apk")
    set(unaligned "${CMAKE_CURRENT_BINARY_DIR}/${name}-unaligned.apk")
    set(aligned "${CMAKE_CURRENT_BINARY_DIR}/${name}-aligned.apk")
    add_custom_command(OUTPUT "${apk}"
        COMMAND "${CMAKE_COMMAND}" -E copy "${manifest_apk}" "${unaligned}"
        COMMAND "${CMAKE_COMMAND}" -E chdir "${packaging}" "${WA_ZIP_EXECUTABLE}" -q -0
            "${unaligned}" classes.dex classes2.dex ${entries}
        COMMAND "${WA_ZIPALIGN_EXECUTABLE}" -f -P 16 4 "${unaligned}" "${aligned}"
        COMMAND "${WA_APKSIGNER_EXECUTABLE}" sign --key "${test_key}" --cert "${test_cert}"
            --v1-signing-enabled false --v2-signing-enabled true --v3-signing-enabled false
            --v4-signing-enabled false --out "${apk}" "${aligned}"
        COMMAND "${WA_ZIPALIGN_EXECUTABLE}" -c -P 16 4 "${apk}"
        COMMAND "${WA_APKSIGNER_EXECUTABLE}" verify "${apk}"
        DEPENDS "${manifest_apk}" "${dex}" "${dex2}" ${dependencies}
            "${WA_ZIP_EXECUTABLE}" "${WA_ZIPALIGN_EXECUTABLE}" "${WA_APKSIGNER_EXECUTABLE}"
            "${test_key}" "${test_cert}"
        COMMENT "Packaging ${name}.apk with NDK ELF libraries and two DEX files"
        VERBATIM)
    set(${name} "${apk}" PARENT_SCOPE)
endfunction()
package_hybrid(hybrid_art_ndk ${libraries_arm64-v8a} ${libraries_armeabi-v7a})
package_hybrid(hybrid_art_ndk_arm32 ${libraries_armeabi-v7a})
add_custom_target(wa_hybrid_apks ALL DEPENDS "${hybrid_art_ndk}" "${hybrid_art_ndk_arm32}")

get_target_property(fallback aosp17_framework_core WA_FRAMEWORK_DEX)
set(framework_inputs "${WA_LAUNCHER_FRAMEWORK_DEX}" "${fallback}" "${WA_LAUNCHER_FRAMEWORK_RES}")
set(hybrid_inputs ${framework_inputs} "${hybrid_art_ndk}" "${hybrid_art_ndk_arm32}")
if(EMSCRIPTEN)
    add_test(NAME test_m17_hybrid_art_ndk_wasm COMMAND "${WA_NODE_EXECUTABLE}"
        "${CMAKE_CURRENT_SOURCE_DIR}/test_hybrid.mjs" "$<TARGET_FILE:android_system>" ${hybrid_inputs})
    set_tests_properties(test_m17_hybrid_art_ndk_wasm PROPERTIES TIMEOUT 180)
else()
    add_executable(test_m17_hybrid_art_ndk test_hybrid.cpp)
    target_compile_features(test_m17_hybrid_art_ndk PRIVATE cxx_std_23)
    target_link_libraries(test_m17_hybrid_art_ndk PRIVATE wa_android_system wa_android_system_foreground)
    add_dependencies(test_m17_hybrid_art_ndk wa_hybrid_apks wa_foreground_framework_assets)
    add_test(NAME test_m17_hybrid_art_ndk COMMAND test_m17_hybrid_art_ndk ${hybrid_inputs})
    set_tests_properties(test_m17_hybrid_art_ndk PROPERTIES TIMEOUT 120)

    add_executable(test_m17_vlc_probe test_vlc_probe.cpp)
    target_include_directories(test_m17_vlc_probe PRIVATE ../../runtime/android_system)
    target_link_libraries(test_m17_vlc_probe PRIVATE wa_android_system wa_android_system_foreground aosp17_art_interp)
    add_dependencies(test_m17_vlc_probe wa_foreground_framework_assets)
    add_test(NAME test_m17_vlc_probe COMMAND test_m17_vlc_probe ${framework_inputs}
        "${CMAKE_SOURCE_DIR}/third_party/commercial_apks/VLC-Android-3.6.3-arm64-v8a.apk")
    set_tests_properties(test_m17_vlc_probe PROPERTIES TIMEOUT 120)
endif()
