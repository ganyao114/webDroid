#include "wasmandroid/foreground_activity.h"

#include <algorithm>
#include <array>
#include <atomic>
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
    size_t webviews = 0;
    bool modal = false;
    std::vector<Label> labels;

    bool Has(std::string_view label) const {
        return std::ranges::find(labels, label, &Label::text) != labels.end();
    }
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
        if (!(layer.desc.flags & WA_SF_VISIBLE)) continue;
        frame.modal |= (layer.desc.flags & WA_SF_INPUT_MODAL) != 0;
        if (layer.desc.surface_kind == WA_SF_SURFACE_IFRAME_WEBVIEW) ++frame.webviews;
        if (layer.desc.surface_kind != WA_SF_SURFACE_DISPLAY_LIST) continue;
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
    std::cout << step << ' ' << frame.draws << " draws, " << frame.webviews << " WebView layers\n";
    for (const auto& label : frame.labels) {
        std::cout << label.x << ',' << label.y << ' ' << label.text << '\n';
    }
}

class Browser {
  public:
    explicit Browser(wa_sf_t* scene) : scene_(scene) {}

    Frame Advance(const char* step, int frames) {
        for (int i = 0; i < frames; ++i) {
            now_ += 16'666'667;
            Check(step, wa_system_app_tick(now_));
            PumpWebViews();
        }
        return ReadFrame(scene_);
    }

    void Tap(float x, float y) {
        for (const auto action : {WA_ACTION_DOWN, WA_ACTION_UP}) {
            Push({.kind = WA_INPUT_MOTION, .action = action, .x = x, .y = y});
        }
    }

    void Tap(const Frame& frame, std::string_view label) {
        const auto match = std::ranges::find(frame.labels, label, &Label::text);
        if (match == frame.labels.end()) throw std::runtime_error("Missing visible label " + std::string(label));
        Tap(match->x, match->y);
    }

    void Type(std::string_view text) {
        wa_input_event_t event{.kind = WA_INPUT_IME, .action = WA_IME_COMMIT,
                               .text_len = static_cast<uint32_t>(text.size())};
        if (text.size() > sizeof(event.text)) throw std::runtime_error("Input is too long");
        std::ranges::copy(text, event.text);
        Push(event);
    }

    void Enter() {
        for (const auto action : {WA_ACTION_DOWN, WA_ACTION_UP}) {
            Push({.kind = WA_INPUT_KEY, .action = action, .keycode = 66});
        }
    }

    void Back() {
        if (wa_system_app_send_back() != 1) throw std::runtime_error("Back left Via: " + State());
    }

    const std::string& url() const { return url_; }

    std::string Document(const char* url) const {
        const auto size = wa_system_app_read_webview_file(view_, url, nullptr, 0);
        Check("Read WebView file size", size);
        std::string content(size, '\0');
        const auto length = wa_system_app_read_webview_file(view_, url,
            reinterpret_cast<uint8_t*>(content.data()), content.size());
        if (length != size) throw std::runtime_error("WebView file changed while reading");
        return content;
    }

  private:
    void Push(wa_input_event_t event) {
        event.event_time_ms = now_ / 1'000'000.0 + (event.action == WA_ACTION_UP ? 3 : 1);
        Check("Push input", wa_input_push(wa_sf_input(scene_), &event));
    }

    void PumpWebViews() {
        auto& requests = *wa_sf_webview_requests(scene_);
        std::atomic_ref read(requests.header.read_index), write(requests.header.write_index);
        for (auto index = read.load(); index != write.load(); read.store(++index)) {
            const auto& request = requests.entries[index % WA_WEBVIEW_CAPACITY];
            if (request.action != WA_WEBVIEW_OPEN) continue;
            if (request.text_len > sizeof(request.text)) throw std::runtime_error("Invalid WebView URL");
            view_ = request.webview_id;
            url_.assign(request.text, request.text_len);
            std::cout << "WebView opened " << url_ << '\n';
            auto completion = url_;
            if (url_.starts_with("file:///")) {
                const auto html = Document(url_.c_str());
                const auto tag = html.find("<title>");
                if (tag != html.npos) {
                    const auto start = tag + 7;
                    const auto end = html.find("</title>", start);
                    if (end != html.npos && completion.size() + 1 + end - start <= WA_WEBVIEW_TEXT_CAPACITY) {
                        completion.push_back('\0');
                        completion.append(html, start, end - start);
                    }
                }
            }
            auto& events = *wa_sf_webview_events(scene_);
            std::atomic_ref loaded(events.header.write_index);
            const auto event_index = loaded.load();
            if (event_index - std::atomic_ref(events.header.read_index).load() >= WA_WEBVIEW_CAPACITY) {
                throw std::runtime_error("WebView completion queue is full");
            }
            auto& event = events.entries[event_index % WA_WEBVIEW_CAPACITY];
            event = {.status = WA_WEBVIEW_PAGE_LOADED, .webview_id = view_,
                     .text_len = static_cast<uint32_t>(completion.size())};
            std::memcpy(event.text, completion.data(), completion.size());
            loaded.store(event_index + 1);
        }
    }

