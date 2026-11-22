#include "wasmandroid/surfaceflinger.h"
#include <emscripten/emscripten.h>

#include <math.h>
#include <stdio.h>
#include <string.h>

enum { WALLPAPER = 1, STATUS, WORKSPACE, HOTSEAT, ALL_APPS, WEBVIEW };
enum { WEBVIEW_ID = 20, DECODER_ID = 21 };

typedef struct demo_state {
    uint32_t frame_count, failed_frames, input_events, codec_status, webview_roundtrips, page;
    float scroll, scroll_target, drawer, drawer_target, width, height, workspace_width;
    float drawer_y, search_x, search_y, search_w;
    uint32_t committed_len, composing_len, webview_errors, ime_updates;
    char committed[512], composing[512];
} demo_state_t;

static wa_sf_t *scene;
static demo_state_t demo;
static uint64_t last_time;
static uint32_t last_vsync;
static float scroll_v, drawer_v, start_x, start_y, start_scroll, start_drawer;
static uint32_t pointer_id;
static int pointer_down, axis, text_dirty;
static char incoming_ime[512];
static uint32_t incoming_len;

static const uint32_t colors[] = {
    0xff5b83fc, 0xff48d4c2, 0xffc9a1ff, 0xffffb48a, 0xffffce66, 0xff72cffa, 0xfffe8aa5,
    0xffaadd8b, 0xff79e1ef, 0xffb3baff, 0xfff3a3da, 0xff9abaff, 0xffefda9b, 0xff75d59c,
    0xfffca997, 0xff669df8, 0xffd3abfc, 0xff7bdbdd, 0xfffa90af, 0xffa2d4ff,
};
static const char *const labels[][20] = {
    {"3D 立方体", "浏览器", "相册", "设置", "Camera", "Maps", "Calendar", "Messages",
     "Photos", "Music", "Clock", "Files", "Notes", "Weather", "Health", "Store",
     "Contacts", "Drive", "Recorder", "Books"},
    {"Chrome", "桌面搜索", "Gallery", "系统设置", "Wallet", "Translate", "Mail", "Chat",
     "图库", "Podcasts", "Timer", "Downloads", "Tasks", "Forecast", "Fitness", "Play",
     "People", "Cloud", "Voice", "阅读"},
};

#define M(x, y) {WA_PATH_MOVE, {x, y}}
#define L(x, y) {WA_PATH_LINE, {x, y}}
#define Q(cx, cy, x, y) {WA_PATH_QUAD, {cx, cy, x, y}}
#define C(x1, y1, x2, y2, x, y) {WA_PATH_CUBIC, {x1, y1, x2, y2, x, y}}
#define Z {WA_PATH_CLOSE, {0}}
#define COUNT(a) (sizeof(a) / sizeof(*(a)))

static const wa_path_verb_t squircle[] = {M(12, 0), C(21.7f, 0, 24, 2.3f, 24, 12),
    C(24, 21.7f, 21.7f, 24, 12, 24), C(2.3f, 24, 0, 21.7f, 0, 12),
    C(0, 2.3f, 2.3f, 0, 12, 0), Z};
static const wa_path_verb_t cube[] = {M(12, 2), L(22, 7), L(22, 17), L(12, 22), L(2, 17),
    L(2, 7), Z, M(2, 7), L(12, 12), L(22, 7), M(12, 12), L(12, 22)};
static const wa_path_verb_t globe[] = {M(22, 12), C(22, 25, 2, 25, 2, 12),
    C(2, -1, 22, -1, 22, 12), Z, M(2, 12), L(22, 12), M(12, 2), Q(4, 12, 12, 22),
    Q(20, 12, 12, 2)};
static const wa_path_verb_t photo[] = {M(2, 5), Q(2, 3, 4, 3), L(20, 3), Q(22, 3, 22, 5),
    L(22, 19), Q(22, 21, 20, 21), L(4, 21), Q(2, 21, 2, 19), Z,
    M(4, 17), L(9, 11), L(12, 15), L(15, 9), L(21, 16)};
