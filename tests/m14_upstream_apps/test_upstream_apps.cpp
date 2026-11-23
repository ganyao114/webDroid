#include "wasmandroid/foreground_activity.h"

#include <algorithm>
#include <array>
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

struct App {
    std::string_view name, package, activity, icon, initial_text;
    std::string_view application = "android.app.Application";
};

constexpr App kApps[] = {
    {"DeskClock", "com.android.deskclock", "com.android.deskclock.DeskClock",
     "mipmap/ic_launcher_alarmclock", "CLOCK", "com.android.deskclock.DeskClockApplication"},
    {"Browser2", "org.chromium.webview_shell", "org.chromium.webview_shell.WebViewBrowserActivity",
     "drawable/ic_launcher", "about:blank"},
    {"Music", "com.android.music", "com.android.music.MusicBrowserActivity",
     "drawable/app_music", "No songs"},
    {"Gallery2", "com.android.gallery3d", "com.android.gallery3d.app.GalleryActivity",
     "mipmap/ic_launcher_gallery", "Albums", "com.android.gallery3d.app.GalleryAppImpl"},
};

constexpr int64_t kVsync = 16'666'667;
constexpr int kWidth = 480, kHeight = 800, kDpi = 160;
constexpr std::string_view kUrl = "https://example.test/";

void Require(bool condition, std::string_view message) {
    if (!condition) throw std::runtime_error(std::string(message));
}

int Check(int status) {
    if (status >= 0) return status;
    std::array<char, 8192> error{};
    wa_system_app_last_error(error.data(), error.size());
    throw std::runtime_error("Status " + std::to_string(status) + " " + error.data());
}

std::vector<uint8_t> ReadFile(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) throw std::runtime_error("Could not read " + path.string());
    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}

std::string State() {
    std::array<char, 4096> output{};
    const int count = Check(wa_system_app_state_json(output.data(), output.size()));
    return {output.data(), static_cast<size_t>(count)};
}

double Metric(const std::string& json, std::string_view key) {
    const auto field = "\"" + std::string(key) + "\":";
    const auto offset = json.find(field);
    Require(offset != json.npos, field);
    const auto* start = json.c_str() + offset + field.size();
    char* end;
    const auto number = std::strtod(start, &end);
    Require(end != start, field);
    return number;
}

struct Frame {
    uint32_t width, height;
    size_t draws = 0, webviews = 0;
    std::string text;
};

Frame Latest(wa_sf_t* scene) {
    const auto* packet = wa_sf_frame_acquire(scene);
    Require(packet != nullptr, "Activity has no SurfaceFlinger packet");
    struct Release {
        wa_sf_t* scene;
        const wa_sf_frame_packet_t* packet;
        ~Release() { wa_sf_frame_release(scene, packet); }
    } release{scene, packet};
    Frame frame{packet->viewport_w, packet->viewport_h};
    const auto* layers = wa_sf_frame_layers(packet);
    for (uint32_t i = 0; i < packet->layer_count; ++i) {
        const auto& layer = layers[i];
        if (layer.desc.surface_kind == WA_SF_SURFACE_IFRAME_WEBVIEW) ++frame.webviews;
        const auto* list = wa_sf_frame_list(packet, &layer);
        for (uint32_t at = 0; list && at < layer.list_size;) {
            wa_canvas_record_t record;
            Require(layer.list_size - at >= sizeof(record), "Truncated canvas record");
            std::memcpy(&record, list + at, sizeof(record));
            Require(record.byte_size >= sizeof(record) && record.byte_size <= layer.list_size - at,
                    "Malformed canvas record");
            if (record.opcode >= WA_CANVAS_DRAW_COLOR && record.opcode <= WA_CANVAS_DRAW_TEXT)
                ++frame.draws;
            if (record.opcode == WA_CANVAS_DRAW_TEXT) {
                wa_canvas_text_t data;
                Require(record.byte_size >= sizeof(record) + sizeof(data), "Truncated canvas text");
                std::memcpy(&data, list + at + sizeof(record), sizeof(data));
                Require(data.utf8_offset <= record.byte_size &&
                        data.utf8_len <= record.byte_size - data.utf8_offset, "Invalid text range");
                frame.text.append(reinterpret_cast<const char*>(list + at + data.utf8_offset), data.utf8_len);
                frame.text += '\n';
            }
            at += record.byte_size;
        }
    }
    return frame;
}

