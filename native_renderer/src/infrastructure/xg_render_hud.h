#ifndef XG_RENDER_HUD_H
#define XG_RENDER_HUD_H

#include "xg_render_source_frame.h"

/* Presentation-only HUD anchors. command_id names the GP0 colour word, not
 * the preceding PsyQ tag. battle_graphics and battle_ui are the current
 * allocations at 0x800c3ea4 and 0x800d2db4 respectively. */
bool xg_render_hud_anchor(
    const XgRenderSourceFrameDescription *description, uint32_t command_id,
    uint32_t battle_graphics, uint32_t battle_ui, const GpuRenderSemantic *source,
    GpuRenderSemantic *out);

#endif
