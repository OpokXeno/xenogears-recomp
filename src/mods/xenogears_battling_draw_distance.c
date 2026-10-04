#include "mod_plugins.h"
#include "xg_render_battling_draw_distance.h"

#include <stdint.h>

/* Per frame buffer: the far terrain's four mesh markers and a pair of
 * POLY_FT4 for each of the 256 ring angles. */
enum { XG_BATTLING_DRAW_DISTANCE_ARENA = 2 * 4 * 0x20 + 2 * 256 * 2 * 0x28 };

static void xg_battling_draw_distance_activate(void) {
    const uint32_t arena = psx_mod_alloc_gpu_dma_memory(
        XG_BATTLING_DRAW_DISTANCE_ARENA, 32u);
    xg_render_battling_set_draw_distance(arena, XG_BATTLING_DRAW_DISTANCE_ARENA);
}

PSX_MOD_CONSTRUCTOR(xg_register_battling_draw_distance) {
    (void)psx_mod_register_activation_plugin(
        "xenogears.battling-draw-distance", xg_battling_draw_distance_activate);
}
