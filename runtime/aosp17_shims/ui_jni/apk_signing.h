#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace wa::ui_jni::detail {

// Reads the leaf certificates recorded in an APK Signature Scheme v2 block.
std::vector<std::vector<uint8_t>> ReadApkV2Signers(const std::string& path);

} // namespace wa::ui_jni::detail
