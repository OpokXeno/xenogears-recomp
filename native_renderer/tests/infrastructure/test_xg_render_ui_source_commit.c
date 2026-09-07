#include "xg_render_semantic_presentation.h"
#include "xg_render_source_frame.h"
#include "xg_render_surface_graph.h"

#include <assert.h>
#include <stddef.h>
#include <stdint.h>

static const XgPresentationIdentity k_identity = {
    .presentation_epoch = 1u,
    .source_sequence = 1u,
    .guest_vblank_sequence = 1u,
    .guest_cycle = 100u,
    .scene_generation = 7u,
};

static const XgSemanticSceneIdentity k_scene = {
    .disc_id = 1u,
    .executable_identity = 2u,
    .primary_overlay_identity = 3u,
    .authored_scene_id = 4u,
    .module = XG_SEMANTIC_MODULE_MENU,
};

static const XgSemanticDisplayState k_display = {
    .width = 320u,
    .height = 240u,
    .aspect_num = 4u,
    .aspect_den = 3u,
};

static const XgSemanticPassRecord k_pass = {
    .pass_id = 1u,
    .store = true,
};

static XgSemanticResourceRef import_resource_for_owner(
        uint64_t resource_id, XgRenderResourceKind kind,
        XgRenderResourceOwnerKind owner_kind, uint64_t owner_generation) {
    static const uint8_t bytes[] = {1u, 2u, 3u, 4u};
    const uint64_t digest = xg_render_resource_digest(bytes, sizeof(bytes));
    const XgRenderResourceImport import = {
        .resource_id = resource_id,
        .kind = kind,
        .owner_kind = owner_kind,
        .owner_generation = owner_generation,
        .state = XG_RENDER_RESOURCE_NATIVE_OWNED,
        .provenance = {
            .kind = XG_RENDER_RESOURCE_PROVENANCE_NONE,
            .synthetic = true,
        },
        .content_digest = digest,
        .bytes = bytes,
        .byte_count = sizeof(bytes),
    };
    XgRenderResourceHandle handle;

    assert(xg_render_resource_import(&import, &handle) ==
           XG_RENDER_RESOURCE_OK);
    return (XgSemanticResourceRef){
        .resource_id = handle.resource_id,
        .generation = handle.generation,
        .content_digest = digest,
    };
}

static XgSemanticResourceRef import_resource(uint64_t resource_id,
                                              XgRenderResourceKind kind) {
    return import_resource_for_owner(
        resource_id, kind, XG_RENDER_RESOURCE_OWNER_SCENE, 7u);
}

static XgRenderSourceBuilder begin_builder(
        const XgSemanticResourceRef *resource) {
    XgRenderSourceBuilder builder;

    assert(xg_render_source_commit_begin(
               &k_identity, &k_scene, &k_display, 1u, false, true, &builder) ==
           XG_RENDER_SOURCE_COMMIT_OK);
    assert(xg_render_source_commit_append_pass(builder, &k_pass) ==
           XG_RENDER_SOURCE_COMMIT_OK);
    assert(xg_render_source_commit_append_resource(builder, resource) ==
           XG_RENDER_SOURCE_COMMIT_OK);
    return builder;
}

static XgSemanticUiNodeRecord root_node(void) {
    return (XgSemanticUiNodeRecord){
        .node_id = 1u,
        .owner_domain = XG_RENDER_UI_OWNER_GENERAL_MENU,
        .owner_root = UINT32_C(0x8006f4f0),
        .owner_receipt = 101u,
        .order = {.pass_id = 1u, .layer = 10, .insertion_ordinal = 1u},
        .kind = XG_SEMANTIC_UI_NODE_WINDOW,
        .semantic_id = 0x100u,
        .state = 2u,
        .value = 1,
        .maximum = 3,
        .flags = 0x5u,
        .bounds = {.x = 8, .y = 8, .width = 160, .height = 48},
        .clip = {.x = 10, .y = 10, .width = 156, .height = 44},
        .color = UINT32_C(0xffffffff),
        .temporal_mode = XG_SEMANTIC_UI_TEMPORAL_DISCRETE,
        .visible = true,
        .clip_enabled = true,
    };
}

static XgSemanticUiNodeRecord child_node(void) {
    XgSemanticUiNodeRecord node = root_node();
    node.node_id = 2u;
    node.parent_node_id = 1u;
    node.order.insertion_ordinal = 2u;
    node.kind = XG_SEMANTIC_UI_NODE_GROUP;
    return node;
}

static XgSemanticUiGlyphRunRecord glyph_run(
        const XgSemanticResourceRef *atlas) {
    return (XgSemanticUiGlyphRunRecord){
        .glyph_run_id = 10u,
        .node_id = 2u,
        .owner_domain = XG_RENDER_UI_OWNER_GENERAL_MENU,
        .owner_root = UINT32_C(0x8006f4f0),
        .owner_receipt = 101u,
        .order = {.pass_id = 1u, .layer = 10, .insertion_ordinal = 3u},
        .atlas = {atlas->resource_id, atlas->generation},
        .bounds = {.x = 16, .y = 16, .width = 18, .height = 8},
        .clip = {.x = 10, .y = 10, .width = 156, .height = 44},
        .placement_offset = 0u,
        .placement_count = 2u,
        .reveal_count = 2u,
        .total_count = 3u,
        .temporal_mode = XG_SEMANTIC_UI_TEMPORAL_DISCRETE,
        .clip_enabled = true,
    };
}

