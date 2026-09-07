#include "xg_render_ui_resources.h"
#include "xg_render_ui_owner_catalog.h"

#include <limits.h>
#include <stdlib.h>
#include <string.h>

typedef struct XgRenderFontEntry {
    uint64_t font_id;
    uint64_t generation;
    XgRenderResourceHandle atlas;
    XgRenderUiOwnerDomain owner_domain;
    uint32_t owner_root;
    XgRenderUiResourceLifecycle lifecycle;
    uint64_t provenance_receipt;
    XgRenderGlyphMetric metrics[XG_RENDER_FONT_GLYPH_CAPACITY];
    size_t metric_count;
    int16_t line_height;
    bool occupied;
} XgRenderFontEntry;

static XgRenderFontEntry g_fonts[XG_RENDER_FONT_CAPACITY];
static uint64_t g_scene_owner_generation;

enum {
    XG_RENDER_UI_CHECKPOINT_MAGIC = 0x49554758u,
    /* v6 omitted atlas interpretation. Nonempty v6 checkpoints cannot be
     * recovered from bytes or metrics; empty v6 sections remain lossless. */
    XG_RENDER_UI_CHECKPOINT_VERSION = 7u,
    XG_RENDER_UI_CHECKPOINT_HEADER_SIZE = 24u,
    XG_RENDER_UI_CHECKPOINT_ENTRY_SIZE =
        148u + XG_RENDER_RESOURCE_CAPABILITY_CHECKPOINT_SIZE + 13u * 4u,
    XG_RENDER_UI_CHECKPOINT_METRIC_SIZE = 20u,
};

typedef struct XgRenderUiCheckpointFont {
    uint64_t font_id;
    uint64_t generation;
    XgRenderUiOwnerDomain owner_domain;
    uint32_t owner_root;
    XgRenderUiResourceLifecycle lifecycle;
    uint64_t provenance_receipt;
    uint64_t atlas_resource_id;
    uint64_t atlas_owner_generation;
    uint64_t atlas_digest;
    uint64_t atlas_byte_count;
    XgRenderResourceIdentity atlas_identity;
    XgRenderResourceOwnerKind atlas_owner_kind;
    XgRenderResourceState atlas_state;
    XgRenderResourceProvenance atlas_provenance;
    XgRenderResourceProvenance restored_atlas_provenance;
    XgRenderResourceCapabilityCheckpoint atlas_authority;
    XgRenderResourceDescriptor atlas_descriptor;
    XgRenderGlyphMetric metrics[XG_RENDER_FONT_GLYPH_CAPACITY];
    const uint8_t *atlas_bytes;
    uint32_t metric_count;
    int16_t line_height;
    bool atlas_has_identity;
} XgRenderUiCheckpointFont;

struct XgRenderUiCheckpointRestore {
    XgRenderFontEntry fonts[XG_RENDER_FONT_CAPACITY];
    uint64_t scene_owner_generation;
    uint32_t font_count;
    bool has_scene_transient;
};

static void ui_write_u16(uint8_t **cursor, uint16_t value) {
    (*cursor)[0] = (uint8_t)value;
    (*cursor)[1] = (uint8_t)(value >> 8u);
    *cursor += 2u;
}

static void ui_write_u32(uint8_t **cursor, uint32_t value) {
    for (uint32_t index = 0u; index < 4u; ++index)
        (*cursor)[index] = (uint8_t)(value >> (index * 8u));
    *cursor += 4u;
}

static void ui_write_u64(uint8_t **cursor, uint64_t value) {
    for (uint32_t index = 0u; index < 8u; ++index)
        (*cursor)[index] = (uint8_t)(value >> (index * 8u));
    *cursor += 8u;
}

static uint16_t ui_read_u16(const uint8_t **cursor) {
    uint16_t value = (uint16_t)(*cursor)[0] |
        (uint16_t)(*cursor)[1] << 8u;
    *cursor += 2u;
    return value;
}

static uint32_t ui_read_u32(const uint8_t **cursor) {
    uint32_t value = 0u;
    for (uint32_t index = 0u; index < 4u; ++index)
        value |= (uint32_t)(*cursor)[index] << (index * 8u);
    *cursor += 4u;
    return value;
}

static uint64_t ui_read_u64(const uint8_t **cursor) {
    uint64_t value = 0u;
    for (uint32_t index = 0u; index < 8u; ++index)
        value |= (uint64_t)(*cursor)[index] << (index * 8u);
    *cursor += 8u;
    return value;
}

static bool ui_identity_is_zero(const XgRenderResourceIdentity *identity) {
    for (size_t index = 0u; index < sizeof(identity->bytes); ++index)
        if (identity->bytes[index] != 0u) return false;
    return true;
}

static bool ui_atlas_descriptor_valid(
        const XgRenderResourceDescriptor *descriptor, size_t byte_count) {
    uint64_t minimum_pitch;

    if (!xg_render_resource_descriptor_validate(descriptor) ||
        descriptor->version != XG_RENDER_RESOURCE_DESCRIPTOR_VERSION)
        return false;
    switch (descriptor->pixel_format) {
    case XG_RENDER_RESOURCE_PIXEL_FORMAT_RGBA8:
        minimum_pitch = (uint64_t)descriptor->width * 4u; break;
    case XG_RENDER_RESOURCE_PIXEL_FORMAT_RGB24:
        minimum_pitch = (uint64_t)descriptor->width * 3u; break;
    case XG_RENDER_RESOURCE_PIXEL_FORMAT_RGB555:
        minimum_pitch = (uint64_t)descriptor->width * 2u; break;
    case XG_RENDER_RESOURCE_PIXEL_FORMAT_INDEX4:
        minimum_pitch = ((uint64_t)descriptor->width + 1u) / 2u; break;
    case XG_RENDER_RESOURCE_PIXEL_FORMAT_INDEX8:
        minimum_pitch = descriptor->width; break;
    default:
        return false;
    }
    return descriptor->row_pitch >= minimum_pitch &&
        descriptor->row_pitch != 0u &&
        descriptor->height <= byte_count / descriptor->row_pitch;
}

bool xg_render_ui_owner_domain_valid(XgRenderUiOwnerDomain owner_domain) {
    return owner_domain > XG_RENDER_UI_OWNER_NONE &&
        owner_domain < XG_RENDER_UI_OWNER_COUNT;
}

