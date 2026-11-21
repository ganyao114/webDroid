# Run the demo's build with this build directory in place of its Wave 3 default.
get_filename_component(ndk_home "${NDK_BIN}/../../../../.." ABSOLUTE)
file(READ "${DEMO}/build_ndk.sh" script)
string(REGEX REPLACE "\ndemo_dir=[^\n]*" "\ndemo_dir=\"$WA_CUBE_DEMO_DIR\"" script "${script}")
string(REGEX REPLACE "\nout_dir=[^\n]*" "\nout_dir=\"$WA_CUBE_NDK_OUT\"" script "${script}")
file(MAKE_DIRECTORY "${OUT}")
file(WRITE "${OUT}/build_ndk.sh" "${script}")
execute_process(COMMAND "${CMAKE_COMMAND}" -E env "ANDROID_NDK_HOME=${ndk_home}"
    "WA_CUBE_DEMO_DIR=${DEMO}" "WA_CUBE_NDK_OUT=${OUT}" bash "${OUT}/build_ndk.sh"
    COMMAND_ERROR_IS_FATAL ANY)
