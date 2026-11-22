#include "art_interp.h"
#include "ui_bridge.h"

#include <ziparchive/zip_archive.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstring>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {

using wa::interp::Value;
using wa::interp::Vm;
using wa::ui_jni::IntentRequest;
using wa::ui_jni::PackageActivity;

constexpr char kActivity[] = "Lorg/wasmandroid/uismoke/SmokeActivity;";
constexpr uint32_t kLayer = 42, kWidth = 400, kHeight = 320, kDpi = 240;

void Require(bool condition, std::string_view message) {
    if (!condition)
        throw std::runtime_error(std::string(message));
}

std::vector<uint8_t> ExtractDex(const char* apk) {
    ZipArchiveHandle archive = nullptr;
    Require(OpenArchive(apk, &archive) == 0, "Open smoke APK");
    std::unique_ptr<ZipArchive, decltype(&CloseArchive)> owner(archive, CloseArchive);
    ZipEntry64 entry{};
    Require(FindEntry(archive, "classes.dex", &entry) == 0, "APK contains compiled app DEX");
    std::vector<uint8_t> dex(entry.uncompressed_length);
    Require(ExtractToMemory(archive, &entry, dex.data(), dex.size()) == 0,
            "Extract APK classes.dex");
    return dex;
}

class Packet {
  public:
    explicit Packet(wa_sf_t* sf) : sf_(sf), frame_(wa_sf_frame_acquire(sf)) {
        Require(frame_ != nullptr, "SurfaceFlinger published a frame");
    }
    ~Packet() { Release(); }
    Packet(const Packet&) = delete;
    Packet& operator=(const Packet&) = delete;
    const wa_sf_frame_packet_t& operator*() const { return *frame_; }
    const wa_sf_frame_packet_t* operator->() const { return frame_; }
    void Release() {
        if (frame_) {
            wa_sf_frame_release(sf_, frame_);
            frame_ = nullptr;
        }
    }

  private:
    wa_sf_t* sf_;
    const wa_sf_frame_packet_t* frame_;
};

struct DisplayList {
    std::array<uint32_t, WA_CANVAS_DRAW_TEXT + 1> counts{};
    std::array<uint32_t, WA_PATH_CLOSE + 1> verbs{};
    uint32_t bytes = 0;
    uint32_t commands = 0;
    uint32_t sequence = 0;
    uint32_t layers = 0;
    float min_translation_x = 0;
    bool rounded_card = false;
    bool adaptive_mask = false;
    bool stroke_path = false;
    bool even_odd = false;
    bool authored_bitmap = false;
    bool resource_bitmap = false;
    std::vector<std::string> text;

    bool HasText(std::string_view value) const {
        return std::ranges::find(text, value) != text.end();
    }
};

template <class Record> Record Read(const uint8_t* data) {
    Record result;
    std::memcpy(&result, data, sizeof(result));
    return result;
}

