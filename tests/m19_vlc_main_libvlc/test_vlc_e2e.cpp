#include "foreground_test.h"
#include "vlc_probe.h"

#include <algorithm>
#include <cstring>
#include <iostream>
#include <set>
#include <sstream>

namespace {

using hybrid_test::Check;
using hybrid_test::Require;
using hybrid_test::State;

constexpr auto kOnboarding = "\"activity\":\"org.videolan.vlc.gui.onboarding.OnboardingActivity\"";
constexpr auto kMain = "\"activity\":\"org.videolan.vlc.gui.MainActivity\"";

size_t Metric(const std::string& state, std::string_view field) {
    const auto key = '"' + std::string(field) + "\":";
    const auto offset = state.find(key);
    Require(offset != state.npos, "Missing " + std::string(field));
    return std::stoull(state.substr(offset + key.size()));
}

std::string Probe(int32_t (*fn)(char*, size_t), std::string_view label) {
    std::array<char, 32768> buffer{};
    Check(fn(buffer.data(), buffer.size()), label);
    return buffer.data();
}

struct Target {
    int x;
    int y;
    std::string label;
};

struct DrawnFrame {
    uint32_t sequence;
    size_t content = 0;
    bool visible_bitmap = false;
    std::vector<std::string> text;
};

DrawnFrame ReadFrame() {
    auto* scene = wa_system_app_sf_ctx();
    const auto* packet = wa_sf_frame_acquire(scene);
    Require(packet != nullptr, "Missing SurfaceFlinger frame");
    struct Release {
        wa_sf_t* scene;
        const wa_sf_frame_packet_t* packet;
        ~Release() { wa_sf_frame_release(scene, packet); }
    } release{scene, packet};
    DrawnFrame frame{packet->sequence};
    const auto* layers = wa_sf_frame_layers(packet);
    for (uint32_t i = 0; i < packet->layer_count; ++i) {
        const auto& layer = layers[i];
        if (!(layer.desc.flags & WA_SF_VISIBLE) || layer.desc.surface_kind != WA_SF_SURFACE_DISPLAY_LIST) continue;
        const auto* list = wa_sf_frame_list(packet, &layer);
        Require(list || !layer.list_size, "Missing display list");
        for (uint32_t offset = 0; offset < layer.list_size;) {
            wa_canvas_record_t record{};
            const auto remaining = layer.list_size - offset;
            Require(remaining >= sizeof(record), "Truncated display list");
            std::memcpy(&record, list + offset, sizeof(record));
            Require(record.byte_size >= sizeof(record) && record.byte_size <= remaining, "Invalid display list");
            const auto* args = list + offset + sizeof(record);
            if (record.opcode >= WA_CANVAS_DRAW_RECT && record.opcode <= WA_CANVAS_DRAW_TEXT) ++frame.content;
            if (record.opcode == WA_CANVAS_DRAW_TEXT) {
                wa_canvas_text_t text{};
                Require(record.byte_size >= sizeof(record) + sizeof(text), "Truncated text");
                std::memcpy(&text, args, sizeof(text));
                Require(text.utf8_offset <= record.byte_size && text.utf8_len <= record.byte_size - text.utf8_offset,
                    "Invalid frame text");
                frame.text.emplace_back(reinterpret_cast<const char*>(list + offset + text.utf8_offset), text.utf8_len);
            }
            if (record.opcode == WA_CANVAS_DRAW_BITMAP && !frame.visible_bitmap) {
                wa_canvas_bitmap_t bitmap{};
                Require(record.byte_size >= sizeof(record) + sizeof(bitmap), "Truncated bitmap");
                std::memcpy(&bitmap, args, sizeof(bitmap));
                auto* store = wa_sf_bitmaps(scene);
                if (const auto* view = wa_bitmap_acquire(store, bitmap.bitmap_id)) {
                    const auto* pixels = wa_bitmap_pixels(view);
                    for (size_t p = 3; pixels && p < view->byte_size; p += 4) {
                        if (!pixels[p]) continue;
                        frame.visible_bitmap = true;
                        break;
                    }
                    wa_bitmap_release(store, view);
                }
            }
            offset += record.byte_size;
        }
    }
    return frame;
}

std::vector<Target> Labels() {
    const auto text = Probe(wa_test_vlc_labels, "Read visible Android view labels");
    std::istringstream lines(text);
    std::vector<Target> labels;
    for (std::string line; std::getline(lines, line);) {
        std::istringstream fields(line);
        Target target{};
        fields >> target.x >> target.y;
        fields.get();
        std::getline(fields, target.label);
        labels.push_back(std::move(target));
    }
    return labels;
}

void Tick(int64_t& now, int frames = 12) {
    for (int i = 0; i < frames; ++i) Check(wa_system_app_tick(now += 16'666'667), "Tick foreground");
}

void Tap(const Target& target, int64_t& now) {
    std::cout << "Tap " << target.label << " at " << target.x << ',' << target.y << std::endl;
    for (const auto action : {WA_ACTION_DOWN, WA_ACTION_UP}) {
        const wa_input_event_t event{.kind = WA_INPUT_MOTION, .action = static_cast<uint32_t>(action),
            .event_time_ms = now / 1e6, .x = static_cast<float>(target.x), .y = static_cast<float>(target.y)};
        Check(wa_input_push(wa_sf_input(wa_system_app_sf_ctx()), &event), "Queue touch");
        Tick(now, 1);
    }
    Tick(now);
}

} // namespace

