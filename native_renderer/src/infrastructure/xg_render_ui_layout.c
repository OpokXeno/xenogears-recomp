#include "xg_render_ui_layout.h"

#include <limits.h>
#include <stdbool.h>
#include <string.h>

#define XG_LAYOUT_HASH_OFFSET UINT64_C(1469598103934665603)
#define XG_LAYOUT_HASH_PRIME UINT64_C(1099511628211)

typedef struct XgLayoutPassResult {
    uint32_t placement_count;
    int32_t content_width;
    int32_t content_height;
} XgLayoutPassResult;

static bool add_i64(int64_t left, int64_t right, int64_t *out_value) {
    if ((right > 0 && left > INT64_MAX - right) ||
        (right < 0 && left < INT64_MIN - right))
        return false;
    *out_value = left + right;
    return true;
}

static bool mul_i64(int64_t left, int64_t right, int64_t *out_value) {
    if (left == 0 || right == 0) {
        *out_value = 0;
        return true;
    }
    if ((left == -1 && right == INT64_MIN) ||
        (right == -1 && left == INT64_MIN))
        return false;
    if (left > 0) {
        if ((right > 0 && left > INT64_MAX / right) ||
            (right < 0 && right < INT64_MIN / left))
            return false;
    } else if ((right > 0 && left < INT64_MIN / right) ||
               (right < 0 && left < INT64_MAX / right)) {
        return false;
    }
    *out_value = left * right;
    return true;
}

static bool fixed_floor_i32(int64_t value, int32_t *out_value) {
    int64_t result = value / XG_RENDER_UI_LAYOUT_FIXED_ONE;
    if (value % XG_RENDER_UI_LAYOUT_FIXED_ONE < 0) --result;
    if (result < INT32_MIN || result > INT32_MAX) return false;
    *out_value = (int32_t)result;
    return true;
}

static bool fixed_ceil_i32(int64_t value, int32_t *out_value) {
    int64_t result = value / XG_RENDER_UI_LAYOUT_FIXED_ONE;
    if (value % XG_RENDER_UI_LAYOUT_FIXED_ONE > 0) ++result;
    if (result < INT32_MIN || result > INT32_MAX) return false;
    *out_value = (int32_t)result;
    return true;
}

static int64_t floor_divide_two(int64_t value) {
    int64_t result = value / 2;
    if (value < 0 && value % 2 != 0) --result;
    return result;
}

static const XgRenderGlyphMetric *find_metric(
        const XgRenderUiLayoutRequest *request, uint32_t glyph_id) {
    for (uint32_t index = 0u; index < request->metric_count; ++index)
        if (request->metrics[index].glyph_id == glyph_id)
            return &request->metrics[index];
    return NULL;
}

