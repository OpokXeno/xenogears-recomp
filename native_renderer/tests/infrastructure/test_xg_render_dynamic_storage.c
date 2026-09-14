#include "../../src/producers/model/xg_render_model_repository.c"
#include "xg_render_motion.h"
#include "xg_render_source_commit.h"

#undef NDEBUG
#include <assert.h>

static XgRenderResourceProvenance authority(uint64_t receipt) {
    XgRenderResourceCapabilityMetadata metadata = {
        .kind = XG_RENDER_RESOURCE_PROVENANCE_SOURCE,
        .receipt = receipt,
        .lifetime = XG_RENDER_RESOURCE_CAPABILITY_SCENE,
        .owner_kind = XG_RENDER_RESOURCE_OWNER_SCENE,
        .owner_generation = 1u,
        .source = {.source_class = XG_RENDER_RESOURCE_SOURCE_MODEL_RANGE,
            .range_size = 4u, .range_content_digest = receipt},
    };
    XgRenderResourceProvenance result;
    assert(xg_render_resource_capability_register(&metadata, &result) == XG_RENDER_RESOURCE_CAPABILITY_OK);
    return result;
}

static void test_resources(void) {
    xg_render_resource_repository_reset();
    XgRenderResourceProvenance provenance = authority(1u);
    XgRenderResourceHandle first = {0};
    uint32_t payload;
    XgRenderResourceImport import = {
        .kind = XG_RENDER_RESOURCE_MODEL, .owner_kind = XG_RENDER_RESOURCE_OWNER_SCENE,
        .owner_generation = 1u, .state = XG_RENDER_RESOURCE_NATIVE_OWNED,
        .provenance = provenance, .bytes = &payload, .byte_count = sizeof(payload),
    };
    /* Keep >4096 distinct resource identities AND live generations. */
    for (uint32_t i = 0u; i < 8192u; ++i) {
        XgRenderResourceHandle handle;
        payload = i; import.resource_id = i + 1u;
        import.content_digest = xg_render_resource_digest(&payload, sizeof(payload));
        assert(xg_render_resource_import_native(&import, &handle) == XG_RENDER_RESOURCE_OK);
        if (!i) { first = handle; assert(xg_render_resource_retain(first) == XG_RENDER_RESOURCE_OK); }
    }
    /* Grow while replacing a retained current generation: both its pointer
     * inside the repository and its externally retained payload must survive. */
    payload = 0x12345678u; import.resource_id = 1u;
    import.content_digest = xg_render_resource_digest(&payload, sizeof(payload));
    XgRenderResourceHandle replacement;
    assert(xg_render_resource_import_native(&import, &replacement) == XG_RENDER_RESOURCE_OK);
    XgRenderResourceView view;
    assert(xg_render_resource_view(first, &view) == XG_RENDER_RESOURCE_OK);
    assert(!view.current && view.retain_count == 1u && *(const uint32_t *)view.bytes == 0u);
    assert(xg_render_resource_view(replacement, &view) == XG_RENDER_RESOURCE_OK);
    assert(view.current && *(const uint32_t *)view.bytes == payload);
    assert(xg_render_resource_release(first) == XG_RENDER_RESOURCE_OK);
    for (uint32_t i = 2u; i < 5000u; ++i) (void)authority(i);
    assert(xg_render_resource_capability_validate(&provenance, XG_RENDER_RESOURCE_OWNER_SCENE,
        1u, NULL) == XG_RENDER_RESOURCE_CAPABILITY_OK);
    xg_render_resource_repository_reset();
}

static void test_model_sources(void) {
    xg_render_model_repository_clear_ft4_sources();
    xg_render_model_repository_clear_ft3_sources(NULL);
    XgRenderModelFt4SourceRecord source = {.valid = true, .opcode = 0x2cu};
    for (uint32_t i = 0u; i < 4095u; ++i) {
        source.source_id = 4u + i * 4u;
        source.interpolation_primitive_id = i;
        assert(xg_render_model_repository_store_ft4_source(&source, NULL, NULL));
    }
    XgRenderModelFt4SourceRecord batch[34];
    XgRenderModelSourcePublication publications[34] = {0};
    for (uint32_t i = 0u; i < 34u; ++i) {
        batch[i] = source;
        batch[i].source_id = 4u + (4095u + i) * 4u;
        batch[i].interpolation_primitive_id = 4095u + i;
    }
    assert(xg_render_model_repository_store_ft4_sources(batch, publications, 34u, NULL));
    assert(ft4_source_count == 4129u);
    for (uint32_t i = 0u; i < 4129u; ++i) {
        uint32_t slot = xg_render_lookup_find(ft4_source_lookup, ft4_source_lookup_epoch,
            4u + i * 4u, ft4_source_count);
        assert(slot < ft4_source_count && ft4_sources[slot].interpolation_primitive_id == i);
    }
    XgRenderModelFt3SourceRecord triangle = {.valid = true, .geometry_ready = true};
    for (uint32_t i = 0u; i < 16400u; ++i) {
        triangle.source_id = 4u + i * 4u;
        triangle.interpolation_primitive_id = i;
        assert(xg_render_model_repository_store_ft3_source(&triangle, NULL, NULL));
    }
    assert(ft3_source_count == 16400u);
    assert(xg_render_model_repository_find_ft3_source(4u)->interpolation_primitive_id == 0u);
    assert(xg_render_model_repository_find_ft3_source(4u + 16399u * 4u)->interpolation_primitive_id == 16399u);
    xg_render_lookup_put(ft4_source_lookup, ft4_source_lookup_epoch, 0x1ffffcu, 65536u);
    assert(xg_render_lookup_find(ft4_source_lookup, ft4_source_lookup_epoch, 0x1ffffcu, 65537u) == 65536u);
}

