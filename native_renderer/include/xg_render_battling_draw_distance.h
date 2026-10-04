#ifndef XG_RENDER_BATTLING_DRAW_DISTANCE_H
#define XG_RENDER_BATTLING_DRAW_DISTANCE_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Battling draw distance (mod xenogears.battling-draw-distance). arena is a
 * 32-byte aligned GPU-DMA aperture allocation of at least
 * 2 * 4 * 0x20 + 2 * 256 * 2 * 0x28 bytes within the aperture's first 4 MiB
 * (mesh markers and ring packets); the Battling producers then add every
 * arena cell and ring segment in view that the guest leaves out. A zero
 * arena disables it. */
void xg_render_battling_set_draw_distance(uint32_t arena, uint32_t size);

#ifdef __cplusplus
}
#endif

#endif
