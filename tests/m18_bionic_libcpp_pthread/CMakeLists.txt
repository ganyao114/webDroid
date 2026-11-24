set(fixture "${CMAKE_CURRENT_SOURCE_DIR}/fixture")
set(packaging "${CMAKE_CURRENT_BINARY_DIR}/package")
set(ndk_bin "${WA_ANDROID_NDK_BIN}")
if(NOT EXISTS "${ndk_bin}/armv7a-linux-androideabi24-clang++")
    file(GLOB ndk_candidates "${WA_ANDROID_SDK}/ndk/*/toolchains/llvm/prebuilt/*/bin")
    list(SORT ndk_candidates COMPARE NATURAL ORDER DESCENDING)
    foreach(candidate IN LISTS ndk_candidates)
        if(EXISTS "${candidate}/armv7a-linux-androideabi24-clang++" AND
           EXISTS "${candidate}/aarch64-linux-android24-clang++")
            set(ndk_bin "${candidate}")
            break()
        endif()
    endforeach()
endif()
get_filename_component(ndk_toolchain "${ndk_bin}" DIRECTORY)

foreach(abi arm64-v8a armeabi-v7a)
    if(abi STREQUAL arm64-v8a)
        set(target aarch64-linux-android)
        set(compiler "${ndk_bin}/${target}24-clang++")
        set(arch_flags -fno-emulated-tls)
    else()
        set(target arm-linux-androideabi)
        set(compiler "${ndk_bin}/armv7a-linux-androideabi24-clang++")
        set(arch_flags -mthumb)
    endif()
    set(libcxx "${ndk_toolchain}/sysroot/usr/lib/${target}/libc++_shared.so")
    set(directory "${packaging}/lib/${abi}")
    if(NOT EXISTS "${compiler}" OR NOT EXISTS "${libcxx}")
        message(FATAL_ERROR "The Phase 8A fixture requires Android NDK clang++ and libc++_shared.so for ${abi}")
    endif()
    add_custom_command(OUTPUT "${directory}/libcpp_pthread_jni.so" "${directory}/libc++_shared.so"
        COMMAND "${CMAKE_COMMAND}" -E make_directory "${directory}"
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different "${libcxx}" "${directory}/libc++_shared.so"
        COMMAND "${compiler}" -std=c++17 -O2 -g -fPIC -shared -fexceptions -frtti ${arch_flags}
            -Wl,--no-undefined -Wl,--pack-dyn-relocs=android+relr -Wl,-soname,libcpp_pthread_jni.so
            -Wl,--no-as-needed "${fixture}/cpp_pthread_jni.cpp" -lc++_shared -llog -ldl
            -o "${directory}/libcpp_pthread_jni.so"
        DEPENDS "${compiler}" "${libcxx}" "${fixture}/cpp_pthread_jni.cpp"
        COMMENT "Building NDK libc++ and pthread JNI fixture for ${abi}"
        VERBATIM)
    set(libraries_${abi} "lib/${abi}/libcpp_pthread_jni.so" "lib/${abi}/libc++_shared.so")
endforeach()

set(dex "${packaging}/classes.dex")
set(classes "${CMAKE_CURRENT_BINARY_DIR}/classes")
add_custom_command(OUTPUT "${dex}"
    COMMAND "${CMAKE_COMMAND}" -E make_directory "${packaging}" "${classes}"
    COMMAND "${WA_JAVAC_EXECUTABLE}" --release 8 -encoding UTF-8 -classpath "${WA_ANDROID_JAR}"
        -d "${classes}" "${fixture}/CppActivity.java"
    COMMAND "${WA_JAR_EXECUTABLE}" cf "${CMAKE_CURRENT_BINARY_DIR}/activity.jar" -C "${classes}" .
    COMMAND "${WA_D8_EXECUTABLE}" --release --min-api 24 --lib "${WA_ANDROID_JAR}"
        --output "${packaging}" "${CMAKE_CURRENT_BINARY_DIR}/activity.jar"
    DEPENDS "${fixture}/CppActivity.java" "${WA_JAVAC_EXECUTABLE}" "${WA_JAR_EXECUTABLE}"
        "${WA_D8_EXECUTABLE}" "${WA_ANDROID_JAR}"
    VERBATIM)

set(manifest_apk "${CMAKE_CURRENT_BINARY_DIR}/manifest.apk")
add_custom_command(OUTPUT "${manifest_apk}"
    COMMAND "${WA_AAPT2_EXECUTABLE}" link -I "${WA_ANDROID_JAR}"
        --manifest "${fixture}/AndroidManifest.xml" --min-sdk-version 24 --target-sdk-version 36
        -o "${manifest_apk}"
    DEPENDS "${fixture}/AndroidManifest.xml" "${WA_AAPT2_EXECUTABLE}" "${WA_ANDROID_JAR}"
    VERBATIM)