bool xg_render_ui_resource_scope_valid(
        const XgRenderResourceView *view, uint32_t scene_generation,
        uint64_t owner_receipt, uint64_t owner_artifact_identity) {
    XgRenderResourceCapabilityMetadata metadata;
    const XgRenderArtifactIdentity *artifact;
    uint64_t artifact_prefix = 0u;

    if (view == NULL || !view->current || view->owner_generation == 0u ||
        view->state != XG_RENDER_RESOURCE_NATIVE_OWNED ||
        scene_generation == 0u || owner_receipt == 0u ||
        (view->owner_kind != XG_RENDER_RESOURCE_OWNER_SCENE &&
         view->owner_kind != XG_RENDER_RESOURCE_OWNER_MODULE))
        return false;
    if (view->owner_kind == XG_RENDER_RESOURCE_OWNER_SCENE &&
        view->owner_generation != scene_generation)
        return false;
    if (view->provenance.synthetic)
        return view->owner_kind == XG_RENDER_RESOURCE_OWNER_SCENE &&
            view->provenance.kind == XG_RENDER_RESOURCE_PROVENANCE_NONE &&
            view->provenance.receipt == 0u && view->provenance.capability == 0u;
    if (view->provenance.receipt != owner_receipt ||
        xg_render_resource_capability_validate(
            &view->provenance, view->owner_kind, view->owner_generation,
            &metadata) != XG_RENDER_RESOURCE_CAPABILITY_OK ||
        metadata.lifetime > (view->owner_kind == XG_RENDER_RESOURCE_OWNER_MODULE
            ? XG_RENDER_RESOURCE_CAPABILITY_MODULE
            : XG_RENDER_RESOURCE_CAPABILITY_SCENE))
        return false;
    if (view->owner_kind == XG_RENDER_RESOURCE_OWNER_SCENE) return true;
    artifact = metadata.kind == XG_RENDER_RESOURCE_PROVENANCE_ARTIFACT
        ? &metadata.artifact : &metadata.source.origin_artifact;
    for (uint32_t index = 0u; index < 8u; ++index)
        artifact_prefix |= (uint64_t)artifact->sha256[index] << (index * 8u);
    return artifact->size != 0u && owner_artifact_identity != 0u &&
        artifact_prefix == owner_artifact_identity;
}

static bool artifact_identity_equal(const XgRenderArtifactIdentity *left,
                                     const XgRenderArtifactIdentity *right) {
    return left->base == right->base && left->size == right->size &&
        memcmp(left->sha256, right->sha256, sizeof(left->sha256)) == 0;
}

bool xg_render_ui_owner_catalog_artifact_identity(
        uint32_t base, uint32_t size,
        const uint8_t sha256[XG_RENDER_RESOURCE_IDENTITY_SIZE],
        XgRenderArtifactIdentity *out_identity) {
    const XgRenderUiOwnerCatalogEntry *match = NULL;

    if (size == 0u || sha256 == NULL || out_identity == NULL) return false;
    for (size_t index = 0u; index < xg_render_ui_owner_catalog_count; ++index) {
        const XgRenderUiOwnerCatalogEntry *entry =
            &xg_render_ui_owner_catalog[index];

        if ((entry->artifact.base & UINT32_C(0x1fffffff)) !=
             (base & UINT32_C(0x1fffffff)) ||
            entry->artifact.size != size ||
            memcmp(entry->artifact.sha256, sha256, sizeof(entry->artifact.sha256)) != 0)
            continue;
        if (match != NULL && !artifact_identity_equal(
                &match->artifact, &entry->artifact))
            return false;
        match = entry;
    }
    if (match == NULL) return false;
    *out_identity = match->artifact;
    return true;
}

static bool ui_owner_metadata_matches(
        XgRenderUiOwnerDomain owner_domain, uint32_t owner_root,
        const XgRenderResourceCapabilityMetadata *metadata) {
    const XgRenderArtifactIdentity *artifact;

    if (metadata == NULL || owner_root == 0u) return false;
    artifact = metadata->kind == XG_RENDER_RESOURCE_PROVENANCE_ARTIFACT
        ? &metadata->artifact : &metadata->source.origin_artifact;
    for (size_t index = 0u; index < xg_render_ui_owner_catalog_count; ++index) {
        const XgRenderUiOwnerCatalogEntry *entry =
            &xg_render_ui_owner_catalog[index];

        if (entry->owner_domain == (uint32_t)owner_domain &&
            entry->root_address == owner_root &&
            artifact_identity_equal(&entry->artifact, artifact))
            return true;
    }
    return false;
}

static bool ui_owner_capability_matches(
        XgRenderUiOwnerDomain owner_domain, uint32_t owner_root,
        const XgRenderResourceView *view) {
    XgRenderResourceCapabilityMetadata metadata;

    return view != NULL &&
        xg_render_resource_capability_validate(
            &view->provenance, view->owner_kind, view->owner_generation,
            &metadata) == XG_RENDER_RESOURCE_CAPABILITY_OK &&
        ui_owner_metadata_matches(owner_domain, owner_root, &metadata);
}

static bool ui_resource_kind_valid(XgRenderUiResourceKind kind) {
    return kind >= XG_RENDER_UI_RESOURCE_WINDOW &&
        kind <= XG_RENDER_UI_RESOURCE_GENERATED_TEXT;
}

static XgRenderResourceKind ui_repository_kind(
        XgRenderUiResourceKind kind) {
    if (kind == XG_RENDER_UI_RESOURCE_DIALOGUE_TEXT)
        return XG_RENDER_RESOURCE_GLYPH_ATLAS;
    if (kind == XG_RENDER_UI_RESOURCE_GENERATED_TEXT)
        return XG_RENDER_RESOURCE_GENERATED_SURFACE;
    return XG_RENDER_RESOURCE_TEXTURE;
}

static XgRenderResourceOwnerKind ui_repository_owner(
        XgRenderUiResourceLifecycle lifecycle) {
    return lifecycle == XG_RENDER_UI_LIFECYCLE_ARTIFACT_PERSISTENT
        ? XG_RENDER_RESOURCE_OWNER_MODULE : XG_RENDER_RESOURCE_OWNER_SCENE;
}

static XgRenderResourceProvenanceKind ui_repository_provenance(
        XgRenderUiResourceLifecycle lifecycle) {
    return lifecycle == XG_RENDER_UI_LIFECYCLE_ARTIFACT_PERSISTENT
        ? XG_RENDER_RESOURCE_PROVENANCE_ARTIFACT
        : XG_RENDER_RESOURCE_PROVENANCE_SOURCE;
}

static bool ui_view_provenance_matches(
        const XgRenderResourceView *view,
        XgRenderUiResourceLifecycle lifecycle, uint64_t receipt) {
    return view->provenance.kind == ui_repository_provenance(lifecycle) &&
        view->provenance.receipt == receipt && !view->provenance.synthetic;
}

