#pragma once

#include <cstdint>

// The sparse checkout omits libartpalette. The upstream STATIC_LIB switch
// disables tracing; these declarations let the unused inline helpers parse.
enum { PALETTE_STATUS_OK = 0 };
extern "C" {
int32_t PaletteTraceEnabled(bool* enabled);
int32_t PaletteTraceBegin(const char* name);
int32_t PaletteTraceEnd();
int32_t PaletteTraceIntegerValue(const char* name, int32_t value);
}