static XgRenderUiLayoutResult make_layer(
        const XgRenderUiLayoutRequest *request,
        const XgRenderGlyphMetric *metric, uint32_t color,
        int64_t left_fp, int64_t top_fp, int64_t right_fp, int64_t bottom_fp,
        XgRenderGlyphPlacement *out_glyph, XgRenderUiLayoutUvRect *out_uv,
        bool *out_visible) {
    int32_t full_left;
    int32_t full_top;
    int32_t full_right;
    int32_t full_bottom;
    int32_t clipped_left;
    int32_t clipped_top;
    int32_t clipped_right;
    int32_t clipped_bottom;
    uint64_t u_base = (uint64_t)metric->atlas_x << 16u;
    uint64_t v_base = (uint64_t)metric->atlas_y << 16u;
    uint64_t u_span = (uint64_t)metric->width << 16u;
    uint64_t v_span = (uint64_t)metric->height << 16u;
    uint64_t u_left;
    uint64_t u_right;
    uint64_t v_top;
    uint64_t v_bottom;
    int64_t raster_width;
    int64_t raster_height;
    int64_t clipped_width;
    int64_t clipped_height;

    memset(out_glyph, 0, sizeof(*out_glyph));
    memset(out_uv, 0, sizeof(*out_uv));
    *out_visible = false;
    if (!fixed_floor_i32(left_fp, &full_left) ||
        !fixed_floor_i32(top_fp, &full_top) ||
        !fixed_ceil_i32(right_fp, &full_right) ||
        !fixed_ceil_i32(bottom_fp, &full_bottom))
        return XG_RENDER_UI_LAYOUT_OVERFLOW;
    raster_width = (int64_t)full_right - full_left;
    raster_height = (int64_t)full_bottom - full_top;
    if (raster_width <= 0 || raster_height <= 0)
        return XG_RENDER_UI_LAYOUT_OVERFLOW;

    clipped_left = full_left > request->clip.left
        ? full_left : request->clip.left;
    clipped_top = full_top > request->clip.top
        ? full_top : request->clip.top;
    clipped_right = full_right < request->clip.right
        ? full_right : request->clip.right;
    clipped_bottom = full_bottom < request->clip.bottom
        ? full_bottom : request->clip.bottom;
    if (clipped_left >= clipped_right || clipped_top >= clipped_bottom)
        return XG_RENDER_UI_LAYOUT_OK;

    clipped_width = (int64_t)clipped_right - clipped_left;
    clipped_height = (int64_t)clipped_bottom - clipped_top;
    if (clipped_width > UINT16_MAX || clipped_height > UINT16_MAX)
        return XG_RENDER_UI_LAYOUT_OVERFLOW;
    u_left = u_base +
        ((uint64_t)((int64_t)clipped_left - full_left) * u_span) /
            (uint64_t)raster_width;
    u_right = u_base +
        ((uint64_t)((int64_t)clipped_right - full_left) * u_span) /
            (uint64_t)raster_width;
    v_top = v_base +
        ((uint64_t)((int64_t)clipped_top - full_top) * v_span) /
            (uint64_t)raster_height;
    v_bottom = v_base +
        ((uint64_t)((int64_t)clipped_bottom - full_top) * v_span) /
            (uint64_t)raster_height;
    if ((u_left >> 16u) > UINT16_MAX || (v_top >> 16u) > UINT16_MAX)
        return XG_RENDER_UI_LAYOUT_OVERFLOW;

    *out_glyph = (XgRenderGlyphPlacement){
        .glyph_id = metric->glyph_id,
        .x = clipped_left,
        .y = clipped_top,
        .atlas_x = (uint16_t)(u_left >> 16u),
        .atlas_y = (uint16_t)(v_top >> 16u),
        .width = (uint16_t)clipped_width,
        .height = (uint16_t)clipped_height,
        .color = color,
    };
    *out_uv = (XgRenderUiLayoutUvRect){
        .left_fp16 = u_left,
        .top_fp16 = v_top,
        .right_fp16 = u_right,
        .bottom_fp16 = v_bottom,
    };
    *out_visible = true;
    return XG_RENDER_UI_LAYOUT_OK;
}

static XgRenderUiLayoutResult validate_request(
        const XgRenderUiLayoutRequest *request,
        XgRenderUiLayoutOutput *out_layout) {
    uint64_t expected_first = 0u;

    if (request == NULL || out_layout == NULL ||
        (request->metric_count != 0u && request->metrics == NULL) ||
        (request->glyph_count != 0u &&
         (request->glyph_ids == NULL || request->styles == NULL)) ||
        (request->line_count != 0u && request->lines == NULL) ||
        request->reveal_count > request->glyph_count ||
        request->scale_fp16 <= 0 || request->line_height <= 0 ||
        request->layout_width < 0 || request->atlas_width == 0u ||
        request->atlas_height == 0u || request->atlas_width > UINT16_MAX ||
        request->atlas_height > UINT16_MAX ||
        request->clip.left >= request->clip.right ||
        request->clip.top >= request->clip.bottom ||
        request->owner.owner_domain <= XG_RENDER_UI_OWNER_NONE ||
        request->owner.owner_domain >= XG_RENDER_UI_OWNER_COUNT ||
        request->owner.owner_root == 0u ||
        request->alignment < XG_RENDER_UI_TEXT_ALIGN_LEFT ||
        request->alignment > XG_RENDER_UI_TEXT_ALIGN_RIGHT ||
        (request->glyph_count != 0u && request->line_count == 0u))
        return XG_RENDER_UI_LAYOUT_INVALID_ARGUMENT;

    for (uint32_t index = 0u; index < request->metric_count; ++index) {
        const XgRenderGlyphMetric *metric = &request->metrics[index];
        uint32_t atlas_right = (uint32_t)metric->atlas_x + metric->width;
        uint32_t atlas_bottom = (uint32_t)metric->atlas_y + metric->height;

        if (metric->glyph_id == 0u || metric->width == 0u ||
            metric->height == 0u || metric->advance_x < 0)
            return XG_RENDER_UI_LAYOUT_INVALID_ARGUMENT;
        if (atlas_right > request->atlas_width ||
            atlas_bottom > request->atlas_height)
            return XG_RENDER_UI_LAYOUT_OUT_OF_BOUNDS;
        for (uint32_t previous = 0u; previous < index; ++previous)
            if (request->metrics[previous].glyph_id == metric->glyph_id)
                return XG_RENDER_UI_LAYOUT_INVALID_ARGUMENT;
    }
    for (uint32_t index = 0u; index < request->glyph_count; ++index)
        if (find_metric(request, request->glyph_ids[index]) == NULL)
            return XG_RENDER_UI_LAYOUT_GLYPH_NOT_FOUND;

    for (uint32_t index = 0u; index < request->line_count; ++index) {
        const XgRenderUiResolvedLine *line = &request->lines[index];
        if (line->first_glyph != expected_first)
            return XG_RENDER_UI_LAYOUT_INVALID_ARGUMENT;
        expected_first += line->glyph_count;
        if (expected_first > request->glyph_count)
            return XG_RENDER_UI_LAYOUT_INVALID_ARGUMENT;
    }
    if (expected_first != request->glyph_count)
        return XG_RENDER_UI_LAYOUT_INVALID_ARGUMENT;
    return XG_RENDER_UI_LAYOUT_OK;
}

