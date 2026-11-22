#include "internal.h"

#include <math.h>
#include <string.h>

#ifdef __EMSCRIPTEN__
#include <emscripten/emscripten.h>
#endif

_Static_assert(sizeof(wa_canvas_record_t) == 4, "record header ABI");
_Static_assert(sizeof(wa_canvas_rect_draw_t) == 28, "rect ABI");
_Static_assert(sizeof(wa_canvas_round_draw_t) == 36, "round rect ABI");
_Static_assert(sizeof(wa_canvas_circle_t) == 24, "circle ABI");
_Static_assert(sizeof(wa_canvas_line_t) == 28, "line ABI");
_Static_assert(sizeof(wa_canvas_gradient_t) == 36, "gradient ABI");
_Static_assert(sizeof(wa_canvas_shadow_t) == 28, "shadow ABI");
_Static_assert(sizeof(wa_canvas_path_t) == 20, "path ABI");
_Static_assert(sizeof(wa_canvas_bitmap_t) == 48, "bitmap ABI");
_Static_assert(sizeof(wa_canvas_text_t) == 40, "text ABI");

wa_canvas_t *wa_canvas_begin(wa_sf_t *sf, uint32_t id) {
    if (!sf || !sf->in_transaction || !id || sf->canvas.data || sf->txn_error)
        return NULL;
    int slot = wa_sf_find_layer(sf->staged, id);
    if (slot < 0 || sf->staged[slot].desc.surface_kind != WA_SF_SURFACE_DISPLAY_LIST)
        return NULL;
    uint32_t bank = sf->committed[slot].bank ^ 1u;
    sf->canvas = (wa_canvas_t){sf, (uint32_t)slot, bank, 0, 0, WA_SF_OK, sf->lists[slot][bank]};
    return &sf->canvas;
}

static int32_t fail(wa_canvas_t *c, int32_t status) {
    if (!c || !c->data)
        return WA_SF_STATE;
    if (!c->status) {
        c->status = status;
        c->sf->txn_error = status;
    }
    return c->status;
}

static uint8_t *reserve(wa_canvas_t *c, uint16_t opcode, uint32_t payload) {
    if (!c || !c->data || c->status)
        return NULL;
    uint32_t size = (payload + sizeof(wa_canvas_record_t) + 3u) & ~3u;
    if (size > WA_SF_LAYER_LIST_CAPACITY - c->used) {
        fail(c, WA_SF_FULL);
        return NULL;
    }
    wa_canvas_record_t header = {opcode, (uint16_t)size};
    uint8_t *dst = c->data + c->used;
    memcpy(dst, &header, sizeof(header));
    memset(dst + sizeof(header) + payload, 0, size - sizeof(header) - payload);
    c->used += size;
    return dst + sizeof(header);
}

static int32_t emit(wa_canvas_t *c, uint16_t opcode, const void *payload, uint32_t size) {
    uint8_t *dst = reserve(c, opcode, size);
    if (!dst)
        return c && c->data ? c->status : WA_SF_STATE;
    if (size)
        memcpy(dst, payload, size);
    return WA_SF_OK;
}

int32_t wa_canvas_end(wa_canvas_t *c) {
    if (!c || !c->data)
        return WA_SF_STATE;
    while (!c->status && c->depth)
        wa_canvas_restore(c);
    if (!c->status) {
        wa_layer_state_t *slot = &c->sf->staged[c->slot];
        slot->bank = c->bank;
        slot->size = c->used;
        slot->generation = wa_next_generation(c->sf->committed[c->slot].generation);
    }
    c->data = NULL;
    return c->status;
}

int32_t wa_canvas_save(wa_canvas_t *c) {
    if (c && c->depth >= WA_CANVAS_SAVE_DEPTH)
        return fail(c, WA_SF_FULL);
    int32_t status = emit(c, WA_CANVAS_SAVE, NULL, 0);
    if (!status)
        ++c->depth;
    return status;
}

int32_t wa_canvas_restore(wa_canvas_t *c) {
    if (!c || !c->depth)
        return fail(c, WA_SF_STATE);
    int32_t status = emit(c, WA_CANVAS_RESTORE, NULL, 0);
    if (!status)
        --c->depth;
    return status;
}

int32_t wa_canvas_translate(wa_canvas_t *c, float x, float y) {
    wa_canvas_xy_t data = {x, y};
    return isfinite(x) && isfinite(y) ? emit(c, WA_CANVAS_TRANSLATE, &data, sizeof(data))
                                     : fail(c, WA_SF_INVALID);
}