static XgSemanticUiGlyphPlacementRecord placement(uint32_t glyph_id,
                                                   int32_t x) {
    return (XgSemanticUiGlyphPlacementRecord){
        .glyph_run_id = 10u,
        .glyph_id = glyph_id,
        .x = x,
        .y = 16,
        .atlas_x = (uint16_t)(glyph_id * 8u),
        .width = 8u,
        .height = 8u,
        .color = UINT32_C(0xffffffff),
        .layer = XG_SEMANTIC_UI_GLYPH_PRIMARY,
    };
}

static XgRenderSourceCommitHandle build_valid_commit(
        const XgSemanticResourceRef *atlas) {
    XgRenderSourceBuilder builder = begin_builder(atlas);
    XgSemanticUiNodeRecord root = root_node();
    XgSemanticUiNodeRecord child = child_node();
    XgSemanticUiGlyphRunRecord run = glyph_run(atlas);
    XgSemanticUiGlyphPlacementRecord first = placement(1u, 16);
    XgSemanticUiGlyphPlacementRecord second = placement(2u, 26);
    XgRenderSourceCommitHandle commit;

    /* Deliberately append child first; seal establishes semantic order. */
    assert(xg_render_source_commit_append_ui_node(builder, &child) ==
           XG_RENDER_SOURCE_COMMIT_OK);
    assert(xg_render_source_commit_append_ui_node(builder, &root) ==
           XG_RENDER_SOURCE_COMMIT_OK);
    assert(xg_render_source_commit_append_ui_glyph_run(builder, &run) ==
           XG_RENDER_SOURCE_COMMIT_OK);
    assert(xg_render_source_commit_append_ui_glyph_placement(builder, &first) ==
           XG_RENDER_SOURCE_COMMIT_OK);
    assert(xg_render_source_commit_append_ui_glyph_placement(builder, &second) ==
           XG_RENDER_SOURCE_COMMIT_OK);
    assert(xg_render_source_commit_seal(builder, &commit) ==
           XG_RENDER_SOURCE_COMMIT_OK);
    return commit;
}

static void test_valid_copy_sort_and_digest(void) {
    XgSemanticResourceRef atlas;
    XgRenderSourceCommitHandle first;
    XgRenderSourceCommitHandle second;
    XgRenderSourceCommitHeader first_header;
    XgRenderSourceCommitHeader second_header;
    XgSemanticUiNodeRecord copied_node;
    XgSemanticUiGlyphRunRecord copied_run;
    XgSemanticUiGlyphPlacementRecord copied_placement;
    XgRenderResourceView atlas_view;

    xg_render_resource_repository_reset();
    xg_render_source_commit_reset();
    atlas = import_resource(100u, XG_RENDER_RESOURCE_GLYPH_ATLAS);
    first = build_valid_commit(&atlas);
    second = build_valid_commit(&atlas);

    assert(xg_render_source_commit_header_copy(first, &first_header) ==
           XG_RENDER_SOURCE_COMMIT_OK);
    assert(xg_render_source_commit_header_copy(second, &second_header) ==
           XG_RENDER_SOURCE_COMMIT_OK);
    assert(first_header.ui_node_count == 2u);
    assert(first_header.ui_glyph_run_count == 1u);
    assert(first_header.ui_glyph_placement_count == 2u);
    assert(first_header.digest != 0u &&
           first_header.digest == second_header.digest);
    assert(xg_render_source_commit_ui_node_copy(first, 0u, &copied_node) ==
           XG_RENDER_SOURCE_COMMIT_OK);
    assert(copied_node.node_id == 1u && copied_node.parent_node_id == 0u);
    assert(copied_node.semantic_id == 0x100u && copied_node.state == 2u &&
           copied_node.value == 1 && copied_node.maximum == 3 &&
           copied_node.flags == 0x5u);
    assert(xg_render_source_commit_ui_node_copy(first, 1u, &copied_node) ==
           XG_RENDER_SOURCE_COMMIT_OK);
    assert(copied_node.node_id == 2u && copied_node.parent_node_id == 1u);
    assert(xg_render_source_commit_ui_glyph_run_copy(first, 0u, &copied_run) ==
           XG_RENDER_SOURCE_COMMIT_OK);
    assert(copied_run.reveal_count == 2u && copied_run.total_count == 3u);
    assert(xg_render_source_commit_ui_glyph_placement_copy(
               first, 1u, &copied_placement) == XG_RENDER_SOURCE_COMMIT_OK);
    assert(copied_placement.glyph_id == 2u && copied_placement.x == 26);
    assert(xg_render_source_commit_ui_node_copy(first, 2u, &copied_node) ==
           XG_RENDER_SOURCE_COMMIT_OUT_OF_RANGE);

    assert(xg_render_source_commit_retire(first) ==
           XG_RENDER_SOURCE_COMMIT_OK);
    assert(xg_render_source_commit_retire(second) ==
           XG_RENDER_SOURCE_COMMIT_OK);
    assert(xg_render_resource_view((XgRenderResourceHandle){
               atlas.resource_id, atlas.generation}, &atlas_view) ==
           XG_RENDER_RESOURCE_OK);
    assert(atlas_view.retain_count == 0u);
}