DisplayList Inspect(wa_sf_t* sf, const Packet& packet) {
    Require(packet->magic == WA_SF_FRAME_MAGIC && packet->abi_version == WA_SF_ABI_VERSION,
            "Frame ABI is valid");
    Require(packet->viewport_w == kWidth && packet->viewport_h == kHeight, "Frame viewport");
    Require(packet->layers_offset >= sizeof(*packet) && packet->layer_count <= WA_SF_MAX_LAYERS &&
                packet->payload_offset >=
                    packet->layers_offset + packet->layer_count * sizeof(wa_sf_frame_layer_t) &&
                packet->byte_size == packet->payload_offset + packet->payload_size,
            "Frame header and payload bounds");
    const auto* layers = wa_sf_frame_layers(&*packet);
    const wa_sf_frame_layer_t* layer = nullptr;
    for (uint32_t i = 0; i < packet->layer_count; ++i) {
        if (layers[i].desc.layer_id == kLayer)
            layer = &layers[i];
    }
    Require(layer && layer->desc.width == kWidth && layer->desc.height == kHeight &&
                layer->desc.surface_kind == WA_SF_SURFACE_DISPLAY_LIST,
            "Java window uses the bound display-list layer");
    Require(layer->list_size > 0 && layer->list_offset >= packet->payload_offset &&
                layer->list_offset <= packet->byte_size &&
                layer->list_size <= packet->byte_size - layer->list_offset &&
                layer->list_generation > 0,
            "Display-list packet bounds");

    DisplayList stats;
    stats.bytes = layer->list_size;
    stats.sequence = packet->sequence;
    stats.layers = packet->layer_count;
    const uint8_t* bytes = wa_sf_frame_list(&*packet, layer);
    uint32_t save_depth = 0;
    for (size_t at = 0; at < layer->list_size;) {
        Require(layer->list_size - at >= sizeof(wa_canvas_record_t), "Complete record header");
        auto record = Read<wa_canvas_record_t>(bytes + at);
        Require(record.opcode > 0 && record.opcode < stats.counts.size() &&
                    record.byte_size >= sizeof(record) && record.byte_size % 4 == 0 &&
                    record.byte_size <= layer->list_size - at,
                "Record has a known opcode and fits the packet");
        stats.counts[record.opcode]++;
        stats.commands++;
        const uint8_t* body = bytes + at + sizeof(record);
        const size_t body_size = record.byte_size - sizeof(record);

        switch (record.opcode) {
        case WA_CANVAS_SAVE:
            ++save_depth;
            break;
        case WA_CANVAS_RESTORE:
            Require(save_depth != 0, "Canvas restore is balanced");
            --save_depth;
            break;
        case WA_CANVAS_TRANSLATE: {
            Require(body_size == sizeof(wa_canvas_xy_t), "Translate payload");
            auto translate = Read<wa_canvas_xy_t>(body);
            stats.min_translation_x = std::min(stats.min_translation_x, translate.x);
            break;
        }
        case WA_CANVAS_DRAW_ROUND_RECT: {
            Require(body_size == sizeof(wa_canvas_round_draw_t), "Rounded shape payload");
            auto shape = Read<wa_canvas_round_draw_t>(body);
            stats.rounded_card |= shape.paint.argb == 0xff29466b && shape.shape.rx == 21 &&
                                  shape.shape.rect.r > shape.shape.rect.l &&
                                  shape.shape.rect.b > shape.shape.rect.t;
            break;
        }
        case WA_CANVAS_CLIP_ROUND_RECT: {
            Require(body_size == sizeof(wa_round_rect_t), "Adaptive icon clip payload");
            auto shape = Read<wa_round_rect_t>(body);
            stats.adaptive_mask |= shape.rx > 0 && shape.rx * 2 == shape.rect.r - shape.rect.l &&
                                   shape.ry * 2 == shape.rect.b - shape.rect.t;
            break;
        }
        case WA_CANVAS_DRAW_PATH: {
            Require(body_size >= sizeof(wa_canvas_path_t), "Path header payload");
            auto path = Read<wa_canvas_path_t>(body);
            stats.stroke_path |= path.paint.style == WA_PAINT_STROKE && path.paint.stroke_width > 0;
            stats.even_odd |= path.fill_rule == WA_PATH_EVEN_ODD;
            size_t position = sizeof(path);
            constexpr std::array<size_t, 6> coords{0, 2, 2, 4, 6, 0};
            for (uint32_t i = 0; i < path.verb_count; ++i) {
                Require(body_size - position >= sizeof(uint32_t), "Path verb fits");
                uint32_t verb = Read<uint32_t>(body + position);
                position += sizeof(verb);
                Require(verb > 0 && verb < coords.size() && (i != 0 || verb == WA_PATH_MOVE),
                        "Path verb type");
                Require(coords[verb] * sizeof(float) <= body_size - position,
                        "Path coordinates fit");
                stats.verbs[verb]++;
                for (size_t point = 0; point < coords[verb]; ++point) {
                    Require(std::isfinite(Read<float>(body + position)), "Finite path coordinates");
                    position += sizeof(float);
                }
            }
            Require(position == body_size, "Complete serialized path");
            break;
        }
        case WA_CANVAS_DRAW_BITMAP: {
            Require(body_size == sizeof(wa_canvas_bitmap_t), "Bitmap payload");
            auto draw = Read<wa_canvas_bitmap_t>(body);
            auto* store = wa_sf_bitmaps(sf);
            auto* view = wa_bitmap_acquire(store, draw.bitmap_id);
            Require(view != nullptr && view->width == 2,
                    "Published bitmap is available to frame consumers");
            const uint8_t* pixels = wa_bitmap_pixels(view);
            if (view->height == 2) {
                constexpr std::array<uint8_t, 8> expected{0x26, 0x71, 0xc4, 0xff,
                                                          0xe9, 0x49, 0x50, 0xff};
                stats.authored_bitmap = std::ranges::equal(expected, std::span(pixels, 8));
            } else if (view->height == 1) {
                constexpr std::array<uint8_t, 8> expected{0x66, 0xcc, 0x33, 0xff,
                                                          0x33, 0x66, 0x99, 0x80};
                stats.resource_bitmap = std::ranges::equal(expected, std::span(pixels, 8));
            }
            wa_bitmap_release(store, view);
            break;
        }
        case WA_CANVAS_DRAW_TEXT: {
            Require(body_size >= sizeof(wa_canvas_text_t), "Text payload");
            auto text = Read<wa_canvas_text_t>(body);
            Require(text.utf8_offset == sizeof(record) + sizeof(text) &&
                        text.utf8_len <= record.byte_size - text.utf8_offset &&
                        text.font_size > 0 && std::isfinite(text.y) && text.y > 0,
                    "Text bytes and measured baseline fit the record");
            stats.text.emplace_back(reinterpret_cast<const char*>(bytes + at + text.utf8_offset),
                                    text.utf8_len);
            break;
        }
        }
        at += record.byte_size;
    }
    Require(save_depth == 0, "Canvas stack is balanced at commit");
    return stats;
}