int32_t wa_canvas_scale(wa_canvas_t *c, float x, float y) {
    wa_canvas_xy_t data = {x, y};
    return isfinite(x) && isfinite(y) ? emit(c, WA_CANVAS_SCALE, &data, sizeof(data))
                                     : fail(c, WA_SF_INVALID);
}

int32_t wa_canvas_rotate(wa_canvas_t *c, float degrees, float x, float y) {
    wa_canvas_rotation_t data = {degrees, x, y};
    return isfinite(degrees) && isfinite(x) && isfinite(y)
               ? emit(c, WA_CANVAS_ROTATE, &data, sizeof(data))
               : fail(c, WA_SF_INVALID);
}

static int valid_rect(wa_rect_t r) {
    return isfinite(r.l) && isfinite(r.t) && isfinite(r.r) && isfinite(r.b);
}

static int valid_radius(float r) {
    return isfinite(r) && r >= 0;
}

static int valid_round(wa_round_rect_t r) {
    return valid_rect(r.rect) && valid_radius(r.rx) && valid_radius(r.ry);
}

static int valid_paint(wa_paint_t p) {
    return p.style <= WA_PAINT_STROKE && valid_radius(p.stroke_width);
}

int32_t wa_canvas_clip_rect(wa_canvas_t *c, wa_rect_t rect) {
    return valid_rect(rect) ? emit(c, WA_CANVAS_CLIP_RECT, &rect, sizeof(rect))
                            : fail(c, WA_SF_INVALID);
}

int32_t wa_canvas_clip_round_rect(wa_canvas_t *c, wa_round_rect_t shape) {
    return valid_round(shape) ? emit(c, WA_CANVAS_CLIP_ROUND_RECT, &shape, sizeof(shape))
                              : fail(c, WA_SF_INVALID);
}

int32_t wa_canvas_draw_color(wa_canvas_t *c, uint32_t argb) {
    return emit(c, WA_CANVAS_DRAW_COLOR, &argb, sizeof(argb));
}

int32_t wa_canvas_draw_rect(wa_canvas_t *c, wa_rect_t rect, wa_paint_t p) {
    wa_canvas_rect_draw_t data = {rect, p};
    return valid_rect(rect) && valid_paint(p) ? emit(c, WA_CANVAS_DRAW_RECT, &data, sizeof(data))
                                             : fail(c, WA_SF_INVALID);
}

int32_t wa_canvas_draw_round_rect(wa_canvas_t *c, wa_round_rect_t shape, wa_paint_t p) {
    wa_canvas_round_draw_t data = {shape, p};
    return valid_round(shape) && valid_paint(p) ? emit(c, WA_CANVAS_DRAW_ROUND_RECT, &data, sizeof(data))
                                               : fail(c, WA_SF_INVALID);
}

int32_t wa_canvas_draw_circle(wa_canvas_t *c, float x, float y, float radius, wa_paint_t p) {
    wa_canvas_circle_t data = {x, y, radius, p};
    return isfinite(x) && isfinite(y) && valid_radius(radius) && valid_paint(p)
               ? emit(c, WA_CANVAS_DRAW_CIRCLE, &data, sizeof(data))
               : fail(c, WA_SF_INVALID);
}

int32_t wa_canvas_draw_line(wa_canvas_t *c, float x0, float y0, float x1, float y1, wa_paint_t p) {
    wa_canvas_line_t data = {x0, y0, x1, y1, p};
    return isfinite(x0) && isfinite(y0) && isfinite(x1) && isfinite(y1) && valid_paint(p)
               ? emit(c, WA_CANVAS_DRAW_LINE, &data, sizeof(data))
               : fail(c, WA_SF_INVALID);
}

int32_t wa_canvas_draw_linear_gradient(wa_canvas_t *c, const wa_canvas_gradient_t *g) {
    return g && valid_round(g->shape) && isfinite(g->degrees)
               ? emit(c, WA_CANVAS_DRAW_LINEAR_GRADIENT, g, sizeof(*g))
               : fail(c, WA_SF_INVALID);
}

int32_t wa_canvas_draw_shadow(wa_canvas_t *c, const wa_canvas_shadow_t *s) {
    return s && valid_rect(s->rect) && valid_radius(s->radius) && valid_radius(s->elevation)
               ? emit(c, WA_CANVAS_DRAW_SHADOW, s, sizeof(*s))
               : fail(c, WA_SF_INVALID);
}