static const wa_path_verb_t settings[] = {M(10, 2), L(14, 2), L(15, 5), L(18, 5), L(21, 8),
    L(20, 11), L(22, 13), L(21, 17), L(18, 17), L(16, 20), L(13, 22), L(9, 20), L(8, 18),
    L(4, 18), L(2, 14), L(4, 11), L(3, 8), L(6, 5), L(9, 5), Z,
    M(16, 12), C(16, 17, 8, 17, 8, 12), C(8, 7, 16, 7, 16, 12), Z};
static const wa_path_verb_t camera[] = {M(3, 7), L(7, 7), L(9, 4), L(15, 4), L(17, 7),
    L(21, 7), Q(23, 7, 23, 9), L(23, 18), Q(23, 20, 21, 20), L(3, 20), Q(1, 20, 1, 18),
    L(1, 9), Q(1, 7, 3, 7), Z, M(16, 13), C(16, 18, 8, 18, 8, 13), C(8, 8, 16, 8, 16, 13)};
static const wa_path_verb_t location[] = {M(12, 22), C(9, 17, 4, 13, 4, 9),
    C(4, -1, 20, -1, 20, 9), C(20, 13, 15, 17, 12, 22), Z,
    M(15, 9), C(15, 13, 9, 13, 9, 9), C(9, 5, 15, 5, 15, 9)};
static const wa_path_verb_t calendar[] = {M(3, 5), L(21, 5), L(21, 21), L(3, 21), Z,
    M(3, 10), L(21, 10), M(7, 2), L(7, 7), M(17, 2), L(17, 7), M(8, 15), L(16, 15)};
static const wa_path_verb_t chat[] = {M(3, 4), L(21, 4), L(21, 16), L(10, 16),
    L(4, 21), L(4, 16), L(3, 16), Z, M(7, 9), L(17, 9), M(7, 12), L(13, 12)};
static const wa_path_verb_t music[] = {M(8, 18), C(8, 13, 1, 14, 1, 18), C(1, 22, 8, 22, 8, 18),
    L(8, 6), L(21, 3), L(21, 15), C(21, 19, 14, 20, 14, 16), C(14, 12, 21, 11, 21, 15)};
static const wa_path_verb_t clock_face[] = {M(22, 12), C(22, 25, 2, 25, 2, 12),
    C(2, -1, 22, -1, 22, 12), Z, M(12, 6), L(12, 12), L(16, 15)};
static const wa_path_verb_t folder[] = {M(2, 7), Q(2, 4, 5, 4), L(10, 4), L(13, 7), L(20, 7),
    Q(22, 7, 22, 9), L(22, 18), Q(22, 20, 20, 20), L(4, 20), Q(2, 20, 2, 18), Z};
static const wa_path_verb_t book[] = {M(3, 4), Q(8, 2, 12, 6), Q(16, 2, 21, 4),
    L(21, 20), Q(16, 18, 12, 22), Q(8, 18, 3, 20), Z, M(12, 6), L(12, 22)};
static const struct { const wa_path_verb_t *verbs; uint32_t count; } icons[] = {
    {cube, COUNT(cube)}, {globe, COUNT(globe)}, {photo, COUNT(photo)}, {settings, COUNT(settings)},
    {camera, COUNT(camera)}, {location, COUNT(location)}, {calendar, COUNT(calendar)}, {chat, COUNT(chat)},
    {music, COUNT(music)}, {clock_face, COUNT(clock_face)}, {folder, COUNT(folder)}, {book, COUNT(book)},
};

