#ifndef XG_RENDER_UI_OT_H
#define XG_RENDER_UI_OT_H

#include "xg_render_invalidation_event.h"

#include "gpu_render.h"
#include "xg_render_snapshot_types.h"

#include <stdint.h>

typedef uint32_t (*XgRenderUiOtReadWord)(uint32_t address);

void xg_render_ui_ot_note_draw_observation(
    uint32_t frame, uint32_t start_address,
    GpuRenderTransactionId visual_id);
void xg_render_ui_ot_clear_pending(void);
bool xg_render_ui_ot_prepare(
    uint32_t start_address, GuestRenderRenderMode requested_mode,
    uint32_t current_frame, XgRenderUiOtReadWord read_word);
void xg_render_ui_ot_snapshot(PsxXgRenderUiOtSnapshot *out_snapshot);
void xg_render_ui_ot_reset(void);
void xg_render_ui_ot_handle_invalidation(
    const XgRenderInvalidationEvent *event,
    const XgRenderInvalidationServices *services);

#endif
