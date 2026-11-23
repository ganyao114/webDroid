#include "internal.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

_Static_assert(sizeof(float) == 4, "ABI float");
_Static_assert(sizeof(wa_sf_layer_desc_t) == 88, "layer descriptor ABI");
_Static_assert(offsetof(wa_sf_layer_desc_t, flags) == 76, "layer flags ABI");
_Static_assert(sizeof(wa_sf_frame_packet_t) == 48, "frame header ABI");
_Static_assert(sizeof(wa_sf_frame_layer_t) == 104, "frame layer ABI");
_Static_assert(offsetof(wa_frame_storage_t, data) == 48 + 104 * WA_SF_MAX_LAYERS, "frame layout");
_Static_assert(sizeof(wa_bitmap_view_t) == 24, "bitmap view ABI");

wa_sf_t *wa_sf_create(void) {
    wa_sf_t *sf = calloc(1, sizeof(*sf));
    if (sf)
        sf->published = UINT32_MAX;
    return sf;
}

void wa_sf_destroy(wa_sf_t *sf) {
    if (sf) {
        for (uint32_t i = 0; i < WA_SF_MAX_LAYERS; ++i) {
            free(sf->lists[i][0]);
            free(sf->lists[i][1]);
        }
        for (uint32_t i = 0; i < WA_BITMAP_SLOTS; ++i)
            free(sf->bitmaps.entries[i]);
        free(sf);
    }
}

int wa_sf_find_layer(const wa_layer_state_t *layers, uint32_t id) {
    for (uint32_t i = 0; i < WA_SF_MAX_LAYERS; ++i)
        if (layers[i].desc.layer_id == id)
            return (int)i;
    return -1;
}

int32_t wa_sf_begin(wa_sf_t *sf, uint32_t width, uint32_t height, uint32_t density) {
    if (!sf || !width || !height || !density)
        return WA_SF_INVALID;
    if (sf->in_transaction)
        return WA_SF_STATE;
    memcpy(sf->staged, sf->committed, sizeof(sf->staged));
    sf->viewport_w = width;
    sf->viewport_h = height;
    sf->density_dpi = density;
    sf->txn_error = WA_SF_OK;
    sf->in_transaction = 1;
    return WA_SF_OK;
}

static int valid_layer(const wa_sf_layer_desc_t *d) {
    const float values[] = {d->x, d->y, d->width, d->height, d->scroll_x, d->scroll_y,
                            d->scale_x, d->scale_y, d->rotation_deg, d->alpha, d->corner_radius,
                            d->clip_x, d->clip_y, d->clip_w, d->clip_h, d->backdrop_blur_px};
    for (size_t i = 0; i < sizeof(values) / sizeof(*values); ++i)
        if (!isfinite(values[i]))
            return 0;
    return d->layer_id && d->layer_id != d->parent_id && d->width >= 0 && d->height >= 0 &&
           d->alpha >= 0 && d->alpha <= 1 && d->corner_radius >= 0 && d->clip_w >= 0 &&
           d->clip_h >= 0 && d->backdrop_blur_px >= 0 && !(d->flags & ~15u) &&
           d->surface_kind >= WA_SF_SURFACE_DISPLAY_LIST &&
           d->surface_kind <= WA_SF_SURFACE_VIDEO_DECODER;
}

int32_t wa_sf_set_layer(wa_sf_t *sf, const wa_sf_layer_desc_t *desc) {
    if (!sf || !desc || !valid_layer(desc))
        return WA_SF_INVALID;
    if (!sf->in_transaction || sf->canvas.data)
        return WA_SF_STATE;
    int slot = wa_sf_find_layer(sf->staged, desc->layer_id);
    if (slot < 0) {
        slot = wa_sf_find_layer(sf->staged, 0);
        if (slot < 0)
            return WA_SF_FULL;
    }
    if (sf->staged[slot].desc.surface_kind != desc->surface_kind)
        sf->staged[slot].size = 0;
    sf->staged[slot].desc = *desc;
    return WA_SF_OK;
}

int32_t wa_sf_remove_layer(wa_sf_t *sf, uint32_t id) {
    if (!sf || !id)
        return WA_SF_INVALID;
    if (!sf->in_transaction || sf->canvas.data)
        return WA_SF_STATE;
    int slot = wa_sf_find_layer(sf->staged, id);
    if (slot >= 0) {
        sf->staged[slot].desc = (wa_sf_layer_desc_t){0};
        sf->staged[slot].size = 0;
    }
    return WA_SF_OK;
}

void wa_sf_abort(wa_sf_t *sf) {
    if (sf) {
        sf->canvas.data = NULL;
        sf->in_transaction = 0;
    }
}

/* Insert into one sibling list per parent, then emit depth-first. A walk
 * that cannot reach every active slot from the root contains a cycle. */
static void order_children(const int *first, const int *next, int parent,
                           uint8_t *order, uint32_t *size) {
    for (int slot = first[parent]; slot >= 0; slot = next[slot]) {
        order[(*size)++] = (uint8_t)slot;
        order_children(first, next, slot, order, size);
    }
}

