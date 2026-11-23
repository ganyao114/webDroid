#ifndef WASMANDROID_SURFACEFLINGER_H
#define WASMANDROID_SURFACEFLINGER_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Version 1 wire structures contain only little-endian fixed-width fields.
 * Pointers returned by this API are offsets into linear memory on wasm32.
 * Each scene has one transaction producer. Frame readers use acquire/release. */
#define WA_SF_ABI_VERSION 1u
#define WA_SF_FRAME_MAGIC 0x31464157u /* WAF1 */
#define WA_SF_MAX_LAYERS 64u
#define WA_SF_LAYER_LIST_CAPACITY 32768u
#define WA_CANVAS_SAVE_DEPTH 64u
#define WA_INPUT_CAPACITY 128u
#define WA_INPUT_TEXT_CAPACITY 256u
#define WA_FONT_FACE_COUNT 3u
#define WA_FONT_WEIGHT_COUNT 9u
#define WA_CODEC_REQUEST_CAPACITY 8u
#define WA_CODEC_EVENT_CAPACITY 64u
#define WA_CODEC_AU_CAPACITY (512u * 1024u)
#define WA_CODEC_NAME_CAPACITY 48u
#define WA_WEBVIEW_CAPACITY 16u
#define WA_WEBVIEW_TEXT_CAPACITY 2048u

enum wa_sf_result {
    WA_SF_OK = 0,
    WA_SF_INVALID = -1,
    WA_SF_FULL = -2,
    WA_SF_BUSY = -3,
    WA_SF_STATE = -4,
    WA_SF_NOMEM = -5,
};

enum wa_sf_layer_flags {
    WA_SF_VISIBLE = 1u << 0,
    WA_SF_CLIP_BOUNDS = 1u << 1,
    WA_SF_OPAQUE = 1u << 2,
    WA_SF_BLUR_BEHIND = 1u << 3,
};

enum wa_sf_surface_kind {
    WA_SF_SURFACE_DISPLAY_LIST = 1,
    WA_SF_SURFACE_RGBA_BITMAP = 2,
    WA_SF_SURFACE_GL_TEXTURE = 3,
    WA_SF_SURFACE_IFRAME_WEBVIEW = 4,
    WA_SF_SURFACE_VIDEO_DECODER = 5,
};

/* ID 0 is reserved for the root. Siblings sort by (z_order, layer_id).
 * Geometry is in logical pixels, relative to the parent's scrolled origin.
 * Rotation is clockwise around the local origin. Clip and radius stay fixed
 * while scroll offsets move content and descendants. resource_id selects the
 * bitmap, external texture, WebView, or decoder for non-display surfaces. */
typedef struct wa_sf_layer_desc {
    uint32_t layer_id, parent_id;
    int32_t z_order;
    float x, y, width, height;
    float scroll_x, scroll_y;
    float scale_x, scale_y, rotation_deg, alpha, corner_radius;
    float clip_x, clip_y, clip_w, clip_h, backdrop_blur_px;
    uint32_t flags, surface_kind, resource_id;
} wa_sf_layer_desc_t;

typedef struct wa_sf_frame_packet {
    uint32_t magic, abi_version, byte_size, sequence;
    uint32_t layer_count, layers_offset, payload_offset, payload_size;
    uint32_t viewport_w, viewport_h, density_dpi, reserved;
} wa_sf_frame_packet_t;

typedef struct wa_sf_frame_layer {
    wa_sf_layer_desc_t desc;
    uint32_t list_offset, list_size, list_generation, reserved;
} wa_sf_frame_layer_t;

typedef struct wa_sf wa_sf_t;
typedef struct wa_canvas wa_canvas_t;
typedef struct wa_bitmap_store wa_bitmap_store_t;