static bool ui_provenance_matches(
        const XgRenderResourceProvenance *provenance,
        XgRenderUiResourceLifecycle lifecycle, uint64_t receipt) {
    return provenance->kind == ui_repository_provenance(lifecycle) &&
        provenance->receipt == receipt && !provenance->synthetic;
}

XgRenderUiResult xg_render_ui_resource_record_validate(
        const XgRenderUiResourceRecord *record,
        XgRenderUiOwnerDomain expected_owner_domain,
        XgRenderUiResourceKind expected_kind) {
    XgRenderResourceView view;

    if (record == NULL || !xg_render_ui_owner_domain_valid(
            expected_owner_domain) || !xg_render_ui_owner_domain_valid(
            record->owner_domain))
        return XG_RENDER_UI_INVALID_OWNER;
    if (record->provenance_receipt == 0u || record->owner_root == 0u)
        return XG_RENDER_UI_INVALID_PROVENANCE;
    if (record->owner_domain != expected_owner_domain)
        return XG_RENDER_UI_OWNER_MISMATCH;
    if (!ui_resource_kind_valid(expected_kind) ||
        !ui_resource_kind_valid(record->kind) || record->kind != expected_kind)
        return XG_RENDER_UI_RESOURCE_KIND_MISMATCH;
    if (record->lifecycle != XG_RENDER_UI_LIFECYCLE_ARTIFACT_PERSISTENT &&
        record->lifecycle != XG_RENDER_UI_LIFECYCLE_SCENE_TRANSIENT)
        return XG_RENDER_UI_INVALID_LIFECYCLE;
    if (record->kind == XG_RENDER_UI_RESOURCE_GENERATED_TEXT &&
        record->lifecycle != XG_RENDER_UI_LIFECYCLE_SCENE_TRANSIENT)
        return XG_RENDER_UI_INVALID_LIFECYCLE;
    if (xg_render_resource_view(record->handle, &view) !=
            XG_RENDER_RESOURCE_OK || !view.current)
        return XG_RENDER_UI_STALE_RESOURCE;
    if (view.kind != ui_repository_kind(record->kind))
        return XG_RENDER_UI_RESOURCE_KIND_MISMATCH;
    if (view.owner_kind != ui_repository_owner(record->lifecycle))
        return XG_RENDER_UI_INVALID_LIFECYCLE;
    if (!ui_view_provenance_matches(
            &view, record->lifecycle, record->provenance_receipt))
        return XG_RENDER_UI_INVALID_PROVENANCE;
    if (!ui_owner_capability_matches(
            record->owner_domain, record->owner_root, &view))
        return XG_RENDER_UI_OWNER_MISMATCH;
    if (view.state != XG_RENDER_RESOURCE_NATIVE_OWNED)
        return XG_RENDER_UI_STALE_RESOURCE;
    return XG_RENDER_UI_OK;
}

static const XgRenderFontEntry *find_font(uint64_t font_id, uint64_t generation) {
    size_t index;
    for (index = 0; index < XG_RENDER_FONT_CAPACITY; index++) {
        if (g_fonts[index].occupied && g_fonts[index].font_id == font_id &&
            g_fonts[index].generation == generation)
            return &g_fonts[index];
    }
    return NULL;
}

static const XgRenderGlyphMetric *find_glyph(const XgRenderFontEntry *font,
                                             uint32_t glyph_id) {
    size_t index;
    for (index = 0; index < font->metric_count; index++) {
        if (font->metrics[index].glyph_id == glyph_id)
            return &font->metrics[index];
    }
    return NULL;
}

void xg_render_ui_resources_reset(void) {
    size_t index;
    for (index = 0; index < XG_RENDER_FONT_CAPACITY; index++) {
        if (g_fonts[index].occupied)
            (void)xg_render_resource_release(g_fonts[index].atlas);
    }
    memset(g_fonts, 0, sizeof(g_fonts));
    g_scene_owner_generation = 0u;
}

void xg_render_ui_resources_scene_boundary(uint64_t scene_generation) {
    for (size_t index = 0u; index < XG_RENDER_FONT_CAPACITY; ++index) {
        XgRenderFontEntry *font = &g_fonts[index];

        if (!font->occupied ||
            font->lifecycle != XG_RENDER_UI_LIFECYCLE_SCENE_TRANSIENT)
            continue;
        (void)xg_render_resource_release(font->atlas);
        memset(font, 0, sizeof(*font));
    }
    g_scene_owner_generation = scene_generation;
}

XgRenderUiResult xg_render_font_import(const XgRenderFontImport *import) {
    XgRenderResourceView atlas;
    XgRenderFontEntry *available = NULL;
    size_t index;
    if (import == NULL || import->font_id == 0u || import->generation == 0u ||
        !xg_render_ui_owner_domain_valid(import->owner_domain) ||
        import->owner_root == 0u || import->provenance_receipt == 0u ||
        (import->lifecycle != XG_RENDER_UI_LIFECYCLE_ARTIFACT_PERSISTENT &&
         import->lifecycle != XG_RENDER_UI_LIFECYCLE_SCENE_TRANSIENT) ||
        import->metrics == NULL || import->metric_count == 0u ||
        import->metric_count > XG_RENDER_FONT_GLYPH_CAPACITY ||
        import->line_height <= 0)
        return XG_RENDER_UI_INVALID_ARGUMENT;
    for (index = 0u; index < import->metric_count; ++index) {
        if (import->metrics[index].glyph_id == 0u)
            return XG_RENDER_UI_INVALID_ARGUMENT;
        for (size_t previous = 0u; previous < index; ++previous)
            if (import->metrics[previous].glyph_id ==
                import->metrics[index].glyph_id)
                return XG_RENDER_UI_INVALID_ARGUMENT;
    }
    if (xg_render_resource_retain(import->atlas) != XG_RENDER_RESOURCE_OK)
        return XG_RENDER_UI_STALE_RESOURCE;
    if (xg_render_resource_view(import->atlas, &atlas) != XG_RENDER_RESOURCE_OK ||
        !atlas.current || atlas.kind != XG_RENDER_RESOURCE_GLYPH_ATLAS ||
        !ui_atlas_descriptor_valid(&atlas.descriptor, atlas.byte_count) ||
        atlas.state != XG_RENDER_RESOURCE_NATIVE_OWNED ||
        atlas.owner_kind != ui_repository_owner(import->lifecycle) ||
        !ui_view_provenance_matches(
            &atlas, import->lifecycle, import->provenance_receipt) ||
        !ui_owner_capability_matches(
            import->owner_domain, import->owner_root, &atlas)) {
        (void)xg_render_resource_release(import->atlas);
        return XG_RENDER_UI_STALE_RESOURCE;
    }
    for (index = 0u; index < import->metric_count; ++index) {
        const XgRenderGlyphMetric *metric = &import->metrics[index];
        if (metric->width == 0u || metric->height == 0u ||
            (uint32_t)metric->atlas_x + metric->width > atlas.descriptor.width ||
            (uint32_t)metric->atlas_y + metric->height > atlas.descriptor.height) {
            (void)xg_render_resource_release(import->atlas);
            return XG_RENDER_UI_INVALID_ARGUMENT;
        }
    }
    for (index = 0; index < XG_RENDER_FONT_CAPACITY; index++) {
        if (g_fonts[index].occupied &&
            g_fonts[index].font_id == import->font_id) {
            available = &g_fonts[index];
            break;
        }
        if (!g_fonts[index].occupied && available == NULL)
            available = &g_fonts[index];
    }
    if (available == NULL) {
        (void)xg_render_resource_release(import->atlas);
        return XG_RENDER_UI_CAPACITY_EXCEEDED;
    }
    if (available->occupied && available->generation >= import->generation) {
        (void)xg_render_resource_release(import->atlas);
        return XG_RENDER_UI_INVALID_ARGUMENT;
    }
    if (available->occupied)
        (void)xg_render_resource_release(available->atlas);
    memset(available, 0, sizeof(*available));
    available->font_id = import->font_id;
    available->generation = import->generation;
    available->atlas = import->atlas;
    available->owner_domain = import->owner_domain;
    available->owner_root = import->owner_root;
    available->lifecycle = import->lifecycle;
    available->provenance_receipt = import->provenance_receipt;
    memcpy(available->metrics, import->metrics,
           import->metric_count * sizeof(import->metrics[0]));
    available->metric_count = import->metric_count;
    available->line_height = import->line_height;
    available->occupied = true;
    return XG_RENDER_UI_OK;
}

