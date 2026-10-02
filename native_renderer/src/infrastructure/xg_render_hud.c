#include "xg_render_hud.h"

#include <limits.h>

/* The host identifies the currently resident Field/World/Battle module from
 * its retail overlay tag. primary_overlay_identity is optional capture
 * metadata: it can name a companion or be absent, so it is not a layout gate. */

static bool packet_array(uint32_t packet, uint32_t base, uint32_t count,
                         uint32_t stride) {
    return packet >= base && packet - base < count * stride &&
           (packet - base) % stride == 0u;
}

static int hud_anchor(const XgSemanticSceneIdentity *scene, uint32_t packet,
                      uint32_t battle_graphics, uint32_t battle_ui) {
    if (scene->module == XG_SEMANTIC_MODULE_FIELD) {
        /* FieldRenderCompass: 25 objects, two FT4 buffers at +0x20/+0x48.
         * Includes the rotating rose, cardinal letters and shadow. */
        return packet_array(packet, 0x000b06dcu, 25u, 0x70u) ||
               packet_array(packet, 0x000b0704u, 25u, 0x70u);
    }
    if (scene->module == XG_SEMANTIC_MODULE_WORLD) {
        /* WorldMapRenderMinimap: panel, heading triangles and all markers. */
        return packet_array(packet, 0x0009c5c0u, 2u, 0x28u) ||
               packet_array(packet, 0x0009c664u, 8u, 0x1cu) ||
               packet_array(packet, 0x0009c898u, 64u, 0x10u);
    }
    if (scene->module != XG_SEMANTIC_MODULE_BATTLE) return 0;
    if (battle_ui >= 0x80010000u && battle_ui <= 0x801fa25cu &&
        (battle_ui & 3u) == 0u) {
        const uint32_t ui_base = battle_ui & 0x001fffffu;
        if (packet >= ui_base) {
            const uint32_t ui_offset = packet - ui_base;
            /* Bottom-left Gear widgets move to the left edge, with the left
             * gauge; the surrounding full-width bottom frame stays centred.
             * 0x800891e4 builds the Attack level and Fuel boxes from two
             * glyphs (base byte 0x800d2c34 - 0x5d / - 0x25) into the 0x3e80
             * labels/digit and 0x43d0 box banks; 0x800898f0 builds the Fuel
             * value into 0x5280 (current) and 0x53c0 ("/" and maximum).
             * Each bank is linked every other 0x28 packet from parity byte
             * 0x800ccb34 (0x80074ab8), so anchor every slot of each bank. */
            if (packet_array(ui_offset, 0x3e80u, 34u, 0x28u) ||
                packet_array(ui_offset, 0x43d0u, 18u, 0x28u) ||
                packet_array(ui_offset, 0x5280u, 8u, 0x28u) ||
                packet_array(ui_offset, 0x53c0u, 10u, 0x28u))
                return -1;
            /* BattleBuildGearCommandHeaderText (0x8008860c): glyph 0x9e
             * contributes 42 label/arrow pairs; glyph 0xda's pairs 1..14
             * form the status-panel background. The neighbouring glyphs
             * draw the attack-level widget and the full-screen Gear frame.
             * Booster and the four numeric fields have independent banks. */
            if (packet_array(ui_offset, 0x1720u, 84u, 0x28u) ||
                packet_array(ui_offset, 0x2580u, 28u, 0x28u) ||
                packet_array(ui_offset, 0x3ac0u, 22u, 0x28u) ||
                packet_array(ui_offset, 0x4ce0u, 10u, 0x28u) ||
                packet_array(ui_offset, 0x4e70u, 8u, 0x28u) ||
                packet_array(ui_offset, 0x4fb0u, 6u, 0x28u) ||
                packet_array(ui_offset, 0x50a0u, 8u, 0x28u) ||
                packet_array(ui_offset, 0x51e0u, 4u, 0x28u))
                return 1;
        }
    }
    if (battle_graphics < 0x80010000u || battle_graphics > 0x801f5d4cu ||
        (battle_graphics & 3u) != 0u)
        return false;
    const uint32_t base = battle_graphics & 0x001fffffu;
    if (packet < base) return false;
    const uint32_t offset = packet - base;
    /* Only the action menu and its lateral gauge move left. Party portraits,
     * HP/AP/fuel, selection highlight and party labels stay at authored XY.
     * BattleUpdateStatusGaugePolys fixes the fourth GT4 gauge X at 12..20;
     * BattleCaptureActionLines adds its six horizontal tick marks (12..18). */
    return packet_array(offset, 0x06d8u, 2u, 0x34u) ||
           packet_array(offset, 0x0908u, 12u, 0x10u) ||
           packet_array(offset, 0x3768u, 20u, 0x28u) || /* active turn text */
           packet_array(offset, 0x641cu, 100u, 0x28u) ||
           packet_array(offset, 0x73bcu, 100u, 0x28u) ? -1 : 0;
}