wa_sf_t *wa_sf_create(void);
void wa_sf_destroy(wa_sf_t *sf);
int32_t wa_sf_begin(wa_sf_t *sf, uint32_t viewport_w, uint32_t viewport_h, uint32_t density_dpi);
int32_t wa_sf_set_layer(wa_sf_t *sf, const wa_sf_layer_desc_t *desc);
int32_t wa_sf_remove_layer(wa_sf_t *sf, uint32_t layer_id);
/* BUSY leaves the transaction open for retry. Other commit errors abort it. */
int32_t wa_sf_commit(wa_sf_t *sf);
void wa_sf_abort(wa_sf_t *sf);
/* A reader must release each acquired packet before it can be reused. */
const wa_sf_frame_packet_t *wa_sf_frame_acquire(wa_sf_t *sf);
void wa_sf_frame_release(wa_sf_t *sf, const wa_sf_frame_packet_t *frame);
const wa_sf_frame_layer_t *wa_sf_frame_layers(const wa_sf_frame_packet_t *frame);
const uint8_t *wa_sf_frame_list(const wa_sf_frame_packet_t *frame, const wa_sf_frame_layer_t *layer);

enum wa_canvas_opcode {
    WA_CANVAS_SAVE = 1, WA_CANVAS_RESTORE, WA_CANVAS_TRANSLATE, WA_CANVAS_SCALE,
    WA_CANVAS_ROTATE, WA_CANVAS_CLIP_RECT, WA_CANVAS_CLIP_ROUND_RECT, WA_CANVAS_DRAW_COLOR,
    WA_CANVAS_DRAW_RECT, WA_CANVAS_DRAW_ROUND_RECT, WA_CANVAS_DRAW_CIRCLE, WA_CANVAS_DRAW_LINE,
    WA_CANVAS_DRAW_LINEAR_GRADIENT, WA_CANVAS_DRAW_SHADOW, WA_CANVAS_DRAW_PATH,
    WA_CANVAS_DRAW_BITMAP, WA_CANVAS_DRAW_TEXT,
};
enum wa_paint_style { WA_PAINT_FILL = 0, WA_PAINT_STROKE = 1 };
enum wa_path_command { WA_PATH_MOVE = 1, WA_PATH_LINE, WA_PATH_QUAD, WA_PATH_CUBIC, WA_PATH_CLOSE };
enum wa_path_fill_rule { WA_PATH_NONZERO = 0, WA_PATH_EVEN_ODD = 1 };
enum wa_bitmap_mask { WA_MASK_RECT = 0, WA_MASK_ROUNDED_RECT, WA_MASK_CIRCLE, WA_MASK_SQUIRCLE };
enum wa_text_align { WA_TEXT_LEFT = 0, WA_TEXT_CENTER, WA_TEXT_RIGHT };
enum wa_font_face { WA_FONT_SYSTEM = 0, WA_FONT_ROBOTO, WA_FONT_MONOSPACE };

typedef struct wa_canvas_record { uint16_t opcode, byte_size; } wa_canvas_record_t;
typedef struct wa_rect { float l, t, r, b; } wa_rect_t;
typedef struct wa_round_rect { wa_rect_t rect; float rx, ry; } wa_round_rect_t;
typedef struct wa_paint { uint32_t argb, style; float stroke_width; } wa_paint_t;
typedef struct wa_path_verb { uint32_t command; float points[6]; } wa_path_verb_t;
typedef struct wa_canvas_xy { float x, y; } wa_canvas_xy_t;
typedef struct wa_canvas_rotation { float degrees, px, py; } wa_canvas_rotation_t;
typedef struct wa_canvas_rect_draw { wa_rect_t rect; wa_paint_t paint; } wa_canvas_rect_draw_t;
typedef struct wa_canvas_round_draw { wa_round_rect_t shape; wa_paint_t paint; } wa_canvas_round_draw_t;
typedef struct wa_canvas_circle {
    float cx, cy, radius;
    wa_paint_t paint;
} wa_canvas_circle_t;
typedef struct wa_canvas_line { float x0, y0, x1, y1; wa_paint_t paint; } wa_canvas_line_t;
typedef struct wa_canvas_gradient {
    wa_round_rect_t shape;
    uint32_t argb0, argb1;
    float degrees;
} wa_canvas_gradient_t;
typedef struct wa_canvas_shadow {
    wa_rect_t rect;
    float radius, elevation;
    uint32_t argb;
} wa_canvas_shadow_t;
/* Path header followed by packed uint32 command and 2/2/4/6/0 float
 * coordinates per MOVE/LINE/QUAD/CUBIC/CLOSE command. */
