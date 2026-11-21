cmake_minimum_required(VERSION 3.18)

if(CMAKE_CURRENT_SOURCE_DIR STREQUAL CMAKE_SOURCE_DIR)
    project(wa_binder_ipc LANGUAGES CXX)
endif()

if(NOT TARGET wa_libkern)
    add_subdirectory(../libkern-wasm libkern)
endif()

add_library(wa_binder_ipc STATIC parcel.cpp ibinder.cpp servicemanager.cpp)
target_compile_features(wa_binder_ipc PUBLIC cxx_std_20)
target_link_libraries(wa_binder_ipc PUBLIC wa_libkern)

if(EMSCRIPTEN)
    target_compile_options(wa_binder_ipc PRIVATE
        -O3 -matomics -mbulk-memory -msimd128)
endif()
