#include "foreground_test.h"

#include <algorithm>
#include <cstring>
#include <iostream>
#include <map>

namespace {

using namespace hybrid_test;

struct Point { float x, y; };
struct Scene {
    std::map<std::string, Point, std::less<>> labels;
    std::vector<uint8_t> pixels;
};

Scene ReadScene(wa_sf_t* sf) {
    const auto* packet = wa_sf_frame_acquire(sf);
    Require(packet, "Missing SurfaceFlinger frame");
    struct Release {
        wa_sf_t* sf;
        const wa_sf_frame_packet_t* frame;
        ~Release() { wa_sf_frame_release(sf, frame); }
    } release{sf, packet};
    Scene result;
    const auto* layers = wa_sf_frame_layers(packet);
    for (size_t i = 0; i < packet->layer_count; ++i) {
        const auto& layer = layers[i];
        if (layer.desc.surface_kind != WA_SF_SURFACE_DISPLAY_LIST || !(layer.desc.flags & WA_SF_VISIBLE)) continue;
        const auto* commands = wa_sf_frame_list(packet, &layer);
        struct Transform { float x, y, sx = 1, sy = 1; };
        Transform t{layer.desc.x - layer.desc.scroll_x, layer.desc.y - layer.desc.scroll_y};
        std::vector<Transform> stack;
        for (size_t offset = 0; offset < layer.list_size;) {
            wa_canvas_record_t header;
            Require(layer.list_size - offset >= sizeof(header), "Truncated display record");
            std::memcpy(&header, commands + offset, sizeof(header));
            Require(header.byte_size >= sizeof(header) && header.byte_size <= layer.list_size - offset,
                    "Invalid display record");
            const auto* data = commands + offset + sizeof(header);
            offset += header.byte_size;
            switch (header.opcode) {
            case WA_CANVAS_SAVE: stack.push_back(t); break;
            case WA_CANVAS_RESTORE:
                Require(!stack.empty(), "Invalid canvas restore");
                t = stack.back();
                stack.pop_back();
                break;
            case WA_CANVAS_TRANSLATE: case WA_CANVAS_SCALE: {
                wa_canvas_xy_t xy;
                std::memcpy(&xy, data, sizeof(xy));
                if (header.opcode == WA_CANVAS_TRANSLATE) {
                    t.x += t.sx * xy.x;
                    t.y += t.sy * xy.y;
                } else {
                    t.sx *= xy.x;
                    t.sy *= xy.y;
                }
                break;
            }
            case WA_CANVAS_DRAW_TEXT: {
                wa_canvas_text_t text;
                std::memcpy(&text, data, sizeof(text));
                if (text.utf8_len == 0 || (text.argb >> 24) == 0) break;
                Require(text.utf8_offset <= header.byte_size && text.utf8_len <= header.byte_size - text.utf8_offset,
                        "Truncated display text");
                const auto* label = reinterpret_cast<const char*>(commands + offset - header.byte_size + text.utf8_offset);
                const auto metrics = wa_canvas_measure_text(sf, label, text.utf8_len, text.font_size,
                                                             text.weight, text.font_face);
                const float width = text.max_width > 0 ? std::min(metrics.advance, text.max_width) : metrics.advance;
                result.labels.try_emplace(std::string(label, text.utf8_len),
                    Point{t.x + t.sx * (text.x + width * (1 - text.align) / 2),
                          t.y + t.sy * (text.y + (metrics.ascent + metrics.descent) / 2)});
                break;
            }
            case WA_CANVAS_DRAW_BITMAP: {
                wa_canvas_bitmap_t draw;
                std::memcpy(&draw, data, sizeof(draw));
                auto* store = wa_sf_bitmaps(sf);
                const auto* view = wa_bitmap_acquire(store, draw.bitmap_id);
                if (view) {
                    if (view->width == 32 && view->height == 8) {
                        const auto* pixels = wa_bitmap_pixels(view);
                        result.pixels.assign(pixels, pixels + view->byte_size);
                    }
                    wa_bitmap_release(store, view);
                }
                break;
            }
            default: break;
            }
        }
    }
    return result;
}

void Run(char** inputs, const char* path, std::string_view abi) {
    Foreground foreground(inputs[0], inputs[1], inputs[2]);
    Check(foreground.Boot(path), "Boot hybrid APK");
    const auto boot = State();
    Require(boot.contains("\"dexFiles\":2") && boot.contains("\"nativeLibraries\":2") &&
        boot.contains("\"nativeAbi\":\"" + std::string(abi) + "\""), boot);
    auto* sf = wa_system_app_sf_ctx();
    const auto before = ReadScene(sf);
    for (const auto label : {"DT_NEEDED + .init_array OK", "Checksum 4294967297", "Double score 19.25",
                             "Callbacks 1", "CAUGHT_IAE", "Direct buffer OK", "Re-entry OK", "JNI ABI OK"})
        Require(before.labels.contains(label), std::string("Missing initial UI label: ") + label);
    const auto button = before.labels.at("Run Native Compute (+1)");
    for (auto action : {WA_ACTION_DOWN, WA_ACTION_UP}) {
        const wa_input_event_t event{.kind = WA_INPUT_MOTION, .action = action,
            .event_time_ms = action == WA_ACTION_DOWN ? 1.0 : 3.0, .x = button.x, .y = button.y};
        Check(wa_input_push(wa_sf_input(sf), &event), "Tap hybrid compute button");
    }
    for (int i = 1; i <= 6; ++i) Check(wa_system_app_tick(i * 16'666'667LL), "Run hybrid frame");
    const auto after = ReadScene(sf);
    Require(after.labels.contains("Callbacks 2") && after.labels.contains("Checksum 4294967334") &&
        after.labels.contains("Double score 21.25"), "Native click did not update the UI: " + State());
    Require(!before.pixels.empty() && !after.pixels.empty() && after.pixels != before.pixels,
            "Native click did not change the status bitmap pixels");
    Require(LastError().empty(), LastError());
    std::cout << abi << " " << State() << '\n';
}

} // namespace

int main(int argc, char** argv) try {
    Require(argc == 6, "Expected framework DEX, fallback DEX, resources, and two hybrid APKs");
    Run(argv + 1, argv[4], "arm64-v8a");
    Run(argv + 1, argv[5], "armeabi-v7a");
    return 0;
} catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
}
