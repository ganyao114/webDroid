cmake_minimum_required(VERSION 3.18)

if(CMAKE_CURRENT_SOURCE_DIR STREQUAL CMAKE_SOURCE_DIR)
    project(wa_m2_binder_posix LANGUAGES CXX)
    enable_testing()
    if(EMSCRIPTEN)
        add_subdirectory(../../runtime/libkern-wasm libkern)
    else()
        add_subdirectory(../m0_libkern m0_libkern)
    endif()
endif()

if(EMSCRIPTEN)
    add_executable(m2_posix m2_posix_wasm.cpp)
    set_target_properties(m2_posix PROPERTIES SUFFIX ".wasm")
    target_link_libraries(m2_posix PRIVATE wa_libkern)
    target_compile_options(m2_posix PRIVATE -O3 -matomics -mbulk-memory -msimd128)
    target_link_options(m2_posix PRIVATE
        -O3 -matomics -mbulk-memory -msimd128
        -sSHARED_MEMORY=1 -sIMPORTED_MEMORY=1 -sFILESYSTEM=0 -sABORTING_MALLOC=0
        -sINITIAL_MEMORY=33554432 -sMAXIMUM_MEMORY=33554432 -sSTACK_SIZE=262144
        --no-entry -Wl,--export=m2_posix_smoke
    )
    find_program(WA_NODE_EXECUTABLE NAMES node NO_CMAKE_FIND_ROOT_PATH REQUIRED)
    add_test(NAME test_m2_posix_node COMMAND "${WA_NODE_EXECUTABLE}"
        "${CMAKE_CURRENT_SOURCE_DIR}/run_m2_chrome_e2e.mjs" --node
        --build-dir "$<TARGET_FILE_DIR:m2_posix>")
else()
    add_executable(test_m2_binder_posix test_m2_binder_posix.cpp)
    target_link_libraries(test_m2_binder_posix PRIVATE wa_libkern)
    add_test(NAME test_m2_binder_posix COMMAND test_m2_binder_posix)
    set_tests_properties(test_m2_binder_posix PROPERTIES TIMEOUT 45)
endif()
