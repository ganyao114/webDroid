#include "wasmandroid/surfaceflinger.h"

#include <math.h>
#include <pthread.h>
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(expr) do { \
    if (!(expr)) { \
        fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #expr); \
        exit(EXIT_FAILURE); \
    } \
} while (0)

static wa_sf_layer_desc_t layer(uint32_t id, uint32_t parent, int32_t order) {
    return (wa_sf_layer_desc_t){.layer_id=id, .parent_id=parent, .z_order=order,
        .width=640, .height=480, .scale_x=1, .scale_y=1, .alpha=1,
        .flags=WA_SF_VISIBLE, .surface_kind=WA_SF_SURFACE_DISPLAY_LIST};
}

static void check_records(const wa_sf_frame_packet_t *frame, const wa_sf_frame_layer_t *layer_record) {
    const uint8_t *bytes = wa_sf_frame_list(frame, layer_record);
    const uint16_t ops[] = {
        WA_CANVAS_SAVE, WA_CANVAS_TRANSLATE, WA_CANVAS_SCALE, WA_CANVAS_ROTATE, WA_CANVAS_CLIP_RECT,
        WA_CANVAS_CLIP_ROUND_RECT, WA_CANVAS_DRAW_COLOR, WA_CANVAS_DRAW_RECT, WA_CANVAS_DRAW_ROUND_RECT,
        WA_CANVAS_DRAW_CIRCLE, WA_CANVAS_DRAW_LINE, WA_CANVAS_DRAW_LINEAR_GRADIENT, WA_CANVAS_DRAW_SHADOW,
        WA_CANVAS_DRAW_PATH, WA_CANVAS_DRAW_BITMAP, WA_CANVAS_DRAW_TEXT, WA_CANVAS_RESTORE,
    };
    const uint16_t sizes[] = {4, 12, 12, 16, 20, 28, 8, 32, 40, 28, 32, 40, 32, 100, 52, 56, 4};
    uint32_t at = 0;
    for (size_t i = 0; i < sizeof(ops) / sizeof(*ops); ++i) {
        wa_canvas_record_t h;
        CHECK(at % 4 == 0 && at + sizeof(h) <= layer_record->list_size);
        memcpy(&h, bytes + at, sizeof(h));
        CHECK(h.opcode == ops[i] && h.byte_size == sizes[i]);
        const uint8_t *data = bytes + at + sizeof(h);
        if (h.opcode == WA_CANVAS_DRAW_PATH) {
            wa_canvas_path_t path;
            memcpy(&path, data, sizeof(path));
            CHECK(path.verb_count == 5 && path.fill_rule == WA_PATH_EVEN_ODD);
            const uint8_t *verb = data + sizeof(path);
            const uint32_t coords[] = {2, 2, 4, 6, 0};
            for (uint32_t j = 0; j < 5; ++j) {
                uint32_t cmd;
                memcpy(&cmd, verb, sizeof(cmd));
                CHECK(cmd == j + 1);
                verb += 4 + coords[j] * 4;
            }
            CHECK(verb == bytes + at + h.byte_size);
        } else if (h.opcode == WA_CANVAS_DRAW_BITMAP) {
            wa_canvas_bitmap_t bitmap;
            memcpy(&bitmap, data, sizeof(bitmap));
            CHECK(bitmap.bitmap_id == 17 && bitmap.mask == WA_MASK_SQUIRCLE && bitmap.alpha == 0.75f);
        } else if (h.opcode == WA_CANVAS_DRAW_TEXT) {
            wa_canvas_text_t text;
            memcpy(&text, data, sizeof(text));
            CHECK(text.utf8_offset == sizeof(h) + sizeof(text) && text.utf8_len == 12);
            CHECK(!memcmp(bytes + at + text.utf8_offset, "桌面搜索", text.utf8_len));
        }
        at += h.byte_size;
    }
    CHECK(at == layer_record->list_size);
}

