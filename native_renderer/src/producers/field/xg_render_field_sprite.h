#ifndef XG_RENDER_FIELD_SPRITE_H
#define XG_RENDER_FIELD_SPRITE_H

#include "cpu_state.h"
#include "guest_render_types.h"
#include "guest_render_native_stream.h"
#include "xg_render_snapshot_types.h"
#include "xg_render_field_sprite_types.h"
#include "xg_render_ir.h"
#include "xg_render_producer_lifecycle.h"
#include "xg_render_invalidation_event.h"

#include <stdbool.h>
#include <stdint.h>

typedef struct XgRenderFieldSpriteServices {
    const XgRenderProducerLifecycleServices *lifecycle;
    bool (*stage_primitive)(
        const XgRenderIrNativePrimitive *primitive, uint32_t packet_address,
        uint32_t source_primitive_index, uint32_t interpolation_producer_id,
        uint32_t interpolation_primitive_id, uint32_t *failure_detail);
    bool (*publish_overlay)(
        const XgRenderFieldSpriteOverlayPublication *publication,
        uint32_t *failure_detail);
    void (*register_replay_command)(uint32_t command_id);
    void (*watch_resource)(uint32_t address, uint32_t size);
    uint64_t (*interpolation_scene)(void);
} XgRenderFieldSpriteServices;

uint16_t xg_render_field_sprite_tpage(
    uint16_t texture_depth, int16_t x, int16_t y);
uint16_t xg_render_field_sprite_clut(int16_t x, int16_t y);

bool xg_render_field_sprite_observe(
    CPUState *cpu, uint32_t pc, uint32_t instruction_word,
    GuestRenderRenderMode render_mode, bool scene_active,
    const XgRenderFieldSpriteServices *services);
bool xg_render_field_sprite_resolve(
    const GuestRenderNativeStreamMissContext *context,
    GpuRenderSemantic *out_semantic, GuestRenderRenderMode render_mode,
    const XgRenderFieldSpriteServices *services);
void xg_render_field_sprite_clear_builder(
    const XgRenderFieldSpriteServices *services);
void xg_render_field_sprite_clear_templates(
    const XgRenderFieldSpriteServices *services);
void xg_render_field_sprite_retain_resident(
    const XgRenderFieldSpriteServices *services);
void xg_render_field_sprite_invalidate_template(
    uint32_t packet_address, const XgRenderFieldSpriteServices *services);
bool xg_render_field_sprite_has_template(uint32_t packet_address);
uint32_t xg_render_field_sprite_available_template_capacity(void);
bool xg_render_field_sprite_capture_template(
    const XgRenderFieldSpriteTemplateInput *input,
    const XgRenderFieldSpriteServices *services);
void xg_render_field_sprite_invalidate_overlapping(
    uint32_t address, uint32_t size);
void xg_render_field_sprite_invalidate_code(
    bool code_write, bool shared_data_write,
    const XgRenderFieldSpriteServices *services);
void xg_render_field_sprite_diagnostics_update_snapshot(
    PsxXgRenderSpriteFt4ShadowSnapshot *in_out_snapshot);
void xg_render_field_sprite_diagnostics_snapshot(
    PsxXgRenderSpriteFt4ShadowSnapshot *out_snapshot);
void xg_render_field_sprite_diagnostics_reset(void);
void xg_render_field_sprite_reset(void);
void xg_render_field_sprite_handle_invalidation(
    const XgRenderInvalidationEvent *event,
    const XgRenderInvalidationServices *services);

#endif
