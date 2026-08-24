#ifndef XG_WORLD_MINIMAP_SHADOW_H
#define XG_WORLD_MINIMAP_SHADOW_H

#include "cpu_state.h"
#include "gpu.h"
#include "xg_world_minimap_snapshot_types.h"

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define XG_WORLD_MINIMAP_SHADOW_BEGIN_PC UINT32_C(0x800740b8)
#define XG_WORLD_MINIMAP_SHADOW_FINISH_PC UINT32_C(0x80074564)
#define XG_WORLD_MINIMAP_SHADOW_CALLER_RETURN UINT32_C(0x80071b84)
#define XG_WORLD_MINIMAP_SHADOW_NO_SOURCE UINT32_MAX

void xg_world_minimap_shadow_reset(void);
XgWorldMinimapShadowResult xg_world_minimap_shadow_begin(
    CPUState *cpu, uint64_t authentication_generation,
    const GpuDrawState *draw_state);
XgWorldMinimapShadowResult xg_world_minimap_shadow_finish(CPUState *cpu);
void xg_world_minimap_shadow_invalidate(void);
void xg_world_minimap_shadow_block(XgWorldMinimapShadowBlocker blocker);
bool xg_world_minimap_shadow_record_native_cutover(uint32_t primitive_count);
void xg_world_minimap_shadow_snapshot(
    XgWorldMinimapShadowSnapshot *out_snapshot);

#ifdef __cplusplus
}
#endif

#endif