static void test_motion(void) {
    XgRenderMotionSource source = {.provenance = authority(9000u),
        .presentation_epoch = 1u, .scene_generation = 1u, .continuity_generation = 1u, .source_update = 1u};
    XgRenderMotionPose pose = {.node_count = 1u, .geometry_id = 1u,
        .geometry_generation = 1u, .camera_id = 1u, .geometry_scale = 1.0,
        .projection_distance = 256.0};
    pose.camera.rotation[3] = 1.0;
    pose.camera.scale[0] = pose.camera.scale[1] = pose.camera.scale[2] = 1.0;
    pose.nodes[0].id = 1u; pose.nodes[0].parent = -1;
    pose.nodes[0].local = pose.camera;
    pose.nodes[0].source_matrix_valid = 1u;
    for (unsigned i = 0u; i < 3u; ++i) pose.nodes[0].source_model_to_view.rotation[i][i] = 4096;
    pose.nodes[0].source_model_to_view.translation[2] = 1024;
    XgRenderMotionRef first = {0}, ref;
    for (uint32_t i = 0u; i < 600u; ++i) {
        pose.entity_id = i + 1u;
        assert(xg_render_motion_publish(&source, &pose, &ref));
        if (!i) first = ref;
    }
    const XgRenderMotionPose *view;
    assert(xg_render_motion_view(first, &view) && view->entity_id == 1u);
    const XgRenderMotionDrawBinding binding = {.motion = first, .triangle_count = 1u};
    for (uint32_t i = 0u; i < 17000u; ++i)
        assert(xg_render_motion_register_command(0x40000u + i * 4u, &binding, 99u, i));
    xg_render_motion_forget_range(0x48000u, 4096u);
    for (uint32_t i = 0u; i < 17000u; ++i) {
        XgRenderNativeOperation operation = {.semantic = {
            .triangle_count = 1u, .interpolation_identity = {1u, 99u, i, 1u},
        }};
        for (unsigned v = 0u; v < 3u; ++v) operation.semantic.triangles[0].vertices[v].native_view_position = 1u;
        const uint32_t address = 0x40000u + i * 4u;
        assert(xg_render_motion_bind_command(address, &operation) == !(address >= 0x48000u && address < 0x49000u));
    }
    XgRenderMotionDiagnostics diagnostics;
    xg_render_motion_diagnostics(&diagnostics);
    assert(diagnostics.active_instances == 600u && diagnostics.active_commands == 17000u - 1024u);
    xg_render_motion_reset();
    xg_render_motion_diagnostics(&diagnostics);
    assert(diagnostics.active_instances == 0u && diagnostics.active_commands == 0u);
    xg_render_resource_repository_reset();
}

static void test_streaming_buffers(void) {
    uint32_t capacity = 0u;
    XgRenderRetiredBuffer *retired = NULL;
    uint8_t *data = xg_render_append_reserve(NULL, 1u, &capacity, 0u, 16u, UINT32_MAX, &retired);
    assert(data); memset(data, 0x5a, 16u);
    const uint8_t *published = data;
    data = xg_render_append_reserve(data, 1u, &capacity, 16u, 64u * 1024u * 1024u + 1u,
        UINT32_MAX, &retired);
    assert(data && data != published);
    for (uint32_t i = 0u; i < 16u; ++i) assert(data[i] == 0x5a && published[i] == 0x5a);
    data[64u * 1024u * 1024u] = 1u;
    /* The reader's old prefix remains valid until explicit retirement. */
    data[0] = 0u; assert(published[0] == 0x5a);
    xg_render_retired_buffers_free(&retired); free(data);
}

int main(void) {
    test_resources(); test_model_sources(); test_motion(); test_streaming_buffers();
    free(ft4_sources); free(ft4_source_producers);
    free(ft3_sources); free(ft3_source_addresses); free(ft3_source_producers);
    return 0;
}
