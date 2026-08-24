#ifndef XG_RENDER_ADDRESS_LOOKUP_H
#define XG_RENDER_ADDRESS_LOOKUP_H

#include <stdbool.h>
#include <stdint.h>

/* Direct lookup for aligned words in the PSX's 2 MiB RAM aperture. */
#define XG_RENDER_LOOKUP_WORD_CAPACITY (UINT32_C(0x200000) / 4u)

typedef struct XgRenderAddressLookupSlot {
    uint16_t index;
    uint16_t epoch;
} XgRenderAddressLookupSlot;

bool xg_render_lookup_key(uint32_t address, uint32_t *out_key);
uint32_t xg_render_lookup_find(
    const XgRenderAddressLookupSlot *lookup, uint16_t epoch,
    uint32_t address, uint32_t count);
void xg_render_lookup_put(
    XgRenderAddressLookupSlot *lookup, uint16_t epoch,
    uint32_t address, uint32_t index);
void xg_render_lookup_remove(
    XgRenderAddressLookupSlot *lookup, uint16_t epoch,
    uint32_t address, uint32_t index);
void xg_render_lookup_reset(
    XgRenderAddressLookupSlot *lookup, uint16_t *epoch);

#endif
