#ifndef XG_RENDER_RESIDENT_TEXT_H
#define XG_RENDER_RESIDENT_TEXT_H

#include "guest_render_types.h"
#include "xg_render_invalidation_event.h"
#include "xg_render_snapshot_types.h"

#include <stdbool.h>
#include <stdint.h>

typedef struct CPUState CPUState;

typedef struct XgRenderResidentTextServices {
    bool (*native_text_authorizes_pc)(uint32_t owner_entry);
    bool (*guest_data_range_is_valid)(
        uint32_t address, uint32_t size, uint32_t alignment,
        bool allow_scratchpad);
    void (*watch_resource)(uint32_t address, uint32_t size);
} XgRenderResidentTextServices;

bool xg_render_resident_text_observe(
    CPUState *cpu, uint32_t action, uint32_t pc,
    uint32_t instruction_word, GuestRenderRenderMode render_mode,
    const XgRenderResidentTextServices *services);
void xg_render_resident_text_snapshot(
    PsxXgRenderResidentTextSnapshot *out_snapshot);
void xg_render_resident_text_handle_invalidation(
    const XgRenderInvalidationEvent *event,
    const XgRenderInvalidationServices *services);
void xg_render_resident_text_classify_code_write(
    uint32_t address, uint32_t size,
    XgRenderMutationClassification *out_classification);
void xg_render_resident_text_register_code_watches(
    void (*set_range)(uint32_t physical_address, uint32_t size));

#endif
