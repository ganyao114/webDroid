file(READ "${WA_WASM}" bytes HEX)
string(REGEX REPLACE "(..)" "0x\\1," bytes "${bytes}")
file(WRITE "${WA_WASM_HEADER}" "${bytes}\n")
