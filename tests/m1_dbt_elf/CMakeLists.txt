cmake_minimum_required(VERSION 3.20)

if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    project(WasmAndroidM1DbtElf LANGUAGES C CXX)
    enable_testing()
    set(wa_root "${CMAKE_CURRENT_SOURCE_DIR}/../..")
    include("${wa_root}/cmake/SvmEmbed.cmake")
    add_subdirectory("${wa_root}/runtime/softmmu_opfs" softmmu)
    add_subdirectory("${wa_root}/runtime/linker_ffi" linker_ffi)
endif()

set(WA_ANDROID_NDK_BIN
    "$ENV{HOME}/Library/Android/sdk/ndk/29.0.13846066/toolchains/llvm/prebuilt/darwin-x86_64/bin"
    CACHE PATH "Directory containing the Android NDK 29 clang drivers")
set(WA_NODE_EXECUTABLE "/opt/homebrew/bin/node" CACHE FILEPATH "Node.js for Wasm validation")

set(fixture_source "${CMAKE_CURRENT_SOURCE_DIR}/guest_fixture.c")
set(provider_source "${CMAKE_CURRENT_SOURCE_DIR}/virtual_provider.c")
foreach(arch IN ITEMS armeabi-v7a arm64-v8a)
    if(arch STREQUAL "armeabi-v7a")
        set(driver armv7a-linux-androideabi21-clang)
        set(arch_flags -mthumb -mfpu=neon)
        set(hash_style sysv)
    else()
        set(driver aarch64-linux-android21-clang)
        set(arch_flags)
        set(hash_style gnu)
    endif()
    set(cc "${WA_ANDROID_NDK_BIN}/${driver}")
    set(fixture_dir "${CMAKE_CURRENT_BINARY_DIR}/${arch}")
    set(provider "${fixture_dir}/libwa_virtual.so")
    set(fixture "${fixture_dir}/libwa_fixture.so")
    add_custom_command(OUTPUT "${fixture}" "${provider}"
        COMMAND "${CMAKE_COMMAND}" -E make_directory "${fixture_dir}"
        COMMAND "${cc}" ${arch_flags} -O2 -fPIC -shared -Wl,-soname,libwa_virtual.so
                "${provider_source}" -o "${provider}"
        COMMAND "${cc}" ${arch_flags} -std=c11 -O2 -fno-builtin -fPIC -shared
                "-Wl,--hash-style=${hash_style}" -Wl,-soname,libwa_fixture.so -Wl,--no-undefined
                "${fixture_source}" "${provider}" -lm -o "${fixture}"
        DEPENDS "${fixture_source}" "${provider_source}"
        VERBATIM)
    list(APPEND fixtures "${fixture}")
endforeach()

add_custom_target(wa_m1_dbt_elf_fixtures DEPENDS ${fixtures})
add_executable(test_m1_dbt_elf test_m1_dbt_elf.cpp)
add_dependencies(test_m1_dbt_elf wa_m1_dbt_elf_fixtures)
target_link_libraries(test_m1_dbt_elf PRIVATE wa_linker_ffi)
if(EMSCRIPTEN)
    target_include_directories(test_m1_dbt_elf PRIVATE "../../runtime/softmmu_opfs")
    target_compile_options(test_m1_dbt_elf PRIVATE
        -O3 -msimd128 -matomics -mbulk-memory -fwasm-exceptions)
    target_link_options(test_m1_dbt_elf PRIVATE -sENVIRONMENT=node -sNODERAWFS=1
        -sALLOW_MEMORY_GROWTH=1 -sEXIT_RUNTIME=1)
endif()

set(units "${CMAKE_CURRENT_BINARY_DIR}/arm32-unit.wasm" "${CMAKE_CURRENT_BINARY_DIR}/arm64-unit.wasm")
add_test(NAME test_m1_dbt_elf COMMAND test_m1_dbt_elf ${fixtures} ${units})
set_tests_properties(test_m1_dbt_elf PROPERTIES FIXTURES_SETUP wa_wasm_units)
add_test(NAME test_m1_dbt_elf_wasm
    COMMAND "${WA_NODE_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/validate_units.mjs" ${units})
set_tests_properties(test_m1_dbt_elf_wasm PROPERTIES FIXTURES_REQUIRED wa_wasm_units)