XgRenderUiResult xg_render_glyph_run_build(
        const XgRenderGlyphRunRequest *request, XgRenderGlyphRun *out_run) {
    const XgRenderFontEntry *font;
    XgRenderResourceView atlas;
    size_t visible_count;
    size_t index;
    int64_t pen_x;
    int64_t pen_y;
    int64_t max_x;

    if (request == NULL || out_run == NULL || request->glyph_ids == NULL ||
        request->glyph_count > XG_RENDER_GLYPH_RUN_CAPACITY ||
        request->reveal_count > request->glyph_count)
        return XG_RENDER_UI_INVALID_ARGUMENT;
    font = find_font(request->font_id, request->font_generation);
    if (font == NULL) return XG_RENDER_UI_FONT_NOT_FOUND;
    if (!xg_render_ui_owner_domain_valid(request->owner_domain))
        return XG_RENDER_UI_INVALID_OWNER;
    if (request->provenance_receipt == 0u)
        return XG_RENDER_UI_INVALID_PROVENANCE;
    if (font->owner_domain != request->owner_domain ||
        font->owner_root != request->owner_root ||
        font->provenance_receipt != request->provenance_receipt)
        return XG_RENDER_UI_OWNER_MISMATCH;
    if (xg_render_resource_view(font->atlas, &atlas) != XG_RENDER_RESOURCE_OK ||
        !atlas.current || atlas.kind != XG_RENDER_RESOURCE_GLYPH_ATLAS ||
        atlas.state != XG_RENDER_RESOURCE_NATIVE_OWNED ||
        atlas.owner_kind != ui_repository_owner(font->lifecycle) ||
        !ui_view_provenance_matches(
            &atlas, font->lifecycle, font->provenance_receipt))
        return XG_RENDER_UI_STALE_RESOURCE;
    memset(out_run, 0, sizeof(*out_run));
    out_run->atlas = font->atlas;
    out_run->owner_domain = font->owner_domain;
    out_run->owner_root = font->owner_root;
    out_run->lifecycle = font->lifecycle;
    out_run->provenance_receipt = font->provenance_receipt;
    visible_count = request->reveal_count;
    pen_x = request->origin_x;
    pen_y = request->origin_y;
    max_x = request->origin_x;
    for (index = 0; index < visible_count; index++) {
        const XgRenderGlyphMetric *metric = find_glyph(font,
                                                       request->glyph_ids[index]);
        XgRenderGlyphPlacement *placement;
        if (metric == NULL) return XG_RENDER_UI_GLYPH_NOT_FOUND;
        if (request->multiline && request->max_width > 0 &&
            pen_x != request->origin_x &&
            pen_x + metric->advance_x - request->origin_x > request->max_width) {
            pen_x = request->origin_x;
            pen_y += font->line_height;
        }
        if (pen_x + metric->bearing_x < INT32_MIN ||
            pen_x + metric->bearing_x > INT32_MAX ||
            pen_y + metric->bearing_y < INT32_MIN ||
            pen_y + metric->bearing_y > INT32_MAX)
            return XG_RENDER_UI_INVALID_ARGUMENT;
        placement = &out_run->glyphs[out_run->glyph_count++];
        *placement = (XgRenderGlyphPlacement){
            .glyph_id = metric->glyph_id,
            .x = (int32_t)(pen_x + metric->bearing_x),
            .y = (int32_t)(pen_y + metric->bearing_y),
            .atlas_x = metric->atlas_x,
            .atlas_y = metric->atlas_y,
            .width = metric->width,
            .height = metric->height,
            .color = request->color,
        };
        pen_x += metric->advance_x;
        if (pen_x > max_x) max_x = pen_x;
    }
    if (max_x - request->origin_x < INT32_MIN ||
        max_x - request->origin_x > INT32_MAX ||
        (visible_count != 0u &&
         (pen_y - request->origin_y + font->line_height < INT32_MIN ||
          pen_y - request->origin_y + font->line_height > INT32_MAX)))
        return XG_RENDER_UI_INVALID_ARGUMENT;
    out_run->width = (int32_t)(max_x - request->origin_x);
    out_run->height = visible_count == 0u ? 0 : (int32_t)(
        pen_y - request->origin_y + font->line_height);
    out_run->digest = xg_render_resource_digest(
        out_run->glyphs, out_run->glyph_count * sizeof(out_run->glyphs[0]));
    return XG_RENDER_UI_OK;
}

