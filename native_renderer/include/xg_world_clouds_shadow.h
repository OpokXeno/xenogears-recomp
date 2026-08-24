#ifndef XG_WORLD_CLOUDS_SHADOW_H
#define XG_WORLD_CLOUDS_SHADOW_H

#include "cpu_state.h"
#include "gpu.h"
#include "xg_world_clouds.h"
#include "xg_world_clouds_snapshot_types.h"
#include "xg_world_clouds_source_capture.h"

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define XG_WORLD_CLOUDS_SHADOW_BEGIN_PC UINT32_C(0x80086798)
#define XG_WORLD_CLOUDS_SHADOW_FINISH_PC UINT32_C(0x800876dc)
#define XG_WORLD_CLOUDS_SHADOW_FULL_RETURN UINT32_C(0x80071b68)
#define XG_WORLD_CLOUDS_SHADOW_CALLBACK_POINTER UINT32_C(0x8009cd40)
#define XG_WORLD_CLOUDS_SHADOW_CALLBACK_PHYSICAL UINT32_C(0x00086700)

void xg_world_clouds_shadow_reset(void);
XgWorldCloudsShadowResult xg_world_clouds_shadow_begin(
    CPUState *cpu, uint64_t generation, GpuDrawState *draw_state,
    int32_t screen_x_cull_margin);
void xg_world_clouds_shadow_observe_anchor(CPUState *cpu);
XgWorldCloudsShadowResult xg_world_clouds_shadow_finish(CPUState *cpu);
void xg_world_clouds_shadow_invalidate(void);
bool xg_world_clouds_shadow_record_native_cutover(uint32_t primitive_count);
void xg_world_clouds_shadow_snapshot(
    XgWorldCloudsShadowSnapshot *out_snapshot);

#ifdef __cplusplus
}
#endif

#endif
