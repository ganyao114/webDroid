#ifndef WA_SURFACEFLINGER_INTERNAL_H
#define WA_SURFACEFLINGER_INTERNAL_H

#include "wasmandroid/surfaceflinger.h"

#include <stddef.h>

#define WA_FRAME_DATA_CAPACITY (WA_SF_MAX_LAYERS * WA_SF_LAYER_LIST_CAPACITY)
#define WA_BITMAP_SLOTS 128u
#define WA_BITMAP_BUDGET (32u * 1024u * 1024u)
#define WA_EXCLUSIVE (1u << 31)

typedef struct wa_layer_state {
    wa_sf_layer_desc_t desc;
    uint32_t bank, size, generation;
} wa_layer_state_t;

typedef struct wa_frame_storage {
    wa_sf_frame_packet_t header;
    wa_sf_frame_layer_t layers[WA_SF_MAX_LAYERS];
    uint8_t data[WA_FRAME_DATA_CAPACITY];
} wa_frame_storage_t;

struct wa_canvas {
    wa_sf_t *sf;
    uint32_t slot, bank, depth, used;
    int32_t status;
    uint8_t *data;
};

struct wa_bitmap_store {
    uint32_t lock, generation, bytes;
    wa_bitmap_view_t *entries[WA_BITMAP_SLOTS];
};

struct wa_sf {
    uint32_t published, frame_locks[2], sequence;
    uint32_t viewport_w, viewport_h, density_dpi;
    int32_t txn_error;
    uint32_t in_transaction;
    wa_layer_state_t committed[WA_SF_MAX_LAYERS], staged[WA_SF_MAX_LAYERS];
    uint8_t lists[WA_SF_MAX_LAYERS][2][WA_SF_LAYER_LIST_CAPACITY];
    wa_canvas_t canvas;
    wa_frame_storage_t frames[2];
    wa_bitmap_store_t bitmaps;
    wa_input_ring_t input;
    wa_sf_vsync_t vsync;
    wa_font_metric_t fonts[WA_FONT_FACE_COUNT][WA_FONT_WEIGHT_COUNT];
    wa_codec_requests_t codec_requests;
    wa_codec_events_t codec_events;
    wa_webview_requests_t webview_requests;
    wa_webview_events_t webview_events;
};

/* Fixed-width wire fields also have a C99/C++ declaration. The atomic
 * builtins work directly on that storage with Clang and GCC. */
static inline uint32_t wa_atomic_load(const uint32_t *word) {
    return __atomic_load_n(word, __ATOMIC_SEQ_CST);
}

static inline void wa_atomic_store(uint32_t *word, uint32_t value) {
    __atomic_store_n(word, value, __ATOMIC_SEQ_CST);
}

static inline uint32_t wa_next_generation(uint32_t generation) {
    return generation == UINT32_MAX ? 1 : generation + 1;
}

int wa_sf_find_layer(const wa_layer_state_t *layers, uint32_t id);
int wa_utf8_next(const char *text, uint32_t size, uint32_t *offset, uint32_t *codepoint);
int wa_utf8_valid(const char *text, uint32_t size);

#endif