int ReadInt(Vm& vm, const char* method) {
    return vm.CallStatic(kActivity, method, "()I").i();
}
bool ReadBool(Vm& vm, const char* method) {
    return vm.CallStatic(kActivity, method, "()Z").i();
}

std::string ReadString(Vm& vm, const char* method) {
    return vm.StringUtf8(vm.CallStatic(kActivity, method, "()Ljava/lang/String;").ref());
}

void Push(wa_sf_t* sf, const wa_input_event_t& event) {
    Require(wa_input_push(wa_sf_input(sf), &event) == WA_SF_OK,
            "Input queued in the compositor ring");
}

void Motion(wa_sf_t* sf, uint32_t action, double time, float x, float y) {
    wa_input_event_t event{};
    event.kind = WA_INPUT_MOTION;
    event.action = action;
    event.pointer_id = 3;
    event.event_time_ms = time;
    event.x = x;
    event.y = y;
    Push(sf, event);
}

void Key(wa_sf_t* sf, uint32_t action, double time, uint32_t code) {
    wa_input_event_t event{};
    event.kind = WA_INPUT_KEY;
    event.action = action;
    event.keycode = code;
    event.event_time_ms = time;
    Push(sf, event);
}

void Ime(wa_sf_t* sf, uint32_t action, uint32_t flags, double time, std::string_view text) {
    wa_input_event_t event{};
    event.kind = WA_INPUT_IME;
    event.action = action;
    event.flags = flags;
    event.event_time_ms = time;
    Require(text.size() <= sizeof(event.text), "IME text fits a ring record");
    event.text_len = text.size();
    std::memcpy(event.text, text.data(), text.size());
    Push(sf, event);
}

void PrintList(std::string_view label, const DisplayList& frame) {
    std::cout << label << " seq=" << frame.sequence << " layers=" << frame.layers
              << " bytes=" << frame.bytes << " commands=" << frame.commands
              << " round_rect=" << frame.counts[WA_CANVAS_DRAW_ROUND_RECT]
              << " path=" << frame.counts[WA_CANVAS_DRAW_PATH]
              << " text=" << frame.counts[WA_CANVAS_DRAW_TEXT]
              << " bitmap=" << frame.counts[WA_CANVAS_DRAW_BITMAP]
              << " round_clip=" << frame.counts[WA_CANVAS_CLIP_ROUND_RECT] << '\n';
}