size_t xg_render_ui_resources_checkpoint_size(void) {
    size_t size = XG_RENDER_UI_CHECKPOINT_HEADER_SIZE;

    for (size_t index = 0u; index < XG_RENDER_FONT_CAPACITY; ++index) {
        XgRenderResourceView atlas;
        const XgRenderFontEntry *font = &g_fonts[index];
        size_t metrics_size;
        if (!font->occupied) continue;
        if (xg_render_resource_view(font->atlas, &atlas) !=
                XG_RENDER_RESOURCE_OK || !atlas.current ||
             atlas.kind != XG_RENDER_RESOURCE_GLYPH_ATLAS ||
             atlas.state != XG_RENDER_RESOURCE_NATIVE_OWNED ||
             !ui_atlas_descriptor_valid(&atlas.descriptor, atlas.byte_count) ||
            atlas.owner_kind != ui_repository_owner(font->lifecycle) ||
            !ui_view_provenance_matches(
                &atlas, font->lifecycle, font->provenance_receipt) ||
            font->metric_count > SIZE_MAX / XG_RENDER_UI_CHECKPOINT_METRIC_SIZE)
            return 0u;
        metrics_size = font->metric_count * XG_RENDER_UI_CHECKPOINT_METRIC_SIZE;
        if (XG_RENDER_UI_CHECKPOINT_ENTRY_SIZE > SIZE_MAX - size ||
            metrics_size > SIZE_MAX - size - XG_RENDER_UI_CHECKPOINT_ENTRY_SIZE ||
            atlas.byte_count > SIZE_MAX - size -
                XG_RENDER_UI_CHECKPOINT_ENTRY_SIZE - metrics_size)
            return 0u;
        size += XG_RENDER_UI_CHECKPOINT_ENTRY_SIZE + metrics_size +
            atlas.byte_count;
    }
    return size;
}

XgRenderUiResult xg_render_ui_resources_checkpoint_write(
        void *out_checkpoint, size_t checkpoint_size) {
    const size_t required = xg_render_ui_resources_checkpoint_size();
    uint8_t *cursor = (uint8_t *)out_checkpoint;
    uint32_t font_count = 0u;

    if (required == 0u || out_checkpoint == NULL || checkpoint_size != required)
        return XG_RENDER_UI_INVALID_ARGUMENT;
    for (size_t index = 0u; index < XG_RENDER_FONT_CAPACITY; ++index)
        if (g_fonts[index].occupied) font_count++;
    ui_write_u32(&cursor, XG_RENDER_UI_CHECKPOINT_MAGIC);
    ui_write_u32(&cursor, XG_RENDER_UI_CHECKPOINT_VERSION);
    ui_write_u64(&cursor, required);
    ui_write_u32(&cursor, font_count);
    ui_write_u32(&cursor, 0u);
    for (size_t index = 0u; index < XG_RENDER_FONT_CAPACITY; ++index) {
        const XgRenderFontEntry *font = &g_fonts[index];
        XgRenderResourceView atlas;
        if (!font->occupied) continue;
        if (xg_render_resource_view(font->atlas, &atlas) !=
                XG_RENDER_RESOURCE_OK || !atlas.current ||
            atlas.kind != XG_RENDER_RESOURCE_GLYPH_ATLAS ||
            atlas.state != XG_RENDER_RESOURCE_NATIVE_OWNED ||
            atlas.owner_kind != ui_repository_owner(font->lifecycle) ||
            !ui_view_provenance_matches(
                &atlas, font->lifecycle, font->provenance_receipt))
            return XG_RENDER_UI_STALE_RESOURCE;
        ui_write_u64(&cursor, font->font_id);
        ui_write_u64(&cursor, font->generation);
        ui_write_u32(&cursor, (uint32_t)font->owner_domain);
        ui_write_u32(&cursor, (uint32_t)font->lifecycle);
        ui_write_u32(&cursor, font->owner_root);
        ui_write_u64(&cursor, font->provenance_receipt);
        ui_write_u32(&cursor, (uint32_t)(int32_t)font->line_height);
        ui_write_u32(&cursor, (uint32_t)font->metric_count);
        ui_write_u64(&cursor, atlas.handle.resource_id);
        memcpy(cursor, atlas.identity.bytes, sizeof(atlas.identity.bytes));
        cursor += sizeof(atlas.identity.bytes);
        ui_write_u32(&cursor, atlas.has_identity ? 1u : 0u);
        ui_write_u32(&cursor, (uint32_t)atlas.owner_kind);
        ui_write_u64(&cursor, atlas.owner_generation);
        ui_write_u32(&cursor, (uint32_t)atlas.state);
        ui_write_u32(&cursor, 0u);
        ui_write_u64(&cursor, atlas.content_digest);
        ui_write_u64(&cursor, atlas.byte_count);
        ui_write_u32(&cursor, (uint32_t)atlas.provenance.kind);
        ui_write_u32(&cursor, atlas.provenance.synthetic ? 1u : 0u);
        ui_write_u64(&cursor, atlas.provenance.receipt);
        ui_write_u64(&cursor, atlas.provenance.capability);
        {
            XgRenderResourceCapabilityCheckpoint authority;
            if (xg_render_resource_capability_checkpoint(
                    &atlas.provenance, atlas.owner_kind,
                    atlas.owner_generation, &authority) !=
                        XG_RENDER_RESOURCE_CAPABILITY_OK)
                return XG_RENDER_UI_STALE_RESOURCE;
            memcpy(cursor, authority.bytes, sizeof(authority.bytes));
            cursor += sizeof(authority.bytes);
        }
        ui_write_u32(&cursor, atlas.descriptor.version);
        ui_write_u32(&cursor, atlas.descriptor.pixel_format);
        ui_write_u32(&cursor, atlas.descriptor.width);
        ui_write_u32(&cursor, atlas.descriptor.height);
        ui_write_u32(&cursor, atlas.descriptor.row_pitch);
        ui_write_u32(&cursor, atlas.descriptor.vram_x);
        ui_write_u32(&cursor, atlas.descriptor.vram_y);
        ui_write_u32(&cursor, atlas.descriptor.vram_width);
        ui_write_u32(&cursor, atlas.descriptor.vram_height);
        ui_write_u32(&cursor, atlas.descriptor.sampler);
        ui_write_u32(&cursor, atlas.descriptor.wrap_u);
        ui_write_u32(&cursor, atlas.descriptor.wrap_v);
        ui_write_u32(&cursor, atlas.descriptor.flags);
        for (size_t metric_index = 0u;
             metric_index < font->metric_count; ++metric_index) {
            const XgRenderGlyphMetric *metric = &font->metrics[metric_index];
            ui_write_u32(&cursor, metric->glyph_id);
            ui_write_u16(&cursor, metric->atlas_x);
            ui_write_u16(&cursor, metric->atlas_y);
            ui_write_u16(&cursor, metric->width);
            ui_write_u16(&cursor, metric->height);
            ui_write_u16(&cursor, (uint16_t)metric->bearing_x);
            ui_write_u16(&cursor, (uint16_t)metric->bearing_y);
            ui_write_u16(&cursor, (uint16_t)metric->advance_x);
            ui_write_u16(&cursor, 0u);
        }
        memcpy(cursor, atlas.bytes, atlas.byte_count);
        cursor += atlas.byte_count;
    }
    return cursor == (uint8_t *)out_checkpoint + checkpoint_size
        ? XG_RENDER_UI_OK : XG_RENDER_UI_INVALID_CHECKPOINT;
}