static void scene_test(wa_sf_t *sf) {
    CHECK(!wa_sf_frame_acquire(sf));
    CHECK(!wa_sf_begin(sf, 640, 480, 160));
    wa_sf_layer_desc_t a = layer(11, 0, 1), b = layer(4, 0, -3), c = layer(9, 4, 10);
    wa_sf_layer_desc_t d = layer(7, 0, 1);
    CHECK(!wa_sf_set_layer(sf, &a) && !wa_sf_set_layer(sf, &c));
    CHECK(!wa_sf_set_layer(sf, &d) && !wa_sf_set_layer(sf, &b));
    wa_canvas_t *canvas = wa_canvas_begin(sf, 9);
    CHECK(canvas);
    const wa_rect_t box = {2, 3, 31, 44};
    const wa_round_rect_t round = {box, 5, 6};
    const wa_paint_t paint = {0xff557799, WA_PAINT_STROKE, 2.5};
    const wa_canvas_gradient_t gradient = {round, 0xff334455, 0xaf667788, 33};
    const wa_canvas_shadow_t shadow = {box, 4, 9, 0xa0334455};
    const wa_canvas_bitmap_t bitmap = {17, WA_MASK_SQUIRCLE, box, box, 6, 0.75};
    const wa_path_verb_t path[] = {{WA_PATH_MOVE, {1, 1}}, {WA_PATH_LINE, {4, 4}},
        {WA_PATH_QUAD, {5, 6, 7, 8}}, {WA_PATH_CUBIC, {1, 2, 3, 4, 5, 6}}, {WA_PATH_CLOSE, {0}}};
    const wa_canvas_text_t text = {0, 12, 7, 22, 37, 11, 600, WA_TEXT_CENTER, 0xff112233, WA_FONT_ROBOTO};

    CHECK(!wa_canvas_save(canvas) && !wa_canvas_translate(canvas, 3, 4));
    CHECK(!wa_canvas_scale(canvas, 2, 2) && !wa_canvas_rotate(canvas, 15, 2, 3));
    CHECK(!wa_canvas_clip_rect(canvas, box) && !wa_canvas_clip_round_rect(canvas, round));
    CHECK(!wa_canvas_draw_color(canvas, 0xffabcdef) && !wa_canvas_draw_rect(canvas, box, paint));
    CHECK(!wa_canvas_draw_round_rect(canvas, round, paint) && !wa_canvas_draw_circle(canvas, 3, 4, 9, paint));
    CHECK(!wa_canvas_draw_line(canvas, 3, 4, 19, 20, paint) && !wa_canvas_draw_linear_gradient(canvas, &gradient));
    CHECK(!wa_canvas_draw_shadow(canvas, &shadow) && !wa_canvas_draw_path(canvas, path, 5, WA_PATH_EVEN_ODD, paint));
    CHECK(!wa_canvas_draw_bitmap(canvas, &bitmap) && !wa_canvas_draw_text(canvas, &text, "桌面搜索"));
    CHECK(!wa_canvas_end(canvas)); /* auto-restores outstanding saves */
    CHECK(!wa_sf_commit(sf));

    const wa_sf_frame_packet_t *first = wa_sf_frame_acquire(sf);
    CHECK(first && first->magic == WA_SF_FRAME_MAGIC && first->abi_version == 1);
    CHECK(first->sequence == 1 && first->layer_count == 4 && first->density_dpi == 160);
    CHECK(first->byte_size == first->payload_offset + first->payload_size);
    const wa_sf_frame_layer_t *layers = wa_sf_frame_layers(first);
    CHECK(layers[0].desc.layer_id == 4 && layers[1].desc.layer_id == 9);
    CHECK(layers[2].desc.layer_id == 7 && layers[3].desc.layer_id == 11);
    CHECK(layers[1].list_generation == 1);
    check_records(first, layers + 1);

    CHECK(!wa_sf_begin(sf, 800, 600, 240));
    c.scroll_x = 22;
    CHECK(!wa_sf_set_layer(sf, &c) && !wa_sf_commit(sf));
    const wa_sf_frame_packet_t *second = wa_sf_frame_acquire(sf);
    CHECK(second && second != first && second->sequence == 2 && second->viewport_w == 800);
    CHECK(wa_sf_frame_layers(second)[1].desc.scroll_x == 22 && layers[1].desc.scroll_x == 0);
    CHECK(wa_sf_frame_layers(second)[1].list_generation == 1);
    check_records(second, wa_sf_frame_layers(second) + 1);

    CHECK(!wa_sf_begin(sf, 800, 600, 240));
    CHECK(!wa_sf_remove_layer(sf, 11));
    CHECK(wa_sf_commit(sf) == WA_SF_BUSY);
    CHECK(first->sequence == 1 && second->sequence == 2);
    wa_sf_frame_release(sf, first);
    CHECK(!wa_sf_commit(sf));
    const wa_sf_frame_packet_t *third = wa_sf_frame_acquire(sf);
    CHECK(third == first && third->layer_count == 3 && third->sequence == 3);
    wa_sf_frame_release(sf, second);

    CHECK(!wa_sf_begin(sf, 800, 600, 240));
    CHECK(!wa_sf_remove_layer(sf, 4) && wa_sf_commit(sf) == WA_SF_INVALID);
    CHECK(!wa_sf_begin(sf, 800, 600, 240));
    b.parent_id = 9; /* 4 -> 9 -> 4 */
    CHECK(!wa_sf_set_layer(sf, &b) && wa_sf_commit(sf) == WA_SF_INVALID);
    CHECK(!wa_sf_begin(sf, 800, 600, 240));
    canvas = wa_canvas_begin(sf, 9);
    CHECK(canvas);
    int32_t result;
    do { result = wa_canvas_draw_color(canvas, 0xff000000); } while (!result);
    CHECK(result == WA_SF_FULL && wa_canvas_end(canvas) == WA_SF_FULL);
    CHECK(wa_sf_commit(sf) == WA_SF_FULL);
    const wa_sf_frame_packet_t *unchanged = wa_sf_frame_acquire(sf);
    CHECK(unchanged == third && unchanged->sequence == 3);
    check_records(unchanged, wa_sf_frame_layers(unchanged) + 1);
    wa_sf_frame_release(sf, unchanged);
    wa_sf_frame_release(sf, third);

    CHECK(!wa_sf_begin(sf, 640, 480, 160));
    CHECK(!wa_sf_remove_layer(sf, 9) && !wa_sf_remove_layer(sf, 4) && !wa_sf_remove_layer(sf, 7));
    for (uint32_t i = 0; i < WA_SF_MAX_LAYERS; ++i) {
        wa_sf_layer_desc_t many = layer(100 + i, 0, (int32_t)(WA_SF_MAX_LAYERS - i));
        CHECK(!wa_sf_set_layer(sf, &many));
    }
    CHECK(wa_sf_set_layer(sf, &a) == WA_SF_FULL && !wa_sf_commit(sf));
    const wa_sf_frame_packet_t *max = wa_sf_frame_acquire(sf);
    CHECK(max->layer_count == 64 && !max->payload_size && wa_sf_frame_layers(max)[0].desc.layer_id == 163);
    wa_sf_frame_release(sf, max);
}

