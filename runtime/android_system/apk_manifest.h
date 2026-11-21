#pragma once

#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace wa::system {

struct ApkManifest {
    std::string package;
    std::string label;
    int64_t version_code;
    std::string launcher;
    std::string home;
    std::vector<std::string> activities;
    int32_t abi_mask;
};

std::string ActivityClassName(std::string_view package, std::string_view name);
int32_t ReadApkManifest(std::span<const uint8_t> bytes, ApkManifest& out);

} // namespace wa::system