static XgRenderUiLayoutResult run_layout_pass(
        const XgRenderUiLayoutRequest *request,
        XgRenderUiLayoutPlacement *placements,
        XgRenderUiLayoutLineRecord *line_records,
        XgLayoutPassResult *out_pass) {
    int64_t origin_x_fp;
    int64_t origin_y_fp;
    int64_t layout_width_fp;
    int64_t line_height_fp;
    int64_t total_height_fp;
    int32_t line_pixel_height;
    int32_t max_width = 0;
    uint32_t placement_count = 0u;

    if (!mul_i64(request->origin_x, XG_RENDER_UI_LAYOUT_FIXED_ONE,
                 &origin_x_fp) ||
        !mul_i64(request->origin_y, XG_RENDER_UI_LAYOUT_FIXED_ONE,
                 &origin_y_fp) ||
        !mul_i64(request->layout_width, XG_RENDER_UI_LAYOUT_FIXED_ONE,
                 &layout_width_fp) ||
        !mul_i64(request->line_height, request->scale_fp16,
                 &line_height_fp) ||
        !mul_i64(request->line_count, line_height_fp, &total_height_fp) ||
        !fixed_ceil_i32(line_height_fp, &line_pixel_height))
        return XG_RENDER_UI_LAYOUT_OVERFLOW;

    for (uint32_t line_index = 0u; line_index < request->line_count;
         ++line_index) {
        const XgRenderUiResolvedLine *line = &request->lines[line_index];
        int64_t advance_fp = 0;
        int64_t remaining_width_fp;
        int64_t alignment_fp = 0;
        int64_t pen_fp;
        int64_t baseline_fp;
        int32_t line_x;
        int32_t baseline_y;
        int32_t line_width;
        uint32_t first_placement = placement_count;
        uint32_t revealed_in_line = 0u;

        for (uint32_t offset = 0u; offset < line->glyph_count; ++offset) {
            const XgRenderGlyphMetric *metric = find_metric(
                request, request->glyph_ids[line->first_glyph + offset]);
            int64_t glyph_advance;
            if (!mul_i64(metric->advance_x, request->scale_fp16,
                         &glyph_advance) ||
                !add_i64(advance_fp, glyph_advance, &advance_fp))
                return XG_RENDER_UI_LAYOUT_OVERFLOW;
        }
        if (!add_i64(layout_width_fp, -advance_fp, &remaining_width_fp))
            return XG_RENDER_UI_LAYOUT_OVERFLOW;
        if (request->alignment == XG_RENDER_UI_TEXT_ALIGN_CENTER)
            alignment_fp = floor_divide_two(remaining_width_fp);
        else if (request->alignment == XG_RENDER_UI_TEXT_ALIGN_RIGHT)
            alignment_fp = remaining_width_fp;
        if (!add_i64(origin_x_fp, alignment_fp, &pen_fp) ||
            !mul_i64(line_index, line_height_fp, &baseline_fp) ||
            !add_i64(origin_y_fp, baseline_fp, &baseline_fp) ||
            !fixed_floor_i32(pen_fp, &line_x) ||
            !fixed_floor_i32(baseline_fp, &baseline_y) ||
            !fixed_ceil_i32(advance_fp, &line_width))
            return XG_RENDER_UI_LAYOUT_OVERFLOW;
        if (line_width > max_width) max_width = line_width;

        for (uint32_t offset = 0u; offset < line->glyph_count; ++offset) {
            uint32_t glyph_index = line->first_glyph + offset;
            const XgRenderGlyphMetric *metric = find_metric(
                request, request->glyph_ids[glyph_index]);
            const XgRenderUiResolvedGlyphStyle *style =
                &request->styles[glyph_index];
            XgRenderUiLayoutPlacement placement;
            int64_t bearing_x_fp;
            int64_t bearing_y_fp;
            int64_t width_fp;
            int64_t height_fp;
            int64_t left_fp;
            int64_t top_fp;
            int64_t right_fp;
            int64_t bottom_fp;
            int64_t shadow_x_fp;
            int64_t shadow_y_fp;
            bool primary_visible;
            bool shadow_visible = false;
            XgRenderUiLayoutResult result;

            memset(&placement, 0, sizeof(placement));
            if (!mul_i64(metric->bearing_x, request->scale_fp16,
                         &bearing_x_fp) ||
                !mul_i64(metric->bearing_y, request->scale_fp16,
                         &bearing_y_fp) ||
                !mul_i64(metric->width, request->scale_fp16, &width_fp) ||
                !mul_i64(metric->height, request->scale_fp16, &height_fp) ||
                !add_i64(pen_fp, bearing_x_fp, &left_fp) ||
                !add_i64(baseline_fp, bearing_y_fp, &top_fp) ||
                !add_i64(left_fp, width_fp, &right_fp) ||
                !add_i64(top_fp, height_fp, &bottom_fp))
                return XG_RENDER_UI_LAYOUT_OVERFLOW;
            result = make_layer(request, metric, style->color,
                                left_fp, top_fp, right_fp, bottom_fp,
                                &placement.primary, &placement.primary_uv,
                                &primary_visible);
            if (result != XG_RENDER_UI_LAYOUT_OK) return result;
            if (style->shadow_enabled != 0u) {
                if (!mul_i64(style->shadow_offset_x, request->scale_fp16,
                             &shadow_x_fp) ||
                    !mul_i64(style->shadow_offset_y, request->scale_fp16,
                             &shadow_y_fp) ||
                    !add_i64(left_fp, shadow_x_fp, &shadow_x_fp) ||
                    !add_i64(top_fp, shadow_y_fp, &shadow_y_fp) ||
                    !add_i64(shadow_x_fp, width_fp, &right_fp) ||
                    !add_i64(shadow_y_fp, height_fp, &bottom_fp))
                    return XG_RENDER_UI_LAYOUT_OVERFLOW;
                result = make_layer(
                    request, metric, style->shadow_color,
                    shadow_x_fp, shadow_y_fp, right_fp, bottom_fp,
                    &placement.shadow, &placement.shadow_uv, &shadow_visible);
                if (result != XG_RENDER_UI_LAYOUT_OK) return result;
            }
            placement.glyph_index = glyph_index;
            placement.line_index = line_index;
            placement.palette_index = style->palette_index;
            placement.shadow_palette_index = style->shadow_palette_index;
            placement.layer_mask =
                (primary_visible ? XG_RENDER_UI_LAYOUT_LAYER_PRIMARY : 0u) |
                (shadow_visible ? XG_RENDER_UI_LAYOUT_LAYER_SHADOW : 0u);
            if (glyph_index < request->reveal_count) {
                ++revealed_in_line;
                if (placement.layer_mask != 0u) {
                    if (placements != NULL)
                        placements[placement_count] = placement;
                    if (placement_count == UINT32_MAX)
                        return XG_RENDER_UI_LAYOUT_OVERFLOW;
                    ++placement_count;
                }
            }
            if (!mul_i64(metric->advance_x, request->scale_fp16,
                         &bearing_x_fp) ||
                !add_i64(pen_fp, bearing_x_fp, &pen_fp))
                return XG_RENDER_UI_LAYOUT_OVERFLOW;
        }
        if (line_records != NULL) {
            line_records[line_index] = (XgRenderUiLayoutLineRecord){
                .first_glyph = line->first_glyph,
                .glyph_count = line->glyph_count,
                .revealed_glyph_count = revealed_in_line,
                .first_placement = first_placement,
                .placement_count = placement_count - first_placement,
                .x = line_x,
                .baseline_y = baseline_y,
                .width = line_width,
                .height = line_pixel_height,
                .advance_x_fp16 = advance_fp,
            };
        }
    }
    if (!fixed_ceil_i32(total_height_fp, &out_pass->content_height))
        return XG_RENDER_UI_LAYOUT_OVERFLOW;
    out_pass->placement_count = placement_count;
    out_pass->content_width = max_width;
    return XG_RENDER_UI_LAYOUT_OK;
}

