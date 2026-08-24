#ifndef XG_RENDER_WORLD_EXECUTION_H
#define XG_RENDER_WORLD_EXECUTION_H

#include "xg_render_invalidation_event.h"

#include "xg_render_snapshot_types.h"

#include <stdbool.h>
#include <stdint.h>

bool xg_render_world_execution_site_authorized(
    uint32_t pc, uint32_t instruction_word);
void xg_render_world_execution_observe(
    bool enabled, uint32_t pc, uint32_t instruction_word);
void xg_render_world_execution_snapshot(
    PsxXgRenderWorldExecutionSnapshot *out_snapshot);
void xg_render_world_execution_reset(void);
void xg_render_world_execution_handle_invalidation(
    const XgRenderInvalidationEvent *event,
    const XgRenderInvalidationServices *services);

#endif
