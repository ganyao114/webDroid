#include "wasmandroid/foreground_activity.h"

#include <array>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string_view>
#include <vector>

namespace {

void Require(bool condition, std::string_view message) {
    if (!condition) throw std::runtime_error(std::string(message));
}

void Check(int32_t status) {
    if (status >= 0) return;
    std::array<char, 16384> error{};
    wa_system_app_last_error(error.data(), error.size());
    throw std::runtime_error(error.data());
}

std::vector<uint8_t> Read(const char* path) {
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    const auto size = file.tellg();
    if (size <= 0) throw std::runtime_error(std::string("Could not open ") + path);
    std::vector<uint8_t> bytes(static_cast<size_t>(size));
    if (!file.seekg(0).read(reinterpret_cast<char*>(bytes.data()), size)) {
        throw std::runtime_error(std::string("Could not read ") + path);
    }
    return bytes;
}

std::string State() {
    std::array<char, 4096> buffer{};
    Check(wa_system_app_state_json(buffer.data(), buffer.size()));
    return buffer.data();
}

double Metric(const std::string& state, std::string_view field) {
    const auto key = '"' + std::string(field) + "\":";
    const auto pos = state.find(key);
    if (pos == state.npos) throw std::runtime_error("Missing " + std::string(field));
    const auto* value = state.c_str() + pos + key.size();
    char* end = nullptr;
    const double result = std::strtod(value, &end);
    Require(end != value, field);
    return result;
}

struct Frame {
    uint32_t sequence = 0;
    size_t draws = 0;
    size_t content = 0;
    bool has_bitmap_pixels = false;
};

Frame ReadFrame(wa_sf_t* scene) {
    const auto* packet = wa_sf_frame_acquire(scene);
    Require(packet != nullptr, "Missing SurfaceFlinger frame");
    struct Release {
        wa_sf_t* scene;
        const wa_sf_frame_packet_t* packet;
        ~Release() { wa_sf_frame_release(scene, packet); }
    } release{scene, packet};
    Frame result{.sequence = packet->sequence};
    const auto* layers = wa_sf_frame_layers(packet);
    for (uint32_t i = 0; i < packet->layer_count; ++i) {
        const auto& layer = layers[i];
        if (!(layer.desc.flags & WA_SF_VISIBLE) || layer.desc.surface_kind != WA_SF_SURFACE_DISPLAY_LIST) continue;
        const auto* list = wa_sf_frame_list(packet, &layer);
        Require(list || layer.list_size == 0, "Missing display list");
        for (uint32_t offset = 0; offset < layer.list_size;) {
            const auto remaining = layer.list_size - offset;
            wa_canvas_record_t record{};
            Require(remaining >= sizeof(record), "Truncated display-list record");
            std::memcpy(&record, list + offset, sizeof(record));
            Require(record.byte_size >= sizeof(record) && record.byte_size <= remaining,
                    "Invalid display-list record size");
            if (record.opcode >= WA_CANVAS_DRAW_COLOR && record.opcode <= WA_CANVAS_DRAW_TEXT) ++result.draws;
            if (record.opcode >= WA_CANVAS_DRAW_RECT && record.opcode <= WA_CANVAS_DRAW_TEXT) ++result.content;
            if (record.opcode == WA_CANVAS_DRAW_BITMAP && !result.has_bitmap_pixels) {
                wa_canvas_bitmap_t bitmap{};
                Require(record.byte_size >= sizeof(record) + sizeof(bitmap), "Truncated bitmap command");
                std::memcpy(&bitmap, list + offset + sizeof(record), sizeof(bitmap));
                auto* store = wa_sf_bitmaps(scene);
                if (const auto* view = wa_bitmap_acquire(store, bitmap.bitmap_id)) {
                    const auto* pixels = wa_bitmap_pixels(view);
                    for (size_t p = 3; p < view->byte_size && pixels; p += 4) {
                        if (pixels[p] == 0) continue;
                        result.has_bitmap_pixels = true;
                        break;
                    }
                    wa_bitmap_release(store, view);
                }
            }
            offset += record.byte_size;
        }
    }
    return result;
}

} // namespace