static void test_seal_rejections(void) {
    XgSemanticResourceRef atlas;
    XgSemanticResourceRef texture;
    XgRenderSourceBuilder builder;
    XgRenderSourceCommitHandle commit;
    XgSemanticUiNodeRecord root;
    XgSemanticUiNodeRecord child;
    XgSemanticUiGlyphRunRecord run;
    XgSemanticUiGlyphPlacementRecord first;
    XgRenderResourceView resource_view;

    xg_render_resource_repository_reset();
    xg_render_source_commit_reset();
    atlas = import_resource(200u, XG_RENDER_RESOURCE_GLYPH_ATLAS);
    texture = import_resource(201u, XG_RENDER_RESOURCE_TEXTURE);

    builder = begin_builder(&atlas);
    root = root_node();
    assert(xg_render_source_commit_append_ui_node(builder, &root) ==
           XG_RENDER_SOURCE_COMMIT_OK);
    assert(xg_render_source_commit_append_ui_node(builder, &root) ==
           XG_RENDER_SOURCE_COMMIT_OK);
    assert(xg_render_source_commit_seal(builder, &commit) ==
           XG_RENDER_SOURCE_COMMIT_PROOF_REJECTED);

    assert(xg_render_resource_view((XgRenderResourceHandle){
               atlas.resource_id, atlas.generation}, &resource_view) ==
           XG_RENDER_RESOURCE_OK);
    assert(resource_view.retain_count == 0u);
    assert(xg_render_resource_view((XgRenderResourceHandle){
               texture.resource_id, texture.generation}, &resource_view) ==
           XG_RENDER_RESOURCE_OK);
    assert(resource_view.retain_count == 0u);

    builder = begin_builder(&atlas);
    root = root_node();
    child = child_node();
    root.parent_node_id = child.node_id;
    assert(xg_render_source_commit_append_ui_node(builder, &root) ==
           XG_RENDER_SOURCE_COMMIT_OK);
    assert(xg_render_source_commit_append_ui_node(builder, &child) ==
           XG_RENDER_SOURCE_COMMIT_OK);
    assert(xg_render_source_commit_seal(builder, &commit) ==
           XG_RENDER_SOURCE_COMMIT_INVALID_ORDER);

    builder = begin_builder(&atlas);
    root = root_node();
    root.order.pass_id = 2u;
    assert(xg_render_source_commit_append_ui_node(builder, &root) ==
           XG_RENDER_SOURCE_COMMIT_OK);
    assert(xg_render_source_commit_seal(builder, &commit) ==
           XG_RENDER_SOURCE_COMMIT_INVALID_ORDER);

    builder = begin_builder(&texture);
    root = root_node();
    child = child_node();
    run = glyph_run(&texture);
    first = placement(1u, 16);
    run.placement_count = run.reveal_count = 1u;
    assert(xg_render_source_commit_append_ui_node(builder, &root) ==
           XG_RENDER_SOURCE_COMMIT_OK);
    assert(xg_render_source_commit_append_ui_node(builder, &child) ==
           XG_RENDER_SOURCE_COMMIT_OK);
    assert(xg_render_source_commit_append_ui_glyph_run(builder, &run) ==
           XG_RENDER_SOURCE_COMMIT_OK);
    assert(xg_render_source_commit_append_ui_glyph_placement(builder, &first) ==
           XG_RENDER_SOURCE_COMMIT_OK);
    assert(xg_render_source_commit_seal(builder, &commit) ==
           XG_RENDER_SOURCE_COMMIT_PROOF_REJECTED);

    builder = begin_builder(&atlas);
    root = root_node();
    child = child_node();
    run = glyph_run(&atlas);
    first = placement(1u, 16);
    run.placement_count = 1u;
    run.reveal_count = run.total_count + 1u;
    assert(xg_render_source_commit_append_ui_node(builder, &root) ==
           XG_RENDER_SOURCE_COMMIT_OK);
    assert(xg_render_source_commit_append_ui_node(builder, &child) ==
           XG_RENDER_SOURCE_COMMIT_OK);
    assert(xg_render_source_commit_append_ui_glyph_run(builder, &run) ==
           XG_RENDER_SOURCE_COMMIT_OK);
    assert(xg_render_source_commit_append_ui_glyph_placement(builder, &first) ==
           XG_RENDER_SOURCE_COMMIT_OK);
    assert(xg_render_source_commit_seal(builder, &commit) ==
           XG_RENDER_SOURCE_COMMIT_PROOF_REJECTED);

    builder = begin_builder(&atlas);
    root = root_node();
    root.temporal_mode = (XgSemanticUiTemporalMode)1;
    assert(xg_render_source_commit_append_ui_node(builder, &root) ==
           XG_RENDER_SOURCE_COMMIT_OK);
    assert(xg_render_source_commit_seal(builder, &commit) ==
           XG_RENDER_SOURCE_COMMIT_PROOF_REJECTED);
}