static float clamp(float v, float min, float max) { return fminf(max, fmaxf(min, v)); }
static wa_rect_t rect(float x, float y, float w, float h) { return (wa_rect_t){x, y, x + w, y + h}; }
static wa_round_rect_t rounded(float x, float y, float w, float h, float r) {
    return (wa_round_rect_t){rect(x, y, w, h), r, r};
}
static wa_paint_t color(uint32_t argb) { return (wa_paint_t){argb, WA_PAINT_FILL, 0}; }
static wa_paint_t stroke(uint32_t argb, float w) { return (wa_paint_t){argb, WA_PAINT_STROKE, w}; }

static void draw_label(wa_canvas_t *c, const char *value, float x, float y, float width,
                       float px, uint32_t weight, uint32_t align, uint32_t argb) {
    wa_canvas_text_t text = {0, (uint32_t)strlen(value), x, y, fmaxf(0, width), px,
                             weight, align, argb, WA_FONT_ROBOTO};
    wa_canvas_draw_text(c, &text, value);
}

static void gradient(wa_canvas_t *c, wa_round_rect_t shape, uint32_t a, uint32_t b, float angle) {
    wa_canvas_gradient_t data = {shape, a, b, angle};
    wa_canvas_draw_linear_gradient(c, &data);
}

static void shadow(wa_canvas_t *c, wa_rect_t box, float radius, float depth, uint32_t argb) {
    wa_canvas_shadow_t data = {box, radius, depth, argb};
    wa_canvas_draw_shadow(c, &data);
}

static void upload_icons(void) {
    uint8_t rgba[32 * 32 * 4];
    for (uint32_t i = 0; i < 4; ++i) {
        for (uint32_t y = 0; y < 32; ++y) {
            for (uint32_t x = 0; x < 32; ++x) {
                uint32_t p = (y * 32 + x) * 4;
                uint32_t line = ((x + i * y) / 5 + y / 5) % 3;
                rgba[p] = (uint8_t)(82 + i * 27 + line * 21);
                rgba[p + 1] = (uint8_t)(110 + x * 3 + line * 13);
                rgba[p + 2] = (uint8_t)(224 - i * 24 + y / 2);
                rgba[p + 3] = 255;
            }
        }
        wa_bitmap_put(wa_sf_bitmaps(scene), i + 1, 32, 32, 32 * 4, rgba);
    }
}

static void icon(wa_canvas_t *c, uint32_t i, uint32_t page, float x, float y, float size, float label_w) {
    uint32_t tone = colors[(i + page * 3) % COUNT(colors)];
    const int bitmap = i % 5 == 4;
    if (bitmap) {
        wa_canvas_bitmap_t b = {i / 5 + 1, WA_MASK_SQUIRCLE, rect(0, 0, 32, 32),
                                 rect(x, y, size, size), 0, 1};
        wa_canvas_draw_bitmap(c, &b);
    }
    uint32_t shape = (i + page * 2) % COUNT(icons);
    wa_canvas_save(c);
    wa_canvas_translate(c, x, y);
    wa_canvas_scale(c, size / 24, size / 24);
    if (!bitmap)
        wa_canvas_draw_path(c, squircle, COUNT(squircle), WA_PATH_NONZERO, color(tone));
    wa_canvas_translate(c, 6, 6);
    wa_canvas_scale(c, 0.5f, 0.5f);
    wa_canvas_draw_path(c, icons[shape].verbs, icons[shape].count, WA_PATH_NONZERO,
                        stroke(0xff15264a, 1.8f));
    wa_canvas_restore(c);
    if (label_w > 0)
        draw_label(c, labels[page][i], x + size / 2, y + size + 17, label_w, 12.5f,
                   500, WA_TEXT_CENTER, 0xfff4f6ff);
}

