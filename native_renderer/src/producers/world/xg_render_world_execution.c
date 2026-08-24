#include "xg_render_world_execution.h"

typedef struct XgRenderWorldEntry {
    uint32_t pc;
    uint32_t instruction;
    PsxXgRenderWorldFamily family;
} XgRenderWorldEntry;

static const XgRenderWorldEntry world_entries[] = {
    {UINT32_C(0x800983a0), UINT32_C(0x27bdff90),
     PSX_XG_RENDER_WORLD_TERRAIN_WATER},
    {UINT32_C(0x800848f4), UINT32_C(0x24020800),
     PSX_XG_RENDER_WORLD_MODELS},
    {UINT32_C(0x80085cdc), UINT32_C(0x3c02800a),
     PSX_XG_RENDER_WORLD_ACTOR_SPRITES},
    {UINT32_C(0x800747dc), UINT32_C(0x3c02800a),
     PSX_XG_RENDER_WORLD_ENTITY_SHADOWS},
    {UINT32_C(0x80086798), UINT32_C(0x3c02800a),
     PSX_XG_RENDER_WORLD_CLOUDS},
    {UINT32_C(0x80089c78), UINT32_C(0x27bdffb0),
     PSX_XG_RENDER_WORLD_EFFECTS},
    {UINT32_C(0x8008615c), UINT32_C(0x27bdffd8),
     PSX_XG_RENDER_WORLD_DECORATIONS},
    {UINT32_C(0x80073b04), UINT32_C(0x27bdffc0),
     PSX_XG_RENDER_WORLD_HORIZON},
    {UINT32_C(0x800737ec), UINT32_C(0x27bdffb8),
     PSX_XG_RENDER_WORLD_SKY},
    {UINT32_C(0x800740b8), UINT32_C(0x27bdffc8),
     PSX_XG_RENDER_WORLD_MINIMAP},
};

static PsxXgRenderWorldExecutionSnapshot execution;
static bool execution_active;

static bool physical_address_equals(uint32_t left, uint32_t right) {
    return (left & UINT32_C(0x1fffffff)) ==
        (right & UINT32_C(0x1fffffff));
}

bool xg_render_world_execution_site_authorized(
        uint32_t pc, uint32_t instruction_word) {
    for (uint32_t index = 0u;
         index < sizeof(world_entries) / sizeof(world_entries[0]); ++index) {
        if (physical_address_equals(pc, world_entries[index].pc) &&
            instruction_word == world_entries[index].instruction)
            return true;
    }
    return false;
}

void xg_render_world_execution_observe(
        bool enabled, uint32_t pc, uint32_t instruction_word) {
    if (!enabled) return;
    if (physical_address_equals(pc, UINT32_C(0x80071a58))) {
        if (instruction_word != UINT32_C(0x3c02800a)) {
            execution_active = false;
        } else {
            execution_active = true;
            if (execution.full_dispatcher_count == UINT64_MAX)
                execution.overflowed = true;
            else
                ++execution.full_dispatcher_count;
        }
        return;
    }
    if (!execution_active) return;
    for (uint32_t index = 0u;
         index < sizeof(world_entries) / sizeof(world_entries[0]); ++index) {
        const XgRenderWorldEntry *entry = &world_entries[index];
        const uint32_t bit = UINT32_C(1) << entry->family;

        if (!physical_address_equals(pc, entry->pc)) continue;
        if (instruction_word != entry->instruction) return;
        if (execution.family_entry_count[entry->family] == UINT64_MAX) {
            execution.overflowed = true;
        } else {
            ++execution.family_entry_count[entry->family];
            execution.observed_family_mask |= bit;
        }
        return;
    }
}

void xg_render_world_execution_snapshot(
        PsxXgRenderWorldExecutionSnapshot *out_snapshot) {
    if (out_snapshot != NULL) *out_snapshot = execution;
}

void xg_render_world_execution_reset(void) {
    execution = (PsxXgRenderWorldExecutionSnapshot){0};
    execution_active = false;
}

void xg_render_world_execution_handle_invalidation(
        const XgRenderInvalidationEvent *event,
        const XgRenderInvalidationServices *services) {
    (void)services;
    if (event->kind == XG_RENDER_INVALIDATION_RESET)
        xg_render_world_execution_reset();
}