set(signing "${PROJECT_SOURCE_DIR}/tools/aosp/launcher3_build/signing")
set(test_key "${signing}/offline-test.pk8")
set(test_cert "${signing}/offline-test.x509.pem")
function(package_cpp_pthread name)
    set(entries ${ARGN})
    set(dependencies ${entries})
    list(TRANSFORM dependencies PREPEND "${packaging}/")
    set(apk "${CMAKE_CURRENT_BINARY_DIR}/${name}.apk")
    set(unaligned "${CMAKE_CURRENT_BINARY_DIR}/${name}-unaligned.apk")
    set(aligned "${CMAKE_CURRENT_BINARY_DIR}/${name}-aligned.apk")
    add_custom_command(OUTPUT "${apk}"
        COMMAND "${CMAKE_COMMAND}" -E copy "${manifest_apk}" "${unaligned}"
        COMMAND "${CMAKE_COMMAND}" -E chdir "${packaging}" "${WA_ZIP_EXECUTABLE}" -q -0
            "${unaligned}" classes.dex ${entries}
        COMMAND "${WA_ZIPALIGN_EXECUTABLE}" -f -P 16 4 "${unaligned}" "${aligned}"
        COMMAND "${WA_APKSIGNER_EXECUTABLE}" sign --key "${test_key}" --cert "${test_cert}"
            --v1-signing-enabled false --v2-signing-enabled true --v3-signing-enabled false
            --v4-signing-enabled false --out "${apk}" "${aligned}"
        COMMAND "${WA_ZIPALIGN_EXECUTABLE}" -c -P 16 4 "${apk}"
        COMMAND "${WA_APKSIGNER_EXECUTABLE}" verify "${apk}"
        DEPENDS "${manifest_apk}" "${dex}" ${dependencies}
            "${WA_ZIP_EXECUTABLE}" "${WA_ZIPALIGN_EXECUTABLE}" "${WA_APKSIGNER_EXECUTABLE}"
            "${test_key}" "${test_cert}"
        COMMENT "Packaging signed ${name}.apk with NDK ELF libraries"
        VERBATIM)
    set(${name} "${apk}" PARENT_SCOPE)
endfunction()
package_cpp_pthread(cpp_pthread ${libraries_arm64-v8a} ${libraries_armeabi-v7a})
package_cpp_pthread(cpp_pthread_arm32 ${libraries_armeabi-v7a})
add_custom_target(wa_cpp_pthread_apks ALL DEPENDS "${cpp_pthread}" "${cpp_pthread_arm32}")

get_target_property(fallback aosp17_framework_core WA_FRAMEWORK_DEX)
set(inputs "${WA_LAUNCHER_FRAMEWORK_DEX}" "${fallback}" "${WA_LAUNCHER_FRAMEWORK_RES}"
    "${cpp_pthread}" "${cpp_pthread_arm32}")
if(EMSCRIPTEN)
    add_test(NAME test_m18_cpp_pthread_hybrid_wasm COMMAND "${WA_NODE_EXECUTABLE}"
        "${CMAKE_CURRENT_SOURCE_DIR}/test_cpp_pthread_hybrid.mjs" "$<TARGET_FILE:android_system>" ${inputs})
    set_tests_properties(test_m18_cpp_pthread_hybrid_wasm PROPERTIES TIMEOUT 180)
else()
    add_executable(test_m18_cpp_pthread_hybrid test_cpp_pthread_hybrid.cpp)
    target_include_directories(test_m18_cpp_pthread_hybrid PRIVATE ../m17_hybrid_art_ndk)
    target_compile_features(test_m18_cpp_pthread_hybrid PRIVATE cxx_std_23)
    target_link_libraries(test_m18_cpp_pthread_hybrid PRIVATE wa_android_system wa_android_system_foreground)
    add_dependencies(test_m18_cpp_pthread_hybrid wa_cpp_pthread_apks wa_foreground_framework_assets)
    add_test(NAME test_m18_cpp_pthread_hybrid COMMAND test_m18_cpp_pthread_hybrid ${inputs})
    set_tests_properties(test_m18_cpp_pthread_hybrid PROPERTIES TIMEOUT 120)

    add_executable(test_m18_vlc_native_so test_vlc_native_so.cpp)
    target_compile_features(test_m18_vlc_native_so PRIVATE cxx_std_23)
    target_include_directories(test_m18_vlc_native_so PRIVATE ../m17_hybrid_art_ndk ../../runtime/android_system)
    target_link_libraries(test_m18_vlc_native_so PRIVATE wa_android_system wa_android_system_foreground aosp17_art_interp)
    add_dependencies(test_m18_vlc_native_so wa_foreground_framework_assets)
    add_test(NAME test_m18_vlc_native_so COMMAND test_m18_vlc_native_so
        "${CMAKE_SOURCE_DIR}/third_party/commercial_apks/VLC-Android-3.6.3-arm64-v8a.apk"
        "${WA_LAUNCHER_FRAMEWORK_DEX}" "${fallback}")
    set_tests_properties(test_m18_vlc_native_so PROPERTIES TIMEOUT 120)
endif()
