#include <wasmandroid/launcher_session.h>

#include "art_sf_entry.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <string_view>
#include <vector>

namespace {

void Require(bool condition, std::string_view detail) {
    if (!condition) throw std::runtime_error(std::string(detail));
}

void Check(int status, const char* operation) {
    if (status == WA_OK) return;
    char error[4096];
    wa_system_launcher_last_error(error, sizeof(error));
    throw std::runtime_error(std::string(operation) + " (" + std::to_string(status) + ") " + error);
}

struct File {
    explicit File(const char* path) : name(std::filesystem::path(path).filename().string()) {
        std::ifstream stream(path, std::ios::binary | std::ios::ate);
        Require(static_cast<bool>(stream), path);
        bytes.resize(stream.tellg());
        stream.seekg(0);
        Require(static_cast<bool>(stream.read(reinterpret_cast<char*>(bytes.data()), bytes.size())), path);
    }
    void Install() const {
        Check(wa_system_launcher_install_apk(name.c_str(), bytes.data(), bytes.size()), "install");
    }
    std::string name;
    std::vector<uint8_t> bytes;
};

struct State {
    State() { Check(wa_system_launcher_state_json(text, sizeof(text)), "state"); }

    std::string_view Value(const char* key) const {
        const std::string field = std::string("\"") + key + "\":";
        const std::string_view json(text);
        const auto at = json.find(field);
        Require(at != json.npos, field);
        return json.substr(at + field.size());
    }
    int Integer(const char* key) const { return std::atoi(Value(key).data()); }
    double Number(const char* key) const { return std::atof(Value(key).data()); }
    bool Boolean(const char* key) const { return Value(key).starts_with("true"); }
    bool Is(const char* key, const char* value) const {
        return Value(key).starts_with(std::string("\"") + value + "\"");
    }
    void Print(const char* locale, const char* phase) const {
        std::printf("%s %s %s\n", locale, phase, text);
    }
    char text[4096];
};

struct Position { float x, y; };
struct Transform {
    Position At(float x, float y) const { return {tx + a * x + c * y, ty + b * x + d * y}; }
    void Translate(float x, float y) {
        const auto next = At(x, y);
        tx = next.x;
        ty = next.y;
    }
    void Scale(float x, float y) { a *= x; b *= x; c *= y; d *= y; }
    void Rotate(wa_canvas_rotation_t rotation) {
        const float angle = rotation.degrees * 0.01745329252f;
        const auto center = At(rotation.px, rotation.py);
        const float cosine = std::cos(angle), sine = std::sin(angle);
        const float next_a = a * cosine + c * sine, next_b = b * cosine + d * sine;
        c = c * cosine - a * sine;
        d = d * cosine - b * sine;
        a = next_a;
        b = next_b;
        tx = center.x - a * rotation.px - c * rotation.py;
        ty = center.y - b * rotation.px - d * rotation.py;
    }
    float a = 1, b = 0, c = 0, d = 1, tx = 0, ty = 0;
};

struct Layer {
    bool Has(uint16_t opcode) const { return opcodes[opcode]; }
    Position Find(std::string_view label) const {
        for (auto it = labels.rbegin(); it != labels.rend(); ++it) {
            if (it->first == label) return it->second;
        }
        throw std::runtime_error("Rendered label missing: " + std::string(label));
    }
    std::array<bool, WA_CANVAS_DRAW_TEXT + 1> opcodes{};
    std::vector<std::pair<std::string, Position>> labels;
};

struct Frame {
    explicit Frame(wa_sf_t* scene) : scene(scene), packet(wa_sf_frame_acquire(scene)) {
        Require(packet != nullptr, "Published SurfaceFlinger packet");
    }
    ~Frame() { wa_sf_frame_release(scene, packet); }
    wa_sf_t* scene;
    const wa_sf_frame_packet_t* packet;
};

template<class Record>
Record ReadRecord(const uint8_t* source, size_t available) {
    Require(available >= sizeof(Record), "Truncated display list record");
    Record result;
    std::memcpy(&result, source, sizeof(result));
    return result;
}

std::array<Layer, 3> Inspect(wa_sf_t* scene, uint32_t width = 960, uint32_t height = 660, uint32_t dpi = 160) {
    Frame guard(scene);
    const auto* frame = guard.packet;
    Require(frame->magic == WA_SF_FRAME_MAGIC && frame->layer_count == 3, "Three display layers");
    Require(frame->viewport_w == width && frame->viewport_h == height && frame->density_dpi == dpi, "Viewport");
    const auto* layers = wa_sf_frame_layers(frame);
    std::array<Layer, 3> result;
    for (size_t index = 0; index < result.size(); ++index) {
        const auto& layer = layers[index];
        Require(layer.desc.z_order == static_cast<int32_t>(index) &&
                layer.desc.surface_kind == WA_SF_SURFACE_DISPLAY_LIST, "Display layer stacking");
        auto& details = result[index];
        const uint8_t* list = wa_sf_frame_list(frame, &layer);
        Require(list != nullptr && layer.list_size, "Display list");
        Transform transform;
        std::vector<Transform> stack;
        for (size_t offset = 0; offset < layer.list_size;) {
            const auto* record = list + offset;
            const auto header = ReadRecord<wa_canvas_record_t>(record, layer.list_size - offset);
            Require(header.byte_size >= sizeof(header) && header.byte_size <= layer.list_size - offset &&
                    header.opcode > 0 && header.opcode <= WA_CANVAS_DRAW_TEXT, "Display list record framing");
            const auto* payload = record + sizeof(header);
            const auto length = header.byte_size - sizeof(header);
            details.opcodes[header.opcode] = true;
            switch (header.opcode) {
            case WA_CANVAS_SAVE: stack.push_back(transform); break;
            case WA_CANVAS_RESTORE:
                Require(!stack.empty(), "Canvas restore");
                transform = stack.back();
                stack.pop_back();
                break;
            case WA_CANVAS_TRANSLATE: {
                const auto xy = ReadRecord<wa_canvas_xy_t>(payload, length);
                transform.Translate(xy.x, xy.y);
                break;
            }
            case WA_CANVAS_SCALE: {
                const auto xy = ReadRecord<wa_canvas_xy_t>(payload, length);
                transform.Scale(xy.x, xy.y);
                break;
            }
            case WA_CANVAS_ROTATE:
                transform.Rotate(ReadRecord<wa_canvas_rotation_t>(payload, length));
                break;
            case WA_CANVAS_DRAW_TEXT: {
                const auto text = ReadRecord<wa_canvas_text_t>(payload, length);
                Require(text.utf8_offset <= header.byte_size &&
                        text.utf8_len <= header.byte_size - text.utf8_offset, "Display text bounds");
                const auto point = transform.At(text.x, text.y);
                if (point.x >= 0 && point.x < width && point.y >= 0 && point.y < height) {
                    details.labels.emplace_back(std::string(
                        reinterpret_cast<const char*>(record + text.utf8_offset), text.utf8_len), point);
                }
                break;
            }
            }
            offset += header.byte_size;
        }
        Require(stack.empty(), "Balanced canvas state");
    }
    return result;
}

struct Session {
    explicit Session(const char* locale) {
        Check(wa_system_launcher_create(960, 660, 160, locale), "create");
        scene = wa_system_launcher_sf_ctx();
        Require(scene != nullptr, "Launcher scene");
    }
    ~Session() { wa_system_launcher_destroy(); }
    void Tick(int count = 1) {
        for (int i = 0; i < count; i++) {
            time += 16'666'667;
            Check(wa_system_launcher_tick(time), "tick");
        }
    }
    void Push(int kind, int action, double delta, float x, float y, std::string_view text = {}) {
        Require(text.size() <= WA_INPUT_TEXT_CAPACITY, "Test text buffer");
        wa_input_event_t event{};
        event.kind = kind;
        event.action = action;
        event.event_time_ms = time / 1e6 + delta;
        event.x = x;
        event.y = y;
        event.text_len = text.size();
        if (!text.empty()) std::memcpy(event.text, text.data(), text.size());
        Check(wa_input_push(wa_sf_input(scene), &event), "push input");
    }
    void Tap(float x, float y) {
        Push(WA_INPUT_MOTION, WA_ACTION_DOWN, 1, x, y);
        Push(WA_INPUT_MOTION, WA_ACTION_UP, 3, x, y);
    }
    void Drag(float x, float y, float dx, float dy) {
        Push(WA_INPUT_MOTION, WA_ACTION_DOWN, 1, x, y);
        Push(WA_INPUT_MOTION, WA_ACTION_MOVE, 5, x + dx * .2f, y + dy * .2f);
        Push(WA_INPUT_MOTION, WA_ACTION_MOVE, 10, x + dx * .75f, y + dy * .75f);
        Push(WA_INPUT_MOTION, WA_ACTION_UP, 15, x + dx, y + dy);
    }
    void Swipe(int from, int to) {
        const State before;
        Require(before.Integer("workspacePage") == from, before.text);
        const int start = before.Integer("workspaceScrollX");
        Drag(to > from ? 805 : 154, 290, to > from ? -565 : 565, 0);
        std::array<int, 15> scrolls;
        for (auto& scroll : scrolls) {
            Tick();
            scroll = State().Integer("workspaceScrollX");
        }
        const State after;
        Require(after.Integer("workspacePage") == to, after.text);
        const int end = scrolls.back();
        Require(std::abs(start - end) > 200, "Actual workspace page travel");
        Require(std::ranges::any_of(scrolls, [&](int x) {
            return x > std::min(start, end) && x < std::max(start, end);
        }), "Intermediate animation positions");
    }
    wa_sf_t* scene;
    int64_t time = 0;
};

void Run(const char* locale, const char* hint, const char* composition, const char* query,
         const char* cube_label, const File& dex, const File& resources,
         const File& launcher, const std::vector<File>& apps) {
    Session session(locale);
    Check(wa_system_launcher_load_framework_dex(dex.bytes.data(), dex.bytes.size()), "load framework");
    Check(wa_system_launcher_set_framework_res_apk(resources.bytes.data(), resources.bytes.size()), "mount framework");
    for (const auto& app : apps) app.Install();
    Check(wa_system_launcher_boot_apk(launcher.name.c_str(), launcher.bytes.data(), launcher.bytes.size()), "boot");

    const State boot;
    boot.Print(locale, "boot");
    Require(boot.Boolean("booted") && boot.Is("package", "com.android.launcher3") &&
            boot.Is("activity", "com.android.launcher3.Launcher"), boot.text);
    Require(boot.Integer("installedApps") >= 6 && boot.Integer("pageCount") == 2 &&
            boot.Integer("lastDrawCommands") >= 40, boot.text);
    const auto layers = Inspect(session.scene);
    Require(layers[0].Has(WA_CANVAS_DRAW_LINEAR_GRADIENT) && layers[0].Has(WA_CANVAS_DRAW_PATH), "Wallpaper");
    Require(layers[1].Has(WA_CANVAS_DRAW_ROUND_RECT) && layers[1].Has(WA_CANVAS_DRAW_TEXT) &&
            layers[1].Has(WA_CANVAS_DRAW_PATH), "Launcher view tree");
    Require(layers[2].Has(WA_CANVAS_DRAW_ROUND_RECT) && layers[2].Has(WA_CANVAS_DRAW_TEXT), "System bars");

    session.Tick(60);
    State().Print(locale, "home-60");
    const auto first_page = Inspect(session.scene)[1].labels;
    Require(!first_page.empty(), "First workspace page contains labels");
    session.Swipe(0, 1);
    const auto second_page = Inspect(session.scene)[1].labels;
    Require(std::ranges::any_of(second_page, [&](const auto& label) {
        return std::ranges::none_of(first_page, [&](const auto& first) { return first.first == label.first; });
    }), "Real app label on second workspace page");
    session.Swipe(1, 0);
    session.Drag(520, 530, 0, -240);
    session.Tick(12);
    const State drawer;
    Require(drawer.Boolean("drawerOpen") && drawer.Number("drawerProgress") > 0.9 &&
            drawer.Integer("searchResults") == static_cast<int>(apps.size()), drawer.text);

    const Position search = Inspect(session.scene)[1].Find(hint);
    session.Tap(search.x + 30, search.y - 5);
    session.Push(WA_INPUT_IME, WA_IME_COMPOSING, 8, 0, 0, composition);
    session.Tick();
    const State composing;
    Require(composing.Is("searchQuery", composition), composing.text);
    session.Push(WA_INPUT_IME, WA_IME_COMMIT, 8, 0, 0, query);
    session.Tick();
    const State found;
    Require(found.Is("searchQuery", query) && found.Integer("searchResults") == 1, found.text);
    const Position cube = Inspect(session.scene)[1].Find(cube_label);
    session.Tap(cube.x, cube.y - 5);
    session.Tick();
    char launch[512];
    Require(wa_system_launcher_consume_launch(launch, 2) == WA_ERR_OVERFLOW, "Launch survives small buffer");
    Require(wa_system_launcher_consume_launch(launch, sizeof(launch)) > 0, "Pending launch");
    Require(std::string_view(launch).find("\"pending\":true,\"package\":\"org.wasmandroid.cube\","
            "\"activity\":\"org.wasmandroid.cube.CubeActivity\",\"apkName\":\"wa_cube_app.apk\",\"label\":\""
            + std::string(cube_label) + "\"") != std::string_view::npos, launch);
    Require(wa_system_launcher_consume_launch(launch, sizeof(launch)) == 0, "Launch is consumed once");

    Check(wa_system_launcher_send_home(), "send home");
    const State before_gc;
    Check(wa_system_art_gc_collect(), "explicit GC");
    session.Tick(15);
    const State final;
    final.Print(locale, "interactive-120");
    Require(final.Integer("frames") == 120 && !final.Boolean("drawerOpen") &&
            final.Number("drawerProgress") == 0 && final.Is("searchQuery", "") &&
            final.Integer("workspacePage") == 0 && final.Integer("launchCount") == 1, final.text);
    Require(final.Integer("gcCollections") > before_gc.Integer("gcCollections") &&
            final.Integer("gcReclaimed") > 0 && final.Integer("lastDrawCommands") >= 40 &&
            final.Integer("inputEventsProcessed") >= 18, final.text);
    Check(wa_system_launcher_pause(), "pause");
    session.Tick();
    Require(State().Integer("frames") == 120, "Paused frames are not presented");
    Check(wa_system_launcher_resume(), "resume");
    Check(wa_system_launcher_send_back(), "send back");
    session.Tick();
    Require(State().Integer("frames") == 121, "Resume draws again");
    {
        const Frame held(session.scene);
        session.Tick();
        session.time += 16'666'667;
        Require(wa_system_launcher_tick(session.time) == WA_ERR_BUSY && State().Integer("frames") == 122,
                "Held frame blocks reusing its packet");
    }
    Check(wa_system_launcher_tick(session.time), "retry frame");
    Require(State().Integer("frames") == 123, "Retried frame publishes once");
    Check(wa_system_launcher_resize(720, 540, 160), "resize");
    session.Tick();
    Inspect(session.scene, 720, 540);
}

}  // namespace

int main(int argc, char** argv) {
    try {
        Require(argc == 10, "Expected framework DEX, framework resources, Launcher3 and six apps");
        const File dex(argv[1]), resources(argv[2]), launcher(argv[3]);
        std::vector<File> apps;
        for (int i = 4; i < argc; ++i) apps.emplace_back(argv[i]);
        Run("en-US", "Search apps", "C", "Cube", "WasmAndroid 3D Cube", dex, resources, launcher, apps);
        Run("zh-CN", "搜索应用", "立", "立方体", "WasmAndroid 3D 立方体", dex, resources, launcher, apps);
        Require(wa_system_launcher_sf_ctx() == nullptr, "Destroyed session");
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "%s\n", error.what());
        return 1;
    }
}