static void wallpaper(float seconds) {
    wa_canvas_t *c = wa_canvas_begin(scene, WALLPAPER);
    float w = demo.width, h = demo.height;
    gradient(c, rounded(0, 0, w, h, 0), 0xff1f3473, 0xff491f63, 34 + 7 * sinf(seconds * 0.55f));
    gradient(c, rounded(0, 0, w, h, 0), 0x0b55cbd6, 0xc61a1b39, 112);
    const float drift = sinf(seconds * 0.32f) * w * 0.04f;
    wa_canvas_draw_circle(c, w * 0.88f + drift, h * 0.23f, w * 0.38f, color(0x20ba8bf6));
    wa_canvas_draw_circle(c, w * 0.95f + drift, h * 0.10f, w * 0.28f, color(0x1854d9d9));
    wa_canvas_draw_circle(c, w * 0.05f, h * 1.02f, w * 0.29f, color(0x1443d7e1));
    wa_canvas_end(c);
}

static void status_bar(void) {
    wa_canvas_t *c = wa_canvas_begin(scene, STATUS);
    const float w = demo.width;
    if ((w >= 770 || demo.drawer < 0.05f) && demo.height >= 360) {
        const float card = w >= 770 ? 223 : 170;
        shadow(c, rect(w - card - 13, 64, card, 166), 20, 16, 0x77131036);
    }
    draw_label(c, "9:41", 29, 33, 60, 17, 650, WA_TEXT_LEFT, 0xfff4f6ff);
    draw_label(c, "5G", w - 103, 32, 40, 13, 750, WA_TEXT_LEFT, 0xfff4f6ff);
    const wa_path_verb_t wifi[] = {M(w - 80, 24), Q(w - 72, 17, w - 64, 24),
        M(w - 77, 27), Q(w - 72, 23, w - 67, 27), M(w - 74, 30), Q(w - 72, 28, w - 70, 30)};
    wa_canvas_draw_path(c, wifi, COUNT(wifi), WA_PATH_NONZERO, stroke(0xfff4f6ff, 1.5f));
    wa_canvas_draw_round_rect(c, rounded(w - 54, 21, 23, 12, 3), stroke(0xfff4f6ff, 1.5f));
    wa_canvas_draw_round_rect(c, rounded(w - 51, 24, 16, 6, 1), color(0xfff4f6ff));
    wa_canvas_draw_line(c, w - 28, 25, w - 28, 29, stroke(0xfff4f6ff, 1.5f));
    draw_label(c, "Saturday, November 21", 30, 72, demo.workspace_width - 50, 14, 500, WA_TEXT_LEFT, 0xffbec9f4);
    draw_label(c, "Home", 28, 111, demo.workspace_width - 50, 34, 650, WA_TEXT_LEFT, 0xfff5f6ff);
    wa_canvas_end(c);
}

static void workspace(void) {
    wa_canvas_t *c = wa_canvas_begin(scene, WORKSPACE);
    const float step_x = demo.workspace_width / 4;
    const float step_y = fmaxf(33, (demo.height - 233) / 5);
    const float size = clamp(fminf(step_x - 18, step_y - 22), 16, 57);
    for (uint32_t page = 0; page < 2; ++page) {
        wa_canvas_save(c);
        wa_canvas_translate(c, page * demo.workspace_width, 0);
        for (uint32_t i = 0; i < 20; ++i)
            icon(c, i, page, (i % 4 + 0.5f) * step_x - size / 2, (i / 4) * step_y, size, step_x - 10);
        wa_canvas_restore(c);
    }
    wa_canvas_end(c);
}

static void hotseat(void) {
    wa_canvas_t *c = wa_canvas_begin(scene, HOTSEAT);
    float w = fmaxf(1, demo.workspace_width - 26);
    gradient(c, rounded(0, 0, w, 76, 25), 0xd44d5988, 0xd535355c, 47);
    wa_canvas_draw_round_rect(c, rounded(0.8f, 0.8f, w - 1.6f, 74.4f, 25),
                              stroke(0x57dae0ff, 1.2f));
    float available = fmaxf(80, w - 115);
    const float size = fminf(46, available / 4 - 5);
    for (uint32_t i = 0; i < 4; ++i)
        icon(c, i, 0, (i + 0.5f) * available / 4 - size / 2, (76 - size) / 2, size, 0);
    wa_canvas_draw_round_rect(c, rounded(w - 107, 19, 94, 38, 16), color(0x3fadbaff));
    draw_label(c, "All apps", w - 60, 43, 78, 13, 650, WA_TEXT_CENTER, 0xfff4f6ff);
    wa_canvas_end(c);
}

