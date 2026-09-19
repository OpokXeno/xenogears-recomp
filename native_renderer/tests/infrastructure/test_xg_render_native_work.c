#include "xg_render_native_work.h"
#include "xg_render_semantic_presentation.h"

#undef NDEBUG
#include <assert.h>
#include <string.h>

/* This test exercises the collector, source FIFO and real motion binding;
 * producer temporal coverage is absent in these packet fixtures. */
bool xg_render_submission_temporal_binding(
        const GpuRenderSemantic *semantic, XgRenderTemporalBinding *out) {
    (void)semantic;
    *out = (XgRenderTemporalBinding){0};
    return false;
}

static XgRenderNativeOperation captured;
static bool describe(XgRenderSourceFrameDescription *out) {
    *out = (XgRenderSourceFrameDescription){
        .scene = {.disc_id = 1u, .executable_identity = 1u,
                  .module = XG_SEMANTIC_MODULE_FIELD},
        .display = {.width = 320u, .height = 216u, .aspect_num = 16u, .aspect_den = 9u},
        .scene_generation = 1u,
    };
    return true;
}
static void notify(void *user) { (void)user; }
static XgRenderCompileResult capture(XgRenderSourceCommitHandle commit,
        XgRenderCompiledEndpoint *endpoint, uint64_t *fence, void *user) {
    (void)user;
    XgRenderSourceCommitHeader header;
    assert(xg_render_source_commit_header_copy(commit, &header) == XG_RENDER_SOURCE_COMMIT_OK);
    assert(header.native_operation_count == 1u);
    assert(xg_render_source_commit_copy_native_operation(commit, 0u, &captured) == XG_RENDER_SOURCE_COMMIT_OK);
    *endpoint = (XgRenderCompiledEndpoint){0};
    *fence = 0u;
    return XG_RENDER_COMPILE_APPLIED;
}
static XgRenderFenceStatus fence_status(uint64_t fence, void *user) {
    (void)fence; (void)user;
    return XG_RENDER_FENCE_READY;
}
static void discard(uint64_t fence, void *user) { (void)fence; (void)user; }
static void release(const XgRenderCompiledEndpoint *endpoint, void *user) {
    (void)endpoint; (void)user;
}

static void submit(const GpuRenderSemantic *semantic, unsigned accepted) {
    const GpuRenderSemantic before = *semantic;
    XgRenderNativeWorkSnapshot snapshot;
    assert(xg_render_native_work_draw(semantic, 100u));
    assert(memcmp(semantic, &before, sizeof(before)) == 0);
    xg_render_native_work_snapshot(&snapshot);
    assert(snapshot.buffered_operations == (accepted != 0u));
    if (!accepted) return;
    assert(xg_render_native_work_flush(false, 100u));
    const XgRenderWorkerServices worker = {
        .compile = capture, .fence_status = fence_status,
        .discard_fence = discard, .release_endpoint = release,
    };
    assert(xg_render_worker_compile_next(&worker) == XG_RENDER_WORKER_APPLIED);
    assert(captured.semantic.triangle_count == accepted);
    for (unsigned t = 0; t < accepted; ++t) {
        assert(captured.semantic.triangles[t].split_index == t);
        assert(captured.semantic.triangles[t].split_count == accepted);
    }
}

static GpuRenderSemantic quad(const int x[4], const int y[4]) {
    const unsigned indices[2][3] = {{0, 1, 2}, {2, 1, 3}};
    GpuRenderSemantic result = {
        .topology = GPU_RENDER_SEMANTIC_TRIANGLES, .triangle_count = 2u,
        .material = {.draw_area_right = 319u, .draw_area_bottom = 215u},
    };
    for (unsigned t = 0; t < 2; ++t) {
        result.triangles[t].split_index = t;
        result.triangles[t].split_count = 2u;
        for (unsigned v = 0; v < 3; ++v) {
            unsigned i = indices[t][v];
            result.triangles[t].vertices[v] = (GpuRenderSemanticVertex){
                .x = x[i] * INT32_C(65536), .y = y[i] * INT32_C(65536),
                .u = i * INT32_C(65536), .v = (i + 1) * INT32_C(65536),
                .r = 128u, .g = 128u, .b = 128u,
                /* Deliberately exceed PS1 limits only in the Native plane. */
                .native_view_position = 1u,
                .native_view_x = (int32_t)i * 2000 * INT32_C(65536),
                .native_view_y = (int32_t)i * 1000 * INT32_C(65536),
            };
        }
    }
    return result;
}

