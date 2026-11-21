#pragma once

#include "wasmandroid/hal_gfx.h"

namespace wa::gfx {

const wa_gfx_surface_config_t* surface_config();
uint32_t surface_generation();
bool has_current_surface();
int32_t make_current(bool current);

} // namespace wa::gfx