static void all_apps(void) {
    wa_canvas_t *c = wa_canvas_begin(scene, ALL_APPS);
    float w = fmaxf(1, demo.workspace_width - 26), h = fmaxf(240, demo.height * 0.78f);
    gradient(c, rounded(0, 0, w, h, 29), 0xff313960, 0xff20233f, 63);
    wa_canvas_draw_round_rect(c, rounded(w / 2 - 22, 9, 44, 4, 2), color(0xff8b97c0));
    draw_label(c, "All apps", 23, 50, w - 44, 25, 650, WA_TEXT_LEFT, 0xfff6f6ff);
    wa_canvas_draw_round_rect(c, rounded(19, 66, w - 38, 46, 18), color(0xff454e7b));
    wa_canvas_draw_circle(c, 39, 87, 6.2f, stroke(0xffd7dcff, 1.6f));
    wa_canvas_draw_line(c, 43, 92, 48, 97, stroke(0xffd7dcff, 1.7f));
    char search[sizeof(demo.committed) + sizeof(demo.composing)];
    int has_query = demo.committed_len || demo.composing_len;
    snprintf(search, sizeof(search), "%s%s", demo.committed, demo.composing);
    draw_label(c, has_query ? search : "Search your apps", 59, 95, w - 94, 17, 500,
               WA_TEXT_LEFT, has_query ? 0xfff7f8ff : 0xffc4cdef);
    if (demo.composing_len) {
        wa_text_metrics_t prefix = wa_canvas_measure_text(scene, demo.committed, demo.committed_len,
                                                          17, 500, WA_FONT_ROBOTO);
        wa_text_metrics_t suffix = wa_canvas_measure_text(scene, demo.composing, demo.composing_len,
                                                          17, 500, WA_FONT_ROBOTO);
        float from = fminf(w - 27, 59 + prefix.advance);
        wa_canvas_draw_line(c, from, 101, fminf(w - 27, from + suffix.advance), 101,
                            stroke(0xffb6b5ff, 2));
    }
    const float step = w / 4, size = clamp(step - 14, 4, 50);
    for (uint32_t i = 0; i < 12; ++i)
        icon(c, i, 0, (i % 4 + 0.5f) * step - size / 2, 138 + i / 4 * 79, size, step - 8);
    wa_canvas_end(c);
}

static void delete_codepoint_tail(void) {
    if (!demo.committed_len)
        return;
    do {
        --demo.committed_len;
    } while (demo.committed_len && ((unsigned char)demo.committed[demo.committed_len] & 0xc0) == 0x80);
    demo.committed[demo.committed_len] = 0;
}

static void ime_event(const wa_input_event_t *e) {
    if (!(e->flags & WA_IME_CONTINUATION))
        incoming_len = 0;
    if (incoming_len + e->text_len < sizeof(incoming_ime)) {
        memcpy(incoming_ime + incoming_len, e->text, e->text_len);
        incoming_len += e->text_len;
    }
    if (e->flags & WA_IME_MORE)
        return;
    if (e->action == WA_IME_COMPOSING) {
        memcpy(demo.composing, incoming_ime, incoming_len);
        demo.composing_len = incoming_len;
        demo.composing[incoming_len] = 0;
        ++demo.ime_updates;
    } else {
        if (demo.committed_len + incoming_len < sizeof(demo.committed)) {
            memcpy(demo.committed + demo.committed_len, incoming_ime, incoming_len);
            demo.committed_len += incoming_len;
            demo.committed[demo.committed_len] = 0;
        }
        demo.composing_len = 0;
        demo.composing[0] = 0;
    }
    text_dirty = 1;
}