int main(int argc, char** argv) try {
    Require(argc == 5, "Expected framework DEX, fallback DEX, framework resources and VLC APK");
    const std::filesystem::path apk(argv[4]);
    if (!std::filesystem::exists(apk)) {
        std::cout << "VLC APK is absent\n";
        return 0;
    }
    Check(wa_system_app_create(480, 800, 160, "en-US"));
    struct Destroy { ~Destroy() { wa_system_app_destroy(); } } destroy;
    const auto load = [](const char* path, auto install) {
        const auto bytes = Read(path);
        Check(install(bytes.data(), bytes.size()));
    };
    load(argv[1], wa_system_app_load_framework_dex);
    load(argv[2], wa_system_app_load_framework_fallback_dex);
    load(argv[3], wa_system_app_set_framework_res_apk);

    const auto started = std::chrono::steady_clock::now();
    try {
        {
            const auto bytes = Read(argv[4]);
            Check(wa_system_app_boot_apk(apk.filename().string().c_str(), bytes.data(), bytes.size(), nullptr));
        }
        const auto boot = State();
        Require(boot.contains("\"booted\":true") && boot.contains("\"package\":\"org.videolan.vlc\"") &&
            boot.contains("\"application\":\"org.videolan.mobile.app.VLCApplication\""), boot);
        for (const auto* field : {"applicationCreateCalls", "applicationCreateReturns", "onCreateCalls",
                                  "onStartCalls", "onResumeCalls"}) {
            Require(Metric(boot, field) == 1, field);
        }
        for (const auto* field : {"executedApplicationMethods", "executedActivityMethods"}) {
            Require(Metric(boot, field) > 0, field);
        }
        Require(boot.contains("\"launchActivity\":\"org.videolan.vlc.StartActivity\"") &&
                Metric(boot, "dexFiles") == 6 && Metric(boot, "dexClasses") == 32842 &&
                Metric(boot, "providersCreated") == 5, boot);

        auto* scene = wa_system_app_sf_ctx();
        const auto first = ReadFrame(scene);
        int64_t now = 0;
        for (int frame = 0; frame < 60; ++frame) {
            now += 16'666'667;
            Check(wa_system_app_tick(now));
        }
        const auto shown = ReadFrame(scene);
        const auto steady = State();
        const bool onboarding = steady.contains("\"activity\":\"org.videolan.vlc.gui.onboarding.OnboardingActivity\"");
        Require(onboarding ||
                steady.contains("\"activity\":\"org.videolan.vlc.gui.MainActivity\""), steady);
        Require(shown.sequence > first.sequence && shown.content > 0 && Metric(steady, "frames") >= 60 &&
                Metric(steady, "drawCommands") > 0 && steady.contains("\"finishing\":false"), steady);
        Require(shown.has_bitmap_pixels, "VLC rendered no visible bitmap pixels");
        std::cout << steady << '\n';
        std::cout << "SurfaceFlinger draws " << shown.draws
                  << ", bitmap pixels " << shown.has_bitmap_pixels << '\n';

        const auto back = wa_system_app_send_back();
        Check(back);
        const auto after_back = State();
        if (onboarding) {
            Require(back == 0 && Metric(after_back, "onDestroyCalls") > Metric(steady, "onDestroyCalls"),
                    after_back);
        }
        if (back == 1) Check(wa_system_app_tick(now + 16'666'667));
        Require(State().contains(back == 0 ? "\"finishing\":true" : "\"finishing\":false"),
                "Back result and activity state disagree");
        std::cout << "Back " << after_back << '\n';
    } catch (...) {
        const std::chrono::duration<double, std::milli> elapsed = std::chrono::steady_clock::now() - started;
        std::cerr << "VLC run stopped after " << elapsed.count() << " ms\n" << State() << '\n';
        throw;
    }
    return 0;
} catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
}
