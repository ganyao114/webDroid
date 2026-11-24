#include "foreground_test.h"

#include <cstring>
#include <iostream>
#include <set>

namespace {

using namespace hybrid_test;

void Run(char** inputs, const char* apk, std::string_view abi) {
    Foreground foreground(inputs[0], inputs[1], inputs[2]);
    Check(foreground.Boot(apk), "Boot NDK C++ and pthread APK");
    const auto state = State();
    Require(state.contains("\"nativeAbi\":\"" + std::string(abi) + "\"") &&
                state.contains("\"nativeLibraries\":2"),
            state);
    auto* sf = wa_system_app_sf_ctx();
    const auto* packet = wa_sf_frame_acquire(sf);
    Require(packet, "Missing SurfaceFlinger frame");
    struct Release {
        wa_sf_t* sf;
        const wa_sf_frame_packet_t* packet;
        ~Release() { wa_sf_frame_release(sf, packet); }
    } release{sf, packet};
    std::set<std::string> labels;
    const auto* layers = wa_sf_frame_layers(packet);
    for (size_t i = 0; i < packet->layer_count; ++i) {
        const auto& layer = layers[i];
        if (layer.desc.surface_kind != WA_SF_SURFACE_DISPLAY_LIST ||
            !(layer.desc.flags & WA_SF_VISIBLE))
            continue;
        const auto* list = wa_sf_frame_list(packet, &layer);
        for (size_t offset = 0; offset < layer.list_size;) {
            wa_canvas_record_t record;
            Require(layer.list_size - offset >= sizeof(record), "Incomplete display record");
            std::memcpy(&record, list + offset, sizeof(record));
            Require(record.byte_size >= sizeof(record) &&
                        record.byte_size <= layer.list_size - offset,
                    "Invalid display record size");
            if (record.opcode == WA_CANVAS_DRAW_TEXT) {
                wa_canvas_text_t text;
                Require(record.byte_size >= sizeof(record) + sizeof(text),
                        "Incomplete text record");
                std::memcpy(&text, list + offset + sizeof(record), sizeof(text));
                Require(text.utf8_offset <= record.byte_size &&
                            text.utf8_len <= record.byte_size - text.utf8_offset,
                        "Invalid text record");
                if (text.utf8_len && (text.argb >> 24))
                    labels.emplace(reinterpret_cast<const char*>(list + offset + text.utf8_offset),
                                   text.utf8_len);
            }
            offset += record.byte_size;
        }
    }
    for (const auto* badge : {"NDK C++ containers OK", "cpp-exc-ok", "pthread TLS and handoff OK",
                              "Guest stdio and ELF unwind OK"})
        Require(labels.contains(badge), std::string("Missing UI badge ") + badge);
    std::cout << abi << " rendered C++ exception, pthread and guest libc badges\n";
}

} // namespace

int main(int argc, char** argv) try {
    Require(argc == 6, "Expected framework DEX, fallback DEX, resources and both NDK APKs");
    Run(argv + 1, argv[4], "arm64-v8a");
    Run(argv + 1, argv[5], "armeabi-v7a");
    return 0;
} catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
}
