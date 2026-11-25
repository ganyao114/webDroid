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
endif()
