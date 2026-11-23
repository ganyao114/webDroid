#include "wasmandroid/foreground_activity.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <string_view>
#include <vector>

namespace {

std::vector<uint8_t> Read(const char* path) {
    std::ifstream stream(path, std::ios::binary);
    if (!stream) throw std::runtime_error(std::string("Could not open ") + path);
    return {std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>()};
}

void Check(const char* step, int32_t status) {
    if (status >= 0) return;
    std::array<char, 8192> error{};
    wa_system_app_last_error(error.data(), error.size());
    throw std::runtime_error(std::string(step) + " (" + std::to_string(status) + "): " + error.data());
}

std::string State() {
    std::array<char, 8192> state{};
    Check("State", wa_system_app_state_json(state.data(), state.size()));
    return state.data();
}

double Metric(const std::string& state, const char* name) {
    const auto key = std::string("\"") + name + "\":";
    const auto offset = state.find(key);
    if (offset == state.npos) throw std::runtime_error("Missing metric " + std::string(name));
    return std::strtod(state.c_str() + offset + key.size(), nullptr);
}

struct Label {
    std::string text;
    float x, y;
};

struct Frame {
    size_t draws = 0;
    std::vector<Label> labels;
};

struct Transform {
    float x, y;
    float scale_x = 1, scale_y = 1;
};

Frame ReadFrame(wa_sf_t* scene) {
    const auto* packet = wa_sf_frame_acquire(scene);
    if (!packet) throw std::runtime_error("No SurfaceFlinger frame");
    struct Release {
        wa_sf_t* scene;
        const wa_sf_frame_packet_t* packet;
        ~Release() { wa_sf_frame_release(scene, packet); }
    } release{scene, packet};
    const auto* layers = wa_sf_frame_layers(packet);
    Frame frame;
    for (uint32_t i = 0; i < packet->layer_count; ++i) {
        const auto& layer = layers[i];
        if (!(layer.desc.flags & WA_SF_VISIBLE) || layer.desc.surface_kind != WA_SF_SURFACE_DISPLAY_LIST) continue;
        const auto* list = wa_sf_frame_list(packet, &layer);
        Transform transform{layer.desc.x - layer.desc.scroll_x, layer.desc.y - layer.desc.scroll_y};
        std::vector<Transform> stack;
        for (uint32_t offset = 0; list && offset < layer.list_size;) {
            const auto available = layer.list_size - offset;
            wa_canvas_record_t record;
            if (available < sizeof(record)) throw std::runtime_error("Truncated display-list record");
            std::memcpy(&record, list + offset, sizeof(record));
            if (record.byte_size < sizeof(record) || record.byte_size > available) {
                throw std::runtime_error("Invalid display-list record");
            }
            if (record.opcode >= WA_CANVAS_DRAW_COLOR && record.opcode <= WA_CANVAS_DRAW_TEXT) ++frame.draws;
            if (record.opcode == WA_CANVAS_SAVE) stack.push_back(transform);
            if (record.opcode == WA_CANVAS_RESTORE) {
                if (stack.empty()) throw std::runtime_error("Unbalanced display-list restore");
                transform = stack.back();
                stack.pop_back();
            }
            if (record.opcode == WA_CANVAS_TRANSLATE || record.opcode == WA_CANVAS_SCALE) {
                wa_canvas_xy_t xy;
                if (record.byte_size < sizeof(record) + sizeof(xy)) throw std::runtime_error("Truncated transform");
                std::memcpy(&xy, list + offset + sizeof(record), sizeof(xy));
                if (record.opcode == WA_CANVAS_TRANSLATE) {
                    transform.x += transform.scale_x * xy.x;
                    transform.y += transform.scale_y * xy.y;
                } else {
                    transform.scale_x *= xy.x;
                    transform.scale_y *= xy.y;
                }
            }
            if (record.opcode == WA_CANVAS_DRAW_TEXT) {
                wa_canvas_text_t text;
                if (record.byte_size < sizeof(record) + sizeof(text)) throw std::runtime_error("Truncated text record");
                std::memcpy(&text, list + offset + sizeof(record), sizeof(text));
                if (text.utf8_offset > record.byte_size || text.utf8_len > record.byte_size - text.utf8_offset) {
                    throw std::runtime_error("Invalid display-list text");
                }
                const auto* utf8 = reinterpret_cast<const char*>(list + offset + text.utf8_offset);
                if (text.utf8_len != 0 && (text.argb >> 24) != 0) {
                    const auto metrics = wa_canvas_measure_text(scene, utf8, text.utf8_len, text.font_size,
                                                                text.weight, text.font_face);
                    const float width = text.max_width > 0 ? std::min(metrics.advance, text.max_width) : metrics.advance;
                    const float x = transform.x + transform.scale_x * (text.x + width * (1.0f - text.align) / 2);
                    const float y = transform.y + transform.scale_y * (text.y + (metrics.descent - metrics.ascent) / 2);
                    if (x >= 0 && x < packet->viewport_w && y >= 0 && y < packet->viewport_h) {
                        frame.labels.push_back({std::string(utf8, text.utf8_len), x, y});
                    }
                }
            }
            offset += record.byte_size;
        }
    }
    if (frame.draws == 0) throw std::runtime_error("Empty Via display list");
    return frame;
}

void PrintFrame(std::string_view step, const Frame& frame) {
    std::cout << step << ' ' << frame.draws << " draws\n";
    for (const auto& label : frame.labels) {
        std::cout << label.x << ',' << label.y << ' ' << label.text << '\n';
    }
}

void Tap(wa_sf_t* scene, const Frame& frame, std::string_view label, int64_t ns) {
    const auto match = std::ranges::find(frame.labels, label, &Label::text);
    if (match == frame.labels.end()) throw std::runtime_error("Missing visible label " + std::string(label));
    for (const auto action : {WA_ACTION_DOWN, WA_ACTION_UP}) {
        wa_input_event_t event{};
        event.kind = WA_INPUT_MOTION;
        event.action = action;
        event.event_time_ms = ns / 1'000'000.0 + (action == WA_ACTION_UP ? 3 : 1);
        event.x = match->x;
        event.y = match->y;
        Check("Push tap", wa_input_push(wa_sf_input(scene), &event));
    }
}

} // namespace