static void motion(const wa_input_event_t *e) {
    const float w = demo.workspace_width;
    switch (e->action) {
    case WA_ACTION_DOWN:
        if (pointer_down)
            return;
        pointer_down = 1;
        pointer_id = e->pointer_id;
        start_x = e->x;
        start_y = e->y;
        start_drawer = demo.drawer;
        start_scroll = demo.scroll;
        scroll_v = drawer_v = 0;
        axis = 0;
        break;
    case WA_ACTION_MOVE: {
        if (!pointer_down || e->pointer_id != pointer_id)
            return;
        float dx = e->x - start_x, dy = e->y - start_y;
        if (!axis && (fabsf(dx) > 8 || fabsf(dy) > 8))
            axis = (fabsf(dx) > fabsf(dy) && start_drawer < 0.1f) ? 1 : 2;
        if (axis == 1)
            demo.scroll = clamp(start_scroll - dx, 0, w);
        if (axis == 2)
            demo.drawer = clamp(start_drawer - dy / (demo.height * 0.52f), 0, 1);
        break;
    }
    case WA_ACTION_UP:
    case WA_ACTION_CANCEL: {
        if (!pointer_down || e->pointer_id != pointer_id)
            return;
        int released = e->action == WA_ACTION_UP;
        if (axis == 1 && released) {
            float projected = demo.scroll - clamp(e->velocity_x, -2000, 2000) * 0.16f;
            demo.page = projected > w / 2;
            demo.scroll_target = demo.page * w;
            scroll_v = -clamp(e->velocity_x, -2400, 2400);
        }
        if (axis == 2 && released) {
            demo.drawer_target = (demo.drawer - e->velocity_y / fmaxf(1, demo.height) * 0.15f > 0.42f);
            drawer_v = -clamp(e->velocity_y / fmaxf(1, demo.height), -5, 5);
        }
        if (!axis && released && e->y > demo.height - 76 && e->x > w - 125 && e->x < w)
            demo.drawer_target = 1;
        pointer_down = axis = 0;
        break;
    }
    case WA_ACTION_SCROLL:
        if (fabsf(e->dx) > fabsf(e->dy)) {
            demo.page = e->dx > 0;
            demo.scroll_target = demo.page * w;
        } else if (e->dy) {
            demo.drawer_target = e->dy > 0;
        }
        break;
    }
}

static void platform_events(void) {
    wa_input_event_t input;
    while (wa_input_poll(wa_sf_input(scene), &input) > 0) {
        ++demo.input_events;
        if (input.kind == WA_INPUT_MOTION) motion(&input);
        if (input.kind == WA_INPUT_IME) ime_event(&input);
        if (input.kind == WA_INPUT_KEY && input.action == WA_ACTION_DOWN) {
            if (input.keycode == 4) demo.drawer_target = 0;
            if (input.keycode == 67) {
                delete_codepoint_tail();
                text_dirty = 1;
            }
        }
    }
    wa_codec_event_t codec;
    while (wa_codec_poll(scene, &codec) > 0) {
        if (codec.decoder_id == DECODER_ID)
            demo.codec_status = codec.status;
    }
    wa_webview_event_t event;
    while (wa_webview_poll(scene, &event) > 0) {
        if (event.webview_id != WEBVIEW_ID)
            continue;
        if (event.status == WA_WEBVIEW_INTERFACE) {
            static const char response[] = "\"wasm:桌面搜索\"";
            static const char ping[] = "{\"method\":\"ping\",\"args\":[\"桌面搜索\"]}";
            if (event.text_len == sizeof(ping) - 1 && !memcmp(event.text, ping, sizeof(ping) - 1))
                wa_webview_reply(scene, WEBVIEW_ID, event.request_id, response, sizeof(response) - 1);
        } else if (event.status == WA_WEBVIEW_READY) {
            static const char script[] = "globalThis.roundTrip()";
            wa_webview_eval(scene, WEBVIEW_ID, 1, script, sizeof(script) - 1);
        } else if (event.status == WA_WEBVIEW_EVAL_RESULT && event.request_id == 1) {
            static const char reply[] = "\"wasm:桌面搜索\"";
            if (event.text_len == sizeof(reply) - 1 && !memcmp(event.text, reply, sizeof(reply) - 1))
                ++demo.webview_roundtrips;
        } else if (event.status == WA_WEBVIEW_ERROR) {
            ++demo.webview_errors;
        }
    }
    static uint32_t reported_codec;
    if (!demo.frame_count)
        reported_codec = 0;
    if (demo.webview_roundtrips && demo.codec_status && demo.codec_status != reported_codec) {
        char script[80];
        int len = snprintf(script, sizeof(script), "globalThis.showCodec(%u)", demo.codec_status);
        if (!wa_webview_eval(scene, WEBVIEW_ID, 2, script, (uint32_t)len))
            reported_codec = demo.codec_status;
    }
}

