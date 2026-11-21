#include "internal.h"

#include <math.h>
#include <string.h>

_Static_assert(sizeof(wa_sf_ring_header_t) == 16, "ring header ABI");
_Static_assert(sizeof(wa_input_event_t) == 320, "input event ABI");
_Static_assert(offsetof(wa_input_event_t, text) == 56, "IME text ABI");
_Static_assert(sizeof(wa_sf_vsync_t) == 32, "vsync ABI");
_Static_assert(sizeof(wa_font_metric_t) == 20, "font metric ABI");
_Static_assert(offsetof(wa_codec_request_t, data) == 88, "codec request ABI");
_Static_assert(sizeof(wa_codec_request_t) == 88 + WA_CODEC_AU_CAPACITY, "codec request stride");
_Static_assert(sizeof(wa_codec_event_t) == 32, "codec event ABI");
_Static_assert(sizeof(wa_webview_request_t) == 24 + WA_WEBVIEW_TEXT_CAPACITY, "WebView request ABI");
_Static_assert(sizeof(wa_webview_event_t) == 16 + WA_WEBVIEW_TEXT_CAPACITY, "WebView event ABI");

static void *ring_write(wa_sf_ring_header_t *h, void *entries, uint32_t count, size_t stride) {
    uint32_t write = __atomic_load_n(&h->write_index, __ATOMIC_RELAXED);
    uint32_t read = __atomic_load_n(&h->read_index, __ATOMIC_ACQUIRE);
    if (write - read >= count) {
        __atomic_fetch_add(&h->dropped, 1, __ATOMIC_RELAXED);
        return NULL;
    }
    return (uint8_t *)entries + (write & (count - 1)) * stride;
}

static void ring_publish(wa_sf_ring_header_t *h) {
    __atomic_fetch_add(&h->write_index, 1, __ATOMIC_RELEASE);
}

static int32_t ring_poll(wa_sf_ring_header_t *h, const void *entries, uint32_t count,
                         size_t stride, void *out) {
    uint32_t read = __atomic_load_n(&h->read_index, __ATOMIC_RELAXED);
    uint32_t write = __atomic_load_n(&h->write_index, __ATOMIC_ACQUIRE);
    if (read == write)
        return 0;
    memcpy(out, (const uint8_t *)entries + (read & (count - 1)) * stride, stride);
    __atomic_store_n(&h->read_index, read + 1, __ATOMIC_RELEASE);
    return 1;
}

wa_input_ring_t *wa_sf_input(wa_sf_t *sf) {
    return sf ? &sf->input : NULL;
}

int32_t wa_input_push(wa_input_ring_t *r, const wa_input_event_t *e) {
    if (!r || !e || !isfinite(e->event_time_ms) || e->text_len > WA_INPUT_TEXT_CAPACITY)
        return WA_SF_INVALID;
    switch (e->kind) {
    case WA_INPUT_MOTION:
        if ((e->action > WA_ACTION_CANCEL && e->action != WA_ACTION_SCROLL) ||
            !isfinite(e->x) || !isfinite(e->y) || !isfinite(e->dx) || !isfinite(e->dy) ||
            !isfinite(e->velocity_x) || !isfinite(e->velocity_y))
            return WA_SF_INVALID;
        break;
    case WA_INPUT_KEY:
        if (e->action > WA_ACTION_UP)
            return WA_SF_INVALID;
        break;
    case WA_INPUT_IME:
        if ((e->action != WA_IME_COMPOSING && e->action != WA_IME_COMMIT) ||
            (e->flags & ~(WA_IME_MORE | WA_IME_CONTINUATION)) || !wa_utf8_valid(e->text, e->text_len))
            return WA_SF_INVALID;
        break;
    default:
        return WA_SF_INVALID;
    }
    wa_input_event_t *slot = ring_write(&r->header, r->events, WA_INPUT_CAPACITY, sizeof(*slot));
    if (!slot)
        return WA_SF_FULL;
    *slot = *e;
    slot->reserved = 0;
    ring_publish(&r->header);
    return WA_SF_OK;
}

int32_t wa_input_poll(wa_input_ring_t *r, wa_input_event_t *e) {
    return r && e ? ring_poll(&r->header, r->events, WA_INPUT_CAPACITY, sizeof(*e), e)
                  : WA_SF_INVALID;
}

