#ifndef XG_RENDER_RESIDENT_LINE_F2_H
#define XG_RENDER_RESIDENT_LINE_F2_H

#include "xg_render_invalidation_event.h"

#include "cpu_state.h"
#include "guest_render_types.h"
#include "gpu_render.h"

#include <stdbool.h>
#include <stdint.h>

typedef struct XgRenderResidentLineF2Services {
    bool (*word_address_is_valid)(uint32_t address);
    bool (*guest_data_range_is_valid)(
        uint32_t address, uint32_t size, uint32_t alignment,
        bool allow_scratchpad);
    bool (*stage_semantic)(
        const GpuRenderSemantic *semantic, uint32_t packet_address,
        uint32_t source_primitive_index, uint32_t interpolation_producer_id,
        uint32_t interpolation_primitive_id);
    void (*abort_submission)(void);
} XgRenderResidentLineF2Services;

typedef enum XgRenderResidentLineF2Observation {
    XG_RENDER_RESIDENT_LINE_F2_OBSERVATION_NONE = 0,
    XG_RENDER_RESIDENT_LINE_F2_OBSERVATION_CAPTURE,
    XG_RENDER_RESIDENT_LINE_F2_OBSERVATION_STAGE,
} XgRenderResidentLineF2Observation;

void xg_render_resident_line_f2_clear(void);
void xg_render_resident_line_f2_handle_invalidation(
    const XgRenderInvalidationEvent *event,
    const XgRenderInvalidationServices *services);
bool xg_render_resident_line_f2_capture(
    CPUState *cpu, GuestRenderRenderMode render_mode,
    const XgRenderResidentLineF2Services *services);
bool xg_render_resident_line_f2_stage(
    CPUState *cpu, GuestRenderRenderMode render_mode,
    const XgRenderResidentLineF2Services *services);
XgRenderResidentLineF2Observation xg_render_resident_line_f2_observe(
    CPUState *cpu, uint32_t pc, uint32_t instruction_word,
    GuestRenderRenderMode render_mode,
    const XgRenderResidentLineF2Services *services);

#endif