void Push(wa_sf_t* scene, const wa_input_event_t& event) {
    Require(wa_input_push(wa_sf_input(scene), &event) == WA_SF_OK, "Input event was dropped");
}

void Tap(wa_sf_t* scene, float x, float y, double when_ms) {
    Push(scene, {.kind = WA_INPUT_MOTION, .action = WA_ACTION_DOWN, .pointer_id = 1,
                 .event_time_ms = when_ms, .x = x, .y = y});
    Push(scene, {.kind = WA_INPUT_MOTION, .action = WA_ACTION_UP, .pointer_id = 1,
                 .event_time_ms = when_ms + 1, .x = x, .y = y});
}

bool HasClockTime(std::string_view text) {
    const auto digit = [](char c) { return c >= '0' && c <= '9'; };
    for (size_t i = 1; i + 2 < text.size(); ++i) {
        if (text[i] == ':' && digit(text[i - 1]) && digit(text[i + 1]) && digit(text[i + 2]))
            return true;
    }
    return false;
}

void Verify(const App& app, const std::filesystem::path& path, const std::vector<uint8_t>& framework,
            const std::vector<uint8_t>& fallback, const std::vector<uint8_t>& resources) {
    auto bytes = ReadFile(path);
    Check(wa_system_app_create(kWidth, kHeight, kDpi, "en-US"));
    struct Destroy {
        ~Destroy() { wa_system_app_destroy(); }
    } destroy;
    Check(wa_system_app_load_framework_dex(framework.data(), framework.size()));
    Check(wa_system_app_load_framework_fallback_dex(fallback.data(), fallback.size()));
    Check(wa_system_app_set_framework_res_apk(resources.data(), resources.size()));
    const auto file = path.filename().string();
    Check(wa_system_app_boot_apk(file.c_str(), bytes.data(), bytes.size(), nullptr));
    auto* scene = wa_system_app_sf_ctx();
    const auto boot = State();
    Require(boot.contains("\"booted\":true"), boot);
    Require(boot.contains("\"package\":\"" + std::string(app.package) + "\""), boot);
    Require(boot.contains("\"activity\":\"" + std::string(app.activity) + "\""), boot);
    Require(boot.contains("\"application\":\"" + std::string(app.application) + "\""), boot);
    Require(boot.contains("\"iconLoaded\":true"), boot);
    Require(boot.contains("\"iconName\":\"" + std::string(app.package) + ":" + std::string(app.icon) + "\""), boot);
    for (auto key : {"onCreateCalls", "onStartCalls", "onResumeCalls", "executedActivityMethods", "gcCollections"})
        Require(Metric(boot, key) > 0, boot);
    const auto first = Latest(scene);
    Require(first.draws > 0 && first.text.contains(app.initial_text), first.text);
    if (app.name == "DeskClock") {
        for (auto label : {"ALARM\n", "CLOCK\n", "TIMER\n", "STOPWATCH\n"})
            Require(first.text.contains(label), first.text);
        Require(HasClockTime(first.text), first.text);
    } else if (app.name == "Gallery2") {
        Require(first.text.contains("images/videos available."), first.text);
    }

    const bool browser = app.name == "Browser2";
    Tap(scene, 25.f, browser ? 12.f : 90.f, 1);
    int64_t tick = 0;
    const auto next = [&] {
        tick += kVsync;
        Check(wa_system_app_tick(tick));
    };
    next();
    Require(Metric(State(), "inputEventsProcessed") == 2, "Touch did not reach the Activity input bridge");

    if (browser) {
        Require(State().contains("\"imeFocused\":true"), "Browser URL field did not receive focus");
        wa_input_event_t ime{.kind = WA_INPUT_IME, .action = WA_IME_COMMIT,
                             .text_len = static_cast<uint32_t>(kUrl.size()), .event_time_ms = 20};
        std::memcpy(ime.text, kUrl.data(), kUrl.size());
        Push(scene, ime);
        Push(scene, {.kind = WA_INPUT_KEY, .action = WA_ACTION_DOWN, .keycode = 66, .event_time_ms = 21});
        Push(scene, {.kind = WA_INPUT_KEY, .action = WA_ACTION_UP, .keycode = 66, .event_time_ms = 22});
        next();
        const auto browser_frame = Latest(scene);
        Require(browser_frame.webviews == 1 && browser_frame.text.contains(kUrl),
                "Browser navigation did not publish a WebView surface");
        const auto& request = wa_sf_webview_requests(scene)->entries[0];
        Require(request.action == WA_WEBVIEW_OPEN && std::string_view(request.text, request.text_len) == kUrl,
                "The WebView did not submit its HTTP(S) URL to SurfaceFlinger");
        Require(Check(wa_system_app_send_back()) == 1, "Browser Back should stay within WebView history");
        next();
        const auto back = Latest(scene);
        Require(!back.webviews && back.text.contains("about:blank"), "Browser Back did not restore the initial page");
    } else if (app.name == "DeskClock") {
        const auto tap = [&](float x, float y) {
            Tap(scene, x, y, tick / 1'000'000 + 1);
            next();
        };
        tap(kWidth / 8.f, 25.f);
        const auto alarms = Latest(scene);
        Require(alarms.text.contains("No Alarms"), alarms.text);
        tap(kWidth * 5 / 8.f, 25.f);
        const auto timer = Latest(scene);
        Require(timer.text.contains("00h 00m 00s") && timer.text.contains("\n5\n"), timer.text);
        const double press_time = tick / 1'000'000 + 1;
        Push(scene, {.kind = WA_INPUT_KEY, .action = WA_ACTION_DOWN, .keycode = 12, .event_time_ms = press_time});
        Push(scene, {.kind = WA_INPUT_KEY, .action = WA_ACTION_UP, .keycode = 12, .event_time_ms = press_time + 1});
        next();
        const auto entered = Latest(scene);
        Require(entered.text.contains("00h 00m 05s"), entered.text);
        tap(kWidth / 2.f, kHeight - 40.f);
        for (int frame = 0; frame < 66; ++frame) next();
        const auto countdown = Latest(scene);
        Require(countdown.text.contains("00h 00m 04s"), countdown.text);
        tap(kWidth * 7 / 8.f, 25.f);
        const auto stopped = Latest(scene);
        Require(stopped.text.contains("\n00\n"), stopped.text);
        tap(kWidth / 2.f, kHeight - 40.f);
        for (int frame = 0; frame < 10; ++frame) next();
        const auto running = Latest(scene);
        Require(running.text != stopped.text && !running.text.contains("\n00\n"), running.text);
        tap(kWidth * 3 / 8.f, 25.f);
        const auto clock = Latest(scene);
        Require(HasClockTime(clock.text), clock.text);
    }

    Check(wa_system_app_resize(kWidth + 32, kHeight - 32, kDpi));
    next();
    const auto resized = Latest(scene);
    Require(resized.width == kWidth + 32 && resized.height == kHeight - 32, "Display did not resize");
    for (int frame = 0; frame < 120; ++frame) next();
    const auto measured = State();
    Require(Metric(measured, "frames") >= 120 && Metric(measured, "drawCommands") > 0, measured);
    Require(Check(wa_system_app_send_back()) == 0, "Back should finish the Activity at its root");
    Require(State().contains("\"finishing\":true"), "Finish did not reach the foreground runtime");
    std::cout << app.name << " " << measured << '\n';
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 5) {
        std::cerr << "usage: test_m14_upstream_apps FRAMEWORK FALLBACK RES APK_DIR [APP ...]\n";
        return 2;
    }
    try {
        const auto framework = ReadFile(argv[1]), fallback = ReadFile(argv[2]), resources = ReadFile(argv[3]);
        bool passed = true;
        for (const auto& app : kApps) {
            if (argc > 5 && std::find(argv + 5, argv + argc, app.name) == argv + argc) continue;
            try {
                Verify(app, std::filesystem::path(argv[4]) / (std::string(app.name) + "-upstream.apk"),
                       framework, fallback, resources);
            } catch (const std::exception& error) {
                std::cerr << app.name << " " << error.what() << '\n';
                passed = false;
            }
            Require(wa_system_app_sf_ctx() == nullptr, "Foreground scene leaked across processes");
        }
        return passed ? 0 : 1;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
