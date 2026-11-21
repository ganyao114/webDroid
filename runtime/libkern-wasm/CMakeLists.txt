cmake_minimum_required(VERSION 3.18)
project(wa_libkern LANGUAGES CXX)

add_library(wa_libkern STATIC
    futex.cpp
    epoll.cpp
    binder_ring.cpp
    vfs_opfs.cpp
    syscall.cpp
    jit_registry.cpp
)
target_compile_features(wa_libkern PUBLIC cxx_std_20)
target_include_directories(wa_libkern PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/../../include"
    "${CMAKE_CURRENT_SOURCE_DIR}"
)

if(EMSCRIPTEN)
    target_compile_options(wa_libkern PRIVATE -O3 -sSHARED_MEMORY=1 -matomics -mbulk-memory -msimd128)
else()
    find_package(Threads REQUIRED)
    target_link_libraries(wa_libkern PUBLIC Threads::Threads)
endif()