static XgRenderMotionDrawBinding bind_motion(GpuRenderSemantic *semantic) {
    XgRenderResourceCapabilityMetadata metadata = {
        .kind = XG_RENDER_RESOURCE_PROVENANCE_SOURCE, .receipt = 1u,
        .lifetime = XG_RENDER_RESOURCE_CAPABILITY_SCENE,
        .owner_kind = XG_RENDER_RESOURCE_OWNER_SCENE, .owner_generation = 1u,
        .source = {.source_class = XG_RENDER_RESOURCE_SOURCE_MODEL_RANGE,
                   .range_size = 4u, .range_content_digest = 1u},
    };
    XgRenderMotionSource source = {
        .presentation_epoch = 1u, .scene_generation = 1u,
        .continuity_generation = 1u, .source_update = 1u,
    };
    assert(xg_render_resource_capability_register(&metadata, &source.provenance) == XG_RENDER_RESOURCE_CAPABILITY_OK);
    XgRenderMotionPose pose = {
        .entity_id = 1u, .node_count = 1u, .geometry_id = 1u,
        .geometry_generation = 1u, .camera_id = 1u,
        .geometry_scale = 1.0, .projection_distance = 256.0,
    };
    pose.camera.rotation[3] = 1.0;
    for (unsigned i = 0; i < 3; ++i) {
        pose.camera.scale[i] = 1.0;
        pose.nodes[0].source_model_to_view.rotation[i][i] = 4096;
    }
    pose.nodes[0].id = 1u; pose.nodes[0].parent = -1;
    pose.nodes[0].local = pose.camera;
    pose.nodes[0].source_matrix_valid = 1u;
    pose.nodes[0].source_model_to_view.translation[2] = 1024;
    XgRenderMotionDrawBinding binding = {.triangle_count = 2u};
    assert(xg_render_motion_publish(&source, &pose, &binding.motion));
    for (unsigned t = 0; t < 2; ++t)
        for (unsigned v = 0; v < 3; ++v) {
            binding.local[t][v].x = t * 30 + v * 10;
            binding.vertex_ids[t][v] = t * 3 + v;
        }
    semantic->submission_command_id = 0x40004u;
    semantic->interpolation_identity = (GpuRenderInterpolationIdentity){1u, 99u, 7u, 1u};
    assert(xg_render_motion_register_command(0x40004u, &binding, 99u, 7u));
    return binding;
}

int main(void) {
    const XgRenderNativeWorkServices services = {.describe = describe, .notify = notify};
    assert(xg_render_presentation_lifecycle_claim_open(1u) == XG_RENDER_PRESENTATION_LIFECYCLE_OK);
    assert(xg_render_native_work_configure(&services));
    /* Inclusive hardware limits, offsets, widescreen and subpixel positions. */
    GpuRenderSemantic s = quad((int[]){-512, 511, -512, 511}, (int[]){-256, -256, 255, 255});
    s.material.draw_offset_x = 160; s.material.draw_offset_y = 108;
    submit(&s, 2);
    assert(memcmp(&captured.semantic, &s, sizeof(s)) == 0);
    s = quad((int[]){-512, 512, -512, 512}, (int[]){0, 0, 10, 10});
    submit(&s, 0);
    s = quad((int[]){0, 10, 0, 10}, (int[]){-256, -256, 256, 256});
    submit(&s, 0);
    /* Actual foreground quad from field 45's entry cinematic. */
    s = quad((int[]){-833, 924, -466, 1023}, (int[]){-269, -191, 20, -228});
    submit(&s, 0);
    s.triangle_count = 1; s.triangles[0].split_count = 1;
    submit(&s, 0);
    s = quad((int[]){0, 1, 2, 1025}, (int[]){0, 0, 1, 1});
    submit(&s, 1);
    assert(memcmp(captured.semantic.triangles[0].vertices, s.triangles[0].vertices,
                  sizeof(s.triangles[0].vertices)) == 0);
    /* Reject the first half, retaining the second half's UVs AND motion indices. */
    s = quad((int[]){0, 1024, 1, 2}, (int[]){0, 0, 1, 1});
    XgRenderMotionDrawBinding binding = bind_motion(&s);
    submit(&s, 1);
    assert(captured.motion.motion.handle.resource_id == binding.motion.handle.resource_id);
    assert(captured.motion.triangle_count == 1u);
    assert(memcmp(captured.motion.local[0], binding.local[1], sizeof(binding.local[1])) == 0);
    assert(memcmp(captured.motion.vertex_ids[0], binding.vertex_ids[1], sizeof(binding.vertex_ids[1])) == 0);
    for (unsigned v = 0; v < 3; ++v) {
        assert(captured.semantic.triangles[0].vertices[v].x == s.triangles[1].vertices[v].x);
        assert(captured.semantic.triangles[0].vertices[v].u == s.triangles[1].vertices[v].u);
    }
    xg_render_native_work_cancel_pending();
    xg_render_motion_reset();
    xg_render_semantic_presentation_reset();
    xg_render_resource_repository_reset();
    return 0;
}