static XgRenderUiResult ui_checkpoint_process(
        const void *checkpoint, size_t checkpoint_size,
        uint64_t restored_owner_generation,
        XgRenderUiCheckpointRestore **out_restore) {
    XgRenderUiCheckpointFont fonts[XG_RENDER_FONT_CAPACITY];
    const uint8_t *cursor = (const uint8_t *)checkpoint;
    const uint8_t *end;
    uint64_t checkpoint_scene_generation = 0u;
    uint32_t font_count;
    uint32_t version;

    if (checkpoint == NULL || restored_owner_generation == 0u ||
        checkpoint_size < XG_RENDER_UI_CHECKPOINT_HEADER_SIZE)
        return XG_RENDER_UI_INVALID_ARGUMENT;
    end = cursor + checkpoint_size;
    if (ui_read_u32(&cursor) != XG_RENDER_UI_CHECKPOINT_MAGIC)
        return XG_RENDER_UI_INVALID_CHECKPOINT;
    version = ui_read_u32(&cursor);
    if ((version != XG_RENDER_UI_CHECKPOINT_VERSION && version != 6u) ||
        ui_read_u64(&cursor) != checkpoint_size)
        return XG_RENDER_UI_INVALID_CHECKPOINT;
    font_count = ui_read_u32(&cursor);
    if (ui_read_u32(&cursor) != 0u || font_count > XG_RENDER_FONT_CAPACITY ||
        (version == 6u && font_count != 0u))
        return XG_RENDER_UI_INVALID_CHECKPOINT;
    memset(fonts, 0, sizeof(fonts));
    for (uint32_t index = 0u; index < font_count; ++index) {
        XgRenderUiCheckpointFont *font = &fonts[index];
        uint32_t line_height;
        uint32_t has_identity;
        uint32_t provenance_synthetic;
        uint32_t reserved;
        if ((size_t)(end - cursor) < XG_RENDER_UI_CHECKPOINT_ENTRY_SIZE)
            return XG_RENDER_UI_INVALID_CHECKPOINT;
        font->font_id = ui_read_u64(&cursor);
        font->generation = ui_read_u64(&cursor);
        font->owner_domain = (XgRenderUiOwnerDomain)ui_read_u32(&cursor);
        font->lifecycle = (XgRenderUiResourceLifecycle)ui_read_u32(&cursor);
        font->owner_root = ui_read_u32(&cursor);
        font->provenance_receipt = ui_read_u64(&cursor);
        line_height = ui_read_u32(&cursor);
        font->metric_count = ui_read_u32(&cursor);
        font->atlas_resource_id = ui_read_u64(&cursor);
        memcpy(font->atlas_identity.bytes, cursor,
               sizeof(font->atlas_identity.bytes));
        cursor += sizeof(font->atlas_identity.bytes);
        has_identity = ui_read_u32(&cursor);
        font->atlas_owner_kind =
            (XgRenderResourceOwnerKind)ui_read_u32(&cursor);
        font->atlas_owner_generation = ui_read_u64(&cursor);
        font->atlas_state = (XgRenderResourceState)ui_read_u32(&cursor);
        reserved = ui_read_u32(&cursor);
        font->atlas_digest = ui_read_u64(&cursor);
        font->atlas_byte_count = ui_read_u64(&cursor);
        font->atlas_provenance.kind =
            (XgRenderResourceProvenanceKind)ui_read_u32(&cursor);
        provenance_synthetic = ui_read_u32(&cursor);
        font->atlas_provenance.synthetic = provenance_synthetic != 0u;
        font->atlas_provenance.receipt = ui_read_u64(&cursor);
        font->atlas_provenance.capability = ui_read_u64(&cursor);
        memcpy(font->atlas_authority.bytes, cursor,
                sizeof(font->atlas_authority.bytes));
        cursor += sizeof(font->atlas_authority.bytes);
        font->atlas_descriptor.version = ui_read_u32(&cursor);
        font->atlas_descriptor.pixel_format =
            (XgRenderResourcePixelFormat)ui_read_u32(&cursor);
        font->atlas_descriptor.width = ui_read_u32(&cursor);
        font->atlas_descriptor.height = ui_read_u32(&cursor);
        font->atlas_descriptor.row_pitch = ui_read_u32(&cursor);
        font->atlas_descriptor.vram_x = ui_read_u32(&cursor);
        font->atlas_descriptor.vram_y = ui_read_u32(&cursor);
        font->atlas_descriptor.vram_width = ui_read_u32(&cursor);
        font->atlas_descriptor.vram_height = ui_read_u32(&cursor);
        font->atlas_descriptor.sampler =
            (XgRenderResourceSampler)ui_read_u32(&cursor);
        font->atlas_descriptor.wrap_u =
            (XgRenderResourceWrap)ui_read_u32(&cursor);
        font->atlas_descriptor.wrap_v =
            (XgRenderResourceWrap)ui_read_u32(&cursor);
        font->atlas_descriptor.flags = ui_read_u32(&cursor);
        if (font->font_id == 0u || font->generation == 0u ||
            !xg_render_ui_owner_domain_valid(font->owner_domain) ||
            font->owner_root == 0u ||
            (font->lifecycle !=
                 XG_RENDER_UI_LIFECYCLE_ARTIFACT_PERSISTENT &&
             font->lifecycle != XG_RENDER_UI_LIFECYCLE_SCENE_TRANSIENT) ||
            font->provenance_receipt == 0u ||
            line_height == 0u || line_height > INT16_MAX ||
            font->metric_count == 0u ||
            font->metric_count > XG_RENDER_FONT_GLYPH_CAPACITY ||
            font->atlas_resource_id == 0u || has_identity > 1u ||
            font->atlas_owner_kind != ui_repository_owner(font->lifecycle) ||
            font->atlas_owner_generation == 0u ||
            font->atlas_state != XG_RENDER_RESOURCE_NATIVE_OWNED ||
            reserved != 0u || font->atlas_digest == 0u ||
            font->atlas_byte_count == 0u ||
            font->atlas_byte_count > SIZE_MAX || provenance_synthetic > 1u ||
            !ui_atlas_descriptor_valid(&font->atlas_descriptor,
                                       (size_t)font->atlas_byte_count) ||
            !ui_provenance_matches(&font->atlas_provenance,
                                   font->lifecycle,
                                   font->provenance_receipt))
            return XG_RENDER_UI_INVALID_CHECKPOINT;
        font->restored_atlas_provenance = font->atlas_provenance;
        if (out_restore != NULL) {
            const uint64_t owner_generation =
                font->lifecycle == XG_RENDER_UI_LIFECYCLE_SCENE_TRANSIENT
                    ? restored_owner_generation
                    : font->atlas_owner_generation;
            if (xg_render_resource_capability_checkpoint_restore_stage(
                    &font->atlas_authority, &font->atlas_provenance,
                    font->atlas_owner_kind, font->atlas_owner_generation,
                    owner_generation, &font->restored_atlas_provenance) !=
                        XG_RENDER_RESOURCE_CAPABILITY_OK)
                return XG_RENDER_UI_INVALID_CHECKPOINT;
        } else if (xg_render_resource_capability_checkpoint_validate(
                       &font->atlas_authority, &font->atlas_provenance,
                       font->atlas_owner_kind, font->atlas_owner_generation,
                       NULL) != XG_RENDER_RESOURCE_CAPABILITY_OK) {
            return XG_RENDER_UI_INVALID_CHECKPOINT;
        }
        {
            XgRenderResourceCapabilityMetadata metadata;
            XgRenderResourceView authority_view = {
                .owner_kind = font->atlas_owner_kind,
                .owner_generation = out_restore != NULL &&
                        font->lifecycle ==
                            XG_RENDER_UI_LIFECYCLE_SCENE_TRANSIENT
                    ? restored_owner_generation
                    : font->atlas_owner_generation,
                .provenance = font->restored_atlas_provenance,
            };
            const XgRenderResourceCapabilityResult authority_result =
                out_restore != NULL
                ? xg_render_resource_capability_validate(
                    &authority_view.provenance, font->atlas_owner_kind,
                    authority_view.owner_generation, &metadata)
                : xg_render_resource_capability_checkpoint_validate(
                    &font->atlas_authority, &font->atlas_provenance,
                    font->atlas_owner_kind, font->atlas_owner_generation,
                    &metadata);
            if (authority_result != XG_RENDER_RESOURCE_CAPABILITY_OK ||
                !ui_owner_metadata_matches(
                    font->owner_domain, font->owner_root, &metadata))
                return XG_RENDER_UI_INVALID_CHECKPOINT;
        }
        font->line_height = (int16_t)line_height;
        font->atlas_has_identity = has_identity != 0u;
        if ((font->atlas_has_identity &&
             xg_render_resource_identity_id(&font->atlas_identity) !=
                 font->atlas_resource_id) ||
            (!font->atlas_has_identity &&
              !ui_identity_is_zero(&font->atlas_identity)))
            return XG_RENDER_UI_INVALID_CHECKPOINT;
        if (font->lifecycle == XG_RENDER_UI_LIFECYCLE_SCENE_TRANSIENT) {
            if (checkpoint_scene_generation != 0u &&
                checkpoint_scene_generation != font->atlas_owner_generation)
                return XG_RENDER_UI_INVALID_CHECKPOINT;
            checkpoint_scene_generation = font->atlas_owner_generation;
        }
        if ((size_t)(end - cursor) <
            (size_t)font->metric_count * XG_RENDER_UI_CHECKPOINT_METRIC_SIZE)
            return XG_RENDER_UI_INVALID_CHECKPOINT;
        for (uint32_t metric_index = 0u;
             metric_index < font->metric_count; ++metric_index) {
            XgRenderGlyphMetric *metric = &font->metrics[metric_index];
            metric->glyph_id = ui_read_u32(&cursor);
            metric->atlas_x = ui_read_u16(&cursor);
            metric->atlas_y = ui_read_u16(&cursor);
            metric->width = ui_read_u16(&cursor);
            metric->height = ui_read_u16(&cursor);
            metric->bearing_x = (int16_t)ui_read_u16(&cursor);
            metric->bearing_y = (int16_t)ui_read_u16(&cursor);
            metric->advance_x = (int16_t)ui_read_u16(&cursor);
            if (ui_read_u16(&cursor) != 0u || metric->glyph_id == 0u ||
                metric->width == 0u || metric->height == 0u ||
                (uint32_t)metric->atlas_x + metric->width >
                    font->atlas_descriptor.width ||
                (uint32_t)metric->atlas_y + metric->height >
                    font->atlas_descriptor.height)
                return XG_RENDER_UI_INVALID_CHECKPOINT;
            for (uint32_t previous = 0u; previous < metric_index; ++previous)
                if (font->metrics[previous].glyph_id == metric->glyph_id)
                    return XG_RENDER_UI_INVALID_CHECKPOINT;
        }
        if (font->atlas_byte_count > (uint64_t)(end - cursor))
            return XG_RENDER_UI_INVALID_CHECKPOINT;
        font->atlas_bytes = cursor;
        cursor += (size_t)font->atlas_byte_count;
        if (xg_render_resource_digest(
                font->atlas_bytes, (size_t)font->atlas_byte_count) !=
            font->atlas_digest)
            return XG_RENDER_UI_INVALID_CHECKPOINT;
        for (uint32_t previous = 0u; previous < index; ++previous)
            if (fonts[previous].font_id == font->font_id)
                return XG_RENDER_UI_INVALID_CHECKPOINT;
    }
    if (cursor != end) return XG_RENDER_UI_INVALID_CHECKPOINT;
    if (out_restore == NULL) return XG_RENDER_UI_OK;

    XgRenderUiCheckpointRestore *restore =
        (XgRenderUiCheckpointRestore *)calloc(1u, sizeof(*restore));
    if (restore == NULL) return XG_RENDER_UI_RESOURCE_FAILED;

    for (uint32_t index = 0u; index < font_count; ++index) {
        const XgRenderUiCheckpointFont *font = &fonts[index];
        const uint64_t owner_generation =
            font->lifecycle == XG_RENDER_UI_LIFECYCLE_SCENE_TRANSIENT
                ? restored_owner_generation : font->atlas_owner_generation;
        XgRenderResourceImport import;
        if (xg_render_resource_capability_restore_stage(
                font->restored_atlas_provenance, font->atlas_owner_kind,
                owner_generation) != XG_RENDER_RESOURCE_CAPABILITY_OK) {
            for (uint32_t retained_index = 0u;
                 retained_index < index; ++retained_index)
                (void)xg_render_resource_release(
                    restore->fonts[retained_index].atlas);
            for (uint32_t staged = 0u; staged < index; ++staged)
                xg_render_resource_restore_cancel(
                    restore->fonts[staged].atlas);
            free(restore);
            return XG_RENDER_UI_RESOURCE_FAILED;
        }
        import = (XgRenderResourceImport){
            .resource_id = font->atlas_resource_id,
            .kind = XG_RENDER_RESOURCE_GLYPH_ATLAS,
            .owner_kind = font->atlas_owner_kind,
            .owner_generation = owner_generation,
            .state = XG_RENDER_RESOURCE_NATIVE_OWNED,
            .provenance = font->restored_atlas_provenance,
            .content_digest = font->atlas_digest,
            .bytes = font->atlas_bytes,
            .byte_count = (size_t)font->atlas_byte_count,
            .identity = font->atlas_identity,
            .descriptor = font->atlas_descriptor,
        };
        XgRenderResourceHandle atlas = {0};
        XgRenderResourceView atlas_view;
        bool staged = false;
        bool retained = false;

        staged = xg_render_resource_restore_stage(&import, &atlas) ==
            XG_RENDER_RESOURCE_OK;
        retained = staged && xg_render_resource_retain(atlas) ==
            XG_RENDER_RESOURCE_OK;
        if (!retained || xg_render_resource_view(atlas, &atlas_view) !=
                XG_RENDER_RESOURCE_OK ||
            (!atlas_view.current &&
             atlas_view.state != XG_RENDER_RESOURCE_IMPORTING_NATIVE) ||
            atlas_view.kind != XG_RENDER_RESOURCE_GLYPH_ATLAS ||
            atlas_view.owner_kind != font->atlas_owner_kind ||
            atlas_view.owner_generation != owner_generation ||
            (atlas_view.state != font->atlas_state &&
             atlas_view.state != XG_RENDER_RESOURCE_IMPORTING_NATIVE) ||
            !ui_view_provenance_matches(
                &atlas_view, font->lifecycle, font->provenance_receipt)) {
            if (retained) (void)xg_render_resource_release(atlas);
            for (uint32_t retained_index = 0u;
                 retained_index < index; ++retained_index)
                (void)xg_render_resource_release(
                    restore->fonts[retained_index].atlas);
            for (uint32_t staged = 0u; staged < index; ++staged)
                xg_render_resource_restore_cancel(
                    restore->fonts[staged].atlas);
            if (staged) xg_render_resource_restore_cancel(atlas);
            free(restore);
            return XG_RENDER_UI_RESOURCE_FAILED;
        }
        restore->fonts[index] = (XgRenderFontEntry){
            .font_id = font->font_id,
            .generation = font->generation,
            .atlas = atlas,
            .owner_domain = font->owner_domain,
            .owner_root = font->owner_root,
            .lifecycle = font->lifecycle,
            .provenance_receipt = font->provenance_receipt,
            .metric_count = font->metric_count,
            .line_height = font->line_height,
            .occupied = true,
        };
        memcpy(restore->fonts[index].metrics, font->metrics,
               font->metric_count * sizeof(font->metrics[0]));
    }
    restore->scene_owner_generation = restored_owner_generation;
    restore->font_count = font_count;
    restore->has_scene_transient = checkpoint_scene_generation != 0u;
    *out_restore = restore;
    return XG_RENDER_UI_OK;
}