static uint64_t hash_u8(uint64_t hash, uint8_t value) {
    hash ^= value;
    return hash * XG_LAYOUT_HASH_PRIME;
}

static uint64_t hash_u16(uint64_t hash, uint16_t value) {
    for (uint32_t byte = 0u; byte < 2u; ++byte)
        hash = hash_u8(hash, (uint8_t)(value >> (byte * 8u)));
    return hash;
}

static uint64_t hash_u32(uint64_t hash, uint32_t value) {
    for (uint32_t byte = 0u; byte < 4u; ++byte)
        hash = hash_u8(hash, (uint8_t)(value >> (byte * 8u)));
    return hash;
}

static uint64_t hash_u64(uint64_t hash, uint64_t value) {
    for (uint32_t byte = 0u; byte < 8u; ++byte)
        hash = hash_u8(hash, (uint8_t)(value >> (byte * 8u)));
    return hash;
}

static uint64_t hash_glyph_placement(
        uint64_t hash, const XgRenderGlyphPlacement *glyph) {
    hash = hash_u32(hash, glyph->glyph_id);
    hash = hash_u32(hash, (uint32_t)glyph->x);
    hash = hash_u32(hash, (uint32_t)glyph->y);
    hash = hash_u16(hash, glyph->atlas_x);
    hash = hash_u16(hash, glyph->atlas_y);
    hash = hash_u16(hash, glyph->width);
    hash = hash_u16(hash, glyph->height);
    return hash_u32(hash, glyph->color);
}

