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

struct ApkIntentFilter {
    std::vector<std::string> actions, categories, schemes, mime_types;
    bool restricted_uri = false;
};

struct ApkComponent {
    std::string name, label;
    uint32_t icon = 0;
    bool enabled = true;
    bool exported = false;
    std::vector<ApkIntentFilter> filters;
};

struct LauncherPackage {
    std::string package, label, application, process;
    uint32_t icon = 0;
    uint32_t target_sdk = 0;
    bool enabled = true;
    std::vector<ApkComponent> activities;
};

std::string ActivityClassName(std::string_view package, std::string_view name);
int32_t ReadApkManifest(std::span<const uint8_t> bytes, ApkManifest& out);
int32_t ReadLauncherPackage(std::span<const uint8_t> bytes, const char* locale, int density,
                            LauncherPackage& out);
int32_t ReadLauncherPackage(const char* path, const char* locale, int density, LauncherPackage& out);

} // namespace wa::system
