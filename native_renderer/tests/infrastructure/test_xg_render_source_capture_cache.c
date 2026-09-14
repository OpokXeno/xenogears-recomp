/* The cache is private to submission. Exercise the actual implementation,
 * including its eviction rule, without the unrelated GPU/legacy OT harness. */
#include "../../src/infrastructure/xg_render_submission.c"

#undef NDEBUG
#include <assert.h>

static uint64_t scene(void) { return 7u; }

XgRenderResourceResult xg_render_resource_release(XgRenderResourceHandle handle) {
    (void)handle;
    /* This fixture has no temporal resource bindings. */
    assert(false);
    return XG_RENDER_RESOURCE_INVALID_ARGUMENT;
}

static void store(uint32_t id, int32_t x) {
    const GpuRenderSemantic semantic = {
        .triangle_count = 1u,
        .triangles = {{.vertices = {{
            .x = x & ~INT32_C(65535),
            .native_view_x = x,
            .native_view_position = 1u,
        }}}},
    };
    assert(capture_source_command(&semantic, id, id, (XgRenderIrProvenanceKey){0}));
}

static void check(uint32_t id, int32_t x) {
    const uint32_t index = source_capture_by_command[id >> 2u];
    assert(index < source_capture_count);
    const XgRenderSourceCapture *capture = &source_captures[index];
    assert(capture->command.command_id == id);
    assert(capture->command.semantic.triangles[0].vertices[0].native_view_x == x);
    assert(capture->command.semantic.triangles[0].vertices[0].native_view_position);
}

int main(void) {
    const XgRenderSubmissionServices services = {.scene_generation = scene};
    xg_render_submission_configure(&services);
    const uint32_t initial = 2u * XG_RENDER_IR_ITEM_CAPACITY;
    const uint32_t count = initial + 1u;

    /* Reproduce the live failure: the legacy 8192-entry pool is full and NONE
     * of the captured polygons has reached GPU acceptance yet. */
    for (uint32_t i = 0u; i < count; ++i)
        store(4u + i * 4u, (int32_t)i + 16384);
    assert(source_capture_count == count);
    assert(source_capture_capacity > initial);
    for (uint32_t i = 0u; i < count; ++i) {
        check(4u + i * 4u, (int32_t)i + 16384);
        assert(!source_captures[source_capture_by_command[i + 1u]].consumed);
    }

    /* Updating the second packet arena reuses the exact slot, including after
     * relocation of the backing allocation, without losing subpixel geometry. */
    store(4u, 81920);
    check(4u, 81920);
    assert(source_capture_count == count);

    const uint32_t capacity = source_capture_capacity;
    for (uint32_t i = count; i < capacity; ++i)
        store(4u + i * 4u, (int32_t)i + 16384);
    const uint32_t consumed_id = 8u;
    source_captures[source_capture_by_command[consumed_id >> 2u]].consumed = true;
    store(4u + capacity * 4u, 98304);
    assert(source_capture_capacity == capacity); /* Consume before allocating. */
    assert(source_capture_count == capacity);
    check(4u, 81920); /* Unconsumed entries cannot be sacrificed. */
    check(4u + capacity * 4u, 98304);
    for (uint32_t i = 2u; i < capacity; ++i)
        check(4u + i * 4u, (int32_t)i + 16384);

    clear_source_captures();
    store(4u, 32768);
    check(4u, 32768);
    assert(source_capture_count == 1u && source_capture_capacity == capacity);
    clear_source_captures();
    free(source_captures);
    return 0;
}