wa_sf_vsync_t *wa_sf_vsync(wa_sf_t *sf) {
    return sf ? &sf->vsync : NULL;
}

void wa_sf_vsync_write(wa_sf_vsync_t *v, const wa_sf_vsync_state_t *s) {
    if (!v || !s)
        return;
    uint32_t sequence = wa_atomic_load(&v->sequence);
    wa_atomic_store(&v->sequence, sequence | 1u);
    wa_atomic_store(&v->frame_number, s->frame_number);
    wa_atomic_store(&v->time_ns_lo, (uint32_t)s->frame_time_ns);
    wa_atomic_store(&v->time_ns_hi, (uint32_t)(s->frame_time_ns >> 32));
    wa_atomic_store(&v->viewport_w, s->viewport_w);
    wa_atomic_store(&v->viewport_h, s->viewport_h);
    wa_atomic_store(&v->density_dpi, s->density_dpi);
    wa_atomic_store(&v->sequence, (sequence + 2u) & ~1u);
}

int32_t wa_sf_vsync_read(const wa_sf_vsync_t *v, wa_sf_vsync_state_t *s) {
    if (!v || !s)
        return WA_SF_INVALID;
    for (uint32_t attempt = 0; attempt < 8; ++attempt) {
        uint32_t before = wa_atomic_load(&v->sequence);
        if (before & 1u)
            continue;
        uint64_t lo = wa_atomic_load(&v->time_ns_lo);
        uint64_t hi = wa_atomic_load(&v->time_ns_hi);
        wa_sf_vsync_state_t tmp = {
            lo | hi << 32, wa_atomic_load(&v->frame_number), wa_atomic_load(&v->viewport_w),
            wa_atomic_load(&v->viewport_h), wa_atomic_load(&v->density_dpi),
        };
        if (before == wa_atomic_load(&v->sequence)) {
            *s = tmp;
            return WA_SF_OK;
        }
    }
    return WA_SF_BUSY;
}

wa_codec_requests_t *wa_sf_codec_requests(wa_sf_t *sf) {
    return sf ? &sf->codec_requests : NULL;
}
wa_codec_events_t *wa_sf_codec_events(wa_sf_t *sf) {
    return sf ? &sf->codec_events : NULL;
}

static wa_codec_request_t *codec_slot(wa_sf_t *sf) {
    wa_codec_requests_t *r = &sf->codec_requests;
    wa_codec_request_t *slot = ring_write(&r->header, r->entries, WA_CODEC_REQUEST_CAPACITY,
                                         sizeof(*slot));
    if (slot)
        memset(slot, 0, offsetof(wa_codec_request_t, data));
    return slot;
}

int32_t wa_codec_configure(wa_sf_t *sf, uint32_t id, const wa_codec_desc_t *desc,
                           const void *description, uint32_t size) {
    if (!sf || !id || !desc || desc->codec < WA_CODEC_H264 || desc->codec > WA_CODEC_AV1 ||
        !desc->width || !desc->height || size > WA_CODEC_AU_CAPACITY || (size && !description) ||
        (desc->flags & ~WA_CODEC_AVCC) ||
        (desc->codec == WA_CODEC_H264 && !!(desc->flags & WA_CODEC_AVCC) != !!size) ||
        (desc->codec != WA_CODEC_H264 && (desc->flags & WA_CODEC_AVCC)) ||
        !memchr(desc->codec_string, 0, WA_CODEC_NAME_CAPACITY))
        return WA_SF_INVALID;
    wa_codec_request_t *slot = codec_slot(sf);
    if (!slot)
        return WA_SF_FULL;
    slot->action = WA_CODEC_CONFIGURE;
    slot->decoder_id = id;
    slot->codec = desc->codec;
    slot->flags = desc->flags;
    slot->width = desc->width;
    slot->height = desc->height;
    slot->byte_size = size;
    memcpy(slot->codec_string, desc->codec_string, sizeof(slot->codec_string));
    if (size)
        memcpy(slot->data, description, size);
    ring_publish(&sf->codec_requests.header);
    return WA_SF_OK;
}