static int scene_order(const wa_layer_state_t *layers, uint8_t *order) {
    int first[WA_SF_MAX_LAYERS + 1], next[WA_SF_MAX_LAYERS];
    for (uint32_t i = 0; i <= WA_SF_MAX_LAYERS; ++i)
        first[i] = -1;

    uint32_t count = 0;
    for (uint32_t i = 0; i < WA_SF_MAX_LAYERS; ++i) {
        const wa_sf_layer_desc_t *d = &layers[i].desc;
        if (!d->layer_id)
            continue;
        ++count;
        int parent = d->parent_id ? wa_sf_find_layer(layers, d->parent_id) : (int)WA_SF_MAX_LAYERS;
        if (parent < 0)
            return WA_SF_INVALID;
        int *position = &first[parent];
        while (*position >= 0) {
            const wa_sf_layer_desc_t *other = &layers[*position].desc;
            if (other->z_order > d->z_order ||
                (other->z_order == d->z_order && other->layer_id > d->layer_id))
                break;
            position = &next[*position];
        }
        next[i] = *position;
        *position = (int)i;
    }

    uint32_t size = 0;
    order_children(first, next, WA_SF_MAX_LAYERS, order, &size);
    return size == count ? (int)size : WA_SF_INVALID;
}

int32_t wa_sf_commit(wa_sf_t *sf) {
    if (!sf || !sf->in_transaction)
        return WA_SF_STATE;
    if (sf->canvas.data || sf->txn_error) {
        int32_t status = sf->txn_error ? sf->txn_error : WA_SF_STATE;
        wa_sf_abort(sf);
        return status;
    }

    uint8_t order[WA_SF_MAX_LAYERS];
    int count = scene_order(sf->staged, order);
    if (count < 0) {
        wa_sf_abort(sf);
        return count;
    }
    uint32_t published = wa_atomic_load(&sf->published);
    uint32_t index = published == UINT32_MAX ? 0 : published ^ 1u;
    uint32_t unlocked = 0;
    if (!__atomic_compare_exchange_n(&sf->frame_locks[index], &unlocked, WA_EXCLUSIVE, 0,
                                     __ATOMIC_ACQUIRE, __ATOMIC_RELAXED))
        return WA_SF_BUSY;

    wa_frame_storage_t *storage = &sf->frames[index];
    uint32_t offset = offsetof(wa_frame_storage_t, data);
    uint32_t used = 0;
    for (int i = 0; i < count; ++i) {
        uint32_t slot = order[i];
        const wa_layer_state_t *layer = &sf->staged[slot];
        storage->layers[i] = (wa_sf_frame_layer_t){layer->desc, offset + used, layer->size,
                                                  layer->generation, 0};
        if (layer->size) {
            memcpy(storage->data + used, sf->lists[slot][layer->bank], layer->size);
            used += layer->size;
        }
    }
    if (count < (int)WA_SF_MAX_LAYERS)
        memset(storage->layers + count, 0, (WA_SF_MAX_LAYERS - (size_t)count) * sizeof(*storage->layers));
    sf->sequence = wa_next_generation(sf->sequence);
    storage->header = (wa_sf_frame_packet_t){
        WA_SF_FRAME_MAGIC, WA_SF_ABI_VERSION, offset + used, sf->sequence, (uint32_t)count,
        offsetof(wa_frame_storage_t, layers), offset, used, sf->viewport_w, sf->viewport_h,
        sf->density_dpi, 0,
    };
    memcpy(sf->committed, sf->staged, sizeof(sf->committed));
    sf->in_transaction = 0;
    wa_atomic_store(&sf->frame_locks[index], 0);
    wa_atomic_store(&sf->published, index);
    return WA_SF_OK;
}

static int read_lock(uint32_t *lock) {
    uint32_t count = wa_atomic_load(lock);
    if (count >= WA_EXCLUSIVE - 1)
        return 0;
    return __atomic_compare_exchange_n(lock, &count, count + 1, 0, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED);
}

const wa_sf_frame_packet_t *wa_sf_frame_acquire(wa_sf_t *sf) {
    if (!sf)
        return NULL;
    for (uint32_t attempt = 0; attempt < 32; ++attempt) {
        uint32_t index = wa_atomic_load(&sf->published);
        if (index == UINT32_MAX)
            return NULL;
        if (!read_lock(&sf->frame_locks[index]))
            continue;
        if (index == wa_atomic_load(&sf->published))
            return &sf->frames[index].header;
        __atomic_fetch_sub(&sf->frame_locks[index], 1, __ATOMIC_RELEASE);
    }
    return NULL;
}

void wa_sf_frame_release(wa_sf_t *sf, const wa_sf_frame_packet_t *frame) {
    if (!sf)
        return;
    for (uint32_t i = 0; i < 2; ++i)
        if (frame == &sf->frames[i].header)
            __atomic_fetch_sub(&sf->frame_locks[i], 1, __ATOMIC_RELEASE);
}

