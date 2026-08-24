#include "xg_field_character_adapter.h"
#include "xg_render_quad_builder.h"

#include <stddef.h>
#include <string.h>

static int capture_is_valid(const XgFieldCharacterCapture *capture) {
    const uint16_t texture_depth = (uint16_t)((capture->tpage >> 7u) & 3u);

    return capture->tpage <= UINT16_C(0x01ff) && texture_depth != 3u &&
           capture->clut_x <= 1008u && (capture->clut_x & 15u) == 0u &&
           capture->clut_y <= 511u && capture->draw_area_left <= 1023u &&
           capture->draw_area_top <= 1023u &&
           capture->draw_area_right <= 1023u &&
           capture->draw_area_bottom <= 1023u &&
           capture->draw_area_left <= capture->draw_area_right &&
           capture->draw_area_top <= capture->draw_area_bottom &&
           capture->draw_offset_x >= -1024 && capture->draw_offset_x <= 1023 &&
           capture->draw_offset_y >= -1024 && capture->draw_offset_y <= 1023 &&
           capture->texture_window_mask_x <= 31u &&
           capture->texture_window_mask_y <= 31u &&
            capture->texture_window_offset_x <= 31u &&
            capture->texture_window_offset_y <= 31u && capture->dither <= 1u &&
            capture->mask_set <= 1u && capture->mask_check <= 1u &&
             capture->semi_transparent <= 1u;
}

static int candidate_is_valid(const XgFieldCharacterCandidate *candidate) {
    size_t index;

    if (candidate->finalized != 1u || candidate->tpage > UINT16_C(0x01ff) ||
        ((candidate->tpage >> 7u) & 3u) == 3u ||
        candidate->texture_page_x != (candidate->tpage & UINT16_C(0x000f)) ||
        candidate->texture_page_y != ((candidate->tpage >> 4u) & 1u) ||
        candidate->texture_depth != ((candidate->tpage >> 7u) & 3u) ||
        candidate->blend_mode != ((candidate->tpage >> 5u) & 3u) ||
        candidate->clut_x > 1008u || (candidate->clut_x & 15u) != 0u ||
        candidate->clut_y > 511u || candidate->draw_area_left > 1023u ||
        candidate->draw_area_top > 1023u ||
        candidate->draw_area_right > 1023u ||
        candidate->draw_area_bottom > 1023u ||
        candidate->draw_area_left > candidate->draw_area_right ||
        candidate->draw_area_top > candidate->draw_area_bottom ||
        candidate->draw_offset_x < -1024 || candidate->draw_offset_x > 1023 ||
        candidate->draw_offset_y < -1024 || candidate->draw_offset_y > 1023 ||
        candidate->texture_window_mask_x > 31u ||
        candidate->texture_window_mask_y > 31u ||
        candidate->texture_window_offset_x > 31u ||
        candidate->texture_window_offset_y > 31u || candidate->dither > 1u ||
        candidate->mask_set > 1u || candidate->mask_check > 1u ||
        candidate->semi_transparent > 1u)
        return 0;
    for (index = 0u; index < XG_FIELD_CHARACTER_VERTEX_COUNT; ++index) {
        const XgFieldCharacterCandidateVertex *vertex = &candidate->vertices[index];

        if ((vertex->x & INT32_C(0xffff)) != 0 ||
            (vertex->y & INT32_C(0xffff)) != 0 || vertex->u < 0 ||
            vertex->u > INT32_C(0x00ff0000) ||
            (vertex->u & INT32_C(0xffff)) != 0 || vertex->v < 0 ||
            vertex->v > INT32_C(0x00ff0000) ||
            (vertex->v & INT32_C(0xffff)) != 0 ||
            vertex->red != candidate->vertices[0].red ||
            vertex->green != candidate->vertices[0].green ||
            vertex->blue != candidate->vertices[0].blue)
            return 0;
    }
    return 1;
}