static uint64_t hash_uv(uint64_t hash, const XgRenderUiLayoutUvRect *uv) {
    hash = hash_u64(hash, uv->left_fp16);
    hash = hash_u64(hash, uv->top_fp16);
    hash = hash_u64(hash, uv->right_fp16);
    return hash_u64(hash, uv->bottom_fp16);
}

static uint64_t canonical_digest(
        const XgRenderUiLayoutRequest *request,
        const XgRenderUiLayoutPlacement *placements,
        const XgRenderUiLayoutLineRecord *lines,
        const XgRenderUiLayoutOutput *layout) {
    uint64_t hash = XG_LAYOUT_HASH_OFFSET;

    hash = hash_u32(hash, UINT32_C(0x5836474c)); /* X6GL, version 1. */
    hash = hash_u32(hash, 1u);
    hash = hash_u32(hash, (uint32_t)request->owner.owner_domain);
    hash = hash_u32(hash, request->owner.owner_root);
    hash = hash_u32(hash, request->owner.adapter_kind);
    hash = hash_u64(hash, request->owner.owner_text_id);
    hash = hash_u32(hash, request->atlas_width);
    hash = hash_u32(hash, request->atlas_height);
    hash = hash_u32(hash, request->glyph_count);
    hash = hash_u32(hash, request->reveal_count);
    hash = hash_u32(hash, request->line_count);
    hash = hash_u32(hash, (uint32_t)request->origin_x);
    hash = hash_u32(hash, (uint32_t)request->origin_y);
    hash = hash_u32(hash, (uint32_t)request->layout_width);
    hash = hash_u32(hash, (uint32_t)request->line_height);
    hash = hash_u32(hash, (uint32_t)request->scale_fp16);
    hash = hash_u32(hash, (uint32_t)request->alignment);
    hash = hash_u32(hash, (uint32_t)request->clip.left);
    hash = hash_u32(hash, (uint32_t)request->clip.top);
    hash = hash_u32(hash, (uint32_t)request->clip.right);
    hash = hash_u32(hash, (uint32_t)request->clip.bottom);
    for (uint32_t index = 0u; index < request->line_count; ++index) {
        hash = hash_u32(hash, request->lines[index].first_glyph);
        hash = hash_u32(hash, request->lines[index].glyph_count);
    }
    for (uint32_t index = 0u; index < request->glyph_count; ++index) {
        const XgRenderGlyphMetric *metric = find_metric(
            request, request->glyph_ids[index]);
        const XgRenderUiResolvedGlyphStyle *style = &request->styles[index];
        hash = hash_u32(hash, request->glyph_ids[index]);
        hash = hash_u16(hash, metric->atlas_x);
        hash = hash_u16(hash, metric->atlas_y);
        hash = hash_u16(hash, metric->width);
        hash = hash_u16(hash, metric->height);
        hash = hash_u16(hash, (uint16_t)metric->bearing_x);
        hash = hash_u16(hash, (uint16_t)metric->bearing_y);
        hash = hash_u16(hash, (uint16_t)metric->advance_x);
        hash = hash_u32(hash, style->color);
        hash = hash_u32(hash, style->shadow_color);
        hash = hash_u16(hash, style->palette_index);
        hash = hash_u16(hash, style->shadow_palette_index);
        hash = hash_u16(hash, (uint16_t)style->shadow_offset_x);
        hash = hash_u16(hash, (uint16_t)style->shadow_offset_y);
        hash = hash_u32(hash, style->shadow_enabled != 0u ? 1u : 0u);
    }
    hash = hash_u32(hash, layout->placement_count);
    hash = hash_u32(hash, (uint32_t)layout->content_width);
    hash = hash_u32(hash, (uint32_t)layout->content_height);
    for (uint32_t index = 0u; index < layout->line_count; ++index) {
        const XgRenderUiLayoutLineRecord *line = &lines[index];
        hash = hash_u32(hash, line->first_glyph);
        hash = hash_u32(hash, line->glyph_count);
        hash = hash_u32(hash, line->revealed_glyph_count);
        hash = hash_u32(hash, line->first_placement);
        hash = hash_u32(hash, line->placement_count);
        hash = hash_u32(hash, (uint32_t)line->x);
        hash = hash_u32(hash, (uint32_t)line->baseline_y);
        hash = hash_u32(hash, (uint32_t)line->width);
        hash = hash_u32(hash, (uint32_t)line->height);
        hash = hash_u64(hash, (uint64_t)line->advance_x_fp16);
    }
    for (uint32_t index = 0u; index < layout->placement_count; ++index) {
        const XgRenderUiLayoutPlacement *placement = &placements[index];
        hash = hash_glyph_placement(hash, &placement->primary);
        hash = hash_glyph_placement(hash, &placement->shadow);
        hash = hash_uv(hash, &placement->primary_uv);
        hash = hash_uv(hash, &placement->shadow_uv);
        hash = hash_u32(hash, placement->glyph_index);
        hash = hash_u32(hash, placement->line_index);
        hash = hash_u16(hash, placement->palette_index);
        hash = hash_u16(hash, placement->shadow_palette_index);
        hash = hash_u32(hash, placement->layer_mask);
    }
    return hash;
}

