cmake_minimum_required(VERSION 3.20)
project(WasmAndroid LANGUAGES C CXX)

include(CTest)

if(EMSCRIPTEN)
    add_compile_options(-O3 -flto=thin -msimd128 -matomics -mbulk-memory
        $<$<COMPILE_LANGUAGE:CXX>:-fwasm-exceptions>)
endif()

if(BUILD_TESTING AND NOT EMSCRIPTEN)
    # The standalone M0 test also defines wa_libkern.
    add_subdirectory(tests/m0_libkern)
else()
    add_subdirectory(runtime/libkern-wasm)
endif()
add_subdirectory(runtime/softmmu_opfs)
add_subdirectory(runtime/linker_ffi)
add_subdirectory(runtime/hal_browser)
add_subdirectory(runtime/android_system)

if(BUILD_TESTING AND NOT EMSCRIPTEN)
    add_subdirectory(tests/m1_dbt_elf)
    add_subdirectory(tests/m1_chrome_ndk_cube)
endif()