static XgRenderSourceFrameDescription frame_description(void) {
    return (XgRenderSourceFrameDescription){
        .scene = k_scene,
        .display = k_display,
        .scene_generation = 7u,
        .temporally_eligible = true,
    };
}

static void assert_frame_counts(const XgRenderSourceFrameSnapshot *snapshot,
                                uint32_t passes, uint32_t resources,
                                uint32_t nodes) {
    assert(snapshot->pass_count == passes);
    assert(snapshot->resource_count == resources);
    assert(snapshot->ui_node_count == nodes);
    assert(snapshot->draw_count == 0u);
    assert(snapshot->surface_edge_count == 0u);
    assert(snapshot->ui_glyph_run_count == 0u);
    assert(snapshot->ui_glyph_placement_count == 0u);
}

static void test_source_frame_fragment_atomic_merge(void) {
    XgRenderSourceFrameDescription description = frame_description();
    XgRenderSourceFrameDescription incompatible = description;
    XgSemanticResourceRef primary_resource;
    XgSemanticResourceRef companion_resource;
    XgSemanticResourceRef rollback_resource;
    XgSemanticResourceRef wrong_owner_resource;
    XgSemanticResourceRef companion_resources[3];
    XgSemanticResourceRef failed_resources[2];
    XgSemanticResourceRef conflicting_resource;
    XgSemanticUiNodeRecord companion_node = root_node();
    XgRenderSourceFrameFragment fragment;
    XgRenderSourceFrameSnapshot snapshot;
    XgRenderResourceView view;

    xg_render_semantic_presentation_reset();
    xg_render_source_frame_reset();
    xg_render_surface_graph_reset();
    xg_render_resource_repository_reset();
    primary_resource = import_resource(400u, XG_RENDER_RESOURCE_TEXTURE);
    companion_resource = import_resource(401u, XG_RENDER_RESOURCE_TEXTURE);
    rollback_resource = import_resource(402u, XG_RENDER_RESOURCE_TEXTURE);
    wrong_owner_resource = import_resource_for_owner(
        403u, XG_RENDER_RESOURCE_TEXTURE,
        XG_RENDER_RESOURCE_OWNER_SCENE, 8u);

    failed_resources[0] = rollback_resource;
    failed_resources[1] = (XgSemanticResourceRef){
        .resource_id = 999u,
        .generation = 1u,
        .content_digest = 1u,
    };
    fragment = (XgRenderSourceFrameFragment){
        .resources = failed_resources,
        .resource_count = 2u,
    };
    assert(xg_render_source_frame_append_fragment(&description, &fragment) ==
           XG_RENDER_SOURCE_FRAME_REJECTED);
    xg_render_source_frame_snapshot(&snapshot);
    assert(!snapshot.active && !snapshot.complete && !snapshot.blocked);
    assert_frame_counts(&snapshot, 0u, 0u, 0u);
    assert(xg_render_resource_view((XgRenderResourceHandle){
               rollback_resource.resource_id, rollback_resource.generation},
               &view) == XG_RENDER_RESOURCE_OK && view.retain_count == 0u);

    fragment = (XgRenderSourceFrameFragment){
        .passes = &k_pass,
        .resources = &primary_resource,
        .pass_count = 1u,
        .resource_count = 1u,
    };
    assert(xg_render_source_frame_append_fragment(&description, &fragment) ==
           XG_RENDER_SOURCE_FRAME_OK);
    xg_render_source_frame_snapshot(&snapshot);
    assert(snapshot.active && !snapshot.complete && !snapshot.blocked);
    assert_frame_counts(&snapshot, 1u, 1u, 0u);

    companion_resources[0] = primary_resource;
    companion_resources[1] = companion_resource;
    companion_resources[2] = companion_resource;
    fragment = (XgRenderSourceFrameFragment){
        .resources = companion_resources,
        .ui_nodes = &companion_node,
        .resource_count = 3u,
        .ui_node_count = 1u,
    };
    assert(xg_render_source_frame_append_fragment(&description, &fragment) ==
           XG_RENDER_SOURCE_FRAME_OK);
    companion_node.node_id = 99u; /* The frame owns its synchronous copy. */
    xg_render_source_frame_snapshot(&snapshot);
    assert_frame_counts(&snapshot, 1u, 2u, 1u);
    assert(xg_render_resource_view((XgRenderResourceHandle){
               primary_resource.resource_id, primary_resource.generation},
               &view) == XG_RENDER_RESOURCE_OK && view.retain_count == 1u);
    assert(xg_render_resource_view((XgRenderResourceHandle){
               companion_resource.resource_id,
               companion_resource.generation}, &view) ==
               XG_RENDER_RESOURCE_OK && view.retain_count == 1u);

    incompatible.scene.authored_submode = 1u;
    fragment = (XgRenderSourceFrameFragment){0};
    assert(xg_render_source_frame_append_fragment(&incompatible, &fragment) ==
           XG_RENDER_SOURCE_FRAME_REJECTED);
    xg_render_source_frame_snapshot(&snapshot);
    assert_frame_counts(&snapshot, 1u, 2u, 1u);

    fragment = (XgRenderSourceFrameFragment){
        .resources = &wrong_owner_resource,
        .resource_count = 1u,
    };
    assert(xg_render_source_frame_append_fragment(&description, &fragment) ==
           XG_RENDER_SOURCE_FRAME_REJECTED);
    xg_render_source_frame_snapshot(&snapshot);
    assert_frame_counts(&snapshot, 1u, 2u, 1u);
    assert(xg_render_resource_view((XgRenderResourceHandle){
               wrong_owner_resource.resource_id,
               wrong_owner_resource.generation}, &view) ==
               XG_RENDER_RESOURCE_OK && view.retain_count == 0u);

    fragment.resources = failed_resources;
    fragment.resource_count = 2u;
    assert(xg_render_source_frame_append_fragment(&description, &fragment) ==
           XG_RENDER_SOURCE_FRAME_REJECTED);
    xg_render_source_frame_snapshot(&snapshot);
    assert_frame_counts(&snapshot, 1u, 2u, 1u);
    assert(xg_render_resource_view((XgRenderResourceHandle){
               rollback_resource.resource_id, rollback_resource.generation},
               &view) == XG_RENDER_RESOURCE_OK && view.retain_count == 0u);

    conflicting_resource = primary_resource;
    conflicting_resource.content_digest++;
    fragment = (XgRenderSourceFrameFragment){
        .resources = &conflicting_resource,
        .resource_count = 1u,
    };
    assert(xg_render_source_frame_append_fragment(&description, &fragment) ==
           XG_RENDER_SOURCE_FRAME_INVALID_ARGUMENT);
    fragment = (XgRenderSourceFrameFragment){.pass_count = 1u};
    assert(xg_render_source_frame_append_fragment(&description, &fragment) ==
           XG_RENDER_SOURCE_FRAME_INVALID_ARGUMENT);
    fragment = (XgRenderSourceFrameFragment){
        .passes = &k_pass,
        .pass_count = XG_RENDER_SCENE_PASS_CAPACITY + 1u,
    };
    assert(xg_render_source_frame_append_fragment(&description, &fragment) ==
           XG_RENDER_SOURCE_FRAME_CAPACITY_EXCEEDED);
    xg_render_source_frame_snapshot(&snapshot);
    assert_frame_counts(&snapshot, 1u, 2u, 1u);

    xg_render_source_frame_reset();
    assert(xg_render_resource_view((XgRenderResourceHandle){
               primary_resource.resource_id, primary_resource.generation},
               &view) == XG_RENDER_RESOURCE_OK && view.retain_count == 0u);
    assert(xg_render_resource_view((XgRenderResourceHandle){
               companion_resource.resource_id,
               companion_resource.generation}, &view) ==
               XG_RENDER_RESOURCE_OK && view.retain_count == 0u);
    xg_render_resource_repository_reset();
}