int main(int argc, char** argv) try {
    if (argc != 5) throw std::runtime_error("Expected framework DEX, fallback DEX, resources and Via APK");
    const auto apk = std::filesystem::path(argv[4]);
    if (!std::filesystem::exists(apk)) {
        std::cout << "Via Browser APK is absent\n";
        return 77;
    }
    Check("Create", wa_system_app_create(480, 800, 160, "en-US"));
    struct Destroy { ~Destroy() { wa_system_app_destroy(); } } destroy;
    const auto load = [](const char* path, auto install) {
        const auto bytes = Read(path);
        Check(path, install(bytes.data(), bytes.size()));
    };
    load(argv[1], wa_system_app_load_framework_dex);
    load(argv[2], wa_system_app_load_framework_fallback_dex);
    load(argv[3], wa_system_app_set_framework_res_apk);
    const auto bytes = Read(argv[4]);
    const auto started = std::chrono::steady_clock::now();
    try {
        Check("Boot", wa_system_app_boot_apk(apk.filename().string().c_str(), bytes.data(), bytes.size(), nullptr));
    } catch (const std::runtime_error&) {
        const std::chrono::duration<double, std::milli> elapsed = std::chrono::steady_clock::now() - started;
        std::cerr << "Via boot attempt took " << elapsed.count() << " ms\n" << State() << '\n';
        throw;
    }
    const auto boot = State();
    for (const auto expected : {"\"package\":\"mark.via\"", "\"activity\":\"mark.via.Shell\"",
                                "\"application\":\"mark.via.BrowserApp\""}) {
        if (!boot.contains(expected)) throw std::runtime_error(boot);
    }
    for (const auto field : {"onCreateCalls", "onStartCalls", "onResumeCalls", "executedApplicationMethods"}) {
        if (Metric(boot, field) <= 0) throw std::runtime_error(std::string(field) + " was not called");
    }
    auto* scene = wa_system_app_sf_ctx();
    const auto first = ReadFrame(scene);
    PrintFrame("First frame", first);
    int64_t now = 0;
    for (int64_t frame = 1; frame <= 120; ++frame) {
        now += 16'666'667;
        Check("Tick", wa_system_app_tick(now));
        ReadFrame(scene);
    }
    const auto steady = State();
    if (Metric(steady, "avgFrameMs") >= 3) throw std::runtime_error(steady);
    std::cout << "Via Browser " << steady << '\n';
    Tap(scene, first, "Agree", now);
    for (int i = 0; i < 60; ++i) {
        now += 16'666'667;
        Check("Tick after first-run screen", wa_system_app_tick(now));
    }
    const auto after_agree = ReadFrame(scene);
    PrintFrame("After Agree", after_agree);
    if (std::ranges::find(after_agree.labels, "Welcome", &Label::text) != after_agree.labels.end()) {
        throw std::runtime_error("Via stayed on its first-run screen");
    }
    Check("Back", wa_system_app_send_back());
    return 0;
} catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
}