    wa_sf_t* scene_;
    int64_t now_ = 0;
    uint32_t view_ = 0;
    std::string url_;
};

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
    Browser browser(scene);
    const auto consent = browser.Advance("Settle consent screen", 120);
    if (!consent.Has("Welcome") || !consent.Has("Disagree")) throw std::runtime_error("Via consent is missing");
    browser.Tap(consent, "Agree");
    const auto home = browser.Advance("Tick after first-run screen", 120);
    PrintFrame("Browser home", home);
    const auto home_url = browser.url();
    if (home.Has("Welcome") || !home.Has("Homepage") || home.webviews != 1 ||
        !home_url.starts_with("file:///data/user/0/mark.via/") || !home_url.ends_with(".html")) {
        throw std::runtime_error("Via did not reach Browser Home: " + State());
    }
    const auto document = browser.Document(home_url.c_str());
    if (!document.contains("<input") || !document.contains("<img")) {
        throw std::runtime_error("Via home is missing the real search box or logo");
    }
    const auto tabs = std::ranges::find(home.labels, "1", &Label::text);
    if (tabs == home.labels.end()) throw std::runtime_error("Via home is missing its tab counter");
    const auto open_menu = [&] {
        browser.Tap(432, tabs->y);
        auto menu = browser.Advance("Open Via menu", 36);
        PrintFrame("Browser menu", menu);
        if (!menu.modal || !menu.Has("Bookmarks") || !menu.Has("History") || !menu.Has("Settings")) {
            throw std::runtime_error("Via menu is missing its actions");
        }
        return menu;
    };
    open_menu();
    browser.Back();
    const auto dismissed = browser.Advance("Dismiss Via menu", 36);
    if (dismissed.modal || !dismissed.Has("Homepage") || dismissed.Has("Settings")) {
        throw std::runtime_error("Menu stayed open");
    }

    browser.Tap(open_menu(), "Bookmarks");
    const auto bookmarks = browser.Advance("Open Via bookmarks", 36);
    PrintFrame("Bookmarks", bookmarks);
    if (!bookmarks.Has("Bookmarks") || !bookmarks.Has("Saved pages") || bookmarks.webviews != 0) {
        throw std::runtime_error("Bookmarks did not open");
    }
    browser.Back();
    const auto returned = browser.Advance("Return from bookmarks", 36);
    if (!returned.Has("Homepage") || returned.webviews != 1) throw std::runtime_error("Via did not return home");

    browser.Tap(returned, "Homepage");
    browser.Advance("Focus Via address bar", 16);
    if (!State().contains("\"imeFocused\":true")) throw std::runtime_error("Via address field did not focus");
    constexpr std::string_view test_url = "https://example.com/";
    browser.Type(test_url);
    browser.Enter();
    const auto page = browser.Advance("Load URL from Via address bar", 60);
    PrintFrame("Loaded page", page);
    if (browser.url() != test_url || page.webviews != 1) throw std::runtime_error("Via did not navigate to the typed URL");
    browser.Back();
    const auto final = browser.Advance("Return to Via home", 120);
    PrintFrame("Home after Back", final);
    if (!final.Has("Homepage") || final.webviews != 1 || browser.url() != home_url ||
        !browser.Document(home_url.c_str()).contains("<input")) {
        throw std::runtime_error("Via did not return from the web page");
    }
    std::cout << "Via journey passed " << State() << '\n';
    return 0;
} catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
}