typedef struct FrameTrace {
    XgRenderCompiledEndpoint endpoint;
    uint64_t compile_fence;
    uint64_t completion_fence;
    bool compiled;
    bool composed;
    bool swapped;
    bool compile_fence_discarded;
    bool completion_fence_discarded;
    bool endpoint_released;
} FrameTrace;

static const uint64_t k_endpoint_handle = UINT64_C(0x55494652414d4501);
static const uint64_t k_compile_fence = UINT64_C(0x554946454e434501);
static const uint64_t k_completion_fence = UINT64_C(0x554946454e434502);
static const uint64_t k_presenter_owner = UINT64_C(0x5549524554495245);

static bool compile_frame(XgRenderSourceCommitHandle commit,
                           XgRenderCompiledEndpoint *out_endpoint,
                           uint64_t *out_fence, void *user_data) {
    FrameTrace *trace = (FrameTrace *)user_data;
    XgRenderSourceCommitHeader header;
    XgSemanticUiNodeRecord node;
    XgSemanticUiGlyphRunRecord run;
    XgSemanticUiGlyphPlacementRecord glyph;

    assert(out_endpoint != NULL && out_fence != NULL);
    assert(!trace->compiled);
    assert(xg_render_source_commit_header_copy(commit, &header) ==
            XG_RENDER_SOURCE_COMMIT_OK);
    assert(header.ui_node_count == 2u && header.ui_glyph_run_count == 1u &&
           header.ui_glyph_placement_count == 2u);
    assert(xg_render_source_commit_ui_node_copy(commit, 0u, &node) ==
           XG_RENDER_SOURCE_COMMIT_OK);
    assert(node.node_id == 1u);
    assert(xg_render_source_commit_ui_glyph_run_copy(commit, 0u, &run) ==
           XG_RENDER_SOURCE_COMMIT_OK);
    assert(run.atlas.resource_id == 300u);
    assert(xg_render_source_commit_ui_glyph_placement_copy(
                commit, 0u, &glyph) == XG_RENDER_SOURCE_COMMIT_OK);
    assert(glyph.glyph_run_id == run.glyph_run_id);
    trace->endpoint = (XgRenderCompiledEndpoint){
        .opaque_handle = k_endpoint_handle,
        .identity = header.identity,
        .digest = header.digest,
        .width = header.display.width,
        .height = header.display.height,
        .format = 1u,
        .backend_generation = 1u,
    };
    trace->compile_fence = k_compile_fence;
    trace->compiled = true;
    *out_endpoint = trace->endpoint;
    *out_fence = trace->compile_fence;
    return true;
}