typedef struct wa_canvas_path { wa_paint_t paint; uint32_t fill_rule, verb_count; } wa_canvas_path_t;
typedef struct wa_canvas_bitmap {
    uint32_t bitmap_id, mask;
    wa_rect_t src, dst;
    float corner_radius, alpha;
} wa_canvas_bitmap_t;
/* utf8_offset is relative to the start of the opcode record. y is baseline. */
typedef struct wa_canvas_text {
    uint32_t utf8_offset, utf8_len;
    float x, y, max_width, font_size;
    uint32_t weight, align, argb, font_face;
} wa_canvas_text_t;

/* One open recording per transaction. End replaces that layer's display list.
 * Malformed or overflowing recordings cause commit to reject the transaction.
 * End unwinds any remaining saves, as Android RecordingCanvas does. */
wa_canvas_t *wa_canvas_begin(wa_sf_t *sf, uint32_t layer_id);
int32_t wa_canvas_end(wa_canvas_t *canvas);
int32_t wa_canvas_save(wa_canvas_t *canvas);
int32_t wa_canvas_restore(wa_canvas_t *canvas);
int32_t wa_canvas_translate(wa_canvas_t *canvas, float dx, float dy);
int32_t wa_canvas_scale(wa_canvas_t *canvas, float sx, float sy);
int32_t wa_canvas_rotate(wa_canvas_t *canvas, float degrees, float px, float py);
int32_t wa_canvas_clip_rect(wa_canvas_t *canvas, wa_rect_t rect);
int32_t wa_canvas_clip_round_rect(wa_canvas_t *canvas, wa_round_rect_t shape);
int32_t wa_canvas_draw_color(wa_canvas_t *canvas, uint32_t argb);
int32_t wa_canvas_draw_rect(wa_canvas_t *canvas, wa_rect_t rect, wa_paint_t paint);
int32_t wa_canvas_draw_round_rect(wa_canvas_t *canvas, wa_round_rect_t shape, wa_paint_t paint);
int32_t wa_canvas_draw_circle(wa_canvas_t *canvas, float cx, float cy, float radius, wa_paint_t paint);
int32_t wa_canvas_draw_line(wa_canvas_t *canvas, float x0, float y0, float x1, float y1, wa_paint_t paint);
int32_t wa_canvas_draw_linear_gradient(wa_canvas_t *canvas, const wa_canvas_gradient_t *gradient);
int32_t wa_canvas_draw_shadow(wa_canvas_t *canvas, const wa_canvas_shadow_t *shadow);
int32_t wa_canvas_draw_path(wa_canvas_t *canvas, const wa_path_verb_t *verbs,
                            uint32_t count, uint32_t fill_rule, wa_paint_t paint);
int32_t wa_canvas_draw_bitmap(wa_canvas_t *canvas, const wa_canvas_bitmap_t *bitmap);
int32_t wa_canvas_draw_text(wa_canvas_t *canvas, const wa_canvas_text_t *text, const char *utf8);

/* Bitmap pixels are tightly packed straight-alpha RGBA8 after the view header.
 * The producer may retry BUSY after readers have released their views. */
typedef struct wa_bitmap_view {
    uint32_t bitmap_id, width, height, stride, generation, byte_size;
} wa_bitmap_view_t;
wa_bitmap_store_t *wa_sf_bitmaps(wa_sf_t *sf);
int32_t wa_bitmap_put(wa_bitmap_store_t *store, uint32_t id, uint32_t width, uint32_t height,
                      uint32_t stride, const void *rgba);
int32_t wa_bitmap_remove(wa_bitmap_store_t *store, uint32_t id);
const wa_bitmap_view_t *wa_bitmap_acquire(wa_bitmap_store_t *store, uint32_t id);
void wa_bitmap_release(wa_bitmap_store_t *store, const wa_bitmap_view_t *view);
const uint8_t *wa_bitmap_pixels(const wa_bitmap_view_t *view);

