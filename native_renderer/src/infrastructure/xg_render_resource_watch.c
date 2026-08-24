#include "xg_render_resource_watch.h"

#include "xg_render_address_lookup.h"

#include <string.h>

#define XG_RENDER_RESOURCE_WATCH_BITMAP_WORDS \
    (XG_RENDER_LOOKUP_WORD_CAPACITY / 32u)

static XgRenderResourceWatchCallback resource_watch_callback;
static uint32_t resource_watch_bitmap[XG_RENDER_RESOURCE_WATCH_BITMAP_WORDS];
static uint8_t invalidated_byte_masks[XG_RENDER_LOOKUP_WORD_CAPACITY];
static uint32_t model_ft3_descriptor_bitmap[
    XG_RENDER_RESOURCE_WATCH_BITMAP_WORDS];

static uint8_t byte_mask_for_word(
        uint32_t begin, uint32_t end, uint32_t word) {
    const uint32_t word_begin = word << 2u;
    const uint32_t word_end = word_begin + 4u;
    const uint32_t first = begin > word_begin ? begin : word_begin;
    const uint32_t last = end < word_end ? end : word_end;

    if (first >= last) return 0u;
    const uint32_t low = first - word_begin;
    const uint32_t width = last - first;
    return (uint8_t)(((UINT32_C(1) << width) - 1u) << low);
}

static bool physical_range(
        uint32_t address, uint32_t size,
        uint32_t *out_begin, uint32_t *out_end) {
    const uint32_t physical = address & UINT32_C(0x1fffffff);
    uint32_t end;

    if (size == 0u || physical >= UINT32_C(0x200000)) return false;
    end = physical + size - 1u;
    if (end < physical || end >= UINT32_C(0x200000))
        end = UINT32_C(0x1fffff);
    *out_begin = physical;
    *out_end = end;
    return true;
}

void xg_render_resource_watch_register(XgRenderResourceWatchCallback callback) {
    resource_watch_callback = callback;
}

void xg_render_resource_watch_add(uint32_t address, uint32_t size) {
    uint32_t begin;
    uint32_t end;

    if (!physical_range(address, size, &begin, &end)) return;
    for (uint32_t word = begin >> 2u; word <= (end >> 2u); ++word) {
        const uint32_t bit = UINT32_C(1) << (word & 31u);
        const uint8_t byte_mask = byte_mask_for_word(begin, end + 1u, word);

        resource_watch_bitmap[word >> 5u] |= bit;
        invalidated_byte_masks[word] &= (uint8_t)~byte_mask;
    }
    if (resource_watch_callback != NULL)
        resource_watch_callback(begin, size);
}

void xg_render_resource_watch_add_model_ft3_descriptor(
        uint32_t address, uint32_t size) {
    uint32_t begin;
    uint32_t end;

    if (!physical_range(address, size, &begin, &end)) return;
    for (uint32_t word = begin >> 2u; word <= (end >> 2u); ++word)
        model_ft3_descriptor_bitmap[word >> 5u] |=
            UINT32_C(1) << (word & 31u);
    xg_render_resource_watch_add(address, size);
}

bool xg_render_resource_watch_model_ft3_descriptor_overlaps(
        uint32_t address, uint32_t size) {
    uint32_t begin;
    uint32_t end;

    if (!physical_range(address, size, &begin, &end)) return false;
    for (uint32_t word = begin >> 2u; word <= (end >> 2u); ++word)
        if (model_ft3_descriptor_bitmap[word >> 5u] &
            (UINT32_C(1) << (word & 31u)))
            return true;
    return false;
}

bool xg_render_resource_watch_overlaps(uint32_t address, uint32_t size) {
    uint32_t begin;
    uint32_t end;

    if (!physical_range(address, size, &begin, &end)) return false;
    for (uint32_t word = begin >> 2u; word <= (end >> 2u); ++word)
        if (resource_watch_bitmap[word >> 5u] &
            (UINT32_C(1) << (word & 31u)))
            return true;
    return false;
}

bool xg_render_resource_watch_needs_invalidation(
        uint32_t address, uint32_t size) {
    uint32_t begin;
    uint32_t end;

    if (!physical_range(address, size, &begin, &end)) return false;
    for (uint32_t word = begin >> 2u; word <= (end >> 2u); ++word) {
        const uint32_t bit = UINT32_C(1) << (word & 31u);
        const uint8_t byte_mask = byte_mask_for_word(begin, end + 1u, word);

        if ((resource_watch_bitmap[word >> 5u] & bit) != 0u &&
            (invalidated_byte_masks[word] & byte_mask) != byte_mask)
            return true;
    }
    return false;
}

void xg_render_resource_watch_mark_invalidated(
        uint32_t address, uint32_t size) {
    uint32_t begin;
    uint32_t end;

    if (!physical_range(address, size, &begin, &end)) return;
    for (uint32_t word = begin >> 2u; word <= (end >> 2u); ++word) {
        const uint32_t bit = UINT32_C(1) << (word & 31u);
        const uint8_t byte_mask = byte_mask_for_word(begin, end + 1u, word);

        if (resource_watch_bitmap[word >> 5u] & bit)
            invalidated_byte_masks[word] |= byte_mask;
    }
}

void xg_render_resource_watch_reset(void) {
    memset(resource_watch_bitmap, 0, sizeof(resource_watch_bitmap));
    memset(invalidated_byte_masks, 0, sizeof(invalidated_byte_masks));
}

void xg_render_resource_watch_handle_invalidation(
        const XgRenderInvalidationEvent *event,
        const XgRenderInvalidationServices *services) {
    (void)services;
    if (event->kind == XG_RENDER_INVALIDATION_CODE_WRITE &&
        event->mutation.resource_mutation)
        xg_render_resource_watch_mark_invalidated(event->address, event->size);
    else if (event->kind == XG_RENDER_INVALIDATION_RESET)
        xg_render_resource_watch_reset();
}

void xg_render_resource_watch_reset_model_ft3_descriptors(void) {
    memset(model_ft3_descriptor_bitmap, 0,
           sizeof(model_ft3_descriptor_bitmap));
}