XgRenderUiLayoutResult xg_render_ui_layout_build(
        const XgRenderUiLayoutRequest *request,
        XgRenderUiLayoutPlacement *out_placements,
        size_t placement_capacity,
        XgRenderUiLayoutLineRecord *out_lines,
        size_t line_capacity,
        XgRenderUiLayoutOutput *out_layout) {
    XgLayoutPassResult pass;
    XgRenderUiLayoutOutput layout;
    XgRenderUiLayoutResult result = validate_request(request, out_layout);

    if (result != XG_RENDER_UI_LAYOUT_OK) return result;
    result = run_layout_pass(request, NULL, NULL, &pass);
    if (result != XG_RENDER_UI_LAYOUT_OK) return result;
    if (pass.placement_count > placement_capacity ||
        request->line_count > line_capacity ||
        (pass.placement_count != 0u && out_placements == NULL) ||
        (request->line_count != 0u && out_lines == NULL))
        return XG_RENDER_UI_LAYOUT_CAPACITY_EXCEEDED;
    result = run_layout_pass(request, out_placements, out_lines, &pass);
    if (result != XG_RENDER_UI_LAYOUT_OK) return result;

    layout = (XgRenderUiLayoutOutput){
        .owner = request->owner,
        .total_glyph_count = request->glyph_count,
        .revealed_glyph_count = request->reveal_count,
        .placement_count = pass.placement_count,
        .line_count = request->line_count,
        .content_width = pass.content_width,
        .content_height = pass.content_height,
    };
    layout.digest = canonical_digest(
        request, out_placements, out_lines, &layout);
    *out_layout = layout;
    return XG_RENDER_UI_LAYOUT_OK;
}