XgFieldCharacterAdapterResult xg_field_character_adapter_build(
    const XgFieldCharacterCapture *capture,
    XgFieldCharacterCandidate *out_candidate) {
    size_t index;

    if (!capture || !out_candidate)
        return XG_FIELD_CHARACTER_ADAPTER_INVALID_ARGUMENT;
    memset(out_candidate, 0, sizeof(*out_candidate));
    if (!capture_is_valid(capture))
        return XG_FIELD_CHARACTER_ADAPTER_INVALID_CAPTURE;

    for (index = 0u; index < XG_FIELD_CHARACTER_VERTEX_COUNT; ++index) {
        XgFieldCharacterCandidateVertex *vertex = &out_candidate->vertices[index];

        vertex->x = (int32_t)capture->vertices[index].x * INT32_C(65536);
        vertex->y = (int32_t)capture->vertices[index].y * INT32_C(65536);
        vertex->u = (int32_t)capture->vertices[index].u * INT32_C(65536);
        vertex->v = (int32_t)capture->vertices[index].v * INT32_C(65536);
        vertex->red = capture->red;
        vertex->green = capture->green;
        vertex->blue = capture->blue;
    }
    out_candidate->tpage = capture->tpage;
    out_candidate->texture_page_x = capture->tpage & UINT16_C(0x000f);
    out_candidate->texture_page_y = (capture->tpage >> 4u) & 1u;
    out_candidate->texture_depth = (uint8_t)((capture->tpage >> 7u) & 3u);
    out_candidate->blend_mode = (uint8_t)((capture->tpage >> 5u) & 3u);
    out_candidate->clut_x = capture->clut_x;
    out_candidate->clut_y = capture->clut_y;
    out_candidate->draw_area_left = capture->draw_area_left;
    out_candidate->draw_area_top = capture->draw_area_top;
    out_candidate->draw_area_right = capture->draw_area_right;
    out_candidate->draw_area_bottom = capture->draw_area_bottom;
    out_candidate->draw_offset_x = capture->draw_offset_x;
    out_candidate->draw_offset_y = capture->draw_offset_y;
    out_candidate->texture_window_mask_x = capture->texture_window_mask_x;
    out_candidate->texture_window_mask_y = capture->texture_window_mask_y;
    out_candidate->texture_window_offset_x = capture->texture_window_offset_x;
    out_candidate->texture_window_offset_y = capture->texture_window_offset_y;
    out_candidate->dither = capture->dither;
    out_candidate->mask_set = capture->mask_set;
    out_candidate->mask_check = capture->mask_check;
    out_candidate->semi_transparent = capture->semi_transparent;
    out_candidate->finalized = 1u;
    return XG_FIELD_CHARACTER_ADAPTER_OK;
}

XgFieldCharacterAdapterResult xg_field_character_adapter_build_primitive(
    const XgFieldCharacterCandidate *candidate,
    XgRenderIrNativePrimitive *out_primitive) {
    XgRenderQuadSource source = { 0 };
    size_t index;

    if (candidate == NULL || out_primitive == NULL)
        return XG_FIELD_CHARACTER_ADAPTER_INVALID_ARGUMENT;
    memset(out_primitive, 0, sizeof(*out_primitive));
    if (!candidate_is_valid(candidate))
        return XG_FIELD_CHARACTER_ADAPTER_INVALID_CAPTURE;
    source.material.tpage = candidate->tpage;
    source.material.texture_page_x = candidate->texture_page_x;
    source.material.texture_page_y = candidate->texture_page_y;
    source.material.clut_x = candidate->clut_x;
    source.material.clut_y = candidate->clut_y;
    source.material.draw_area_left = candidate->draw_area_left;
    source.material.draw_area_top = candidate->draw_area_top;
    source.material.draw_area_right = candidate->draw_area_right;
    source.material.draw_area_bottom = candidate->draw_area_bottom;
    source.material.draw_offset_x = candidate->draw_offset_x;
    source.material.draw_offset_y = candidate->draw_offset_y;
    source.material.texture_depth =
        (XgRenderIrTextureDepth)candidate->texture_depth;
    source.material.texture_window_mask_x =
        candidate->texture_window_mask_x;
    source.material.texture_window_mask_y =
        candidate->texture_window_mask_y;
    source.material.texture_window_offset_x =
        candidate->texture_window_offset_x;
    source.material.texture_window_offset_y =
        candidate->texture_window_offset_y;
    source.material.shading = XG_RENDER_IR_SHADING_FLAT;
    source.material.textured = true;
    source.material.raw_texture = false;
    source.material.semi_transparent =
        candidate->semi_transparent != 0u;
    source.material.blend_mode =
        (XgRenderIrBlendMode)candidate->blend_mode;
    source.material.dither = candidate->dither != 0u;
    source.material.mask_set = candidate->mask_set != 0u;
    source.material.mask_check = candidate->mask_check != 0u;
    for (index = 0u; index < XG_FIELD_CHARACTER_VERTEX_COUNT; ++index) {
        source.vertices[index] = (XgRenderQuadSourceVertex){
            (int16_t)(candidate->vertices[index].x / INT32_C(65536)),
            (int16_t)(candidate->vertices[index].y / INT32_C(65536)),
            (uint8_t)(candidate->vertices[index].u / INT32_C(65536)),
            (uint8_t)(candidate->vertices[index].v / INT32_C(65536)),
            candidate->vertices[index].red,
            candidate->vertices[index].green,
            candidate->vertices[index].blue,
        };
    }
    if (xg_render_quad_build_primitive(&source, out_primitive) !=
        XG_RENDER_QUAD_BUILDER_OK)
        return XG_FIELD_CHARACTER_ADAPTER_INVALID_CAPTURE;
    return XG_FIELD_CHARACTER_ADAPTER_OK;
}