enum { TRANSFERS = 32768 };
static void *produce(void *state) {
    wa_input_ring_t *ring = state;
    for (uint32_t i = 0; i < TRANSFERS; ++i) {
        wa_input_event_t e = {.kind=WA_INPUT_MOTION, .action=WA_ACTION_MOVE, .pointer_id=i, .event_time_ms=i};
        while (wa_input_push(ring, &e) == WA_SF_FULL)
            sched_yield();
    }
    return NULL;
}

static void input_test(wa_sf_t *sf) {
    wa_input_ring_t *ring = wa_sf_input(sf);
    ring->header.write_index = ring->header.read_index = UINT32_MAX - 37;
    wa_input_event_t e = {.kind=WA_INPUT_IME, .action=WA_IME_COMMIT, .text_len=12,
                           .flags=WA_IME_MORE, .text="桌面搜索"};
    CHECK(!wa_input_push(ring, &e));
    wa_input_event_t out;
    CHECK(wa_input_poll(ring, &out) == 1 && out.flags == WA_IME_MORE);
    CHECK(out.kind == WA_INPUT_IME && out.text_len == 12 && !memcmp(out.text, "桌面搜索", 12));
    e.text_len = 11;
    CHECK(wa_input_push(ring, &e) == WA_SF_INVALID && !wa_input_poll(ring, &out));
    e = (wa_input_event_t){.kind=WA_INPUT_KEY, .action=WA_ACTION_DOWN, .keycode=67, .meta_state=1};
    CHECK(!wa_input_push(ring, &e) && wa_input_poll(ring, &out) == 1 && out.keycode == 67);

    pthread_t thread;
    CHECK(!pthread_create(&thread, NULL, produce, ring));
    for (uint32_t i = 0; i < TRANSFERS; ++i) {
        int32_t found;
        do {
            found = wa_input_poll(ring, &out);
            if (!found) sched_yield();
        } while (!found);
        CHECK(found == 1 && out.pointer_id == i && out.event_time_ms == i);
    }
    CHECK(!pthread_join(thread, NULL) && !wa_input_poll(ring, &out));
    CHECK(ring->header.read_index < TRANSFERS && ring->header.read_index == ring->header.write_index);
}

