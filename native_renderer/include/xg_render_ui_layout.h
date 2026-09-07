#ifndef XG_RENDER_UI_LAYOUT_H
#define XG_RENDER_UI_LAYOUT_H

#include "xg_render_ui_resources.h"

#include <stddef.h>
#include <stdint.h>

#define XG_RENDER_UI_LAYOUT_FIXED_ONE INT32_C(65536)

#ifdef __cplusplus
extern "C" {
#endif

typedef enum XgRenderUiLayoutResult {
    XG_RENDER_UI_LAYOUT_OK = 0,
    XG_RENDER_UI_LAYOUT_INVALID_ARGUMENT,
    XG_RENDER_UI_LAYOUT_GLYPH_NOT_FOUND,
    XG_RENDER_UI_LAYOUT_OUT_OF_BOUNDS,
    XG_RENDER_UI_LAYOUT_OVERFLOW,
    XG_RENDER_UI_LAYOUT_CAPACITY_EXCEEDED,
} XgRenderUiLayoutResult;

typedef enum XgRenderUiTextAlignment {
    XG_RENDER_UI_TEXT_ALIGN_LEFT = 0,
    XG_RENDER_UI_TEXT_ALIGN_CENTER,
    XG_RENDER_UI_TEXT_ALIGN_RIGHT,
} XgRenderUiTextAlignment;

typedef enum XgRenderUiLayoutLayerMask {
    XG_RENDER_UI_LAYOUT_LAYER_PRIMARY = 1u << 0u,
    XG_RENDER_UI_LAYOUT_LAYER_SHADOW = 1u << 1u,
} XgRenderUiLayoutLayerMask;

typedef struct XgRenderUiLayoutRect {
    int32_t left;
    int32_t top;
    int32_t right;
    int32_t bottom;
} XgRenderUiLayoutRect;

/* A resolved style belongs to exactly one decoded glyph. */
typedef struct XgRenderUiResolvedGlyphStyle {
    uint32_t color;
    uint32_t shadow_color;
    /* Host CLUT555 banks: 16 entries for INDEX4, 256 for INDEX8, in logical
     * row-major texels (descriptor row_pitch skips byte padding). These are
     * not guest layouts/addresses; RGB/RGBA require zero for both selectors. */
    uint16_t palette_index;
    uint16_t shadow_palette_index;
    int16_t shadow_offset_x;
    int16_t shadow_offset_y;
    uint32_t shadow_enabled;
} XgRenderUiResolvedGlyphStyle;

/* Lines must be contiguous, ordered, and cover every decoded glyph once. */
typedef struct XgRenderUiResolvedLine {
    uint32_t first_glyph;
    uint32_t glyph_count;
} XgRenderUiResolvedLine;

/* Opaque owner values are preserved and authenticated, never interpreted. */
typedef struct XgRenderUiLayoutOwnerInput {
    XgRenderUiOwnerDomain owner_domain;
    uint32_t owner_root;
    uint32_t adapter_kind;
    uint64_t owner_text_id;
} XgRenderUiLayoutOwnerInput;

typedef struct XgRenderUiLayoutUvRect {
    uint64_t left_fp16;
    uint64_t top_fp16;
    uint64_t right_fp16;
    uint64_t bottom_fp16;
} XgRenderUiLayoutUvRect;

/* Pointer-free render data. A layer bit is clear when that layer was clipped. */
typedef struct XgRenderUiLayoutPlacement {
    XgRenderGlyphPlacement primary;
    XgRenderGlyphPlacement shadow;
    XgRenderUiLayoutUvRect primary_uv;
    XgRenderUiLayoutUvRect shadow_uv;
    uint32_t glyph_index;
    uint32_t line_index;
    /* Same host CLUT bank contract as XgRenderUiResolvedGlyphStyle. */
    uint16_t palette_index;
    uint16_t shadow_palette_index;
    uint32_t layer_mask;
} XgRenderUiLayoutPlacement;

typedef struct XgRenderUiLayoutLineRecord {
    uint32_t first_glyph;
    uint32_t glyph_count;
    uint32_t revealed_glyph_count;
    uint32_t first_placement;
    uint32_t placement_count;
    int32_t x;
    int32_t baseline_y;
    int32_t width;
    int32_t height;
    int64_t advance_x_fp16;
} XgRenderUiLayoutLineRecord;

typedef struct XgRenderUiLayoutRequest {
    XgRenderUiLayoutOwnerInput owner;
    const XgRenderGlyphMetric *metrics;
    uint32_t metric_count;
    uint32_t atlas_width;
    uint32_t atlas_height;
    const uint32_t *glyph_ids;
    const XgRenderUiResolvedGlyphStyle *styles;
    uint32_t glyph_count;
    uint32_t reveal_count;
    const XgRenderUiResolvedLine *lines;
    uint32_t line_count;
    int32_t origin_x;
    int32_t origin_y;
    int32_t layout_width;
    int32_t line_height;
    int32_t scale_fp16;
    XgRenderUiTextAlignment alignment;
    XgRenderUiLayoutRect clip;
} XgRenderUiLayoutRequest;

typedef struct XgRenderUiLayoutOutput {
    XgRenderUiLayoutOwnerInput owner;
    uint32_t total_glyph_count;
    uint32_t revealed_glyph_count;
    uint32_t placement_count;
    uint32_t line_count;
    int32_t content_width;
    int32_t content_height;
    uint64_t digest;
} XgRenderUiLayoutOutput;

/*
 * The owner adapter must decode strings and resolve styles/line breaks before
 * this call. Output buffers are caller-owned and are not retained.
 */
XgRenderUiLayoutResult xg_render_ui_layout_build(
    const XgRenderUiLayoutRequest *request,
    XgRenderUiLayoutPlacement *out_placements,
    size_t placement_capacity,
    XgRenderUiLayoutLineRecord *out_lines,
    size_t line_capacity,
    XgRenderUiLayoutOutput *out_layout);

#ifdef __cplusplus
}
#endif

#endif
