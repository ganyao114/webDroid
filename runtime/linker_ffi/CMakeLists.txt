if(NOT TARGET svm_wasm)
    include("${CMAKE_CURRENT_LIST_DIR}/../../cmake/SvmEmbed.cmake")
endif()

add_library(wa_guest_audio STATIC ../android_system/guest_audio.cpp)
target_compile_features(wa_guest_audio PUBLIC cxx_std_20)
target_include_directories(wa_guest_audio PRIVATE ../../include)

add_library(wa_linker_ffi STATIC
    dbt.cpp wasm_jit.cpp elf_loader.cpp guest_format.cpp host_symbols.cpp
    bionic_symbols.cpp bionic_dl.cpp bionic_errno.cpp bionic_pthread.cpp
    bionic_stdio.cpp bionic_scan.cpp bionic_locale.cpp bionic_string.cpp bionic_math.cpp
    bionic_posix.cpp bionic_fd.cpp bionic_files.cpp bionic_mmap.cpp bionic_network.cpp bionic_gles.cpp
    bionic_opensles.cpp android_looper.cpp)
target_link_libraries(wa_linker_ffi PUBLIC wa_softmmu
    PRIVATE svm_base svm_arm32_ir svm_arm64_ir svm_ir_optimize svm_wasm wa_guest_audio)
target_include_directories(wa_linker_ffi PRIVATE "${CMAKE_CURRENT_LIST_DIR}/../softmmu_opfs")
if(EMSCRIPTEN)
    target_compile_options(wa_linker_ffi PRIVATE
        -O3 -msimd128 -matomics -mbulk-memory -fwasm-exceptions)
    set(WA_JIT_HOT_THRESHOLD 32 CACHE STRING "Guest entry count before compiling a foreground Wasm unit")
    set_property(SOURCE wasm_jit.cpp APPEND PROPERTY COMPILE_DEFINITIONS
        "WA_JIT_HOT_THRESHOLD=${WA_JIT_HOT_THRESHOLD}")

    execute_process(COMMAND "${EMSCRIPTEN_ROOT_PATH}/em-config${EMCC_SUFFIX}" LLVM_ROOT
        OUTPUT_VARIABLE wa_llvm_root OUTPUT_STRIP_TRAILING_WHITESPACE
        COMMAND_ERROR_IS_FATAL ANY)
    find_program(WA_WASM_LD NAMES wasm-ld HINTS "${wa_llvm_root}" NO_CMAKE_FIND_ROOT_PATH REQUIRED)
    add_library(wa_jit_dispatcher OBJECT wasm_dispatcher.cpp)
    target_include_directories(wa_jit_dispatcher PRIVATE ../../include)
    target_link_libraries(wa_jit_dispatcher PRIVATE svm_wasm)
    target_compile_options(wa_jit_dispatcher PRIVATE -O3 -fno-lto -matomics -mbulk-memory)
    set(embed_wasm "${CMAKE_CURRENT_LIST_DIR}/../../cmake/EmbedWasm.cmake")
    foreach(memory IN ITEMS unshared shared)
        set(memory_flags)
        if(memory STREQUAL "shared")
            set(memory_flags --shared-memory --max-memory=4294967296)
        endif()
        set(dispatcher "${CMAKE_CURRENT_BINARY_DIR}/wasm_dispatcher_${memory}.wasm")
        set(header "${dispatcher}.inc")
        add_custom_command(OUTPUT "${header}"
            COMMAND "${WA_WASM_LD}" --no-entry --import-memory --import-table --strip-all
                    -z stack-size=0 --export=wa_dbt_dispatch ${memory_flags}
                    $<TARGET_OBJECTS:wa_jit_dispatcher> -o "${dispatcher}"
            COMMAND "${CMAKE_COMMAND}" "-DWA_WASM=${dispatcher}" "-DWA_WASM_HEADER=${header}"
                    -P "${embed_wasm}"
            DEPENDS wa_jit_dispatcher $<TARGET_OBJECTS:wa_jit_dispatcher> "${embed_wasm}"
            BYPRODUCTS "${dispatcher}"
            VERBATIM)
        target_sources(wa_linker_ffi PRIVATE "${header}")
    endforeach()
    target_include_directories(wa_linker_ffi PRIVATE "${CMAKE_CURRENT_BINARY_DIR}")
endif()
