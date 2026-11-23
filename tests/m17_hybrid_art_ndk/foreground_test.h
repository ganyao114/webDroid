#pragma once

#include "wasmandroid/foreground_activity.h"

#include <array>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace hybrid_test {

inline std::vector<uint8_t> Read(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file || file.tellg() <= 0) throw std::runtime_error("Cannot read " + path.string());
    std::vector<uint8_t> bytes(static_cast<size_t>(file.tellg()));
    file.seekg(0);
    if (!file.read(reinterpret_cast<char*>(bytes.data()), bytes.size()))
        throw std::runtime_error("Could not read all of " + path.string());
    return bytes;
}

inline void Require(bool condition, std::string message) {
    if (!condition) throw std::runtime_error(std::move(message));
}

inline std::string LastError() {
    std::array<char, 16384> buffer{};
    wa_system_app_last_error(buffer.data(), buffer.size());
    return buffer.data();
}

inline void Check(int32_t status, std::string_view operation) {
    if (status < 0)
        throw std::runtime_error(std::string(operation) + " (" + std::to_string(status) + "): " + LastError());
}

inline std::string State() {
    std::array<char, 8192> buffer{};
    Check(wa_system_app_state_json(buffer.data(), buffer.size()), "Read foreground state");
    return buffer.data();
}

class Foreground {
  public:
    Foreground(const char* framework, const char* fallback, const char* resources) {
        Check(wa_system_app_create(480, 800, 160, "en-US"), "Create foreground process");
        try {
            Load(framework, wa_system_app_load_framework_dex);
            Load(fallback, wa_system_app_load_framework_fallback_dex);
            Load(resources, wa_system_app_set_framework_res_apk);
        } catch (...) {
            wa_system_app_destroy();
            throw;
        }
    }
    ~Foreground() { wa_system_app_destroy(); }
    Foreground(const Foreground&) = delete;
    Foreground& operator=(const Foreground&) = delete;

    int32_t Boot(const std::filesystem::path& path, const char* activity = nullptr) {
        const auto bytes = Read(path);
        return wa_system_app_boot_apk(path.filename().string().c_str(), bytes.data(), bytes.size(), activity);
    }

  private:
    static void Load(const char* path, int32_t (*load)(const uint8_t*, size_t)) {
        const auto bytes = Read(path);
        Check(load(bytes.data(), bytes.size()), path);
    }
};

} // namespace hybrid_test
