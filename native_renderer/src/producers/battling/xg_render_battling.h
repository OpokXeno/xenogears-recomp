#ifndef XG_RENDER_BATTLING_H
#define XG_RENDER_BATTLING_H

#include <stdbool.h>
#include <stdint.h>

#include "gpu_render.h"

typedef struct CPUState CPUState;
typedef struct XgRenderProducerLifecycleServices XgRenderProducerLifecycleServices;

/* Battling (overlay tag 7 at 0x8006faf0) is its own game mode and overlay,
 * not Battle. Its producers:
 *  - Gear parts through the resident model kernel (BattlingRenderModelPacket);
 *  - the arena heightfield (BattlingRenderVisibleTerrainStrips);
 *  - the perimeter wall ring and the actor ground shadows, bound at the
 *    resident AddPrim each links its quads with;
 *  - every GTE-projected effect, particle, trail and ground-projected model
 *    shadow, bound from the GTE projection tap at GPU acceptance.
 * The first three publish rigid poses and interpolate; tapped effects are
 * placed natively but hold between guest frames. */

/* Source authority only: the Battling SubmitModelPacket call site. */
bool xg_render_battling_model_authorizes_call(uint32_t call_pc);
/* SubmitModelPacket entry from that call site. */
bool xg_render_battling_model_capture(
    const CPUState *cpu, const XgRenderProducerLifecycleServices *lifecycle);
/* BattlingRenderVisibleTerrainStrips entry. Once per frame: also opens the
 * frame's effect projection tap. */
bool xg_render_battling_terrain_capture(
    const CPUState *cpu, const XgRenderProducerLifecycleServices *lifecycle);
/* Resident AddPrim entry: binds a perimeter ring or ground shadow quad.
 * Returns false (nothing staged) for any other caller. */
bool xg_render_battling_add_prim_capture(
    const CPUState *cpu, const XgRenderProducerLifecycleServices *lifecycle);
/* GPU acceptance of a draw no producer bound, while Battling is resident:
 * places tapped effects natively. Leaves *semantic unchanged otherwise. */
bool xg_render_battling_bind_projected(GpuRenderSemantic *semantic);
/* Packet or command address inside the status HUD (portraits, names,
 * gauges, health bars and the frame lines joining them). */
bool xg_render_battling_hud_packet(uint32_t address);
/* GPU acceptance of a draw-distance mesh marker (an aperture packet the far
 * terrain linked): appends its MESH operation. False for any other draw. */
bool xg_render_battling_accept_mesh_marker(const GpuRenderSemantic *packet,
                                           uint64_t guest_cycle);
/* A walk packet whose near face the draw distance replaced with its clipped
 * triangles: accepted without drawing. */
bool xg_render_battling_near_suppressed(uint64_t command_id);
/* Any other resident module: closes the projection tap. */
void xg_render_battling_leave(void);

#endif