XgRenderUiResult xg_render_ui_resources_checkpoint_validate(
        const void *checkpoint, size_t checkpoint_size) {
    return ui_checkpoint_process(
        checkpoint, checkpoint_size,
        g_scene_owner_generation != 0u ? g_scene_owner_generation : 1u,
        NULL);
}

XgRenderUiResult xg_render_ui_resources_checkpoint_prepare(
        const void *checkpoint, size_t checkpoint_size,
        uint64_t restored_owner_generation,
        XgRenderUiCheckpointRestore **out_restore) {
    if (out_restore == NULL) return XG_RENDER_UI_INVALID_ARGUMENT;
    *out_restore = NULL;
    return ui_checkpoint_process(
        checkpoint, checkpoint_size, restored_owner_generation, out_restore);
}

void xg_render_ui_resources_checkpoint_commit(
        XgRenderUiCheckpointRestore *restore) {
    if (restore == NULL) return;
    for (uint32_t index = 0u; index < restore->font_count; ++index)
        (void)xg_render_resource_restore_commit(restore->fonts[index].atlas);
    xg_render_ui_resources_reset();
    g_scene_owner_generation = restore->scene_owner_generation;
    memcpy(g_fonts, restore->fonts, sizeof(g_fonts));
    free(restore);
}

void xg_render_ui_resources_checkpoint_cancel(
        XgRenderUiCheckpointRestore *restore) {
    if (restore == NULL) return;
    for (uint32_t index = 0u; index < restore->font_count; ++index) {
        (void)xg_render_resource_release(restore->fonts[index].atlas);
        xg_render_resource_restore_cancel(restore->fonts[index].atlas);
    }
    free(restore);
}

XgRenderUiResult xg_render_ui_resources_checkpoint_restore(
        const void *checkpoint, size_t checkpoint_size) {
    XgRenderUiCheckpointRestore *restore;
    const uint64_t restored_owner_generation =
        g_scene_owner_generation != 0u ? g_scene_owner_generation : 1u;
    XgRenderUiResult result;

    if (!xg_render_resource_capability_restore_begin())
        return XG_RENDER_UI_RESOURCE_FAILED;
    result = xg_render_ui_resources_checkpoint_prepare(
        checkpoint, checkpoint_size, restored_owner_generation, &restore);
    if (result == XG_RENDER_UI_OK && g_scene_owner_generation == 0u &&
        restore->has_scene_transient) {
        xg_render_ui_resources_checkpoint_cancel(restore);
        xg_render_resource_capability_restore_cancel();
        return XG_RENDER_UI_INVALID_ARGUMENT;
    }
    if (result == XG_RENDER_UI_OK) {
        xg_render_resource_capability_restore_commit();
        restore->scene_owner_generation = g_scene_owner_generation;
        xg_render_ui_resources_checkpoint_commit(restore);
    } else {
        xg_render_resource_capability_restore_cancel();
    }
    return result;
}
