#ifndef XG_RENDER_OVERLAY_FT4_H
#define XG_RENDER_OVERLAY_FT4_H

#include "xg_render_invalidation_event.h"

#include "cpu_state.h"
#include "guest_render_types.h"
#include "xg_render_snapshot_types.h"
#include "xg_render_ir.h"
#include "xg_render_producer_lifecycle.h"

#include <stdbool.h>
#include <stdint.h>

typedef struct XgRenderOverlayFt4Template {
    XgRenderIrNativePrimitive primitive;
    XgRenderProducerLifecycle lifecycle;
    XgRenderIrMaterialState material;
    uint32_t packet_address;
    uint32_t source_primitive_index;
    uint32_t interpolation_producer_id;
    uint32_t interpolation_primitive_id;
    uint8_t family;
    bool interpolation_identity_valid;
    bool material_ready;
    bool valid;
} XgRenderOverlayFt4Template;

typedef enum XgRenderOverlayFt4Observation {
    XG_RENDER_OVERLAY_FT4_OBSERVATION_NONE = 0,
    XG_RENDER_OVERLAY_FT4_OBSERVATION_HANDLED,
    XG_RENDER_OVERLAY_FT4_OBSERVATION_RESIDUAL_STATIC_GOURAUD,
    XG_RENDER_OVERLAY_FT4_OBSERVATION_RESIDUAL_PROJECTED_GOURAUD,
    XG_RENDER_OVERLAY_FT4_OBSERVATION_RESIDUAL_PROJECTED_GOURAUD_2E,
} XgRenderOverlayFt4Observation;

typedef struct XgRenderFieldSpriteOverlayPublication
    XgRenderFieldSpriteOverlayPublication;

typedef struct XgRenderOverlayFt4Services {
    const XgRenderProducerLifecycleServices *lifecycle;
    uint64_t (*local_producer_generation)(uint32_t producer_pc);
    bool (*guest_data_range_is_valid)(
        uint32_t address, uint32_t size, uint32_t alignment,
        bool allow_scratchpad);
    bool (*stage_primitive)(
        const XgRenderIrNativePrimitive *primitive, uint32_t packet_address,
        uint32_t source_primitive_index, uint32_t interpolation_producer_id,
        uint32_t interpolation_primitive_id);
    void (*invalidate_field_sprite_template)(uint32_t packet_address);
    void (*watch_resource)(uint32_t address, uint32_t size);
} XgRenderOverlayFt4Services;

bool xg_render_overlay_ft4_lookup(
    uint32_t packet_address, XgRenderOverlayFt4Template *out_template);
bool xg_render_overlay_ft4_publish_field_sprite(
    const XgRenderFieldSpriteOverlayPublication *publication,
    const XgRenderOverlayFt4Services *services, uint32_t *failure_detail);
bool xg_render_overlay_ft4_capture_initialized_packet(
    CPUState *cpu, uint32_t packet_address, uint32_t producer_pc,
    uint8_t expected_opcode, const XgRenderOverlayFt4Services *services);
bool xg_render_overlay_ft4_local_producer_begin(
    CPUState *cpu, GuestRenderRenderMode render_mode, uint64_t generation,
    uint8_t expected_opcode, const XgRenderOverlayFt4Services *services);
bool xg_render_overlay_ft4_local_producer_preflight(
    CPUState *cpu, GuestRenderRenderMode render_mode, uint8_t expected_opcode,
    const XgRenderOverlayFt4Services *services);
bool xg_render_overlay_ft4_local_producer_writer(
    CPUState *cpu, uint32_t writer_index, uint8_t expected_opcode);
bool xg_render_overlay_ft4_local_producer_commit(
    CPUState *cpu, uint64_t generation, uint32_t producer_pc,
    uint8_t expected_opcode, const XgRenderOverlayFt4Services *services);
void xg_render_overlay_ft4_local_producer_cancel(void);
bool xg_render_overlay_ft4_capture_direct_templates(
    CPUState *cpu, uint32_t producer_pc, GuestRenderRenderMode render_mode,
    const XgRenderOverlayFt4Services *services);
bool xg_render_overlay_ft4_capture_rectangle_template(
    CPUState *cpu, uint32_t producer_pc, GuestRenderRenderMode render_mode,
    const XgRenderOverlayFt4Services *services);
bool xg_render_overlay_ft4_capture_projected_material(
    CPUState *cpu, uint32_t producer_pc, GuestRenderRenderMode render_mode,
    const XgRenderOverlayFt4Services *services);
bool xg_render_overlay_ft4_capture_glyph_material(
    CPUState *cpu, uint32_t producer_pc, GuestRenderRenderMode render_mode,
    const XgRenderOverlayFt4Services *services);
bool xg_render_overlay_ft4_capture_projected_2e_material(
    CPUState *cpu, uint32_t template_family, uint32_t producer_pc,
    GuestRenderRenderMode render_mode,
    const XgRenderOverlayFt4Services *services);
bool xg_render_overlay_ft4_capture_projected_geometry(
    CPUState *cpu, GuestRenderRenderMode render_mode,
    const XgRenderOverlayFt4Services *services);
bool xg_render_overlay_ft4_capture_projected_2e_geometry(
    CPUState *cpu, GuestRenderRenderMode render_mode,
    const XgRenderOverlayFt4Services *services);
bool xg_render_overlay_ft4_observe_add_prim(
    CPUState *cpu, uint32_t pc, uint32_t instruction_word,
    GuestRenderRenderMode render_mode,
    const XgRenderOverlayFt4Services *services);
XgRenderOverlayFt4Observation xg_render_overlay_ft4_observe(
    CPUState *cpu, uint32_t pc, uint32_t instruction_word,
    GuestRenderRenderMode render_mode,
    const XgRenderOverlayFt4Services *services);
void xg_render_overlay_ft4_finish_observation(
    XgRenderOverlayFt4Observation observation, CPUState *cpu,
    GuestRenderRenderMode render_mode,
    const XgRenderOverlayFt4Services *services);
bool xg_render_overlay_ft4_observe_field_material(
    CPUState *cpu, uint32_t pc, uint32_t instruction_word,
    GuestRenderRenderMode render_mode,
    const XgRenderOverlayFt4Services *services);
bool xg_render_overlay_ft4_projected_2e_descriptor_scope(void);
void xg_render_overlay_ft4_scene_boundary(void);
void xg_render_overlay_ft4_snapshot(
    PsxXgRenderOverlayFt4Snapshot *out_snapshot);
void xg_render_overlay_ft4_clear(void);
void xg_render_overlay_ft4_invalidate(uint32_t address, uint32_t size);
void xg_render_overlay_ft4_reset(void);
void xg_render_overlay_ft4_handle_invalidation(
    const XgRenderInvalidationEvent *event,
    const XgRenderInvalidationServices *services);

#endif
