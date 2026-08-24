#ifndef XG_RENDER_FIELD_POLYLINE_H
#define XG_RENDER_FIELD_POLYLINE_H

#include "xg_render_invalidation_event.h"

#include "cpu_state.h"
#include "guest_render_types.h"
#include "guest_render_native_stream.h"
#include "xg_render_snapshot_types.h"

#include <stdbool.h>
#include <stdint.h>

typedef struct XgRenderFieldPolylineServices {
    bool (*guest_data_range_is_valid)(
        uint32_t address, uint32_t size, uint32_t alignment,
        bool allow_scratchpad);
    bool (*stage_semantic)(
        const GpuRenderSemantic *semantic, uint32_t packet_address,
        uint32_t source_primitive_index, uint32_t interpolation_producer_id,
        uint32_t interpolation_primitive_id);
    void (*register_replay_command)(uint32_t command_id);
    uint64_t (*interpolation_scene)(void);
    bool (*replay_container_matches_command)(
        const GuestRenderNativeStreamMissContext *context);
} XgRenderFieldPolylineServices;

typedef enum XgRenderFieldPolylineObservation {
    XG_RENDER_FIELD_POLYLINE_OBSERVATION_NONE = 0,
    XG_RENDER_FIELD_POLYLINE_OBSERVATION_BEGIN,
    XG_RENDER_FIELD_POLYLINE_OBSERVATION_FINISH,
} XgRenderFieldPolylineObservation;

XgRenderFieldPolylineObservation xg_render_field_polyline_observe(
    CPUState *cpu, uint32_t pc, uint32_t instruction_word,
    GuestRenderRenderMode render_mode,
    const XgRenderFieldPolylineServices *services);
bool xg_render_field_polyline_resolve(
    const GuestRenderNativeStreamMissContext *context,
    GpuRenderSemantic *out_semantic, GuestRenderRenderMode render_mode,
    const XgRenderFieldPolylineServices *services);
void xg_render_field_polyline_clear_pending(void);
void xg_render_field_polyline_clear_templates(void);
void xg_render_field_polyline_block(uint32_t blocker);
void xg_render_field_polyline_scene_boundary(void);
void xg_render_field_polyline_invalidate_authority(void);
void xg_render_field_polyline_reset(void);
void xg_render_field_polyline_handle_invalidation(
    const XgRenderInvalidationEvent *event,
    const XgRenderInvalidationServices *services);
void xg_render_field_polyline_snapshot(
    PsxXgRenderFieldPolylineSnapshot *out_snapshot);

#endif
