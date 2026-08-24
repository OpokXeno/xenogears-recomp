#ifndef XG_RENDER_WORLD_SKY_PRODUCER_H
#define XG_RENDER_WORLD_SKY_PRODUCER_H

#include "xg_render_invalidation_event.h"

#include "cpu_state.h"
#include "gpu_render.h"
#include "xg_render_snapshot_types.h"
#include "xg_render_ir.h"

#include <stdbool.h>
#include <stdint.h>

typedef struct XgRenderWorldSkyServices {
    bool (*authorize_native_dispatch)(void);
    bool (*authorize_guest_word)(uint32_t address);
    bool (*stage_native)(
        const XgRenderIrNativePrimitive *primitive, uint32_t packet_address,
        uint32_t source_primitive_index, uint32_t interpolation_producer_id,
        uint32_t interpolation_primitive_id);
    bool (*stage_temporal)(
        const XgRenderIrNativePrimitive *primitive,
        uint32_t interpolation_producer_id,
        uint32_t interpolation_primitive_id,
        const GpuRenderTemporalCullPolicy *policy);
    void (*abort_submission)(void);
} XgRenderWorldSkyServices;

bool xg_render_world_sky_cutover(
    CPUState *cpu, const XgRenderWorldSkyServices *services);
void xg_render_world_sky_store_matrix(
    CPUState *cpu, uint32_t address, const XgHost3dMatrix *matrix);
void xg_render_world_sky_snapshot(
    PsxXgRenderWorldNativeSnapshot *out_snapshot);
bool xg_render_world_sky_code_write_overlaps(uint32_t address, uint32_t size);
void xg_render_world_sky_register_code_watches(
    void (*set_range)(uint32_t physical_address, uint32_t size));
void xg_render_world_sky_reset(void);
void xg_render_world_sky_classify_code_write(
    uint32_t address, uint32_t size,
    XgRenderMutationClassification *out_classification);
void xg_render_world_sky_handle_invalidation(
    const XgRenderInvalidationEvent *event,
    const XgRenderInvalidationServices *services);

#endif
