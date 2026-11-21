#include "wasmandroid/art_wasm.h"

extern "C" uint32_t wa_art_browser_abi_version() {
    return WA_ART_ABI_VERSION;
}
