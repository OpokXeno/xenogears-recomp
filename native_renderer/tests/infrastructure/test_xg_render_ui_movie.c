#include "xg_render_movie_publisher.h"
#include "xg_render_ui_owner_catalog.h"
#include "xg_render_ui_resources.h"

#include <assert.h>
#include <stdlib.h>
#include <string.h>

static void write_u32(uint8_t *bytes, uint32_t value) {
    for (uint32_t index = 0u; index < 4u; ++index)
        bytes[index] = (uint8_t)(value >> (index * 8u));
}

static XgRenderResourceHandle import_resource(
        uint64_t resource_id, XgRenderResourceKind kind,
        XgRenderResourceOwnerKind owner_kind, uint64_t owner_generation,
        const uint8_t *bytes, size_t byte_count,
        XgRenderResourceProvenance provenance) {
    XgRenderResourceImport import = {
        .resource_id = resource_id,
        .kind = kind,
        .owner_kind = owner_kind,
        .owner_generation = owner_generation,
        .state = XG_RENDER_RESOURCE_NATIVE_OWNED,
        .provenance = provenance,
        .content_digest = xg_render_resource_digest(bytes, byte_count),
        .bytes = bytes,
        .byte_count = byte_count,
    };
    XgRenderResourceHandle handle = {0};
    XgRenderResourceResult result;
    if (kind == XG_RENDER_RESOURCE_TEXTURE) {
        import.descriptor = (XgRenderResourceDescriptor){
            .version = XG_RENDER_RESOURCE_DESCRIPTOR_VERSION,
            .pixel_format = XG_RENDER_RESOURCE_PIXEL_FORMAT_RGBA8,
            .width = 1u,
            .height = 1u,
            .row_pitch = (uint32_t)byte_count,
            .sampler = XG_RENDER_RESOURCE_SAMPLER_NEAREST,
            .wrap_u = XG_RENDER_RESOURCE_WRAP_CLAMP,
            .wrap_v = XG_RENDER_RESOURCE_WRAP_CLAMP,
        };
    }
    memcpy(import.identity.bytes, &resource_id, sizeof(resource_id));
    result = xg_render_resource_import_native(&import, &handle);
    assert(result == XG_RENDER_RESOURCE_OK);
    return handle;
}

static const XgRenderUiOwnerCatalogEntry *catalog_owner(
        XgRenderUiOwnerDomain owner_domain) {
    for (size_t index = 0u; index < xg_render_ui_owner_catalog_count; ++index)
        if (xg_render_ui_owner_catalog[index].owner_domain ==
                (uint32_t)owner_domain)
            return &xg_render_ui_owner_catalog[index];
    assert(false);
    return NULL;
}

static XgRenderResourceProvenance ui_capability(
        XgRenderUiOwnerDomain owner_domain,
        XgRenderResourceOwnerKind owner_kind, uint64_t owner_generation,
        uint64_t receipt) {
    const XgRenderUiOwnerCatalogEntry *owner = catalog_owner(owner_domain);
    XgRenderResourceCapabilityMetadata metadata = {
        .kind = owner_kind == XG_RENDER_RESOURCE_OWNER_SCENE
            ? XG_RENDER_RESOURCE_PROVENANCE_SOURCE
            : XG_RENDER_RESOURCE_PROVENANCE_ARTIFACT,
        .receipt = receipt,
        .lifetime = owner_kind == XG_RENDER_RESOURCE_OWNER_SCENE
            ? XG_RENDER_RESOURCE_CAPABILITY_SCENE
            : XG_RENDER_RESOURCE_CAPABILITY_MODULE,
        .owner_kind = owner_kind,
        .owner_generation = owner_generation,
        .artifact = owner->artifact,
    };
    XgRenderResourceProvenance provenance;

    if (metadata.kind == XG_RENDER_RESOURCE_PROVENANCE_SOURCE) {
        metadata.source.source_class = XG_RENDER_RESOURCE_SOURCE_UI_GENERATED;
        metadata.source.origin_artifact = owner->artifact;
        metadata.source.identity.bytes[0] = (uint8_t)owner_domain;
        metadata.source.range_size = 1u;
        metadata.source.range_content_digest = receipt;
    }
    assert(xg_render_resource_capability_register(&metadata, &provenance) ==
           XG_RENDER_RESOURCE_CAPABILITY_OK);
    return provenance;
}