static XgRenderFenceStatus ready_fence(uint64_t fence, void *user_data) {
    FrameTrace *trace = (FrameTrace *)user_data;
    assert((fence == trace->compile_fence &&
            !trace->compile_fence_discarded) ||
           (fence == trace->completion_fence &&
            !trace->completion_fence_discarded));
    return XG_RENDER_FENCE_READY;
}

static void discard_fence(uint64_t fence, void *user_data) {
    FrameTrace *trace = (FrameTrace *)user_data;
    if (fence == trace->compile_fence) {
        assert(!trace->compile_fence_discarded);
        trace->compile_fence_discarded = true;
        return;
    }
    assert(fence == trace->completion_fence);
    assert(!trace->completion_fence_discarded);
    trace->completion_fence_discarded = true;
}

static void release_endpoint(const XgRenderCompiledEndpoint *endpoint,
                             void *user_data) {
    FrameTrace *trace = (FrameTrace *)user_data;
    assert(endpoint != NULL);
    assert(endpoint->opaque_handle == trace->endpoint.opaque_handle);
    assert(endpoint->digest == trace->endpoint.digest);
    assert(endpoint->backend_generation == trace->endpoint.backend_generation);
    assert(!trace->endpoint_released);
    assert(trace->compile_fence_discarded);
    trace->endpoint_released = true;
}

static bool compose_frame(const XgRenderCompiledEndpoint *endpoint,
                          uint64_t alpha_numerator,
                          uint64_t alpha_denominator,
                          uint64_t *out_completion_fence,
                          void *user_data) {
    FrameTrace *trace = (FrameTrace *)user_data;
    assert(endpoint != NULL && out_completion_fence != NULL);
    assert(endpoint->opaque_handle == trace->endpoint.opaque_handle);
    assert(alpha_numerator == 1u && alpha_denominator == 1u);
    assert(!trace->composed);
    trace->completion_fence = k_completion_fence;
    trace->composed = true;
    *out_completion_fence = trace->completion_fence;
    return true;
}

static void swap_window(void *user_data) {
    FrameTrace *trace = (FrameTrace *)user_data;
    assert(trace->composed && !trace->swapped);
    trace->swapped = true;
}

typedef struct LateSurfaceCapture {
    XgRenderResourceHandle first;
    XgRenderResourceHandle second;
    XgSemanticSurfaceEdge edge;
    bool captured;
    bool notified;
} LateSurfaceCapture;

static XgRenderResourceIdentity surface_identity(uint8_t value) {
    XgRenderResourceIdentity identity = {0};
    identity.bytes[0] = value;
    return identity;
}

static bool capture_late_surfaces(
        XgRenderSourceCommitHandle commit,
        const XgRenderSourceCommitHeader *header, void *user_data) {
    LateSurfaceCapture *capture = (LateSurfaceCapture *)user_data;
    XgSemanticResourceRef resource;
    XgSemanticSurfaceEdge edge;
    bool found_first = false;
    bool found_second = false;

    assert(!capture->captured);
    assert(header != NULL && header->resource_count == 2u &&
           header->surface_edge_count == 1u);
    for (uint32_t index = 0u; index < header->resource_count; ++index) {
        assert(xg_render_source_commit_resource_copy(commit, index, &resource) ==
               XG_RENDER_SOURCE_COMMIT_OK);
        if (resource.resource_id == capture->first.resource_id &&
            resource.generation == capture->first.generation)
            found_first = true;
        if (resource.resource_id == capture->second.resource_id &&
            resource.generation == capture->second.generation)
            found_second = true;
    }
    assert(found_first && found_second);
    assert(xg_render_source_commit_surface_edge_copy(commit, 0u, &edge) ==
           XG_RENDER_SOURCE_COMMIT_OK);
    assert(edge.source_surface_id == capture->edge.source_surface_id &&
           edge.source_generation == capture->edge.source_generation &&
           edge.target_surface_id == capture->edge.target_surface_id &&
           edge.target_generation == capture->edge.target_generation &&
           edge.kind == capture->edge.kind && edge.width == capture->edge.width &&
           edge.height == capture->edge.height);
    capture->captured = true;
    return false; /* Make capture failure and commit retirement observable. */
}

static void late_surface_published(void *user_data) {
    LateSurfaceCapture *capture = (LateSurfaceCapture *)user_data;
    capture->notified = true;
}

