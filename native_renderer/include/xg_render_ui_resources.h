#ifndef XG_RENDER_UI_RESOURCES_H
#define XG_RENDER_UI_RESOURCES_H

#include "xg_render_resource_repository.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifndef XG_RENDER_FONT_CAPACITY
#define XG_RENDER_FONT_CAPACITY 8u
#endif

#ifndef XG_RENDER_FONT_GLYPH_CAPACITY
#define XG_RENDER_FONT_GLYPH_CAPACITY 256u
#endif

#ifndef XG_RENDER_GLYPH_RUN_CAPACITY
#define XG_RENDER_GLYPH_RUN_CAPACITY 256u
#endif

#ifdef __cplusplus
extern "C" {
#endif

typedef struct XgRenderGlyphMetric {
    uint32_t glyph_id;
    uint16_t atlas_x;
    uint16_t atlas_y;
    uint16_t width;
    uint16_t height;
    int16_t bearing_x;
    int16_t bearing_y;
    int16_t advance_x;
} XgRenderGlyphMetric;

typedef enum XgRenderUiOwnerDomain {
    XG_RENDER_UI_OWNER_NONE = 0,
    XG_RENDER_UI_OWNER_RESIDENT = 1,
    XG_RENDER_UI_OWNER_FIELD_DIALOGUE = 2,
    XG_RENDER_UI_OWNER_FIELD_CREDITS_GENERATED_GLYPH_CACHE = 3,
    XG_RENDER_UI_OWNER_BATTLE_FONT = 4,
    XG_RENDER_UI_OWNER_BATTLE_UI_ATLAS_PORTRAITS = 5,
    XG_RENDER_UI_OWNER_BATTLING_MENU = 6,
    XG_RENDER_UI_OWNER_GENERAL_MENU = 7,
    XG_RENDER_UI_OWNER_MEMBER_CHANGE = 8,
    XG_RENDER_UI_OWNER_ENTER_NAME = 9,
    XG_RENDER_UI_OWNER_SHOP = 10,
    XG_RENDER_UI_OWNER_GEAR_SHOP = 11,
    XG_RENDER_UI_OWNER_GEAR_HELPER = 12,
    XG_RENDER_UI_OWNER_COUNT = 13,
} XgRenderUiOwnerDomain;

typedef enum XgRenderUiResourceLifecycle {
    XG_RENDER_UI_LIFECYCLE_NONE = 0,
    XG_RENDER_UI_LIFECYCLE_ARTIFACT_PERSISTENT = 1,
    XG_RENDER_UI_LIFECYCLE_SCENE_TRANSIENT = 2,
} XgRenderUiResourceLifecycle;

typedef enum XgRenderUiResourceKind {
    XG_RENDER_UI_RESOURCE_NONE = 0,
    XG_RENDER_UI_RESOURCE_WINDOW = 1,
    XG_RENDER_UI_RESOURCE_CURSOR = 2,
    XG_RENDER_UI_RESOURCE_GAUGE = 3,
    XG_RENDER_UI_RESOURCE_PORTRAIT = 4,
    XG_RENDER_UI_RESOURCE_DIALOGUE_TEXT = 5,
    XG_RENDER_UI_RESOURCE_GENERATED_TEXT = 6,
} XgRenderUiResourceKind;

typedef struct XgRenderUiResourceRecord {
    XgRenderResourceHandle handle;
    XgRenderUiOwnerDomain owner_domain;
    uint32_t owner_root;
    XgRenderUiResourceLifecycle lifecycle;
    XgRenderUiResourceKind kind;
    uint64_t provenance_receipt;
} XgRenderUiResourceRecord;

typedef struct XgRenderUiWindowDescriptor {
    XgRenderUiResourceRecord resource;
} XgRenderUiWindowDescriptor;

typedef struct XgRenderUiCursorDescriptor {
    XgRenderUiResourceRecord resource;
} XgRenderUiCursorDescriptor;

typedef struct XgRenderUiGaugeDescriptor {
    XgRenderUiResourceRecord resource;
} XgRenderUiGaugeDescriptor;

typedef struct XgRenderUiPortraitDescriptor {
    XgRenderUiResourceRecord resource;
} XgRenderUiPortraitDescriptor;

typedef struct XgRenderUiDialogueTextReference {
    XgRenderUiResourceRecord resource;
} XgRenderUiDialogueTextReference;

typedef struct XgRenderUiGeneratedTextReference {
    XgRenderUiResourceRecord resource;
} XgRenderUiGeneratedTextReference;

typedef struct XgRenderFontImport {
    uint64_t font_id;
    uint64_t generation;
    XgRenderResourceHandle atlas;
    XgRenderUiOwnerDomain owner_domain;
    uint32_t owner_root;
    XgRenderUiResourceLifecycle lifecycle;
    uint64_t provenance_receipt;
    const XgRenderGlyphMetric *metrics;
    size_t metric_count;
    int16_t line_height;
} XgRenderFontImport;

typedef struct XgRenderGlyphPlacement {
    uint32_t glyph_id;
    int32_t x;
    int32_t y;
    uint16_t atlas_x;
    uint16_t atlas_y;
    uint16_t width;
    uint16_t height;
    uint32_t color;
} XgRenderGlyphPlacement;

typedef struct XgRenderGlyphRunRequest {
    uint64_t font_id;
    uint64_t font_generation;
    XgRenderUiOwnerDomain owner_domain;
    uint32_t owner_root;
    uint64_t provenance_receipt;
    const uint32_t *glyph_ids;
    size_t glyph_count;
    size_t reveal_count;
    int32_t origin_x;
    int32_t origin_y;
    int32_t max_width;
    uint32_t color;
    bool multiline;
} XgRenderGlyphRunRequest;

typedef struct XgRenderGlyphRun {
    XgRenderResourceHandle atlas;
    XgRenderUiOwnerDomain owner_domain;
    uint32_t owner_root;
    XgRenderUiResourceLifecycle lifecycle;
    uint64_t provenance_receipt;
    XgRenderGlyphPlacement glyphs[XG_RENDER_GLYPH_RUN_CAPACITY];
    size_t glyph_count;
    int32_t width;
    int32_t height;
    uint64_t digest;
} XgRenderGlyphRun;

typedef enum XgRenderUiResult {
    XG_RENDER_UI_OK = 0,
    XG_RENDER_UI_INVALID_ARGUMENT,
    XG_RENDER_UI_CAPACITY_EXCEEDED,
    XG_RENDER_UI_FONT_NOT_FOUND,
    XG_RENDER_UI_GLYPH_NOT_FOUND,
    XG_RENDER_UI_STALE_RESOURCE,
    XG_RENDER_UI_INVALID_CHECKPOINT,
    XG_RENDER_UI_RESOURCE_FAILED,
    XG_RENDER_UI_INVALID_OWNER,
    XG_RENDER_UI_INVALID_PROVENANCE,
    XG_RENDER_UI_OWNER_MISMATCH,
    XG_RENDER_UI_RESOURCE_KIND_MISMATCH,
    XG_RENDER_UI_INVALID_LIFECYCLE,
} XgRenderUiResult;

typedef struct XgRenderUiCheckpointRestore XgRenderUiCheckpointRestore;

bool xg_render_ui_owner_domain_valid(XgRenderUiOwnerDomain owner_domain);
/* Scene resources bind to scene_generation. Persistent module resources bind
 * to their registered module generation and the owner's artifact SHA prefix
 * (first eight bytes decoded little-endian, as in runtime scene identities).
 * Other lifetime domains need additional identity not carried by UI scenes. */
bool xg_render_ui_resource_scope_valid(
    const XgRenderResourceView *view, uint32_t scene_generation,
    uint64_t owner_receipt, uint64_t owner_artifact_identity);
XgRenderUiResult xg_render_ui_resource_record_validate(
    const XgRenderUiResourceRecord *record,
    XgRenderUiOwnerDomain expected_owner_domain,
    XgRenderUiResourceKind expected_kind);
void xg_render_ui_resources_reset(void);
void xg_render_ui_resources_scene_boundary(uint64_t scene_generation);
XgRenderUiResult xg_render_font_import(const XgRenderFontImport *import);
XgRenderUiResult xg_render_glyph_run_build(
    const XgRenderGlyphRunRequest *request,
    XgRenderGlyphRun *out_run);
size_t xg_render_ui_resources_checkpoint_size(void);
XgRenderUiResult xg_render_ui_resources_checkpoint_write(
    void *out_checkpoint, size_t checkpoint_size);
XgRenderUiResult xg_render_ui_resources_checkpoint_validate(
    const void *checkpoint, size_t checkpoint_size);
XgRenderUiResult xg_render_ui_resources_checkpoint_restore(
    const void *checkpoint, size_t checkpoint_size);
XgRenderUiResult xg_render_ui_resources_checkpoint_prepare(
    const void *checkpoint, size_t checkpoint_size,
    uint64_t restored_owner_generation,
    XgRenderUiCheckpointRestore **out_restore);
void xg_render_ui_resources_checkpoint_commit(
    XgRenderUiCheckpointRestore *restore);
void xg_render_ui_resources_checkpoint_cancel(
    XgRenderUiCheckpointRestore *restore);

#ifdef __cplusplus
}
#endif

#endif
