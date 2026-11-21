#pragma once

namespace android::os::binder::flags {

// This fixed aconfig flag applies to driver freeze notifications.
inline constexpr bool enable_frozen_object_error() {
    return false;
}

} // namespace android::os::binder::flags