static void test_late_surface_graph_publication(void) {
    static const uint8_t generated_bytes[8] = {1u, 2u, 3u, 4u,
                                                5u, 6u, 7u, 8u};
    static const uint8_t framebuffer_bytes[8] = {8u, 7u, 6u, 5u,
                                                  4u, 3u, 2u, 1u};
    XgRenderSourceFrameDescription description = frame_description();
    XgRenderSourceFrameFragment fragment = {
        .passes = &k_pass,
        .pass_count = 1u,
    };
    XgRenderSurfacePublicationDescription generated_description = {
        .identity = {{0}},
        .kind = XG_RENDER_RESOURCE_GENERATED_SURFACE,
        .format = XG_RENDER_SURFACE_VRAM16,
        .provenance = {
            .kind = XG_RENDER_RESOURCE_PROVENANCE_NONE,
            .synthetic = true,
        },
        .owner_generation = 7u,
        .width = 2u,
        .height = 2u,
        .descriptor = {
            .version = XG_RENDER_RESOURCE_DESCRIPTOR_VERSION,
            .pixel_format = XG_RENDER_RESOURCE_PIXEL_FORMAT_RGB555,
            .width = 2u,
            .height = 2u,
            .row_pitch = 4u,
        },
        .bytes = generated_bytes,
        .byte_count = sizeof(generated_bytes),
    };
    XgRenderSurfacePublicationDescription framebuffer_description =
        generated_description;
    XgRenderSurfacePublication generated;
    XgRenderSurfacePublication framebuffer;
    XgRenderSourceFrameSnapshot snapshot;
    XgRenderResourceView view;
    LateSurfaceCapture capture = {0};
    uint64_t closed_epoch = 0u;
    XgRenderSourceFrameHostCallbacks callbacks = {
        .sealed_capture = capture_late_surfaces,
        .published_notify = late_surface_published,
        .user_data = &capture,
    };

    xg_render_semantic_presentation_reset();
    xg_render_source_frame_reset();
    xg_render_surface_graph_reset();
    xg_render_resource_repository_reset();
    assert(xg_render_presentation_lifecycle_claim_open(k_presenter_owner) ==
           XG_RENDER_PRESENTATION_LIFECYCLE_OK);
    assert(xg_render_source_frame_configure_host_callbacks(&callbacks));
    assert(xg_render_source_frame_append_fragment(&description, &fragment) ==
           XG_RENDER_SOURCE_FRAME_OK);
    assert(xg_render_source_frame_complete() == XG_RENDER_SOURCE_FRAME_OK);
    xg_render_source_frame_snapshot(&snapshot);
    assert(snapshot.complete && snapshot.resource_count == 0u &&
           snapshot.surface_edge_count == 0u);

    /* GPU/MDEC-equivalent publications become visible only after assembly. */
    generated_description.identity = surface_identity(11u);
    framebuffer_description.identity = surface_identity(12u);
    framebuffer_description.kind = XG_RENDER_RESOURCE_FRAMEBUFFER;
    framebuffer_description.bytes = framebuffer_bytes;
    assert(xg_render_surface_graph_publish(&generated_description, &generated) ==
           XG_RENDER_SURFACE_GRAPH_OK);
    assert(xg_render_surface_graph_publish(
               &framebuffer_description, &framebuffer) ==
           XG_RENDER_SURFACE_GRAPH_OK);
    capture.first = generated.handle;
    capture.second = framebuffer.handle;
    capture.edge = (XgSemanticSurfaceEdge){
        .source_surface_id = generated.handle.resource_id,
        .source_generation = generated.handle.generation,
        .target_surface_id = framebuffer.handle.resource_id,
        .target_generation = framebuffer.handle.generation,
        .kind = XG_SEMANTIC_SURFACE_SAMPLE,
        .width = 2u,
        .height = 2u,
    };
    assert(xg_render_surface_graph_append_edge(&capture.edge) ==
           XG_RENDER_SURFACE_GRAPH_OK);
    xg_render_source_frame_snapshot(&snapshot);
    assert(snapshot.resource_count == 0u && snapshot.surface_edge_count == 0u);

    assert(xg_render_timeline_source_boundary(1u, 100u) ==
           XG_RENDER_TIMELINE_OK);
    assert(xg_render_source_frame_publish_boundary() ==
           XG_RENDER_SOURCE_FRAME_CAPTURE_FAILED);
    assert(capture.captured && !capture.notified);
    assert(xg_render_resource_view(generated.handle, &view) ==
           XG_RENDER_RESOURCE_OK && view.retain_count == 0u);
    assert(xg_render_resource_view(framebuffer.handle, &view) ==
           XG_RENDER_RESOURCE_OK && view.retain_count == 0u);
    assert(xg_render_presentation_lifecycle_close(
               k_presenter_owner, XG_RENDER_TIMELINE_RESET, &closed_epoch) ==
           XG_RENDER_PRESENTATION_LIFECYCLE_OK);
    assert(closed_epoch != 0u);
    xg_render_source_frame_clear_host_callbacks();
    xg_render_source_frame_reset();
    xg_render_surface_graph_reset();
    xg_render_resource_repository_reset();
}