bool xg_render_hud_anchor(
        const XgRenderSourceFrameDescription *description, uint32_t command_id,
        uint32_t battle_graphics, uint32_t battle_ui, const GpuRenderSemantic *source,
        GpuRenderSemantic *out) {
    if (!description || !source || !out || command_id < 4u ||
        command_id > 0x001ffffcu || (command_id & 3u) != 0u)
        return false;
    const XgSemanticDisplayState *display = &description->display;
    if (display->disabled || display->depth24 || display->width != 320u ||
        display->aspect_den == 0u ||
        display->aspect_num * 3u <= display->aspect_den * 4u ||
        display->native_offset_x == 0u ||
        display->native_width != 320u + 2u * display->native_offset_x ||
        source->triangle_count > GPU_RENDER_SEMANTIC_TRIANGLE_CAPACITY ||
        source->line_count > GPU_RENDER_SEMANTIC_LINE_CAPACITY ||
        source->native_view_effect != 0u)
        return false;
    const int anchor = hud_anchor(&description->scene, command_id - 4u,
                                  battle_graphics, battle_ui);
    const bool lines = source->topology == GPU_RENDER_SEMANTIC_LINES;
    const uint32_t count = lines ? source->line_count : source->triangle_count;
    const uint32_t corners = lines ? 2u : 3u;
    if (!anchor || count == 0u ||
        (!lines && source->topology != GPU_RENDER_SEMANTIC_TRIANGLES) ||
        (lines ? source->triangle_count != 0u : source->line_count != 0u))
        return false;
    const int64_t margin = (int64_t)display->native_offset_x * 65536;
    /* Native producer coordinates already include the centre margin; packet
     * coordinates do not. Translate the entire group by one reveal margin. */
    for (uint32_t t = 0u; t < count; ++t)
        for (uint32_t v = 0u; v < corners; ++v) {
            const GpuRenderSemanticVertex *vertex = lines
                ? &source->lines[t].vertices[v] : &source->triangles[t].vertices[v];
            const int64_t x = vertex->native_view_position
                ? (int64_t)vertex->native_view_x + anchor * margin
                : (int64_t)vertex->x + (1 + anchor) * margin;
            if (x < INT32_MIN || x > INT32_MAX) return false;
        }
    *out = *source;
    out->screen_space_2d = GPU_RENDER_SCREEN_SPACE_2D_NONE;
    /* Camera motion publications describe the unanchored projection. HUD
     * stays on authored guest frames instead of inheriting camera transforms. */
    out->interpolation_identity = (GpuRenderInterpolationIdentity){0};
    for (uint32_t t = 0u; t < count; ++t)
        for (uint32_t v = 0u; v < corners; ++v) {
            GpuRenderSemanticVertex *vertex = lines
                ? &out->lines[t].vertices[v] : &out->triangles[t].vertices[v];
            vertex->native_view_x = (int32_t)(vertex->native_view_position
                ? (int64_t)vertex->native_view_x + anchor * margin
                : (int64_t)vertex->x + (1 + anchor) * margin);
            if (!vertex->native_view_position) vertex->native_view_y = vertex->y;
            vertex->native_view_position = 1u;
            vertex->projective_position = 0u;
            vertex->interpolation_vertex_identity_valid = 0u;
        }
    return true;
}