int32_t wa_canvas_draw_path(wa_canvas_t *c, const wa_path_verb_t *verbs, uint32_t count,
                            uint32_t fill_rule, wa_paint_t paint) {
    static const uint8_t coordinates[] = {0, 2, 2, 4, 6, 0};
    if (!verbs || !count || verbs[0].command != WA_PATH_MOVE || !valid_paint(paint) ||
        fill_rule > WA_PATH_EVEN_ODD)
        return fail(c, WA_SF_INVALID);
    if (count > WA_SF_LAYER_LIST_CAPACITY / sizeof(uint32_t))
        return fail(c, WA_SF_FULL);
    uint32_t size = sizeof(wa_canvas_path_t);
    for (uint32_t i = 0; i < count; ++i) {
        uint32_t command = verbs[i].command;
        if (!command || command > WA_PATH_CLOSE)
            return fail(c, WA_SF_INVALID);
        uint32_t n = coordinates[command];
        for (uint32_t j = 0; j < n; ++j)
            if (!isfinite(verbs[i].points[j]))
                return fail(c, WA_SF_INVALID);
        size += sizeof(uint32_t) + n * sizeof(float);
    }
    uint8_t *dst = reserve(c, WA_CANVAS_DRAW_PATH, size);
    if (!dst)
        return c && c->data ? c->status : WA_SF_STATE;
    wa_canvas_path_t header = {paint, fill_rule, count};
    memcpy(dst, &header, sizeof(header));
    dst += sizeof(header);
    for (uint32_t i = 0; i < count; ++i) {
        uint32_t bytes = coordinates[verbs[i].command] * sizeof(float);
        memcpy(dst, &verbs[i].command, sizeof(uint32_t));
        memcpy(dst + sizeof(uint32_t), verbs[i].points, bytes);
        dst += sizeof(uint32_t) + bytes;
    }
    return WA_SF_OK;
}

int32_t wa_canvas_draw_bitmap(wa_canvas_t *c, const wa_canvas_bitmap_t *b) {
    return b && b->bitmap_id && b->mask <= WA_MASK_SQUIRCLE && valid_rect(b->src) &&
                   valid_rect(b->dst) && valid_radius(b->corner_radius) &&
                   isfinite(b->alpha) && b->alpha >= 0 && b->alpha <= 1
               ? emit(c, WA_CANVAS_DRAW_BITMAP, b, sizeof(*b))
               : fail(c, WA_SF_INVALID);
}

int32_t wa_canvas_draw_text(wa_canvas_t *c, const wa_canvas_text_t *t, const char *utf8) {
    if (!t || (!utf8 && t->utf8_len) || !isfinite(t->x) || !isfinite(t->y) ||
        !valid_radius(t->max_width) || !valid_radius(t->font_size) || !t->font_size ||
        t->weight > 1000 || t->align > WA_TEXT_RIGHT || t->font_face >= WA_FONT_FACE_COUNT)
        return fail(c, WA_SF_INVALID);
    if (t->utf8_len > WA_SF_LAYER_LIST_CAPACITY - sizeof(*t) - sizeof(wa_canvas_record_t))
        return fail(c, WA_SF_FULL);
    if (!wa_utf8_valid(utf8, t->utf8_len))
        return fail(c, WA_SF_INVALID);
    uint8_t *dst = reserve(c, WA_CANVAS_DRAW_TEXT, sizeof(*t) + t->utf8_len);
    if (!dst)
        return c && c->data ? c->status : WA_SF_STATE;
    wa_canvas_text_t data = *t;
    data.utf8_offset = sizeof(wa_canvas_record_t) + sizeof(*t);
    memcpy(dst, &data, sizeof(data));
    if (t->utf8_len)
        memcpy(dst + sizeof(data), utf8, t->utf8_len);
    return WA_SF_OK;
}

