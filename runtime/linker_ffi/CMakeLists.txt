if(NOT TARGET svm_wasm)
    include("${CMAKE_CURRENT_LIST_DIR}/../../cmake/SvmEmbed.cmake")
endif()

add_library(wa_linker_ffi STATIC
    dbt.cpp elf_loader.cpp guest_format.cpp host_symbols.cpp
    bionic_symbols.cpp bionic_dl.cpp bionic_errno.cpp bionic_pthread.cpp
    bionic_stdio.cpp bionic_scan.cpp bionic_locale.cpp bionic_string.cpp bionic_math.cpp
    bionic_posix.cpp bionic_fd.cpp bionic_files.cpp bionic_mmap.cpp bionic_network.cpp bionic_gles.cpp)
target_link_libraries(wa_linker_ffi PUBLIC wa_softmmu
    PRIVATE svm_base svm_arm32_ir svm_arm64_ir svm_ir_optimize svm_wasm)
target_include_directories(wa_linker_ffi PRIVATE "${CMAKE_CURRENT_LIST_DIR}/../softmmu_opfs")
if(EMSCRIPTEN)
    target_compile_options(wa_linker_ffi PRIVATE
        -O3 -msimd128 -matomics -mbulk-memory -fwasm-exceptions)
endif()