static float spring(float x, float target, float *velocity, float dt, float snap) {
    const float omega = 18;
    const float from = x - target, trend = *velocity + omega * from;
    float e = expf(-omega * dt);
    x = target + (from + trend * dt) * e;
    *velocity = (*velocity - omega * trend * dt) * e;
    if (fabsf(x - target) < snap && fabsf(*velocity) < snap * omega) {
        *velocity = 0;
        return target;
    }
    return x;
}

static int32_t set_layer(uint32_t id, float x, float y, float w, float h, float radius,
                         float scroll, float blur, uint32_t flags, uint32_t surface, uint32_t resource) {
    const wa_sf_layer_desc_t d = {
        .layer_id = id, .z_order = (int32_t)id - 1, .x = x, .y = y, .width = w, .height = h,
        .scroll_x = scroll, .scale_x = 1, .scale_y = 1, .alpha = 1, .corner_radius = radius,
        .clip_w = w, .clip_h = h, .backdrop_blur_px = blur, .flags = flags, .surface_kind = surface,
        .resource_id = resource,
    };
    return wa_sf_set_layer(scene, &d);
}

wa_sf_t *m7_demo_create(void) {
    if (scene)
        return scene;
    demo = (demo_state_t){0};
    last_time = last_vsync = pointer_down = axis = 0;
    scroll_v = drawer_v = 0;
    text_dirty = 1;
    scene = wa_sf_create();
    if (!scene)
        return NULL;
    upload_icons();
    static const char url[] = "./m7_webview_fixture.html";
    wa_webview_desc_t web = {WEBVIEW_ID, WEBVIEW, url, sizeof(url) - 1};
    wa_webview_open(scene, &web);
    wa_codec_desc_t codec = {WA_CODEC_H264, 320, 180, 0, ""};
    wa_codec_configure(scene, DECODER_ID, &codec, NULL, 0);
    return scene;
}

void m7_demo_destroy(void) {
    wa_sf_destroy(scene);
    scene = NULL;
}

const demo_state_t *m7_demo_state(void) { return &demo; }

EMSCRIPTEN_KEEPALIVE
float m7_demo_measure_title(wa_sf_t *scene, uint32_t index, float size, uint32_t weight, uint32_t face) {
    static const char *const titles[] = {"Apps", "Modes", "Communal", "WasmAndroid 3D Cube", "系统设置"};
    if (index >= sizeof(titles) / sizeof(titles[0]))
        return 0;
    const char *title = titles[index];
    return wa_canvas_measure_text(scene, title, strlen(title), size, weight, face).advance;
}