int32_t wa_codec_submit(wa_sf_t *sf, uint32_t id, const void *au, uint32_t size,
                        int64_t timestamp_us, uint32_t duration_us, uint32_t flags) {
    if (!sf || !id || !au || !size || size > WA_CODEC_AU_CAPACITY || (flags & ~WA_CODEC_KEY_FRAME))
        return WA_SF_INVALID;
    wa_codec_request_t *slot = codec_slot(sf);
    if (!slot)
        return WA_SF_FULL;
    slot->action = WA_CODEC_ACCESS_UNIT;
    slot->decoder_id = id;
    slot->flags = flags;
    slot->byte_size = size;
    slot->duration_us = duration_us;
    slot->timestamp_us = timestamp_us;
    memcpy(slot->data, au, size);
    ring_publish(&sf->codec_requests.header);
    return WA_SF_OK;
}

int32_t wa_codec_control(wa_sf_t *sf, uint32_t id, uint32_t action) {
    if (!sf || !id || (action != WA_CODEC_FLUSH && action != WA_CODEC_CLOSE))
        return WA_SF_INVALID;
    wa_codec_request_t *slot = codec_slot(sf);
    if (!slot)
        return WA_SF_FULL;
    slot->action = action;
    slot->decoder_id = id;
    ring_publish(&sf->codec_requests.header);
    return WA_SF_OK;
}

int32_t wa_codec_poll(wa_sf_t *sf, wa_codec_event_t *event) {
    if (!sf || !event)
        return WA_SF_INVALID;
    wa_codec_events_t *r = &sf->codec_events;
    return ring_poll(&r->header, r->entries, WA_CODEC_EVENT_CAPACITY, sizeof(*event), event);
}

wa_webview_requests_t *wa_sf_webview_requests(wa_sf_t *sf) {
    return sf ? &sf->webview_requests : NULL;
}
wa_webview_events_t *wa_sf_webview_events(wa_sf_t *sf) {
    return sf ? &sf->webview_events : NULL;
}

static int32_t webview_submit(wa_sf_t *sf, uint32_t action, uint32_t id, uint32_t layer,
                              uint32_t request, const char *text, uint32_t len) {
    if (!sf || !id || len > WA_WEBVIEW_TEXT_CAPACITY || !wa_utf8_valid(text, len))
        return WA_SF_INVALID;
    wa_webview_requests_t *r = &sf->webview_requests;
    wa_webview_request_t *slot = ring_write(&r->header, r->entries, WA_WEBVIEW_CAPACITY, sizeof(*slot));
    if (!slot)
        return WA_SF_FULL;
    slot->action = action;
    slot->webview_id = id;
    slot->layer_id = layer;
    slot->request_id = request;
    slot->text_len = len;
    slot->reserved = 0;
    if (len)
        memcpy(slot->text, text, len);
    ring_publish(&r->header);
    return WA_SF_OK;
}

int32_t wa_webview_open(wa_sf_t *sf, const wa_webview_desc_t *desc) {
    if (!desc || !desc->layer_id || !desc->url || !desc->url_len ||
        desc->url_len > WA_WEBVIEW_TEXT_CAPACITY || memchr(desc->url, 0, desc->url_len))
        return WA_SF_INVALID;
    return webview_submit(sf, WA_WEBVIEW_OPEN, desc->webview_id, desc->layer_id,
                          0, desc->url, desc->url_len);
}

int32_t wa_webview_close(wa_sf_t *sf, uint32_t id) {
    return webview_submit(sf, WA_WEBVIEW_CLOSE, id, 0, 0, NULL, 0);
}

int32_t wa_webview_eval(wa_sf_t *sf, uint32_t id, uint32_t request, const char *script, uint32_t len) {
    return request && len ? webview_submit(sf, WA_WEBVIEW_EVAL, id, 0, request, script, len)
                          : WA_SF_INVALID;
}

int32_t wa_webview_reply(wa_sf_t *sf, uint32_t id, uint32_t request, const char *json, uint32_t len) {
    return request && len ? webview_submit(sf, WA_WEBVIEW_REPLY, id, 0, request, json, len)
                          : WA_SF_INVALID;
}

int32_t wa_webview_poll(wa_sf_t *sf, wa_webview_event_t *event) {
    if (!sf || !event)
        return WA_SF_INVALID;
    wa_webview_events_t *r = &sf->webview_events;
    return ring_poll(&r->header, r->entries, WA_WEBVIEW_CAPACITY, sizeof(*event), event);
}