static void bitmap_and_feedback_test(wa_sf_t *sf) {
    wa_bitmap_store_t *store = wa_sf_bitmaps(sf);
    uint8_t pixels[24] = {1, 2, 3, 255, 50, 60, 70, 128, 99, 99, 99, 99,
                          8, 7, 6, 255, 3, 2, 1, 255, 99, 99, 99, 99};
    CHECK(!wa_bitmap_put(store, 17, 2, 2, 12, pixels));
    const wa_bitmap_view_t *view = wa_bitmap_acquire(store, 17);
    CHECK(view && view->stride == 8 && view->byte_size == 16);
    CHECK(wa_bitmap_pixels(view)[8] == 8 && wa_bitmap_remove(store, 17) == WA_SF_BUSY);
    uint32_t generation = view->generation;
    wa_bitmap_release(store, view);
    CHECK(!wa_bitmap_put(store, 17, 2, 2, 12, pixels));
    view = wa_bitmap_acquire(store, 17);
    CHECK(view && view->generation == generation);
    wa_bitmap_release(store, view);
    pixels[1] = 22;
    CHECK(!wa_bitmap_put(store, 17, 2, 2, 12, pixels));
    view = wa_bitmap_acquire(store, 17);
    CHECK(view && view->generation != generation && wa_bitmap_pixels(view)[1] == 22);
    wa_bitmap_release(store, view);
    CHECK(!wa_bitmap_remove(store, 17) && !wa_bitmap_acquire(store, 17));

    wa_sf_vsync_state_t expected = {0x123456789abeefull, 33, 940, 620, 240}, read;
    wa_sf_vsync_write(wa_sf_vsync(sf), &expected);
    CHECK(!wa_sf_vsync_read(wa_sf_vsync(sf), &read));
    CHECK(read.frame_time_ns == expected.frame_time_ns && read.frame_number == 33);
    CHECK(read.viewport_w == 940 && read.viewport_h == 620 && read.density_dpi == 240);

    wa_text_metrics_t a = wa_canvas_measure_text(sf, "A桌", 4, 16, 500, WA_FONT_ROBOTO);
    wa_font_metric_t *m = wa_sf_font_metrics(sf) + 9 + 4;
    *m = (wa_font_metric_t){2, 32768, 65536, 49152, 16384};
    wa_text_metrics_t b = wa_canvas_measure_text(sf, "A桌", 4, 16, 500, WA_FONT_ROBOTO);
    CHECK(a.advance > 24 && b.advance == 24 && b.ascent == 12 && b.descent == 4);
}