int32_t m7_demo_tick(void) {
    wa_sf_vsync_state_t vsync;
    if (!scene || wa_sf_vsync_read(wa_sf_vsync(scene), &vsync) || !vsync.viewport_w ||
        !vsync.viewport_h || last_vsync == vsync.frame_number)
        return 0;
    last_vsync = vsync.frame_number;
    float dt = last_time && vsync.frame_time_ns > last_time
        ? fminf(0.05f, (float)(vsync.frame_time_ns - last_time) * 1e-9f) : 1.f / 60;
    last_time = vsync.frame_time_ns;
    int resize = demo.width != vsync.viewport_w || demo.height != vsync.viewport_h;
    if (resize) {
        demo.width = (float)vsync.viewport_w;
        demo.height = (float)vsync.viewport_h;
        demo.workspace_width = demo.width - (demo.width >= 770 ? 246 : 0);
        demo.scroll_target = demo.scroll = demo.page * demo.workspace_width;
        scroll_v = 0;
    }
    platform_events();
    if (axis != 1)
        demo.scroll = clamp(spring(demo.scroll, demo.scroll_target, &scroll_v, dt, 0.025f),
                            0, demo.workspace_width);
    if (axis != 2)
        demo.drawer = clamp(spring(demo.drawer, demo.drawer_target, &drawer_v, dt, 0.0001f), 0, 1);
    const float w = demo.width, h = demo.height, work = demo.workspace_width;
    const float drawer_h = fmaxf(240, h * 0.78f);
    demo.drawer_y = h - drawer_h * demo.drawer;
    demo.search_x = 32;
    demo.search_y = demo.drawer_y + 66;
    demo.search_w = work - 64;
    uint32_t visible = WA_SF_VISIBLE, clipped = visible | WA_SF_CLIP_BOUNDS;
    int32_t result = wa_sf_begin(scene, vsync.viewport_w, vsync.viewport_h, vsync.density_dpi);
    if (!result) result = set_layer(WALLPAPER, 0, 0, w, h, 0, 0, 0, visible | WA_SF_OPAQUE, WA_SF_SURFACE_DISPLAY_LIST, 0);
    if (!result) result = set_layer(STATUS, 0, 0, w, h, 0, 0, 0, visible, WA_SF_SURFACE_DISPLAY_LIST, 0);
    if (!result) result = set_layer(WORKSPACE, 0, 134, work, fmaxf(1, h - 218), 0,
                                    demo.scroll, 0, clipped, WA_SF_SURFACE_DISPLAY_LIST, 0);
    if (!result) result = set_layer(HOTSEAT, 13, h - 87, fmaxf(1, work - 26), 76, 25, 0, 14,
                                    visible | WA_SF_BLUR_BEHIND, WA_SF_SURFACE_DISPLAY_LIST, 0);
    if (!result) result = set_layer(ALL_APPS, 13, demo.drawer_y, fmaxf(1, work - 26), drawer_h,
                                    29, 0, 0, demo.drawer > 0 ? visible : 0, WA_SF_SURFACE_DISPLAY_LIST, 0);
    const float card_w = (w >= 770 ? 223 : 170);
    const uint32_t card_flags = (w >= 770 || demo.drawer < 0.05f) && h >= 360 ? clipped : 0;
    if (!result) result = set_layer(WEBVIEW, w - card_w - 13, 64, card_w, 166, 20, 0, 0,
                                    card_flags, WA_SF_SURFACE_IFRAME_WEBVIEW, WEBVIEW_ID);
    if (!result) {
        wallpaper((float)(last_time % 60000000000ull) * 1e-9f);
        if (resize || !demo.frame_count) {
            status_bar();
            workspace();
            hotseat();
        }
        if (resize || !demo.frame_count || text_dirty) {
            all_apps();
        }
        result = wa_sf_commit(scene);
    }
    if (result) {
        ++demo.failed_frames;
        wa_sf_abort(scene);
        return result;
    }
    text_dirty = 0;
    ++demo.frame_count;
    return 1;
}
