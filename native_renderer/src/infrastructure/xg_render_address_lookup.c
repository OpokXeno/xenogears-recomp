#include "xg_render_address_lookup.h"

#include <limits.h>
#include <string.h>

bool xg_render_lookup_key(uint32_t address, uint32_t *out_key) {
    const uint32_t physical = address & UINT32_C(0x1fffffff);

    if (out_key == NULL || physical >= UINT32_C(0x200000) ||
        (physical & 3u) != 0u)
        return false;
    *out_key = physical >> 2u;
    return true;
}

uint32_t xg_render_lookup_find(
        const XgRenderAddressLookupSlot *lookup, uint16_t epoch,
        uint32_t address, uint32_t count) {
    uint32_t key;

    if (lookup == NULL || !xg_render_lookup_key(address, &key))
        return UINT32_MAX;
    if (lookup[key].epoch != epoch || lookup[key].index >= count)
        return UINT32_MAX;
    return lookup[key].index;
}

void xg_render_lookup_put(
        XgRenderAddressLookupSlot *lookup, uint16_t epoch,
        uint32_t address, uint32_t index) {
    uint32_t key;

    if (lookup == NULL || index > UINT16_MAX ||
        !xg_render_lookup_key(address, &key))
        return;
    lookup[key].index = (uint16_t)index;
    lookup[key].epoch = epoch;
}

void xg_render_lookup_remove(
        XgRenderAddressLookupSlot *lookup, uint16_t epoch,
        uint32_t address, uint32_t index) {
    uint32_t key;

    if (lookup == NULL || index > UINT16_MAX ||
        !xg_render_lookup_key(address, &key))
        return;
    if (lookup[key].epoch == epoch && lookup[key].index == index)
        lookup[key].epoch = 0u;
}

void xg_render_lookup_reset(
        XgRenderAddressLookupSlot *lookup, uint16_t *epoch) {
    if (lookup == NULL || epoch == NULL) return;
    if (*epoch == UINT16_MAX) {
        memset(lookup, 0, XG_RENDER_LOOKUP_WORD_CAPACITY * sizeof(*lookup));
        *epoch = 1u;
    } else {
        ++*epoch;
    }
}