void Run(Vm& vm, const char* upstream, const char* framework, const char* apk, const char* system_resources) {
    vm.LoadDexFile(upstream);
    vm.LoadFallbackDexFile(framework);
    vm.LoadDexFile(std::string(apk) + "!classes.dex", ExtractDex(apk));
    std::unique_ptr<wa_sf_t, decltype(&wa_sf_destroy)> sf(wa_sf_create(), wa_sf_destroy);
    Require(sf != nullptr, "SurfaceFlinger allocation");
    wa::ui_jni::UiBridge bridge;
    bridge.BindSurfaceFlinger(sf.get(), kLayer);
    bridge.RegisterNatives(vm);

    const auto icon = static_cast<uint32_t>(ReadInt(vm, "iconResource"));
    unsigned queried = 0, applications = 0, launches = 0;
    bool user_forwarded = false;
    PackageActivity notes{"org.wasmandroid.notes", "org.wasmandroid.notes.MainActivity", "Notes",
                          apk, icon};
    bridge.SetPackageCallbacks({
        .query_activities =
            [&](const IntentRequest& request) {
                ++queried;
                user_forwarded |= request.user_id == 10;
                Require(
                    request.action == "android.intent.action.MAIN" &&
                        std::ranges::find(request.categories, "android.intent.category.LAUNCHER") !=
                            request.categories.end(),
                    "Package query forwards Android action and categories");
                return std::vector{notes};
            },
        .get_application = [&](std::string_view name, uint64_t flags,
                               int32_t user) -> std::optional<PackageActivity> {
            ++applications;
            Require(name == notes.package_name && flags == 0 && (user == 0 || user == 10),
                    "Application info lookup");
            return notes;
        },
        .start_activity =
            [&](const IntentRequest& request) {
                ++launches;
                Require(request.package_name == notes.package_name &&
                            request.activity_name == notes.activity_name &&
                            (request.intent_flags & 0x10000000) != 0,
                        "Activity launch reaches the host with explicit component and flags");
                return true;
            },
    });

    uint32_t frame_number = 0;
    auto frame = [&](uint64_t time_ms, uint32_t dpi = kDpi) {
        wa_sf_vsync_state_t tick{time_ms * 1'000'000, ++frame_number, kWidth, kHeight, dpi};
        wa_sf_vsync_write(wa_sf_vsync(sf.get()), &tick);
        return bridge.DoFrame(vm);
    };

    std::cout << "Launching compiled SmokeActivity\n";
    bridge.LaunchActivity(vm, {.apk_path = apk,
                               .locale = "en-US",
                               .density_dpi = kDpi,
                               .width = kWidth,
                               .height = kHeight});
    Require(ReadInt(vm, "lifecycle") == 123, "Launch ran onCreate/onStart/onResume");
    Require(ReadBool(vm, "resourcesVerified"),
            "Compiled resources, styles, included layout, and merge inflated");
    Require(ReadBool(vm, "packagesVerified") && user_forwarded && queried == 2 && applications >= 1,
            "PackageManager and LauncherApps query the host and load icon resources");

    wa_font_metric_t& font =
        wa_sf_font_metrics(sf.get())[WA_FONT_ROBOTO * WA_FONT_WEIGHT_COUNT + 6];
    __atomic_store_n(&font.sequence, 1u, __ATOMIC_RELEASE);
    __atomic_store_n(&font.latin_q16, 49152u, __ATOMIC_RELAXED);
    __atomic_store_n(&font.wide_q16, 65536u, __ATOMIC_RELAXED);
    __atomic_store_n(&font.ascent_q16, 49152u, __ATOMIC_RELAXED);
    __atomic_store_n(&font.descent_q16, 16384u, __ATOMIC_RELAXED);
    __atomic_store_n(&font.sequence, 2u, __ATOMIC_RELEASE);
    Require(vm.CallStatic(kActivity, "headingAdvance", "()F").f() == 40.5f &&
                vm.CallStatic(kActivity, "headingAscent", "()F").f() == -20.25f,
            "Paint measures text with SurfaceFlinger-published font metrics");

    Require(frame(1000) == WA_SF_OK, "Initial screen commits");
    Packet first(sf.get());
    auto initial = Inspect(sf.get(), first);
    PrintList("initial", initial);
    Require(first->density_dpi == kDpi && ReadInt(vm, "tileWidth") > 150 &&
                ReadInt(vm, "tileHeight") == 162,
            "Measured custom view bounds respect XML margins, padding and density");
    Require(ReadInt(vm, "handlerValue") == 7, "Handler dispatched delayed Message");
    Require(static_cast<uint32_t>(ReadInt(vm, "swatchPixel")) == 0xffe94950u,
            "Reading a presented bitmap through the SurfaceFlinger store preserves ARGB");
    Require(initial.rounded_card && initial.adaptive_mask && initial.stroke_path &&
                initial.even_odd && initial.authored_bitmap && initial.resource_bitmap &&
                initial.HasText("Ready to launch") && initial.HasText("Launcher tile") &&
                initial.counts[WA_CANVAS_DRAW_COLOR] == 1 && initial.verbs[WA_PATH_CUBIC] > 0 &&
                initial.verbs[WA_PATH_QUAD] > 0 && initial.verbs[WA_PATH_CLOSE] > 0,
            "First frame contains real path, rounded rectangle, text, adaptive mask and bitmap "
            "commands");

    const float x = ReadInt(vm, "tileX"), y = ReadInt(vm, "tileY") + 25;
    Motion(sf.get(), WA_ACTION_DOWN, 1005, x + 45, y);
    Motion(sf.get(), WA_ACTION_UP, 1010, x + 45, y);
    Require(bridge.DispatchInputEvents(vm, sf.get()) == 2 && ReadInt(vm, "clickCount") == 1 &&
                launches == 1,
            "MotionEvent tap invoked OnClickListener");
    Require(frame(1016) == WA_SF_OK, "Tap publishes the next display list");
    {
        Packet second(sf.get());
        auto clicked = Inspect(sf.get(), second);
        PrintList("tap", clicked);
        Require(clicked.HasText("Tile clicked") && clicked.sequence == initial.sequence + 1,
                "Tap changes TextView on the next frame");
    }
    Require(frame(1032) == WA_SF_BUSY, "A reader holding the previous packet delays atomic commit");
    {
        Packet published(sf.get());
        Require(published->sequence == initial.sequence + 1,
                "An incomplete transaction is never exposed");
    }
    first.Release();
    Require(frame(1032) == WA_SF_OK, "Busy commit resumes without replaying Java traversal");
    Require(ReadInt(vm, "animationUpdates") >= 3,
            "ValueAnimator drove updates on Choreographer ticks");

    Key(sf.get(), WA_ACTION_DOWN, 1033, 22);
    Key(sf.get(), WA_ACTION_UP, 1034, 22);
    Ime(sf.get(), WA_IME_COMPOSING, 0, 1035, "输入");
    Ime(sf.get(), WA_IME_COMMIT, WA_IME_MORE, 1036, "输入");
    Ime(sf.get(), WA_IME_COMMIT, WA_IME_CONTINUATION, 1037, " ✓");
    Require(bridge.DispatchInputEvents(vm, sf.get()) == 5 && ReadInt(vm, "keyCount") == 1 &&
                ReadString(vm, "composed") == "输入" && ReadString(vm, "committed") == "输入 ✓",
            "Key and multipart IME events reach the focused Java View");

    Motion(sf.get(), WA_ACTION_DOWN, 1050, x + 170, y);
    Motion(sf.get(), WA_ACTION_MOVE, 1060, x + 137, y);
    Motion(sf.get(), WA_ACTION_MOVE, 1070, x + 104, y);
    Motion(sf.get(), WA_ACTION_MOVE, 1080, x + 71, y);
    Motion(sf.get(), WA_ACTION_UP, 1090, x + 38, y);
    Require(bridge.DispatchInputEvents(vm, sf.get()) == 5 && ReadInt(vm, "motionEvents") == 7 &&
                ReadBool(vm, "flinging") && ReadInt(vm, "clickCount") == 1,
            "MotionEvent drag releases into an upstream OverScroller fling");
    const int released = ReadInt(vm, "scroll");
    std::vector<int> offsets{released};
    DisplayList scrolled;
    for (uint64_t millis : {1106, 1122, 1138, 1154, 1170, 1186}) {
        Require(frame(millis) == WA_SF_OK, "Fling frame commits");
        offsets.push_back(ReadInt(vm, "scroll"));
        Packet published(sf.get());
        scrolled = Inspect(sf.get(), published);
    }
    Require(std::ranges::adjacent_find(offsets, std::greater_equal{}) == offsets.end() &&
                scrolled.min_translation_x == -offsets.back(),
            "Multiple Choreographer ticks advance the actual display-list scroll");
    PrintList("fling", scrolled);
    std::cout << "fling scroll px";
    for (int px : offsets)
        std::cout << ' ' << px;
    std::cout << '\n';

    const auto before = vm.HeapStats();
    const auto collected = vm.Collect();
    Require(collected.collections == before.collections + 1 && collected.live > 0 &&
                collected.reclaimed > 0,
            "Exact GC reclaimed objects while the Activity remained rooted");
    Require(frame(1202) == WA_SF_OK && ReadInt(vm, "clickCount") == 1,
            "Views survive exact GC and render again");
    {
        Packet after_gc(sf.get());
        Require(Inspect(sf.get(), after_gc).HasText("Tile clicked"),
                "Text and drawable references survive GC");
    }
    std::cout << "exact GC collections=" << collected.collections << " live=" << collected.live
              << " reclaimed=" << collected.reclaimed << '\n';

    bridge.PauseActivity(vm);
    bridge.StopActivity(vm);
    Require(ReadInt(vm, "lifecycle") == 12345, "Activity paused and stopped");
    bridge.ResumeActivity(vm);
    Require(ReadInt(vm, "lifecycle") == 12345723, "Activity restarted and resumed");
    Key(sf.get(), WA_ACTION_DOWN, 1205, 4);
    Key(sf.get(), WA_ACTION_UP, 1206, 4);
    Require(bridge.DispatchInputEvents(vm, sf.get()) == 2 && ReadInt(vm, "lifecycle") == 345723456,
            "Back dispatch finishes the Activity and runs lifecycle cleanup");
    Require(frame(1208) == WA_SF_OK, "Finished Activity clears its display layer");
    {
        Packet removed(sf.get());
        Require(removed->layer_count == 0 && removed->payload_size == 0,
                "Layer removal is published atomically");
    }

    bridge.LaunchActivity(vm, {.apk_path = apk,
                               .class_name = ".SmokeActivity",
                               .locale = "zh-rCN",
                               .density_dpi = 160,
                               .width = kWidth,
                               .height = kHeight,
                               .framework_resource_apk = system_resources});
    Require(ReadBool(vm, "resourcesVerified"), "Compiled resources verified at 160 dpi");
    Require(ReadBool(vm, "platformResources"),
            "Package 0x01 resolves through supplied platform assets");
    Require(frame(1220, 160) == WA_SF_OK, "Localized screen commits");
    {
        Packet chinese(sf.get());
        const auto localized = Inspect(sf.get(), chinese);
        PrintList("zh-CN", localized);
        Require(chinese->density_dpi == 160 && localized.HasText("准备启动") &&
                    localized.HasText("启动磁贴"),
                "AOSP resource selection resolves zh-rCN and records UTF-8 text");
    }
    bridge.DestroyActivity(vm);
    vm.Collect();

    const uint64_t apk_calls = vm.NativeCalls("Landroid/content/res/ApkAssets;");
    const uint64_t xml_calls = vm.NativeCalls("Landroid/content/res/XmlBlock;");
    const uint64_t asset_calls = vm.NativeCalls("Landroid/content/res/AssetManager;");
    Require(apk_calls > 0 && xml_calls > 0 && asset_calls > 0, "Original AOSP resource JNI ran");
    std::cout << "AOSP native calls apk=" << apk_calls << " xml=" << xml_calls
              << " assets=" << asset_calls
              << " canvas=" << vm.NativeCalls("Landroid/graphics/Canvas;") << '\n';
}

} // namespace

int main(int argc, char** argv) {
    if (argc != 5) {
        std::cerr
            << "Usage: test_aosp17_ui_framework UPSTREAM.dex CORE.dex APP.apk PLATFORM_RESOURCES.apk\n";
        return 2;
    }
    Vm vm({.gc_interval = 137});
    try {
        Run(vm, argv[1], argv[2], argv[3], argv[4]);
        return 0;
    } catch (const wa::interp::ManagedException&) {
        std::cerr << vm.ExceptionSummary() << '\n';
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
    }
    return 1;
}