int wa_utf8_next(const char *text, uint32_t size, uint32_t *offset, uint32_t *codepoint) {
    if (*offset >= size)
        return 0;
    const uint8_t *bytes = (const uint8_t *)text;
    uint32_t first = bytes[(*offset)++];
    if (first < 0x80) {
        *codepoint = first;
        return 1;
    }
    uint32_t count, value, minimum;
    if (first >= 0xc2 && first <= 0xdf) {
        count = 1;
        value = first & 0x1f;
        minimum = 0x80;
    } else if (first >= 0xe0 && first <= 0xef) {
        count = 2;
        value = first & 0x0f;
        minimum = 0x800;
    } else if (first >= 0xf0 && first <= 0xf4) {
        count = 3;
        value = first & 7;
        minimum = 0x10000;
    } else {
        return -1;
    }
    if (count > size - *offset)
        return -1;
    uint32_t end = *offset + count;
    for (uint32_t i = *offset; i < end; ++i) {
        if ((bytes[i] & 0xc0) != 0x80)
            return -1;
        value = (value << 6) | (bytes[i] & 0x3f);
    }
    if (value < minimum || value > 0x10ffff || (value >= 0xd800 && value <= 0xdfff))
        return -1;
    *offset = end;
    *codepoint = value;
    return 1;
}

int wa_utf8_valid(const char *text, uint32_t size) {
    if (size && !text)
        return 0;
    uint32_t offset = 0, codepoint;
    int status;
    do {
        status = wa_utf8_next(text, size, &offset, &codepoint);
    } while (status > 0);
    return status == 0;
}

wa_font_metric_t *wa_sf_font_metrics(wa_sf_t *sf) {
    return sf ? &sf->fonts[0][0] : NULL;
}

#ifdef __EMSCRIPTEN__
EMSCRIPTEN_KEEPALIVE
#endif
wa_font_advances_t *wa_sf_font_advances(wa_sf_t *sf) {
    return sf ? &sf->advances[0][0] : NULL;
}

uint32_t wa_sf_font_calibration_generation(const wa_sf_t *sf) {
    if (!sf)
        return 0;
    /* The compositor publishes rows in face and weight order. The final row
     * changes only after the new widths for the other faces are available. */
    const wa_font_advances_t *last = &sf->advances[WA_FONT_FACE_COUNT - 1][WA_FONT_WEIGHT_COUNT - 1];
    const uint32_t generation = wa_atomic_load(&last->sequence);
    return generation & 1u ? 0 : generation;
}

_Static_assert(sizeof(wa_font_advances_t) == 404, "font advances ABI");

/* Roboto 3.005 at the default width and upright style. Each row reserves the
 * maximum advance or right ink extent for weights 100–400 or 500–900, plus
 * 1/64 em for kerning, rounded up to hundredths of an em. */
static const uint8_t roboto_widths[2][WA_FONT_ASCII_COUNT] = {
    {
        27, 28, 34, 64, 58, 77, 64, 20, 36, 37, 45, 59, 22, 32, 28, 43,
        58, 58, 58, 58, 58, 58, 58, 58, 58, 58, 26, 23, 53, 58, 54, 49,
        95, 67, 64, 67, 68, 59, 59, 71, 73, 29, 57, 66, 56, 89, 73, 71,
        65, 71, 67, 61, 62, 69, 66, 93, 65, 62, 62, 29, 43, 29, 44, 47,
        33, 56, 58, 54, 58, 55, 37, 58, 57, 26, 26, 53, 26, 92, 57, 59,
        58, 59, 36, 54, 35, 57, 50, 78, 52, 50, 52, 36, 26, 36, 71,
    },
    {
        27, 30, 34, 62, 60, 76, 69, 19, 37, 37, 49, 58, 29, 47, 32, 41,
        60, 60, 60, 60, 60, 60, 60, 60, 60, 60, 32, 31, 53, 61, 54, 53,
        92, 70, 67, 68, 67, 59, 57, 70, 73, 32, 58, 67, 56, 90, 73, 71,
        67, 71, 67, 65, 65, 69, 68, 90, 66, 65, 63, 30, 48, 30, 47, 47,
        36, 56, 58, 54, 58, 57, 38, 60, 59, 30, 29, 58, 30, 89, 59, 59,
        58, 59, 40, 54, 36, 59, 54, 76, 54, 54, 54, 35, 28, 35, 68,
    },
};

static int wide_character(uint32_t cp) {
    return cp >= 0x2e80 || (cp >= 0x1100 && cp <= 0x11ff);
}

static double fallback_width(uint32_t cp, uint32_t face, uint32_t bucket) {
    if (wide_character(cp))
        return 1.06;
    if (face == WA_FONT_MONOSPACE)
        return 0.64;
    if (cp < WA_FONT_ASCII_FIRST + WA_FONT_ASCII_COUNT)
        return roboto_widths[bucket >= 4][cp - WA_FONT_ASCII_FIRST] / 100.0;
    return 0.86;
}