/* All shared indices and feedback words are accessed atomically. Rings have
 * one producer and one consumer, monotonic indices, and power-of-two capacity.
 * Initialize caller-owned instances to zero before sharing. */
typedef struct wa_sf_ring_header {
    uint32_t write_index, read_index, dropped, reserved;
} wa_sf_ring_header_t;
enum wa_input_kind { WA_INPUT_MOTION = 1, WA_INPUT_KEY, WA_INPUT_IME };
enum wa_input_action { WA_ACTION_DOWN = 0, WA_ACTION_UP = 1, WA_ACTION_MOVE = 2,
                       WA_ACTION_CANCEL = 3, WA_ACTION_SCROLL = 8 };
enum wa_ime_action { WA_IME_COMPOSING = 1, WA_IME_COMMIT = 2 };
enum wa_ime_flags { WA_IME_MORE = 1u << 0, WA_IME_CONTINUATION = 1u << 1 };
typedef struct wa_input_event {
    uint32_t kind, action, pointer_id, keycode, meta_state, text_len;
    double event_time_ms;
    float x, y, dx, dy, velocity_x, velocity_y;
    char text[WA_INPUT_TEXT_CAPACITY];
    uint32_t flags, reserved;
} wa_input_event_t;
typedef struct wa_input_ring {
    wa_sf_ring_header_t header;
    wa_input_event_t events[WA_INPUT_CAPACITY];
} wa_input_ring_t;
wa_input_ring_t *wa_sf_input(wa_sf_t *sf);
int32_t wa_input_push(wa_input_ring_t *ring, const wa_input_event_t *event);
int32_t wa_input_poll(wa_input_ring_t *ring, wa_input_event_t *event);

typedef struct wa_sf_vsync {
    uint32_t sequence, frame_number, time_ns_lo, time_ns_hi;
    uint32_t viewport_w, viewport_h, density_dpi, reserved;
} wa_sf_vsync_t;
typedef struct wa_sf_vsync_state {
    uint64_t frame_time_ns;
    uint32_t frame_number, viewport_w, viewport_h, density_dpi;
} wa_sf_vsync_state_t;
wa_sf_vsync_t *wa_sf_vsync(wa_sf_t *sf);
void wa_sf_vsync_write(wa_sf_vsync_t *vsync, const wa_sf_vsync_state_t *state);
int32_t wa_sf_vsync_read(const wa_sf_vsync_t *vsync, wa_sf_vsync_state_t *state);

/* Face-major entries for weights 100 through 900. Values are ratios in Q16. */
typedef struct wa_font_metric {
    uint32_t sequence, latin_q16, wide_q16, ascent_q16, descent_q16;
} wa_font_metric_t;
typedef struct wa_text_metrics { float advance, ascent, descent; } wa_text_metrics_t;
wa_font_metric_t *wa_sf_font_metrics(wa_sf_t *sf);
wa_text_metrics_t wa_canvas_measure_text(const wa_sf_t *sf, const char *utf8, uint32_t len,
                                         float font_size, uint32_t weight, uint32_t font_face);

