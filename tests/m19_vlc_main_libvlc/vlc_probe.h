#pragma once

#include <cstddef>
#include <cstdint>

extern "C" {
int32_t wa_test_vlc_abi(char* out, size_t cap);
int32_t wa_test_vlc_labels(char* out, size_t cap);
int32_t wa_test_vlc_media(char* out, size_t cap);
}