static void test_source_frame_publication(void) {
    const XgRenderSourceFrameDescription description = {
        .scene = {
            .disc_id = 1u,
            .executable_identity = 2u,
            .primary_overlay_identity = 3u,
            .authored_scene_id = 4u,
            .module = XG_SEMANTIC_MODULE_MENU,
        },
        .display = {
            .width = 320u,
            .height = 240u,
            .aspect_num = 4u,
            .aspect_den = 3u,
        },
        .scene_generation = 7u,
        .temporally_eligible = true,
    };
    XgSemanticResourceRef atlas;
    XgSemanticUiNodeRecord root;
    XgSemanticUiNodeRecord child;
    XgSemanticUiGlyphRunRecord run;
    XgSemanticUiGlyphPlacementRecord first;
    XgSemanticUiGlyphPlacementRecord second;
    XgRenderSourceFrameSnapshot snapshot;
    XgRenderResourceView atlas_view;
    FrameTrace trace = {0};
    uint64_t closed_epoch = 0u;
    const XgRenderWorkerServices worker = {
        .compile = compile_frame,
        .fence_status = ready_fence,
        .discard_fence = discard_fence,
        .release_endpoint = release_endpoint,
        .user_data = &trace,
    };
    const XgRenderPresenterServices retirement_owner = {
        .fence_status = ready_fence,
        .compose = compose_frame,
        .swap_window = swap_window,
        .discard_fence = discard_fence,
        .user_data = &trace,
        .owner_token = k_presenter_owner,
    };

    xg_render_semantic_presentation_reset();
    xg_render_source_frame_reset();
    xg_render_surface_graph_reset();
    xg_render_resource_repository_reset();
    assert(xg_render_presentation_lifecycle_claim_open(k_presenter_owner) ==
           XG_RENDER_PRESENTATION_LIFECYCLE_OK);
    atlas = import_resource(300u, XG_RENDER_RESOURCE_GLYPH_ATLAS);
    root = root_node();
    child = child_node();
    run = glyph_run(&atlas);
    first = placement(1u, 16);
    second = placement(2u, 26);

    assert(xg_render_source_frame_begin(&description) ==
           XG_RENDER_SOURCE_FRAME_OK);
    assert(xg_render_source_frame_append_pass(&k_pass) ==
           XG_RENDER_SOURCE_FRAME_OK);
    assert(xg_render_source_frame_append_resource(&atlas) ==
           XG_RENDER_SOURCE_FRAME_OK);
    assert(xg_render_source_frame_append_ui_node(&child) ==
           XG_RENDER_SOURCE_FRAME_OK);
    assert(xg_render_source_frame_append_ui_node(&root) ==
           XG_RENDER_SOURCE_FRAME_OK);
    assert(xg_render_source_frame_append_ui_glyph_run(&run) ==
           XG_RENDER_SOURCE_FRAME_OK);
    assert(xg_render_source_frame_append_ui_glyph_placement(&first) ==
           XG_RENDER_SOURCE_FRAME_OK);
    assert(xg_render_source_frame_append_ui_glyph_placement(&second) ==
           XG_RENDER_SOURCE_FRAME_OK);
    xg_render_source_frame_snapshot(&snapshot);
    assert(snapshot.ui_node_count == 2u && snapshot.ui_glyph_run_count == 1u &&
           snapshot.ui_glyph_placement_count == 2u);
    assert(xg_render_source_frame_complete() == XG_RENDER_SOURCE_FRAME_OK);
    assert(xg_render_timeline_source_boundary(1u, 100u) ==
           XG_RENDER_TIMELINE_OK);
    assert(xg_render_source_frame_publish_boundary() ==
           XG_RENDER_SOURCE_FRAME_OK);
    assert(xg_render_worker_compile_next(&worker) == XG_RENDER_WORKER_OK);
    assert(trace.compiled);
    assert(xg_render_resource_view((XgRenderResourceHandle){
               atlas.resource_id, atlas.generation}, &atlas_view) ==
           XG_RENDER_RESOURCE_OK);
    assert(atlas_view.retain_count == 1u);

    /* Explicit close retires the queued batch and its retained atlas. */
    assert(xg_render_presentation_lifecycle_close(
               k_presenter_owner, XG_RENDER_TIMELINE_RESET, &closed_epoch) ==
           XG_RENDER_PRESENTATION_LIFECYCLE_OK);
    assert(closed_epoch != 0u);
    assert(xg_render_presenter_drain_retirements(&retirement_owner));
    assert(trace.compile_fence_discarded);
    assert(trace.endpoint_released);
    assert(!trace.composed && !trace.swapped);
    assert(xg_render_resource_view((XgRenderResourceHandle){
               atlas.resource_id, atlas.generation}, &atlas_view) ==
           XG_RENDER_RESOURCE_OK);
    assert(atlas_view.retain_count == 0u);
    xg_render_source_frame_reset();
    xg_render_surface_graph_reset();
    xg_render_resource_repository_reset();
}

int main(void) {
    test_valid_copy_sort_and_digest();
    test_seal_rejections();
    test_source_frame_fragment_atomic_merge();
    test_late_surface_graph_publication();
    test_source_frame_publication();
    return 0;
}
