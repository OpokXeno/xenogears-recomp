#ifndef XG_WORLD_TERRAIN_WATER_SHADOW_H
#define XG_WORLD_TERRAIN_WATER_SHADOW_H

#include "cpu_state.h"
#include "gpu.h"
#include "xg_world_terrain_water.h"
#include "xg_world_terrain_water_snapshot_types.h"

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define XG_WORLD_TERRAIN_WATER_SHADOW_BEGIN_PC UINT32_C(0x8009932c)
#define XG_WORLD_TERRAIN_WATER_SHADOW_FINISH_PC UINT32_C(0x800996d4)
#define XG_WORLD_TERRAIN_WATER_SHADOW_CONTEXT_GLOBAL UINT32_C(0x8009be3c)
#define XG_WORLD_TERRAIN_WATER_SHADOW_POSITION_GLOBAL UINT32_C(0x8009be28)
#define XG_WORLD_TERRAIN_WATER_SHADOW_FINAL_COUNT_GLOBAL UINT32_C(0x8009d7dc)

enum {
    XG_WORLD_TERRAIN_WATER_SHADOW_OT_BUCKET_COUNT = 0xf0,
    XG_WORLD_TERRAIN_WATER_SHADOW_PACKET_STRIDE = 0x20,
    XG_WORLD_TERRAIN_WATER_SHADOW_STACK_FRAME_SIZE = 0x38,
};

typedef enum XgWorldTerrainWaterShadowResult {
    XG_WORLD_TERRAIN_WATER_SHADOW_OK = 0,
    XG_WORLD_TERRAIN_WATER_SHADOW_INVALID_ARGUMENT,
    XG_WORLD_TERRAIN_WATER_SHADOW_INVALID_STATE,
    XG_WORLD_TERRAIN_WATER_SHADOW_CAPTURE_FAILED,
    XG_WORLD_TERRAIN_WATER_SHADOW_BUILD_FAILED,
    XG_WORLD_TERRAIN_WATER_SHADOW_BLOCKED,
} XgWorldTerrainWaterShadowResult;

void xg_world_terrain_water_shadow_reset(void);
XgWorldTerrainWaterShadowResult xg_world_terrain_water_shadow_begin(
    CPUState *cpu, uint64_t authentication_generation,
    const GpuDrawState *draw_state, int32_t screen_x_cull_margin);
XgWorldTerrainWaterShadowResult xg_world_terrain_water_shadow_finish(
    CPUState *cpu);
XgWorldTerrainWaterShadowResult
xg_world_terrain_water_shadow_lifecycle_invalidate(void);
XgWorldTerrainWaterShadowResult
xg_world_terrain_water_shadow_block_pending(void);
bool xg_world_terrain_water_shadow_record_native_cutover(
    uint32_t primitive_count);
XgWorldTerrainWaterShadowResult xg_world_terrain_water_shadow_snapshot(
    XgWorldTerrainWaterShadowSnapshot *out_snapshot);

#ifdef __cplusplus
}
#endif

#endif