static void platform_test(wa_sf_t *sf) {
    wa_codec_desc_t desc = {WA_CODEC_H264, 320, 180, WA_CODEC_AVCC, "avc1.42001f"};
    const uint8_t avcc[] = {1, 66, 0, 31};
    const uint8_t au[] = {0, 0, 0, 2, 0x65, 0xab};
    CHECK(wa_codec_configure(sf, 2, &desc, NULL, 0) == WA_SF_INVALID);
    CHECK(!wa_codec_configure(sf, 2, &desc, avcc, sizeof(avcc)));
    CHECK(!wa_codec_submit(sf, 2, au, sizeof(au), 12345, 33333, WA_CODEC_KEY_FRAME));
    CHECK(!wa_codec_control(sf, 2, WA_CODEC_FLUSH));
    wa_codec_requests_t *q = wa_sf_codec_requests(sf);
    CHECK(q->header.write_index == 3 && q->entries[0].codec == WA_CODEC_H264);
    CHECK(q->entries[0].action == WA_CODEC_CONFIGURE && !memcmp(q->entries[0].data, avcc, sizeof(avcc)));
    CHECK(q->entries[1].timestamp_us == 12345 && !memcmp(q->entries[1].data, au, sizeof(au)));
    CHECK(q->entries[2].action == WA_CODEC_FLUSH);
    wa_codec_events_t *r = wa_sf_codec_events(sf);
    r->entries[0] = (wa_codec_event_t){2, WA_CODEC_DECODED, 320, 180, 12345, 0, 0};
    r->header.write_index = 1;
    wa_codec_event_t event;
    CHECK(wa_codec_poll(sf, &event) == 1 && event.timestamp_us == 12345 && !wa_codec_poll(sf, &event));

    const char url[] = "https://example.test/webview";
    wa_webview_desc_t web = {7, 29, url, sizeof(url) - 1};
    CHECK(!wa_webview_open(sf, &web) && !wa_webview_eval(sf, 7, 77, "document.title", 14));
    CHECK(!wa_webview_reply(sf, 7, 78, "\"桌面\"", 8) && !wa_webview_close(sf, 7));
    wa_webview_requests_t *w = wa_sf_webview_requests(sf);
    CHECK(w->header.write_index == 4 && w->entries[0].layer_id == 29);
    CHECK(!memcmp(w->entries[0].text, url, sizeof(url) - 1) && w->entries[1].request_id == 77);
    CHECK(w->entries[2].action == WA_WEBVIEW_REPLY && w->entries[3].action == WA_WEBVIEW_CLOSE);
    wa_webview_events_t *e = wa_sf_webview_events(sf);
    e->entries[0] = (wa_webview_event_t){WA_WEBVIEW_EVAL_RESULT, 7, 77, 6, "桌面"};
    e->header.write_index = 1;
    wa_webview_event_t response;
    CHECK(wa_webview_poll(sf, &response) == 1 && !memcmp(response.text, "桌面", 6));
    CHECK(!wa_webview_poll(sf, &response));
}

int main(void) {
    wa_sf_t *sf = wa_sf_create();
    CHECK(sf);
    scene_test(sf);
    input_test(sf);
    bitmap_and_feedback_test(sf);
    platform_test(sf);
    wa_sf_destroy(sf);
    printf("Verified 17 opcodes, 64 layers, both frame buffers, %d concurrent input transfers, "
           "bitmap generations, vsync, font feedback, codec and WebView ABI\n", TRANSFERS);
    return EXIT_SUCCESS;
}