static XgRenderResourceProvenance movie_capability(
        uint64_t owner_generation, uint64_t receipt) {
    XgRenderResourceCapabilityMetadata metadata = {
        .kind = XG_RENDER_RESOURCE_PROVENANCE_SOURCE,
        .receipt = receipt,
        .lifetime = XG_RENDER_RESOURCE_CAPABILITY_SCENE,
        .owner_kind = XG_RENDER_RESOURCE_OWNER_SOURCE,
        .owner_generation = owner_generation,
        .source = {
            .source_class = XG_RENDER_RESOURCE_SOURCE_MOVIE_OWNER,
            .range_size = 1u,
            .range_content_digest = receipt,
        },
    };
    XgRenderResourceProvenance provenance;

    metadata.source.identity.bytes[0] = 1u;
    assert(xg_render_resource_capability_register(&metadata, &provenance) ==
           XG_RENDER_RESOURCE_CAPABILITY_OK);
    return provenance;
}

int main(int argc, char **argv) {
    uint8_t atlas_bytes[] = { 1u, 2u, 3u, 4u };
    XgRenderResourceImport atlas_import = {
        .resource_id = 100u,
        .kind = XG_RENDER_RESOURCE_GLYPH_ATLAS,
        .owner_kind = XG_RENDER_RESOURCE_OWNER_MODULE,
        .owner_generation = 1u,
        .state = XG_RENDER_RESOURCE_NATIVE_OWNED,
        .provenance = {
            .kind = XG_RENDER_RESOURCE_PROVENANCE_ARTIFACT,
            .receipt = 7001u,
        },
        .bytes = atlas_bytes,
        .byte_count = sizeof(atlas_bytes),
    };
    XgRenderResourceHandle atlas;
    XgRenderResourceHandle texture;
    XgRenderResourceHandle generated_text;
    XgRenderResourceHandle transient_atlas;
    const XgRenderGlyphMetric metrics[] = {
        { .glyph_id = 1u, .width = 4u, .height = 8u, .advance_x = 5 },
        { .glyph_id = 2u, .atlas_x = 4u, .width = 6u, .height = 8u,
          .advance_x = 7 },
    };
    XgRenderFontImport font;
    const uint32_t glyph_ids[] = { 1u, 2u, 1u };
    XgRenderGlyphRunRequest run_request = {
        .font_id = 200u,
        .font_generation = 1u,
        .owner_domain = XG_RENDER_UI_OWNER_FIELD_DIALOGUE,
        .provenance_receipt = 7001u,
        .glyph_ids = glyph_ids,
        .glyph_count = 3u,
        .reveal_count = 2u,
        .origin_x = 10,
        .origin_y = 20,
        .max_width = 8,
        .color = UINT32_C(0xffffffff),
        .multiline = true,
    };
    XgRenderGlyphRunRequest transient_run_request;
    XgRenderGlyphRun run;
    uint8_t *ui_checkpoint;
    uint8_t *invalid_checkpoint;
    size_t ui_checkpoint_size;
    const uint8_t movie_bytes[] = {
        10u, 11u, 12u, 13u, 14u, 15u, 16u, 17u,
    };
    XgRenderMovieFrameDescription movie = {
        .movie_id = 300u,
        .owner_kind = XG_RENDER_MOVIE_OWNER_FIELD,
        .owner_receipt = 4001u,
        .owner_generation = 1u,
        .guest_cycle = 500u,
        .width = 2u,
        .height = 1u,
        .expected_strips = 2u,
        .byte_count = sizeof(movie_bytes),
        .depth24 = true,
    };
    XgRenderMovieFrameHandle frame;
    XgRenderMovieFramePublication publication;
    XgRenderMovieDiagnostics movie_diagnostics;
    XgRenderResourceView movie_view;
    XgRenderResourceProvenance movie_owner_provenance;
    XgRenderUiResult ui_restore_result;
    uint64_t checkpoint_capability;

    xg_render_resource_repository_reset();
    xg_render_ui_resources_reset();
    atlas_import.provenance = ui_capability(
        XG_RENDER_UI_OWNER_FIELD_DIALOGUE,
        XG_RENDER_RESOURCE_OWNER_MODULE, 1u, 7001u);
    atlas_import.content_digest = xg_render_resource_digest(
        atlas_bytes, sizeof(atlas_bytes));
    atlas_import.identity.bytes[0] = 100u;
    assert(xg_render_resource_import_native(&atlas_import, &atlas) ==
           XG_RENDER_RESOURCE_OK);
    texture = (XgRenderResourceHandle){0};
    generated_text = import_resource(102u, XG_RENDER_RESOURCE_GENERATED_SURFACE,
                                     XG_RENDER_RESOURCE_OWNER_SCENE, 1u,
                                     atlas_bytes, sizeof(atlas_bytes),
                                     ui_capability(
                                         XG_RENDER_UI_OWNER_FIELD_CREDITS_GENERATED_GLYPH_CACHE,
                                         XG_RENDER_RESOURCE_OWNER_SCENE,
                                         1u, 6001u));

    assert(!xg_render_ui_owner_domain_valid(XG_RENDER_UI_OWNER_NONE));
    assert(!xg_render_ui_owner_domain_valid(XG_RENDER_UI_OWNER_COUNT));
    assert(XG_RENDER_UI_OWNER_GEAR_SHOP == 11);
    assert(XG_RENDER_UI_OWNER_GEAR_HELPER == 12);
    assert(XG_RENDER_UI_OWNER_COUNT == 13);
    for (int owner = XG_RENDER_UI_OWNER_RESIDENT;
         owner < XG_RENDER_UI_OWNER_COUNT; ++owner) {
        const XgRenderUiOwnerCatalogEntry *catalog =
            catalog_owner((XgRenderUiOwnerDomain)owner);
        texture = import_resource(
            1000u + (uint64_t)owner, XG_RENDER_RESOURCE_TEXTURE,
            XG_RENDER_RESOURCE_OWNER_MODULE, 1u, atlas_bytes,
            sizeof(atlas_bytes), ui_capability(
                (XgRenderUiOwnerDomain)owner,
                XG_RENDER_RESOURCE_OWNER_MODULE, 1u, 5000u + owner));
        XgRenderUiWindowDescriptor window = {
            .resource = {
                .handle = texture,
                .owner_domain = (XgRenderUiOwnerDomain)owner,
                .owner_root = catalog->root_address,
                .lifecycle = XG_RENDER_UI_LIFECYCLE_ARTIFACT_PERSISTENT,
                .kind = XG_RENDER_UI_RESOURCE_WINDOW,
                .provenance_receipt = 5000u + (uint64_t)owner,
            },
        };
        assert(xg_render_ui_owner_domain_valid(
            (XgRenderUiOwnerDomain)owner));
        assert(xg_render_ui_resource_record_validate(
                   &window.resource, (XgRenderUiOwnerDomain)owner,
                   XG_RENDER_UI_RESOURCE_WINDOW) == XG_RENDER_UI_OK);
    }
    {
        XgRenderUiGeneratedTextReference text = {
            .resource = {
                .handle = generated_text,
                .owner_domain =
                    XG_RENDER_UI_OWNER_FIELD_CREDITS_GENERATED_GLYPH_CACHE,
                .owner_root = UINT32_C(0x800ac0f0),
                .lifecycle = XG_RENDER_UI_LIFECYCLE_SCENE_TRANSIENT,
                .kind = XG_RENDER_UI_RESOURCE_GENERATED_TEXT,
                .provenance_receipt = 6001u,
            },
        };
        assert(xg_render_ui_resource_record_validate(
                   &text.resource,
                   XG_RENDER_UI_OWNER_FIELD_CREDITS_GENERATED_GLYPH_CACHE,
                   XG_RENDER_UI_RESOURCE_GENERATED_TEXT) ==
               XG_RENDER_UI_OK);
        text.resource.provenance_receipt = 6002u;
        assert(xg_render_ui_resource_record_validate(
                   &text.resource,
                   XG_RENDER_UI_OWNER_FIELD_CREDITS_GENERATED_GLYPH_CACHE,
                   XG_RENDER_UI_RESOURCE_GENERATED_TEXT) ==
               XG_RENDER_UI_INVALID_PROVENANCE);
        text.resource.provenance_receipt = 0u;
        assert(xg_render_ui_resource_record_validate(
                   &text.resource,
                   XG_RENDER_UI_OWNER_FIELD_CREDITS_GENERATED_GLYPH_CACHE,
                   XG_RENDER_UI_RESOURCE_GENERATED_TEXT) ==
               XG_RENDER_UI_INVALID_PROVENANCE);
        text.resource.provenance_receipt = 6001u;
        text.resource.owner_domain = XG_RENDER_UI_OWNER_NONE;
        assert(xg_render_ui_resource_record_validate(
                   &text.resource,
                   XG_RENDER_UI_OWNER_FIELD_CREDITS_GENERATED_GLYPH_CACHE,
                   XG_RENDER_UI_RESOURCE_GENERATED_TEXT) ==
               XG_RENDER_UI_INVALID_OWNER);
        text.resource.owner_domain = XG_RENDER_UI_OWNER_RESIDENT;
        assert(xg_render_ui_resource_record_validate(
                   &text.resource,
                   XG_RENDER_UI_OWNER_FIELD_CREDITS_GENERATED_GLYPH_CACHE,
                   XG_RENDER_UI_RESOURCE_GENERATED_TEXT) ==
               XG_RENDER_UI_OWNER_MISMATCH);
        text.resource.owner_domain =
            XG_RENDER_UI_OWNER_FIELD_CREDITS_GENERATED_GLYPH_CACHE;
        text.resource.lifecycle =
            XG_RENDER_UI_LIFECYCLE_ARTIFACT_PERSISTENT;
        assert(xg_render_ui_resource_record_validate(
                   &text.resource,
                   XG_RENDER_UI_OWNER_FIELD_CREDITS_GENERATED_GLYPH_CACHE,
                   XG_RENDER_UI_RESOURCE_GENERATED_TEXT) ==
               XG_RENDER_UI_INVALID_LIFECYCLE);
    }
    {
        XgRenderUiCursorDescriptor cursor = {
            .resource = {
                .handle = atlas,
                .owner_domain = XG_RENDER_UI_OWNER_RESIDENT,
                .owner_root = UINT32_C(0x80033558),
                .lifecycle = XG_RENDER_UI_LIFECYCLE_ARTIFACT_PERSISTENT,
                .kind = XG_RENDER_UI_RESOURCE_CURSOR,
                .provenance_receipt = 6002u,
            },
        };
        assert(xg_render_ui_resource_record_validate(
                   &cursor.resource, XG_RENDER_UI_OWNER_RESIDENT,
                   XG_RENDER_UI_RESOURCE_CURSOR) ==
               XG_RENDER_UI_RESOURCE_KIND_MISMATCH);
    }
    font = (XgRenderFontImport){
        .font_id = 200u,
        .generation = 1u,
        .atlas = atlas,
        .owner_domain = XG_RENDER_UI_OWNER_FIELD_DIALOGUE,
        .owner_root = UINT32_C(0x8008004c),
        .lifecycle = XG_RENDER_UI_LIFECYCLE_ARTIFACT_PERSISTENT,
        .provenance_receipt = 7001u,
        .metrics = metrics,
        .metric_count = 2u,
        .line_height = 10,
    };
    font.owner_domain = XG_RENDER_UI_OWNER_NONE;
    assert(xg_render_font_import(&font) == XG_RENDER_UI_INVALID_ARGUMENT);
    font.owner_domain = XG_RENDER_UI_OWNER_FIELD_DIALOGUE;
    font.provenance_receipt = 0u;
    assert(xg_render_font_import(&font) == XG_RENDER_UI_INVALID_ARGUMENT);
    font.provenance_receipt = 7001u;
    assert(xg_render_font_import(&font) == XG_RENDER_UI_OK);
    run_request.owner_domain = XG_RENDER_UI_OWNER_BATTLE_FONT;
    assert(xg_render_glyph_run_build(&run_request, &run) ==
           XG_RENDER_UI_OWNER_MISMATCH);
    run_request.owner_domain = XG_RENDER_UI_OWNER_FIELD_DIALOGUE;
    run_request.owner_root = UINT32_C(0x8008004c);
    run_request.provenance_receipt = 0u;
    assert(xg_render_glyph_run_build(&run_request, &run) ==
           XG_RENDER_UI_INVALID_PROVENANCE);
    run_request.provenance_receipt = 7001u;
    assert(xg_render_glyph_run_build(&run_request, &run) == XG_RENDER_UI_OK);
    assert(run.glyph_count == 2u);
    assert(run.glyphs[0].x == 10);
    assert(run.glyphs[0].y == 20);
    assert(run.glyphs[1].x == 10);
    assert(run.glyphs[1].y == 30);
    assert(run.height == 20);
    assert(run.digest != 0u);
    assert(run.owner_domain == XG_RENDER_UI_OWNER_FIELD_DIALOGUE);
    assert(run.lifecycle == XG_RENDER_UI_LIFECYCLE_ARTIFACT_PERSISTENT);
    assert(run.provenance_receipt == 7001u);
    xg_render_ui_resources_scene_boundary(9u);
    transient_atlas = import_resource(
        103u, XG_RENDER_RESOURCE_GLYPH_ATLAS,
        XG_RENDER_RESOURCE_OWNER_SCENE, 9u,
        atlas_bytes, sizeof(atlas_bytes), ui_capability(
            XG_RENDER_UI_OWNER_FIELD_DIALOGUE,
            XG_RENDER_RESOURCE_OWNER_SCENE, 9u, 7002u));
    font = (XgRenderFontImport){
        .font_id = 201u,
        .generation = 1u,
        .atlas = transient_atlas,
        .owner_domain = XG_RENDER_UI_OWNER_FIELD_DIALOGUE,
        .owner_root = UINT32_C(0x8008004c),
        .lifecycle = XG_RENDER_UI_LIFECYCLE_SCENE_TRANSIENT,
        .provenance_receipt = 7002u,
        .metrics = metrics,
        .metric_count = 2u,
        .line_height = 10,
    };
    assert(xg_render_font_import(&font) == XG_RENDER_UI_OK);
    transient_run_request = run_request;
    transient_run_request.font_id = 201u;
    transient_run_request.provenance_receipt = 7002u;
    assert(xg_render_glyph_run_build(&transient_run_request, &run) ==
           XG_RENDER_UI_OK);
    assert(run.lifecycle == XG_RENDER_UI_LIFECYCLE_SCENE_TRANSIENT);
    ui_checkpoint_size = xg_render_ui_resources_checkpoint_size();
    assert(ui_checkpoint_size != 0u);
    ui_checkpoint = (uint8_t *)malloc(ui_checkpoint_size);
    assert(ui_checkpoint != NULL);
    invalid_checkpoint = (uint8_t *)malloc(ui_checkpoint_size);
    assert(invalid_checkpoint != NULL);
    assert(xg_render_ui_resources_checkpoint_write(
               ui_checkpoint, ui_checkpoint_size) == XG_RENDER_UI_OK);
    checkpoint_capability = atlas_import.provenance.capability;
    memcpy(invalid_checkpoint, ui_checkpoint, ui_checkpoint_size);
    invalid_checkpoint[24u + 148u] ^= 1u;
    assert(xg_render_ui_resources_checkpoint_validate(
               invalid_checkpoint, ui_checkpoint_size) ==
           XG_RENDER_UI_INVALID_CHECKPOINT);
    memcpy(invalid_checkpoint, ui_checkpoint, ui_checkpoint_size);
    write_u32(invalid_checkpoint + 4u, 1u);
    assert(xg_render_ui_resources_checkpoint_validate(
               invalid_checkpoint, ui_checkpoint_size) ==
           XG_RENDER_UI_INVALID_CHECKPOINT);
    memcpy(invalid_checkpoint, ui_checkpoint, ui_checkpoint_size);
    write_u32(invalid_checkpoint + 40u, XG_RENDER_UI_OWNER_NONE);
    assert(xg_render_ui_resources_checkpoint_validate(
               invalid_checkpoint, ui_checkpoint_size) ==
           XG_RENDER_UI_INVALID_CHECKPOINT);
    memcpy(invalid_checkpoint, ui_checkpoint, ui_checkpoint_size);
    write_u32(invalid_checkpoint + 40u, XG_RENDER_UI_OWNER_GEAR_HELPER);
    assert(xg_render_ui_resources_checkpoint_validate(
                invalid_checkpoint, ui_checkpoint_size) ==
            XG_RENDER_UI_INVALID_CHECKPOINT);
    memcpy(invalid_checkpoint, ui_checkpoint, ui_checkpoint_size);
    write_u32(invalid_checkpoint + 40u, XG_RENDER_UI_OWNER_COUNT);
    assert(xg_render_ui_resources_checkpoint_validate(
               invalid_checkpoint, ui_checkpoint_size) ==
           XG_RENDER_UI_INVALID_CHECKPOINT);
    memcpy(invalid_checkpoint, ui_checkpoint, ui_checkpoint_size);
    memset(invalid_checkpoint + 52u, 0, sizeof(uint64_t));
    assert(xg_render_ui_resources_checkpoint_validate(
               invalid_checkpoint, ui_checkpoint_size) ==
           XG_RENDER_UI_INVALID_CHECKPOINT);
    memcpy(invalid_checkpoint, ui_checkpoint, ui_checkpoint_size);
    write_u32(invalid_checkpoint + 112u, XG_RENDER_RESOURCE_OWNER_SCENE);
    assert(xg_render_ui_resources_checkpoint_validate(
               invalid_checkpoint, ui_checkpoint_size) ==
           XG_RENDER_UI_INVALID_CHECKPOINT);
    memcpy(invalid_checkpoint, ui_checkpoint, ui_checkpoint_size);
    write_u32(invalid_checkpoint + 148u,
              XG_RENDER_RESOURCE_PROVENANCE_NONE);
    assert(xg_render_ui_resources_checkpoint_validate(
               invalid_checkpoint, ui_checkpoint_size) ==
           XG_RENDER_UI_INVALID_CHECKPOINT);
    ui_checkpoint[ui_checkpoint_size - 1u] ^= 1u;
    assert(xg_render_ui_resources_checkpoint_validate(
               ui_checkpoint, ui_checkpoint_size) ==
           XG_RENDER_UI_INVALID_CHECKPOINT);
    assert(xg_render_glyph_run_build(&run_request, &run) == XG_RENDER_UI_OK);
    ui_checkpoint[ui_checkpoint_size - 1u] ^= 1u;
    xg_render_resource_repository_reset();
    xg_render_ui_resources_reset();
    ui_restore_result = xg_render_ui_resources_checkpoint_restore(
        ui_checkpoint, ui_checkpoint_size);
    assert(ui_restore_result == XG_RENDER_UI_INVALID_ARGUMENT);
    xg_render_ui_resources_scene_boundary(10u);
    assert(xg_render_ui_resources_checkpoint_restore(
               ui_checkpoint, ui_checkpoint_size) == XG_RENDER_UI_OK);
    assert(xg_render_glyph_run_build(&run_request, &run) == XG_RENDER_UI_OK);
    assert(run.glyph_count == 2u);
    assert(run.owner_domain == XG_RENDER_UI_OWNER_FIELD_DIALOGUE);
    assert(run.provenance_receipt == 7001u);
    assert(xg_render_resource_view(run.atlas, &movie_view) ==
           XG_RENDER_RESOURCE_OK);
    assert(movie_view.owner_kind == XG_RENDER_RESOURCE_OWNER_MODULE);
    assert(movie_view.owner_generation == 1u);
    assert(movie_view.provenance.kind ==
           XG_RENDER_RESOURCE_PROVENANCE_ARTIFACT);
    assert(movie_view.provenance.receipt == 7001u);
    assert(movie_view.provenance.capability != checkpoint_capability);
    assert(!movie_view.provenance.synthetic);
    assert(xg_render_glyph_run_build(&transient_run_request, &run) ==
           XG_RENDER_UI_OK);
    assert(xg_render_resource_view(run.atlas, &movie_view) ==
           XG_RENDER_RESOURCE_OK);
    assert(movie_view.owner_kind == XG_RENDER_RESOURCE_OWNER_SCENE);
    assert(movie_view.owner_generation == 10u);
    xg_render_resource_invalidate_owner(XG_RENDER_RESOURCE_OWNER_MODULE, 1u);
    assert(xg_render_glyph_run_build(&run_request, &run) ==
           XG_RENDER_UI_STALE_RESOURCE);
    assert(xg_render_ui_resources_checkpoint_restore(
               ui_checkpoint, ui_checkpoint_size) == XG_RENDER_UI_OK);
    assert(xg_render_glyph_run_build(&run_request, &run) == XG_RENDER_UI_OK);
    xg_render_ui_resources_scene_boundary(11u);
    xg_render_resource_invalidate_scene_boundary();
    assert(xg_render_glyph_run_build(&transient_run_request, &run) ==
           XG_RENDER_UI_FONT_NOT_FOUND);
    assert(xg_render_glyph_run_build(&run_request, &run) == XG_RENDER_UI_OK);
    for (uint64_t scene_generation = 20u;
         scene_generation < 20u + XG_RENDER_FONT_CAPACITY + 4u;
         ++scene_generation) {
        const uint64_t resource_id = 120u + scene_generation - 20u;
        const uint64_t font_id = 300u + scene_generation;
        XgRenderResourceHandle scene_atlas;

        xg_render_ui_resources_scene_boundary(scene_generation);
        scene_atlas = import_resource(
            resource_id, XG_RENDER_RESOURCE_GLYPH_ATLAS,
            XG_RENDER_RESOURCE_OWNER_SCENE, scene_generation,
            atlas_bytes, sizeof(atlas_bytes), ui_capability(
                XG_RENDER_UI_OWNER_FIELD_DIALOGUE,
                XG_RENDER_RESOURCE_OWNER_SCENE, scene_generation,
                8000u + scene_generation));
        font = (XgRenderFontImport){
            .font_id = font_id,
            .generation = 1u,
            .atlas = scene_atlas,
            .owner_domain = XG_RENDER_UI_OWNER_FIELD_DIALOGUE,
            .owner_root = UINT32_C(0x8008004c),
            .lifecycle = XG_RENDER_UI_LIFECYCLE_SCENE_TRANSIENT,
            .provenance_receipt = 8000u + scene_generation,
            .metrics = metrics,
            .metric_count = 2u,
            .line_height = 10,
        };
        assert(xg_render_font_import(&font) == XG_RENDER_UI_OK);
        transient_run_request.font_id = font_id;
        transient_run_request.provenance_receipt =
            8000u + scene_generation;
        assert(xg_render_glyph_run_build(&transient_run_request, &run) ==
               XG_RENDER_UI_OK);
        xg_render_ui_resources_scene_boundary(scene_generation + 1u);
        xg_render_resource_invalidate_scene_boundary();
        assert(xg_render_resource_view(scene_atlas, &movie_view) ==
               XG_RENDER_RESOURCE_NOT_FOUND);
        assert(xg_render_glyph_run_build(&run_request, &run) ==
               XG_RENDER_UI_OK);
    }
    free(invalid_checkpoint);
    free(ui_checkpoint);

    if (argc == 2 && strcmp(argv[1], "--ui-only") == 0) return 0;

    xg_render_movie_publisher_reset();
    movie_owner_provenance = movie_capability(1u, movie.owner_receipt);
    movie.owner_capability = movie_owner_provenance.capability;
    assert(xg_render_movie_frame_begin(&movie, &frame) == XG_RENDER_MOVIE_OK);
    assert(xg_render_movie_frame_write_strip(frame, 0u, 0u,
                                              movie_bytes, 4u) ==
           XG_RENDER_MOVIE_OK);
    assert(xg_render_movie_frame_publish(frame, &publication) ==
           XG_RENDER_MOVIE_INCOMPLETE_FRAME);
    assert(xg_render_movie_frame_write_strip(frame, 1u, 4u,
                                              movie_bytes + 4u, 4u) ==
           XG_RENDER_MOVIE_OK);
    assert(xg_render_movie_frame_publish(frame, &publication) ==
           XG_RENDER_MOVIE_OK);
    assert(publication.completed_strip_mask == 3u);
    assert(publication.owner_kind == XG_RENDER_MOVIE_OWNER_FIELD);
    assert(publication.owner_receipt == 4001u);
    assert(publication.depth24);
    assert(xg_render_resource_view(publication.surface, &movie_view) ==
           XG_RENDER_RESOURCE_OK);
    assert(movie_view.byte_count == sizeof(movie_bytes));
    assert(movie_view.provenance.kind == XG_RENDER_RESOURCE_PROVENANCE_SOURCE);
    assert(movie_view.provenance.receipt == 4001u);
    assert(!movie_view.provenance.synthetic);
    assert(movie_view.descriptor.version ==
           XG_RENDER_RESOURCE_DESCRIPTOR_VERSION);
    assert(movie_view.descriptor.pixel_format ==
           XG_RENDER_RESOURCE_PIXEL_FORMAT_RGB24);
    assert(movie_view.descriptor.width == movie.width &&
           movie_view.descriptor.height == movie.height);
    assert(movie_view.descriptor.row_pitch == sizeof(movie_bytes));
    assert(movie_view.descriptor.sampler ==
           XG_RENDER_RESOURCE_SAMPLER_NEAREST);
    assert(movie_view.descriptor.wrap_u == XG_RENDER_RESOURCE_WRAP_CLAMP &&
           movie_view.descriptor.wrap_v == XG_RENDER_RESOURCE_WRAP_CLAMP);
    assert(movie_view.descriptor.flags == 0u);
    assert(((const uint8_t *)movie_view.bytes)[7] == 17u);
    xg_render_movie_publisher_diagnostics(&movie_diagnostics);
    assert(movie_diagnostics.complete_frames == 1u);
    assert(movie_diagnostics.partial_publish_attempts == 1u);
    assert(!movie_diagnostics.frame_active);
    return 0;
}
