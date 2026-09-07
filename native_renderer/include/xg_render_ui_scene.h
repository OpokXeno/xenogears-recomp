#ifndef XG_RENDER_UI_SCENE_H
#define XG_RENDER_UI_SCENE_H

#include "xg_render_ui_layout.h"

#include <stdbool.h>
#include <stdint.h>

#ifndef XG_RENDER_SCENE_UI_NODE_CAPACITY
#define XG_RENDER_SCENE_UI_NODE_CAPACITY 256u
#endif

#ifndef XG_RENDER_SCENE_UI_GLYPH_RUN_CAPACITY
#define XG_RENDER_SCENE_UI_GLYPH_RUN_CAPACITY 256u
#endif

#ifndef XG_RENDER_SCENE_UI_GLYPH_PLACEMENT_CAPACITY
#define XG_RENDER_SCENE_UI_GLYPH_PLACEMENT_CAPACITY 4096u
#endif

#ifdef __cplusplus
extern "C" {
#endif

typedef struct XgSemanticOrderKey {
    uint32_t pass_id;
    int32_t layer;
    int32_t authored_depth;
    uint32_t insertion_ordinal;
    uint16_t split_ordinal;
} XgSemanticOrderKey;

typedef struct XgSemanticUiRect {
    int32_t x;
    int32_t y;
    int32_t width;
    int32_t height;
} XgSemanticUiRect;

typedef enum XgSemanticUiNodeKind {
    XG_SEMANTIC_UI_NODE_GROUP = 0,
    XG_SEMANTIC_UI_NODE_WINDOW,
    XG_SEMANTIC_UI_NODE_IMAGE,
    XG_SEMANTIC_UI_NODE_PORTRAIT,
    XG_SEMANTIC_UI_NODE_CURSOR,
    XG_SEMANTIC_UI_NODE_GAUGE,
    XG_SEMANTIC_UI_NODE_CHOICE,
    XG_SEMANTIC_UI_NODE_MASK,
    XG_SEMANTIC_UI_NODE_TEXT,
    XG_SEMANTIC_UI_NODE_MODEL_PREVIEW,
    XG_SEMANTIC_UI_NODE_KIND_COUNT,
} XgSemanticUiNodeKind;

/* UI state is an authored endpoint and is never interpolated by the host. */
typedef enum XgSemanticUiTemporalMode {
    XG_SEMANTIC_UI_TEMPORAL_DISCRETE = 0,
    XG_SEMANTIC_UI_TEMPORAL_MODE_COUNT,
} XgSemanticUiTemporalMode;

typedef struct XgSemanticUiNodeRecord {
    uint64_t node_id;
    uint64_t parent_node_id;
    XgRenderUiOwnerDomain owner_domain;
    uint32_t owner_root;
    uint64_t owner_receipt;
    XgSemanticOrderKey order;
    XgSemanticUiNodeKind kind;
    XgSemanticUiRect bounds;
    XgSemanticUiRect clip;
    XgRenderResourceHandle resource;
    XgRenderResourceHandle clut;
    XgRenderResourceHandle model;
    uint64_t semantic_id;
    uint32_t state;
    int32_t value;
    int32_t maximum;
    uint32_t flags;
    uint32_t color;
    XgSemanticUiTemporalMode temporal_mode;
    bool visible;
    bool clip_enabled;
} XgSemanticUiNodeRecord;

typedef struct XgSemanticUiGlyphRunRecord {
    uint64_t glyph_run_id;
    uint64_t node_id;
    XgRenderUiOwnerDomain owner_domain;
    uint32_t owner_root;
    uint64_t owner_receipt;
    XgSemanticOrderKey order;
    XgRenderResourceHandle atlas;
    XgSemanticUiRect bounds;
    XgSemanticUiRect clip;
    uint32_t placement_offset;
    uint32_t placement_count;
    uint32_t reveal_count;
    uint32_t total_count;
    XgSemanticUiTemporalMode temporal_mode;
    bool clip_enabled;
} XgSemanticUiGlyphRunRecord;

typedef enum XgSemanticUiGlyphLayer {
    XG_SEMANTIC_UI_GLYPH_PRIMARY = 0,
    XG_SEMANTIC_UI_GLYPH_SHADOW = 1,
    XG_SEMANTIC_UI_GLYPH_LAYER_COUNT,
} XgSemanticUiGlyphLayer;

typedef struct XgSemanticUiGlyphPlacementRecord {
    uint64_t glyph_run_id;
    uint32_t glyph_id;
    int32_t x;
    int32_t y;
    uint16_t atlas_x;
    uint16_t atlas_y;
    uint16_t width;
    uint16_t height;
    uint32_t color;
    XgSemanticUiGlyphLayer layer;
    /* Source texels in unsigned 16.16, with exclusive right/bottom bounds.
     * x/y/width/height describe the destination, not the sampled extent.
     * atlas_x/y retain only the floored source origin. */
    XgRenderUiLayoutUvRect source_uv;
    /* Index in the decoded run, shared by primary/shadow. Clipped glyphs may
     * have no placements; reveal_count must not be counted from placements. */
    uint32_t glyph_index;
    /* Host CLUT555 bank in the owning node's clut: 16 entries for INDEX4,
     * 256 for INDEX8, in logical row-major texels using descriptor row_pitch
     * for byte addressing. Not a guest layout/address. RGB/RGBA require zero. */
    uint16_t palette_index;
} XgSemanticUiGlyphPlacementRecord;

#ifdef __cplusplus
}
#endif

#endif