const wa_sf_frame_layer_t *wa_sf_frame_layers(const wa_sf_frame_packet_t *frame) {
    return frame ? (const wa_sf_frame_layer_t *)((const uint8_t *)frame + frame->layers_offset) : NULL;
}

const uint8_t *wa_sf_frame_list(const wa_sf_frame_packet_t *frame, const wa_sf_frame_layer_t *layer) {
    return frame && layer && layer->list_size ? (const uint8_t *)frame + layer->list_offset : NULL;
}

wa_bitmap_store_t *wa_sf_bitmaps(wa_sf_t *sf) {
    return sf ? &sf->bitmaps : NULL;
}

static int bitmap_index(const wa_bitmap_store_t *store, uint32_t id) {
    for (uint32_t i = 0; i < WA_BITMAP_SLOTS; ++i)
        if ((!store->entries[i] && !id) || (store->entries[i] && store->entries[i]->bitmap_id == id))
            return (int)i;
    return -1;
}

static int bitmap_lock(wa_bitmap_store_t *store) {
    uint32_t unlocked = 0;
    return __atomic_compare_exchange_n(&store->lock, &unlocked, WA_EXCLUSIVE, 0,
                                       __ATOMIC_ACQUIRE, __ATOMIC_RELAXED);
}

static int bitmap_equal(const wa_bitmap_view_t *view, uint32_t width, uint32_t height,
                        uint32_t stride, const uint8_t *src) {
    if (!view || view->width != width || view->height != height)
        return 0;
    const uint8_t *dst = (const uint8_t *)(view + 1);
    for (uint32_t y = 0; y < height; ++y)
        if (memcmp(dst + (size_t)y * view->stride, src + (size_t)y * stride, view->stride))
            return 0;
    return 1;
}

int32_t wa_bitmap_put(wa_bitmap_store_t *store, uint32_t id, uint32_t width, uint32_t height,
                      uint32_t stride, const void *rgba) {
    if (!store || !rgba || !id || !width || !height || width > 4096 || height > 4096 ||
        stride < width * 4 || (uint64_t)(height - 1) * stride + width * 4 > SIZE_MAX)
        return WA_SF_INVALID;
    if (!bitmap_lock(store))
        return WA_SF_BUSY;

    int32_t result = WA_SF_OK;
    int slot = bitmap_index(store, id);
    if (slot < 0)
        slot = bitmap_index(store, 0);
    if (slot < 0) {
        result = WA_SF_FULL;
    } else {
        wa_bitmap_view_t *old = store->entries[slot];
        uint32_t used = store->bytes - (old ? old->byte_size : 0);
        uint32_t size = width * height * 4;
        if (size > WA_BITMAP_BUDGET - used) {
            result = WA_SF_FULL;
        } else if (!bitmap_equal(old, width, height, stride, rgba)) {
            wa_bitmap_view_t *view = malloc(sizeof(*view) + size);
            if (!view) {
                result = WA_SF_NOMEM;
            } else {
                store->generation = wa_next_generation(store->generation);
                *view = (wa_bitmap_view_t){id, width, height, width * 4, store->generation, size};
                uint8_t *dst = (uint8_t *)(view + 1);
                const uint8_t *src = rgba;
                for (uint32_t y = 0; y < height; ++y)
                    memcpy(dst + (size_t)y * view->stride, src + (size_t)y * stride, view->stride);
                store->entries[slot] = view;
                store->bytes = used + size;
                free(old);
            }
        }
    }
    wa_atomic_store(&store->lock, 0);
    return result;
}

int32_t wa_bitmap_remove(wa_bitmap_store_t *store, uint32_t id) {
    if (!store || !id)
        return WA_SF_INVALID;
    if (!bitmap_lock(store))
        return WA_SF_BUSY;
    int slot = bitmap_index(store, id);
    if (slot >= 0) {
        store->bytes -= store->entries[slot]->byte_size;
        free(store->entries[slot]);
        store->entries[slot] = NULL;
    }
    wa_atomic_store(&store->lock, 0);
    return WA_SF_OK;
}

const wa_bitmap_view_t *wa_bitmap_acquire(wa_bitmap_store_t *store, uint32_t id) {
    if (!store || !id || !read_lock(&store->lock))
        return NULL;
    int slot = bitmap_index(store, id);
    if (slot >= 0)
        return store->entries[slot];
    __atomic_fetch_sub(&store->lock, 1, __ATOMIC_RELEASE);
    return NULL;
}

void wa_bitmap_release(wa_bitmap_store_t *store, const wa_bitmap_view_t *view) {
    if (store && view)
        __atomic_fetch_sub(&store->lock, 1, __ATOMIC_RELEASE);
}

const uint8_t *wa_bitmap_pixels(const wa_bitmap_view_t *view) {
    return view ? (const uint8_t *)(view + 1) : NULL;
}