static double text_advance(const char *text, uint32_t len, uint32_t face, uint32_t bucket,
                           const wa_font_advances_t *row, float latin, float wide) {
    uint32_t at = 0, cp;
    double advance = 0;
    while (at < len) {
        if (wa_utf8_next(text, len, &at, &cp) < 0)
            cp = 0xfffd;
        if (cp < WA_FONT_ASCII_FIRST || cp == 0x7f || cp == 0x200d ||
            (cp >= 0x300 && cp <= 0x36f) || (cp >= 0xfe00 && cp <= 0xfe0f) ||
            (cp >= 0x1f3fb && cp <= 0x1f3ff))
            continue;
        if (row) {
            uint32_t q16;
            if (cp < WA_FONT_ASCII_FIRST + WA_FONT_ASCII_COUNT)
                q16 = wa_atomic_load(&row->ascii_q16[cp - WA_FONT_ASCII_FIRST]);
            else if (wide_character(cp))
                q16 = wa_atomic_load(&row->wide_q16);
            else if ((cp >= 0xc0 && cp <= 0xde && cp != 0xd7) ||
                     (cp >= 0x391 && cp <= 0x3ab) || (cp >= 0x410 && cp <= 0x42f))
                q16 = wa_atomic_load(&row->upper_q16);
            else if ((cp >= 0xdf && cp <= 0xff && cp != 0xf7) ||
                     (cp >= 0x3b1 && cp <= 0x3cb) || (cp >= 0x430 && cp <= 0x44f))
                q16 = wa_atomic_load(&row->lower_q16);
            else if ((cp >= 0x660 && cp <= 0x669) || (cp >= 0x6f0 && cp <= 0x6f9))
                q16 = wa_atomic_load(&row->digit_q16);
            else
                q16 = wa_atomic_load(&row->punct_q16);
            advance += q16 ? q16 / 65536.0 : fallback_width(cp, face, bucket);
        } else if (latin > 0) {
            if (wide_character(cp))
                advance += wide;
            else
                advance += cp == ' ' && face != WA_FONT_MONOSPACE ? latin * 0.55 : latin;
        } else {
            advance += fallback_width(cp, face, bucket);
        }
    }
    return advance;
}

wa_text_metrics_t wa_canvas_measure_text(const wa_sf_t *sf, const char *text, uint32_t len,
                                         float px, uint32_t weight, uint32_t face) {
    if (!text || !isfinite(px) || px <= 0 || face >= WA_FONT_FACE_COUNT)
        return (wa_text_metrics_t){0};
    uint32_t bucket = !weight ? 4u : weight >= 900 ? 9u : (weight + 50u) / 100u;
    if (bucket)
        --bucket;
    float latin = 0;
    // The sans-serif faces share Roboto's hhea metrics until calibrated.
    float wide = 1.0f;
    float ascent = face == WA_FONT_MONOSPACE ? 0.80f : 1900.0f / 2048.0f;
    float descent = face == WA_FONT_MONOSPACE ? 0.20f : 500.0f / 2048.0f;
    if (sf) {
        const wa_font_metric_t *m = &sf->fonts[face][bucket];
        uint32_t before = wa_atomic_load(&m->sequence);
        if (before && !(before & 1)) {
            uint32_t l = wa_atomic_load(&m->latin_q16), w = wa_atomic_load(&m->wide_q16);
            uint32_t a = wa_atomic_load(&m->ascent_q16), d = wa_atomic_load(&m->descent_q16);
            if (l && w && a && before == wa_atomic_load(&m->sequence)) {
                latin = l / 65536.f;
                wide = w / 65536.f;
                ascent = a / 65536.f;
                descent = d / 65536.f;
            }
        }
        const wa_font_advances_t *row = &sf->advances[face][bucket];
        before = wa_atomic_load(&row->sequence);
        if (before && !(before & 1)) {
            const double advance = text_advance(text, len, face, bucket, row, 0, 0);
            if (before == wa_atomic_load(&row->sequence)) {
                /* Keep a fraction of a pixel for ink and pixel snapping at
                 * small sizes before rounding outward to the layout grid. */
                return (wa_text_metrics_t){advance > 0 ? ceil(advance * px + 0.5) : 0,
                                           ascent * px, descent * px};
            }
        }
    }
    const double advance = text_advance(text, len, face, bucket, NULL, latin, wide) * px;
    return (wa_text_metrics_t){latin > 0 ? advance : ceil(advance), ascent * px, descent * px};
}
