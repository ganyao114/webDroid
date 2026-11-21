#pragma once

#include "system_entry.h"

#include <memory>

namespace wa::system {

void RetainGuestBacking(wa_system_guest_t* guest, std::shared_ptr<void> backing);

} // namespace wa::system