int main(int argc, char** argv) try {
    Require(argc == 5, "Expected framework DEX, fallback DEX, framework resources and VLC APK");
    if (!std::filesystem::exists(argv[4])) {
        std::cout << "VLC APK is absent\n";
        return 0;
    }
    hybrid_test::Foreground foreground(argv[1], argv[2], argv[3]);
    Check(foreground.Boot(argv[4]), "Boot VLC");
    std::cout << Probe(wa_test_vlc_abi, "Check VLC ABI") << '\n';
    int64_t now = 0;
    Tick(now);
    const auto boot = State();
    Require(boot.contains(kOnboarding) && Metric(boot, "dexFiles") == 6 &&
        Metric(boot, "dexClasses") == 32842 && Metric(boot, "providersCreated") == 5, boot);
    const auto onboarding = ReadFrame();
    Require(onboarding.content > 0 && onboarding.visible_bitmap &&
        std::ranges::find(onboarding.text, "Welcome to VLC!") != onboarding.text.end(),
        "VLC Welcome frame did not draw the title and bitmap");
    const std::set<std::string> pages{"Welcome to VLC!", "Permission", "Permission not granted",
        "Make yourself at home"};
    std::set<std::string> visited;
    for (int step = 0; step < 10; ++step) {
        const auto state = State();
        const auto labels = Labels();
        std::cout << state << '\n';
        for (const auto& target : labels)
            std::cout << "  " << target.x << ',' << target.y << ' ' << target.label << '\n';
        if (state.contains(kMain)) {
            Require(visited == pages, "VLC onboarding pages did not complete");
            if (std::ranges::find(labels, "Notification permission", &Target::label) != labels.end()) {
                const auto okay = std::ranges::find(labels, "OK", &Target::label);
                Require(okay != labels.end(), "VLC notification dialog is missing its OK button");
                Tap(*okay, now);
            }
            const auto main = ReadFrame();
            Require(main.sequence > onboarding.sequence && main.content > 0 && !main.text.empty(),
                "MainActivity did not publish a rendered frame");
            std::cout << "MainActivity draws " << main.content << '\n';
            for (const auto tab : {"Video", "Audio", "Browse", "Playlists", "More"})
                Require(std::ranges::find(main.text, tab) != main.text.end(), std::string("VLC did not draw ") + tab);
            std::cout << Probe(wa_test_vlc_media, "Probe LibVLC and WAV") << '\n';
            const auto native = State();
            Require(native.contains("\"nativeAbi\":\"arm64-v8a\"") && Metric(native, "nativeLibraries") >= 3 &&
                Metric(native, "nativeLoadReturns") >= 3 && Metric(native, "nativeFileFaults") > 0, native);
            for (const auto tab : {"Audio", "Browse"}) {
                const auto visible = Labels();
                const auto target = std::ranges::find(visible, tab, &Target::label);
                Require(target != visible.end(), std::string("Missing VLC tab ") + tab);
                const auto before = ReadFrame();
                Tap(*target, now);
                const auto after = ReadFrame();
                Require(State().contains(kMain) && after.content > 0 && before.text != after.text,
                    std::string("VLC tab did not change the rendered screen ") + tab);
            }
            const auto back = wa_system_app_send_back();
            Check(back, "Dispatch Back from MainActivity");
            Require(State().contains(back ? "\"finishing\":false" : "\"finishing\":true"), State());
            if (back) Tick(now);
            return 0;
        }
        for (const auto& target : labels)
            if (pages.contains(target.label)) visited.insert(target.label);
        const Target* next = nullptr;
        for (const auto name : {"Done", "Next", "Continue"}) {
            for (const auto& target : labels) if (target.label == name) next = &target;
            if (next) break;
        }
        Require(next != nullptr, "No visible onboarding action");
        Tap(*next, now);
    }
    Require(false, "VLC onboarding did not reach MainActivity");
    return 1;
} catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
}