enum wa_codec_kind { WA_CODEC_H264 = 1, WA_CODEC_VP9, WA_CODEC_AV1 };
enum wa_codec_action { WA_CODEC_CONFIGURE = 1, WA_CODEC_ACCESS_UNIT, WA_CODEC_FLUSH, WA_CODEC_CLOSE };
enum wa_codec_flags { WA_CODEC_KEY_FRAME = 1u << 0, WA_CODEC_AVCC = 1u << 1 };
enum wa_codec_status {
    WA_CODEC_CONFIGURED = 1, WA_CODEC_DECODED, WA_CODEC_UNSUPPORTED,
    WA_CODEC_ERROR, WA_CODEC_FLUSHED, WA_CODEC_CLOSED,
};
typedef struct wa_codec_desc {
    uint32_t codec, width, height, flags;
    char codec_string[WA_CODEC_NAME_CAPACITY];
} wa_codec_desc_t;
typedef struct wa_codec_request {
    uint32_t action, decoder_id, codec, flags, width, height, byte_size, duration_us;
    int64_t timestamp_us;
    char codec_string[WA_CODEC_NAME_CAPACITY];
    uint8_t data[WA_CODEC_AU_CAPACITY];
} wa_codec_request_t;
typedef struct wa_codec_event {
    uint32_t decoder_id, status, width, height;
    int64_t timestamp_us;
    uint32_t detail, reserved;
} wa_codec_event_t;
typedef struct wa_codec_requests {
    wa_sf_ring_header_t header;
    wa_codec_request_t entries[WA_CODEC_REQUEST_CAPACITY];
} wa_codec_requests_t;
typedef struct wa_codec_events {
    wa_sf_ring_header_t header;
    wa_codec_event_t entries[WA_CODEC_EVENT_CAPACITY];
} wa_codec_events_t;
wa_codec_requests_t *wa_sf_codec_requests(wa_sf_t *sf);
wa_codec_events_t *wa_sf_codec_events(wa_sf_t *sf);
/* AVCC requires an AVCDecoderConfigurationRecord. Annex B uses in-band SPS/PPS. */
int32_t wa_codec_configure(wa_sf_t *sf, uint32_t id, const wa_codec_desc_t *desc,
                           const void *description, uint32_t size);
int32_t wa_codec_submit(wa_sf_t *sf, uint32_t id, const void *au, uint32_t size,
                        int64_t timestamp_us, uint32_t duration_us, uint32_t flags);
int32_t wa_codec_control(wa_sf_t *sf, uint32_t id, uint32_t action);
int32_t wa_codec_poll(wa_sf_t *sf, wa_codec_event_t *event);

enum wa_webview_action { WA_WEBVIEW_OPEN = 1, WA_WEBVIEW_CLOSE, WA_WEBVIEW_EVAL, WA_WEBVIEW_REPLY };
enum wa_webview_status { WA_WEBVIEW_READY = 1, WA_WEBVIEW_EVAL_RESULT,
                         WA_WEBVIEW_INTERFACE, WA_WEBVIEW_ERROR, WA_WEBVIEW_PAGE_LOADED };
typedef struct wa_webview_desc {
    uint32_t webview_id, layer_id;
    const char *url;
    uint32_t url_len;
} wa_webview_desc_t;
typedef struct wa_webview_request {
    uint32_t action, webview_id, layer_id, request_id, text_len, reserved;
    char text[WA_WEBVIEW_TEXT_CAPACITY];
} wa_webview_request_t;
typedef struct wa_webview_event {
    uint32_t status, webview_id, request_id, text_len;
    char text[WA_WEBVIEW_TEXT_CAPACITY];
} wa_webview_event_t;
typedef struct wa_webview_requests {
    wa_sf_ring_header_t header;
    wa_webview_request_t entries[WA_WEBVIEW_CAPACITY];
} wa_webview_requests_t;
typedef struct wa_webview_events {
    wa_sf_ring_header_t header;
    wa_webview_event_t entries[WA_WEBVIEW_CAPACITY];
} wa_webview_events_t;
wa_webview_requests_t *wa_sf_webview_requests(wa_sf_t *sf);
wa_webview_events_t *wa_sf_webview_events(wa_sf_t *sf);
int32_t wa_webview_open(wa_sf_t *sf, const wa_webview_desc_t *desc);
int32_t wa_webview_close(wa_sf_t *sf, uint32_t id);
int32_t wa_webview_eval(wa_sf_t *sf, uint32_t id, uint32_t request_id,
                        const char *script, uint32_t len);
int32_t wa_webview_reply(wa_sf_t *sf, uint32_t id, uint32_t request_id,
                         const char *json, uint32_t len);
int32_t wa_webview_poll(wa_sf_t *sf, wa_webview_event_t *event);

#ifdef __cplusplus
}
#endif
#endif
