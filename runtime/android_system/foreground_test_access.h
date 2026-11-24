#pragma once

#include "art_interp.h"

namespace wa::system {

using ForegroundProbe = std::string (*)(interp::Vm&, interp::Object*);
int32_t RunForegroundProbe(ForegroundProbe probe, char* out, size_t cap);

} // namespace wa::system
