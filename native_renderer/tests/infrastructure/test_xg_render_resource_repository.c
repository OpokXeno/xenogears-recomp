#include "xg_render_resource_repository.h"
#include "xg_render_semantic_presentation.h"
#include "xg_render_source_commit.h"
#include "xg_render_source_frame.h"
#include "xg_render_vram_journal.h"

#include <assert.h>
#include <stdatomic.h>
#include <string.h>
#include <threads.h>

enum { CONCURRENT_CANCEL_ITERATIONS = 256 };

#define TEST_PRESENTER_OWNER UINT64_C(0x52504f5349544f52)

typedef struct ConcurrentCancelContext {
    XgRenderSourceBuilder builder;
    XgSemanticResourceRef resource;
    XgRenderSourceCommitResult result;
    atomic_uint iteration;
    atomic_uint completed;
    atomic_bool ready;
    atomic_bool go;
} ConcurrentCancelContext;

static int append_resource_concurrently(void *user_data) {
    ConcurrentCancelContext *context = (ConcurrentCancelContext *)user_data;
    unsigned int index;

    for (index = 1u; index <= CONCURRENT_CANCEL_ITERATIONS; ++index) {
        while (atomic_load_explicit(&context->iteration,
                                    memory_order_acquire) != index)
            thrd_yield();
        atomic_store_explicit(&context->ready, true, memory_order_release);
        while (!atomic_load_explicit(&context->go, memory_order_acquire))
            thrd_yield();
        context->result = xg_render_source_commit_append_resource(
            context->builder, &context->resource);
        atomic_store_explicit(&context->completed, index,
                              memory_order_release);
    }
    return 0;
}

static void test_concurrent_builder_cancellation(
        const XgPresentationIdentity *identity,
        const XgSemanticSceneIdentity *scene,
        const XgSemanticDisplayState *display,
        XgRenderResourceHandle resource,
        uint64_t content_digest) {
    ConcurrentCancelContext context = {
        .resource = {
            .resource_id = resource.resource_id,
            .generation = resource.generation,
            .content_digest = content_digest,
        },
    };
    XgRenderResourceView view;
    XgRenderResourceResult resource_result;
    XgRenderSourceCommitResult source_result;
    thrd_t append_thread;
    int thread_result;
    unsigned int index;

    atomic_init(&context.iteration, 0u);
    atomic_init(&context.completed, 0u);
    atomic_init(&context.ready, false);
    atomic_init(&context.go, false);
    thread_result = thrd_create(&append_thread, append_resource_concurrently,
                                &context);
    assert(thread_result == thrd_success);
    if (thread_result != thrd_success) return;

    for (index = 1u; index <= CONCURRENT_CANCEL_ITERATIONS; ++index) {
        atomic_store_explicit(&context.ready, false, memory_order_relaxed);
        atomic_store_explicit(&context.go, false, memory_order_relaxed);
        source_result = xg_render_source_commit_begin(
            identity, scene, display, 1u, false, false, &context.builder);
        assert(source_result == XG_RENDER_SOURCE_COMMIT_OK);
        atomic_store_explicit(&context.iteration, index, memory_order_release);
        while (!atomic_load_explicit(&context.ready, memory_order_acquire))
            thrd_yield();
        atomic_store_explicit(&context.go, true, memory_order_release);
        xg_render_source_commit_cancel_builders();
        while (atomic_load_explicit(&context.completed,
                                    memory_order_acquire) != index)
            thrd_yield();
        assert(context.result == XG_RENDER_SOURCE_COMMIT_OK ||
               context.result == XG_RENDER_SOURCE_COMMIT_INVALID_TRANSITION);
        source_result = xg_render_source_commit_append_resource(
            context.builder, &context.resource);
        assert(source_result == XG_RENDER_SOURCE_COMMIT_INVALID_TRANSITION);
        resource_result = xg_render_resource_view(resource, &view);
        assert(resource_result == XG_RENDER_RESOURCE_OK);
        assert(view.current && view.retain_count == 0u);
    }

    thread_result = thrd_join(append_thread, NULL);
    assert(thread_result == thrd_success);
}

static XgSemanticResourceRef import_test_resource(
        uint64_t resource_id, XgRenderResourceKind kind,
        const uint8_t *bytes, size_t byte_count) {
    const uint64_t digest = xg_render_resource_digest(bytes, byte_count);
    const XgRenderResourceImport import = {
        .resource_id = resource_id,
        .kind = kind,
        .owner_kind = XG_RENDER_RESOURCE_OWNER_SCENE,
        .owner_generation = 7u,
        .state = XG_RENDER_RESOURCE_NATIVE_OWNED,
        .provenance = {
            .kind = XG_RENDER_RESOURCE_PROVENANCE_NONE,
            .synthetic = true,
        },
        .content_digest = digest,
        .bytes = bytes,
        .byte_count = byte_count,
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

static void append_test_resource(XgRenderSourceBuilder builder,
                                 const XgSemanticResourceRef *resource) {
    assert(xg_render_source_commit_append_resource(builder, resource) ==
           XG_RENDER_SOURCE_COMMIT_OK);
}

static XgRenderSourceBuilder begin_test_builder(
        const XgPresentationIdentity *identity,
        const XgSemanticSceneIdentity *scene,
        const XgSemanticDisplayState *display,
        const XgSemanticPassRecord *pass) {
    XgRenderSourceBuilder builder;

    assert(xg_render_source_commit_begin(identity, scene, display, 1u,
                                         false, false, &builder) ==
           XG_RENDER_SOURCE_COMMIT_OK);
    assert(xg_render_source_commit_append_pass(builder, pass) ==
           XG_RENDER_SOURCE_COMMIT_OK);
    return builder;
}

static void test_typed_resource_bindings(
        const XgPresentationIdentity *identity,
        const XgSemanticSceneIdentity *scene,
        const XgSemanticDisplayState *display) {
    const uint8_t bytes[] = { 1u, 3u, 5u, 7u };
    const XgSemanticResourceRef texture = import_test_resource(
        20u, XG_RENDER_RESOURCE_TEXTURE, bytes, sizeof(bytes));
    const XgSemanticResourceRef clut = import_test_resource(
        21u, XG_RENDER_RESOURCE_CLUT, bytes, sizeof(bytes));
    const XgSemanticResourceRef framebuffer = import_test_resource(
        22u, XG_RENDER_RESOURCE_FRAMEBUFFER, bytes, sizeof(bytes));
    XgSemanticPassRecord pass = {
        .pass_id = 1u,
        .target_surface_id = framebuffer.resource_id,
        .target_generation = framebuffer.generation,
    };
    XgSemanticDrawRecord draw = {
        .order = { .pass_id = 1u },
        .provenance = {
            .state_id = { .scene_epoch = 1u, .state_sequence = 1u },
            .slot_index = 1u,
        },
        .texture_resource_id = texture.resource_id,
        .texture_generation = texture.generation,
        .clut_resource_id = clut.resource_id,
        .clut_generation = clut.generation,
        .has_provenance = true,
        .primitive = { .triangle_count = 1u },
    };
    XgRenderSourceBuilder builder;
    XgRenderSourceCommitHandle commit;

    xg_render_source_commit_reset();
    builder = begin_test_builder(identity, scene, display, &pass);
    append_test_resource(builder, &texture);
    append_test_resource(builder, &clut);
    append_test_resource(builder, &framebuffer);
    assert(xg_render_source_commit_append_draw(builder, &draw) ==
           XG_RENDER_SOURCE_COMMIT_OK);
    assert(xg_render_source_commit_seal(builder, &commit) ==
           XG_RENDER_SOURCE_COMMIT_OK);
    assert(xg_render_source_commit_retire(commit) ==
           XG_RENDER_SOURCE_COMMIT_OK);

    pass.target_surface_id = 0u;
    pass.target_generation = 0u;
    builder = begin_test_builder(identity, scene, display, &pass);
    append_test_resource(builder, &clut);
    draw.texture_resource_id = clut.resource_id;
    draw.texture_generation = clut.generation;
    draw.clut_resource_id = 0u;
    draw.clut_generation = 0u;
    assert(xg_render_source_commit_append_draw(builder, &draw) ==
           XG_RENDER_SOURCE_COMMIT_OK);
    assert(xg_render_source_commit_seal(builder, &commit) ==
           XG_RENDER_SOURCE_COMMIT_PROOF_REJECTED);

    builder = begin_test_builder(identity, scene, display, &pass);
    append_test_resource(builder, &texture);
    draw.texture_resource_id = 0u;
    draw.texture_generation = 0u;
    draw.clut_resource_id = texture.resource_id;
    draw.clut_generation = texture.generation;
    assert(xg_render_source_commit_append_draw(builder, &draw) ==
           XG_RENDER_SOURCE_COMMIT_OK);
    assert(xg_render_source_commit_seal(builder, &commit) ==
           XG_RENDER_SOURCE_COMMIT_PROOF_REJECTED);

    pass.target_surface_id = texture.resource_id;
    pass.target_generation = texture.generation;
    builder = begin_test_builder(identity, scene, display, &pass);
    append_test_resource(builder, &texture);
    assert(xg_render_source_commit_seal(builder, &commit) ==
           XG_RENDER_SOURCE_COMMIT_PROOF_REJECTED);
}

static void test_source_commit_resource_currentness(
        const XgPresentationIdentity *identity,
        const XgSemanticSceneIdentity *scene,
        const XgSemanticDisplayState *display) {
    const uint8_t first_bytes[] = {1u, 2u, 3u, 4u};
    const uint8_t second_bytes[] = {5u, 6u, 7u, 8u};
    const XgSemanticPassRecord pass = { .pass_id = 1u };
    XgSemanticResourceRef resource;
    XgSemanticResourceRef replacement;
    XgSemanticResourceRef copied;
    XgRenderSourceBuilder builder;
    XgRenderSourceCommitHandle commit;
    XgRenderSourceCommitHeader header;
    XgRenderResourceView view;

    xg_render_source_commit_reset();
    xg_render_resource_repository_reset();
    resource = import_test_resource(30u, XG_RENDER_RESOURCE_TEXTURE,
                                    first_bytes, sizeof(first_bytes));
    builder = begin_test_builder(identity, scene, display, &pass);
    append_test_resource(builder, &resource);
    replacement = import_test_resource(30u, XG_RENDER_RESOURCE_TEXTURE,
                                        second_bytes, sizeof(second_bytes));
    assert(xg_render_source_commit_seal(builder, &commit) ==
           XG_RENDER_SOURCE_COMMIT_PROOF_REJECTED);
    assert(xg_render_resource_view((XgRenderResourceHandle){
               resource.resource_id, resource.generation }, &view) ==
           XG_RENDER_RESOURCE_NOT_FOUND);
    assert(xg_render_resource_view((XgRenderResourceHandle){
               replacement.resource_id, replacement.generation }, &view) ==
           XG_RENDER_RESOURCE_OK);
    assert(view.current);

    resource = import_test_resource(31u, XG_RENDER_RESOURCE_TEXTURE,
                                    first_bytes, sizeof(first_bytes));
    builder = begin_test_builder(identity, scene, display, &pass);
    append_test_resource(builder, &resource);
    xg_render_resource_invalidate_owner(
        XG_RENDER_RESOURCE_OWNER_SCENE, 7u);
    assert(xg_render_source_commit_seal(builder, &commit) ==
           XG_RENDER_SOURCE_COMMIT_PROOF_REJECTED);
    assert(xg_render_resource_view((XgRenderResourceHandle){
               resource.resource_id, resource.generation }, &view) ==
           XG_RENDER_RESOURCE_NOT_FOUND);

    resource = import_test_resource(32u, XG_RENDER_RESOURCE_TEXTURE,
                                    first_bytes, sizeof(first_bytes));
    builder = begin_test_builder(identity, scene, display, &pass);
    append_test_resource(builder, &resource);
    assert(xg_render_source_commit_seal(builder, &commit) ==
           XG_RENDER_SOURCE_COMMIT_OK);
    xg_render_resource_invalidate_owner(
        XG_RENDER_RESOURCE_OWNER_SCENE, 7u);
    assert(xg_render_source_commit_header_copy(commit, &header) ==
           XG_RENDER_SOURCE_COMMIT_OK);
    assert(xg_render_source_commit_resource_copy(commit, 0u, &copied) ==
           XG_RENDER_SOURCE_COMMIT_OK);
    assert(copied.resource_id == resource.resource_id &&
           copied.generation == resource.generation);
    assert(xg_render_resource_view((XgRenderResourceHandle){
               resource.resource_id, resource.generation }, &view) ==
           XG_RENDER_RESOURCE_OK);
    assert(!view.current && view.retain_count == 1u);
    assert(xg_render_source_commit_retire(commit) ==
           XG_RENDER_SOURCE_COMMIT_OK);
    assert(xg_render_resource_view((XgRenderResourceHandle){
               resource.resource_id, resource.generation }, &view) ==
           XG_RENDER_RESOURCE_NOT_FOUND);
}

static void test_authenticated_resource_identity(void) {
    const uint8_t first_bytes[] = { 2u, 4u, 6u, 8u };
    const uint8_t second_bytes[] = { 1u, 3u, 5u, 7u };
    XgRenderResourceIdentity first_identity = {{0}};
    XgRenderResourceIdentity colliding_identity = {{0}};
    XgRenderResourceImport import = {
        .kind = XG_RENDER_RESOURCE_TEXTURE,
        .owner_kind = XG_RENDER_RESOURCE_OWNER_MODULE,
        .owner_generation = 9u,
        .state = XG_RENDER_RESOURCE_NATIVE_OWNED,
        .provenance = {
            .kind = XG_RENDER_RESOURCE_PROVENANCE_NONE,
            .synthetic = true,
        },
        .bytes = first_bytes,
        .byte_count = sizeof(first_bytes),
    };
    XgRenderResourceHandle first;
    XgRenderResourceHandle second;
    XgRenderResourceView view;

    for (uint32_t index = 0u; index < XG_RENDER_RESOURCE_IDENTITY_SIZE;
         ++index) {
        first_identity.bytes[index] = (uint8_t)(index + 1u);
        colliding_identity.bytes[index] = first_identity.bytes[index];
    }
    colliding_identity.bytes[XG_RENDER_RESOURCE_IDENTITY_SIZE - 1u] ^= 1u;
    import.identity = first_identity;
    import.resource_id = xg_render_resource_identity_id(&first_identity);
    import.content_digest = xg_render_resource_digest(
        first_bytes, sizeof(first_bytes));
    assert(import.resource_id != 0u);
    assert(xg_render_resource_import(&import, &first) ==
           XG_RENDER_RESOURCE_OK);
    assert(xg_render_resource_view(first, &view) == XG_RENDER_RESOURCE_OK);
    assert(view.has_identity);
    assert(memcmp(&view.identity, &first_identity, sizeof(first_identity)) == 0);

    import.bytes = second_bytes;
    import.byte_count = sizeof(second_bytes);
    import.content_digest = xg_render_resource_digest(
        second_bytes, sizeof(second_bytes));
    assert(xg_render_resource_import(&import, &second) ==
           XG_RENDER_RESOURCE_OK);
    assert(second.resource_id == first.resource_id);
    assert(second.generation != first.generation);

    import.identity = colliding_identity;
    assert(xg_render_resource_identity_id(&colliding_identity) ==
           import.resource_id);
    assert(xg_render_resource_import(&import, &first) ==
           XG_RENDER_RESOURCE_IDENTITY_COLLISION);
    import.resource_id++;
    assert(xg_render_resource_import(&import, &first) ==
           XG_RENDER_RESOURCE_INVALID_ARGUMENT);
    import.resource_id--;
    assert(xg_render_resource_retain(second) == XG_RENDER_RESOURCE_OK);
    xg_render_resource_invalidate_owner(XG_RENDER_RESOURCE_OWNER_MODULE, 9u);
    assert(xg_render_resource_import(&import, &first) ==
           XG_RENDER_RESOURCE_IDENTITY_COLLISION);
    assert(xg_render_resource_release(second) == XG_RENDER_RESOURCE_OK);
    assert(xg_render_resource_import(&import, &first) ==
           XG_RENDER_RESOURCE_OK);
}

static void test_native_import_transition(void) {
    const uint8_t old_bytes[4] = {1u, 2u, 3u, 4u};
    const uint8_t new_bytes[4] = {5u, 6u, 7u, 8u};
    XgRenderResourceImport import = {
        .resource_id = 77u,
        .kind = XG_RENDER_RESOURCE_GENERATED_SURFACE,
        .owner_kind = XG_RENDER_RESOURCE_OWNER_SCENE,
        .owner_generation = 9u,
        .state = XG_RENDER_RESOURCE_NATIVE_OWNED,
        .provenance = {
            .kind = XG_RENDER_RESOURCE_PROVENANCE_NONE,
            .synthetic = true,
        },
        .bytes = old_bytes,
        .byte_count = sizeof(old_bytes),
    };
    XgRenderResourceHandle old_handle;
    XgRenderResourceHandle pending_handle;
    XgRenderResourceHandle duplicate_pending_handle;
    XgRenderResourceView view;
    XgRenderResourceDiagnostics diagnostics;

    xg_render_resource_repository_reset();
    import.content_digest = xg_render_resource_digest(
        import.bytes, import.byte_count);
    assert(xg_render_resource_import_native(&import, &old_handle) ==
           XG_RENDER_RESOURCE_OK);
    assert(xg_render_resource_retain(old_handle) == XG_RENDER_RESOURCE_OK);

    import.state = XG_RENDER_RESOURCE_IMPORTING_NATIVE;
    import.bytes = new_bytes;
    import.byte_count = sizeof(new_bytes);
    import.content_digest = xg_render_resource_digest(
        import.bytes, import.byte_count);
    assert(xg_render_resource_import_begin(&import, &pending_handle) ==
           XG_RENDER_RESOURCE_OK);
    assert(xg_render_resource_view(old_handle, &view) == XG_RENDER_RESOURCE_OK);
    assert(view.current && view.state == XG_RENDER_RESOURCE_NATIVE_OWNED);
    assert(xg_render_resource_view(pending_handle, &view) ==
           XG_RENDER_RESOURCE_OK);
    assert(!view.current &&
           view.state == XG_RENDER_RESOURCE_IMPORTING_NATIVE);
    assert(xg_render_resource_import_begin(
               &import, &duplicate_pending_handle) ==
           XG_RENDER_RESOURCE_INVALID_STATE);
    assert(xg_render_resource_acquire_current(
               pending_handle, import.content_digest) ==
           XG_RENDER_RESOURCE_STALE);
    assert(xg_render_resource_import_cancel(pending_handle) ==
           XG_RENDER_RESOURCE_OK);
    assert(xg_render_resource_view(old_handle, &view) == XG_RENDER_RESOURCE_OK);
    assert(view.current);

    assert(xg_render_resource_import_begin(&import, &pending_handle) ==
           XG_RENDER_RESOURCE_OK);
    assert(xg_render_resource_import_commit(pending_handle) ==
           XG_RENDER_RESOURCE_OK);
    assert(xg_render_resource_view(old_handle, &view) == XG_RENDER_RESOURCE_OK);
    assert(!view.current && view.retain_count == 1u);
    assert(xg_render_resource_view(pending_handle, &view) ==
           XG_RENDER_RESOURCE_OK);
    assert(view.current && view.state == XG_RENDER_RESOURCE_NATIVE_OWNED);
    assert(memcmp(view.bytes, new_bytes, sizeof(new_bytes)) == 0);
    xg_render_resource_repository_diagnostics(&diagnostics);
    assert(diagnostics.import_starts == 3u);
    assert(diagnostics.imports == 2u);
    assert(diagnostics.import_cancellations == 1u);
    assert(xg_render_resource_release(old_handle) == XG_RENDER_RESOURCE_OK);
}

static void test_transactional_generation_rollback(void) {
    const uint8_t old_bytes[4] = {1u, 3u, 5u, 7u};
    const uint8_t new_bytes[4] = {2u, 4u, 6u, 8u};
    const uint8_t third_bytes[4] = {9u, 10u, 11u, 12u};
    XgRenderResourceImport import = {
        .resource_id = 78u,
        .kind = XG_RENDER_RESOURCE_MOVIE_FRAME,
        .owner_kind = XG_RENDER_RESOURCE_OWNER_SOURCE,
        .owner_generation = 10u,
        .state = XG_RENDER_RESOURCE_NATIVE_OWNED,
        .provenance = {
            .kind = XG_RENDER_RESOURCE_PROVENANCE_NONE,
            .synthetic = true,
        },
        .bytes = old_bytes,
        .byte_count = sizeof(old_bytes),
    };
    XgRenderResourceHandle old_handle;
    XgRenderResourceHandle new_handle;
    XgRenderResourceHandle third_handle;
    XgRenderResourceView view;

    xg_render_resource_repository_reset();
    import.content_digest = xg_render_resource_digest(
        import.bytes, import.byte_count);
    assert(xg_render_resource_import_native(&import, &old_handle) ==
           XG_RENDER_RESOURCE_OK);
    assert(xg_render_resource_retain(old_handle) == XG_RENDER_RESOURCE_OK);
    import.state = XG_RENDER_RESOURCE_IMPORTING_NATIVE;
    import.bytes = new_bytes;
    import.content_digest = xg_render_resource_digest(
        import.bytes, import.byte_count);
    assert(xg_render_resource_import_begin(&import, &new_handle) ==
           XG_RENDER_RESOURCE_OK);
    assert(xg_render_resource_import_commit(new_handle) ==
           XG_RENDER_RESOURCE_OK);
    assert(xg_render_resource_rollback_current(new_handle, old_handle) ==
           XG_RENDER_RESOURCE_OK);
    assert(xg_render_resource_view(old_handle, &view) == XG_RENDER_RESOURCE_OK);
    assert(view.current && view.retain_count == 1u);
    assert(memcmp(view.bytes, old_bytes, sizeof(old_bytes)) == 0);
    assert(xg_render_resource_view(new_handle, &view) ==
           XG_RENDER_RESOURCE_NOT_FOUND);
    assert(xg_render_resource_release(old_handle) == XG_RENDER_RESOURCE_OK);

    import.state = XG_RENDER_RESOURCE_IMPORTING_NATIVE;
    import.bytes = new_bytes;
    import.content_digest = xg_render_resource_digest(
        import.bytes, import.byte_count);
    assert(xg_render_resource_retain(old_handle) == XG_RENDER_RESOURCE_OK);
    assert(xg_render_resource_import_begin(&import, &new_handle) ==
           XG_RENDER_RESOURCE_OK);
    assert(xg_render_resource_import_commit(new_handle) ==
           XG_RENDER_RESOURCE_OK);
    assert(xg_render_resource_retain(new_handle) == XG_RENDER_RESOURCE_OK);
    import.state = XG_RENDER_RESOURCE_NATIVE_OWNED;
    import.bytes = third_bytes;
    import.content_digest = xg_render_resource_digest(
        import.bytes, import.byte_count);
    assert(xg_render_resource_import_native(&import, &third_handle) ==
           XG_RENDER_RESOURCE_OK);
    assert(xg_render_resource_rollback_current(new_handle, old_handle) ==
           XG_RENDER_RESOURCE_STALE);
    assert(xg_render_resource_view(third_handle, &view) ==
           XG_RENDER_RESOURCE_OK);
    assert(view.current);
    assert(xg_render_resource_view(old_handle, &view) == XG_RENDER_RESOURCE_OK);
    assert(!view.current && view.retain_count == 1u);
    assert(xg_render_resource_release(new_handle) == XG_RENDER_RESOURCE_OK);
    assert(xg_render_resource_release(old_handle) == XG_RENDER_RESOURCE_OK);
}

static void test_import_validation_and_provenance(void) {
    const uint8_t bytes[] = {1u, 2u, 3u, 4u};
    XgRenderResourceImport import = {
        .resource_id = 100u,
        .kind = XG_RENDER_RESOURCE_TEXTURE,
        .owner_kind = XG_RENDER_RESOURCE_OWNER_SCENE,
        .owner_generation = 1u,
        .state = XG_RENDER_RESOURCE_NATIVE_OWNED,
        .bytes = bytes,
        .byte_count = sizeof(bytes),
        .descriptor = {
            .version = XG_RENDER_RESOURCE_DESCRIPTOR_VERSION,
            .pixel_format = XG_RENDER_RESOURCE_PIXEL_FORMAT_RGBA8,
            .width = 1u,
            .height = 1u,
            .row_pitch = 4u,
        },
    };
    XgRenderResourceHandle handle;
    XgRenderResourceView view;
    XgRenderResourceCapabilityMetadata metadata = {
        .kind = XG_RENDER_RESOURCE_PROVENANCE_ARTIFACT,
        .receipt = 11u,
        .lifetime = XG_RENDER_RESOURCE_CAPABILITY_SCENE,
        .owner_kind = XG_RENDER_RESOURCE_OWNER_SCENE,
        .owner_generation = 1u,
        .artifact = {
            .base = UINT32_C(0x80010000),
            .size = 4u,
            .crc32 = 1u,
        },
    };

    xg_render_resource_repository_reset();
    import.content_digest = xg_render_resource_digest(bytes, sizeof(bytes));
    assert(xg_render_resource_import(&import, &handle) ==
           XG_RENDER_RESOURCE_INVALID_ARGUMENT);
    import.provenance = (XgRenderResourceProvenance){
        .kind = XG_RENDER_RESOURCE_PROVENANCE_NONE,
        .synthetic = true,
    };
    import.kind = (XgRenderResourceKind)-1;
    assert(xg_render_resource_import(&import, &handle) ==
           XG_RENDER_RESOURCE_INVALID_ARGUMENT);
    import.kind = (XgRenderResourceKind)(XG_RENDER_RESOURCE_MOVIE_FRAME + 1);
    assert(xg_render_resource_import(&import, &handle) ==
           XG_RENDER_RESOURCE_INVALID_ARGUMENT);
    import.kind = XG_RENDER_RESOURCE_TEXTURE;
    import.owner_kind = (XgRenderResourceOwnerKind)-1;
    assert(xg_render_resource_import(&import, &handle) ==
           XG_RENDER_RESOURCE_INVALID_ARGUMENT);
    import.owner_kind =
        (XgRenderResourceOwnerKind)(XG_RENDER_RESOURCE_OWNER_BATCH + 1);
    assert(xg_render_resource_import(&import, &handle) ==
           XG_RENDER_RESOURCE_INVALID_ARGUMENT);
    import.owner_kind = XG_RENDER_RESOURCE_OWNER_SCENE;
    import.provenance.receipt = 1u;
    assert(xg_render_resource_import(&import, &handle) ==
           XG_RENDER_RESOURCE_INVALID_ARGUMENT);
    import.provenance = (XgRenderResourceProvenance){
        .kind = XG_RENDER_RESOURCE_PROVENANCE_ARTIFACT,
    };
    assert(xg_render_resource_import_native(&import, &handle) ==
           XG_RENDER_RESOURCE_INVALID_ARGUMENT);
    import.provenance.receipt = 11u;
    import.provenance.synthetic = true;
    assert(xg_render_resource_import_native(&import, &handle) ==
           XG_RENDER_RESOURCE_INVALID_ARGUMENT);
    import.provenance.synthetic = false;
    assert(xg_render_resource_import_native(&import, &handle) ==
           XG_RENDER_RESOURCE_INVALID_ARGUMENT);
    assert(xg_render_resource_capability_register(
               &metadata, &import.provenance) ==
           XG_RENDER_RESOURCE_CAPABILITY_OK);
    assert(xg_render_resource_import_native(&import, &handle) ==
           XG_RENDER_RESOURCE_OK);
    assert(xg_render_resource_view(handle, &view) == XG_RENDER_RESOURCE_OK);
    assert(view.provenance.kind == XG_RENDER_RESOURCE_PROVENANCE_ARTIFACT);
    assert(view.provenance.receipt == 11u && !view.provenance.synthetic);

    import.resource_id++;
    import.descriptor = (XgRenderResourceDescriptor){0};
    assert(xg_render_resource_import(&import, &handle) ==
           XG_RENDER_RESOURCE_INVALID_ARGUMENT);
    import.descriptor = (XgRenderResourceDescriptor){
        .version = XG_RENDER_RESOURCE_DESCRIPTOR_VERSION,
        .pixel_format = XG_RENDER_RESOURCE_PIXEL_FORMAT_RGBA8,
        .width = 1u,
        .height = 1u,
        .row_pitch = 4u,
    };
    import.provenance.kind = (XgRenderResourceProvenanceKind)99;
    assert(xg_render_resource_import(&import, &handle) ==
           XG_RENDER_RESOURCE_INVALID_ARGUMENT);
}

static void test_immutable_resource_descriptors(void) {
    const uint8_t bytes[] = {1u, 2u, 3u, 4u};
    XgRenderResourceImport import = {
        .resource_id = 501u,
        .kind = XG_RENDER_RESOURCE_TEXTURE,
        .owner_kind = XG_RENDER_RESOURCE_OWNER_SCENE,
        .owner_generation = 1u,
        .state = XG_RENDER_RESOURCE_NATIVE_OWNED,
        .provenance = {
            .kind = XG_RENDER_RESOURCE_PROVENANCE_NONE,
            .synthetic = true,
        },
        .bytes = bytes,
        .byte_count = sizeof(bytes),
        .descriptor = {
            .version = XG_RENDER_RESOURCE_DESCRIPTOR_VERSION,
            .pixel_format = XG_RENDER_RESOURCE_PIXEL_FORMAT_RGBA8,
            .width = 1u,
            .height = 1u,
            .row_pitch = 4u,
            .sampler = XG_RENDER_RESOURCE_SAMPLER_NEAREST,
            .wrap_u = XG_RENDER_RESOURCE_WRAP_CLAMP,
            .wrap_v = XG_RENDER_RESOURCE_WRAP_CLAMP,
        },
    };
    XgRenderResourceHandle first;
    XgRenderResourceHandle second;
    XgRenderResourceView view;

    xg_render_resource_repository_reset();
    import.content_digest = xg_render_resource_digest(bytes, sizeof(bytes));
    assert(xg_render_resource_descriptor_validate(&import.descriptor));
    assert(xg_render_resource_import_native(&import, &first) ==
           XG_RENDER_RESOURCE_OK);
    import.descriptor.width = 99u;
    assert(xg_render_resource_view(first, &view) == XG_RENDER_RESOURCE_OK);
    assert(view.descriptor.version == XG_RENDER_RESOURCE_DESCRIPTOR_VERSION);
    assert(view.descriptor.pixel_format ==
           XG_RENDER_RESOURCE_PIXEL_FORMAT_RGBA8);
    assert(view.descriptor.width == 1u && view.descriptor.height == 1u);

    import.descriptor.width = 1u;
    import.descriptor.pixel_format = XG_RENDER_RESOURCE_PIXEL_FORMAT_RGB24;
    assert(xg_render_resource_import_native(&import, &second) ==
           XG_RENDER_RESOURCE_OK);
    assert(second.generation != first.generation);
    assert(xg_render_resource_view(second, &view) == XG_RENDER_RESOURCE_OK);
    assert(view.descriptor.pixel_format ==
           XG_RENDER_RESOURCE_PIXEL_FORMAT_RGB24);

    import.resource_id++;
    import.descriptor.version++;
    assert(!xg_render_resource_descriptor_validate(&import.descriptor));
    assert(xg_render_resource_import_native(&import, &first) ==
           XG_RENDER_RESOURCE_INVALID_ARGUMENT);
    import.descriptor = (XgRenderResourceDescriptor){
        .version = XG_RENDER_RESOURCE_DESCRIPTOR_VERSION,
        .pixel_format = XG_RENDER_RESOURCE_PIXEL_FORMAT_RGB555,
        .width = 1u,
        .height = 1u,
        .row_pitch = 2u,
        .vram_x = 1023u,
        .vram_width = 2u,
        .vram_height = 1u,
        .sampler = XG_RENDER_RESOURCE_SAMPLER_NEAREST,
        .wrap_u = XG_RENDER_RESOURCE_WRAP_CLAMP,
        .wrap_v = XG_RENDER_RESOURCE_WRAP_CLAMP,
        .flags = XG_RENDER_RESOURCE_DESCRIPTOR_HAS_VRAM_REGION,
    };
    assert(!xg_render_resource_descriptor_validate(&import.descriptor));
}

static void test_capacity_and_identity_reuse(void) {
    enum {
        REUSE_ITERATIONS = XG_RENDER_RESOURCE_REPOSITORY_CAPACITY + 257u,
    };
    uint8_t bytes[8] = {0};
    XgRenderResourceImport import = {
        .kind = XG_RENDER_RESOURCE_MODEL,
        .owner_kind = XG_RENDER_RESOURCE_OWNER_SCENE,
        .state = XG_RENDER_RESOURCE_NATIVE_OWNED,
        .provenance = {
            .kind = XG_RENDER_RESOURCE_PROVENANCE_NONE,
            .synthetic = true,
        },
        .bytes = bytes,
        .byte_count = sizeof(bytes),
    };
    XgRenderResourceHandle first = {0};
    XgRenderResourceHandle current = {0};
    XgRenderResourceView view;
    XgRenderResourceDiagnostics diagnostics;

    xg_render_resource_repository_reset();
    for (uint32_t index = 0u; index < REUSE_ITERATIONS; ++index) {
        memcpy(bytes, &index, sizeof(index));
        import.resource_id = UINT64_C(1000) + index;
        import.owner_generation = (uint64_t)index + 1u;
        import.content_digest = xg_render_resource_digest(bytes, sizeof(bytes));
        assert(xg_render_resource_import(&import, &current) ==
               XG_RENDER_RESOURCE_OK);
        if (index == 0u) first = current;
        xg_render_resource_invalidate_owner(
            XG_RENDER_RESOURCE_OWNER_SCENE, import.owner_generation);
        assert(xg_render_resource_view(current, &view) ==
               XG_RENDER_RESOURCE_NOT_FOUND);
    }
    xg_render_resource_repository_diagnostics(&diagnostics);
    assert(diagnostics.live_resources == 0u);
    assert(xg_render_resource_view(first, &view) ==
           XG_RENDER_RESOURCE_NOT_FOUND);

    import.resource_id = UINT64_C(900000);
    import.owner_generation = 1u;
    for (uint32_t index = 0u; index < REUSE_ITERATIONS; ++index) {
        const uint32_t value = index + 1u;

        memcpy(bytes, &value, sizeof(value));
        import.content_digest = xg_render_resource_digest(bytes, sizeof(bytes));
        assert(xg_render_resource_import(&import, &current) ==
               XG_RENDER_RESOURCE_OK);
        if (index == 0u) first = current;
    }
    assert(current.generation != first.generation);
    assert(xg_render_resource_view(first, &view) ==
           XG_RENDER_RESOURCE_NOT_FOUND);
    assert(xg_render_resource_view(current, &view) == XG_RENDER_RESOURCE_OK);
    assert(view.current);
}

static void test_exact_owner_invalidation(void) {
    const uint8_t bytes[] = {5u, 6u, 7u, 8u};
    const XgRenderResourceOwnerKind owner_kinds[] = {
        XG_RENDER_RESOURCE_OWNER_STATIC,
        XG_RENDER_RESOURCE_OWNER_DISC,
        XG_RENDER_RESOURCE_OWNER_MODULE,
        XG_RENDER_RESOURCE_OWNER_SCENE,
        XG_RENDER_RESOURCE_OWNER_SCENE,
        XG_RENDER_RESOURCE_OWNER_SOURCE,
        XG_RENDER_RESOURCE_OWNER_BATCH,
    };
    XgRenderResourceHandle handles[7];
    XgRenderResourceImport import = {
        .kind = XG_RENDER_RESOURCE_TEXTURE,
        .state = XG_RENDER_RESOURCE_NATIVE_OWNED,
        .provenance = {
            .kind = XG_RENDER_RESOURCE_PROVENANCE_NONE,
            .synthetic = true,
        },
        .bytes = bytes,
        .byte_count = sizeof(bytes),
    };
    XgRenderResourceView view;

    xg_render_resource_repository_reset();
    import.content_digest = xg_render_resource_digest(bytes, sizeof(bytes));
    for (uint32_t index = 0u; index < 7u; ++index) {
        import.resource_id = 200u + index;
        import.owner_kind = owner_kinds[index];
        import.owner_generation = index == 4u ? 43u : 42u;
        assert(xg_render_resource_import(&import, &handles[index]) ==
               XG_RENDER_RESOURCE_OK);
    }
    xg_render_resource_invalidate_owner((XgRenderResourceOwnerKind)-1, 42u);
    xg_render_resource_invalidate_owner(XG_RENDER_RESOURCE_OWNER_SCENE, 42u);
    assert(xg_render_resource_view(handles[3], &view) ==
           XG_RENDER_RESOURCE_NOT_FOUND);
    assert(xg_render_resource_view(handles[4], &view) == XG_RENDER_RESOURCE_OK);
    assert(xg_render_resource_view(handles[2], &view) == XG_RENDER_RESOURCE_OK);

    xg_render_resource_invalidate_scene_boundary();
    for (uint32_t index = 0u; index < 3u; ++index) {
        assert(xg_render_resource_view(handles[index], &view) ==
               XG_RENDER_RESOURCE_OK);
        assert(view.current);
    }
    for (uint32_t index = 4u; index < 7u; ++index)
        assert(xg_render_resource_view(handles[index], &view) ==
               XG_RENDER_RESOURCE_NOT_FOUND);
}

static void test_capability_checkpoint_cold_restore_and_reclamation(void) {
    XgRenderResourceCapabilityMetadata metadata = {
        .kind = XG_RENDER_RESOURCE_PROVENANCE_SOURCE,
        .receipt = UINT64_C(0x12345678),
        .lifetime = XG_RENDER_RESOURCE_CAPABILITY_SCENE,
        .owner_kind = XG_RENDER_RESOURCE_OWNER_SCENE,
        .owner_generation = 41u,
        .source = {
            .source_class = XG_RENDER_RESOURCE_SOURCE_MODEL_RANGE,
            .range_offset = UINT64_C(0x11223344),
            .range_size = 64u,
            .range_content_digest = UINT64_C(0xaabbccdd),
            .origin_artifact = {
                .base = UINT32_C(0x80010000),
                .size = UINT32_C(0x1000),
                .crc32 = UINT32_C(0x10203040),
            },
        },
    };
    XgRenderResourceCapabilityMetadata restored_metadata;
    XgRenderResourceCapabilityCheckpoint checkpoint;
    XgRenderResourceCapabilityCheckpoint tampered;
    XgRenderResourceProvenance original;
    XgRenderResourceProvenance restored;
    XgRenderResourceDiagnostics diagnostics;
    XgRenderResourceProvenance duplicate;
    XgRenderResourceProvenance replacement;
    bool created = false;

    for (size_t index = 0u; index < sizeof(metadata.source.identity.bytes);
         ++index)
        metadata.source.identity.bytes[index] = (uint8_t)(index + 1u);
    for (size_t index = 0u;
         index < sizeof(metadata.source.origin_artifact.sha256); ++index)
        metadata.source.origin_artifact.sha256[index] =
            (uint8_t)(0x80u + index);

    xg_render_resource_repository_reset();
    assert(xg_render_resource_capability_register_tracked(
               &metadata, &original, &created) ==
           XG_RENDER_RESOURCE_CAPABILITY_OK);
    assert(created);
    assert(xg_render_resource_capability_register_tracked(
               &metadata, &duplicate, &created) ==
           XG_RENDER_RESOURCE_CAPABILITY_OK);
    assert(!created && duplicate.capability == original.capability);
    assert(xg_render_resource_capability_checkpoint(
               &original, metadata.owner_kind, metadata.owner_generation,
               &checkpoint) == XG_RENDER_RESOURCE_CAPABILITY_OK);
    {
        const size_t authority_fields[] = {
            16u, 20u, 28u, 32u, 44u, 88u, 92u, 148u,
        };
        for (size_t index = 0u;
             index < sizeof(authority_fields) / sizeof(authority_fields[0]);
             ++index) {
            tampered = checkpoint;
            tampered.bytes[authority_fields[index]] ^= 1u;
            assert(xg_render_resource_capability_checkpoint_validate(
                       &tampered, &original, metadata.owner_kind,
                       metadata.owner_generation, NULL) !=
                   XG_RENDER_RESOURCE_CAPABILITY_OK);
        }
    }

    xg_render_resource_repository_reset();
    assert(xg_render_resource_capability_validate(
               &original, metadata.owner_kind, metadata.owner_generation,
               NULL) == XG_RENDER_RESOURCE_CAPABILITY_NOT_FOUND);
    assert(xg_render_resource_capability_restore_begin());
    assert(xg_render_resource_capability_checkpoint_restore_stage(
               &checkpoint, &original, metadata.owner_kind,
               metadata.owner_generation, 77u, &restored) ==
           XG_RENDER_RESOURCE_CAPABILITY_OK);
    assert(restored.capability != original.capability);
    xg_render_resource_capability_restore_cancel();
    assert(xg_render_resource_capability_validate(
               &restored, metadata.owner_kind, 77u, NULL) ==
           XG_RENDER_RESOURCE_CAPABILITY_NOT_FOUND);
    xg_render_resource_repository_diagnostics(&diagnostics);
    assert(diagnostics.capability_slots == 0u);
    assert(xg_render_resource_capability_restore_begin());
    assert(xg_render_resource_capability_checkpoint_restore_stage(
               &checkpoint, &original, metadata.owner_kind,
               metadata.owner_generation, 77u, &restored) ==
           XG_RENDER_RESOURCE_CAPABILITY_OK);
    assert(xg_render_resource_capability_validate(
               &original, metadata.owner_kind, 77u, NULL) ==
           XG_RENDER_RESOURCE_CAPABILITY_NOT_FOUND);
    xg_render_resource_capability_restore_commit();
    assert(xg_render_resource_capability_validate(
               &restored, metadata.owner_kind, 77u, &restored_metadata) ==
           XG_RENDER_RESOURCE_CAPABILITY_OK);
    metadata.owner_generation = 77u;
    assert(memcmp(&restored_metadata, &metadata, sizeof(metadata)) == 0);

    {
        const uint8_t first_bytes[] = {1u, 2u, 3u, 4u};
        const uint8_t second_bytes[] = {5u, 6u, 7u, 8u};
        XgRenderResourceImport import = {
            .resource_id = xg_render_resource_identity_id(
                &metadata.source.identity),
            .kind = XG_RENDER_RESOURCE_TEXTURE,
            .owner_kind = metadata.owner_kind,
            .owner_generation = metadata.owner_generation,
            .state = XG_RENDER_RESOURCE_NATIVE_OWNED,
            .provenance = restored,
            .bytes = first_bytes,
            .byte_count = sizeof(first_bytes),
            .identity = metadata.source.identity,
            .descriptor = {
                .version = XG_RENDER_RESOURCE_DESCRIPTOR_VERSION,
                .pixel_format = XG_RENDER_RESOURCE_PIXEL_FORMAT_RGB555,
                .width = 2u,
                .height = 1u,
                .row_pitch = 4u,
            },
        };
        XgRenderResourceHandle first;
        XgRenderResourceHandle second;

        import.content_digest = xg_render_resource_digest(
            import.bytes, import.byte_count);
        assert(xg_render_resource_import_native(&import, &first) ==
               XG_RENDER_RESOURCE_OK);
        assert(xg_render_resource_retain(first) == XG_RENDER_RESOURCE_OK);
        assert(xg_render_resource_capability_retire(restored) ==
               XG_RENDER_RESOURCE_CAPABILITY_OK);
        assert(xg_render_resource_capability_validate(
                   &restored, metadata.owner_kind, 77u, NULL) ==
               XG_RENDER_RESOURCE_CAPABILITY_OK);
        assert(xg_render_resource_capability_restore_begin());
        assert(xg_render_resource_capability_restore_stage(
                   restored, metadata.owner_kind, 77u) ==
               XG_RENDER_RESOURCE_CAPABILITY_OK);
        xg_render_resource_capability_restore_commit();
        assert(xg_render_resource_capability_validate(
                   &restored, metadata.owner_kind, 77u, NULL) ==
               XG_RENDER_RESOURCE_CAPABILITY_OK);
        assert(xg_render_resource_capability_retire(restored) ==
               XG_RENDER_RESOURCE_CAPABILITY_OK);

        metadata.receipt++;
        metadata.source.range_content_digest++;
        assert(xg_render_resource_capability_register(
                   &metadata, &replacement) ==
               XG_RENDER_RESOURCE_CAPABILITY_OK);
        import.provenance = replacement;
        import.bytes = second_bytes;
        import.content_digest = xg_render_resource_digest(
            import.bytes, import.byte_count);
        assert(xg_render_resource_import_native(&import, &second) ==
               XG_RENDER_RESOURCE_OK);
        assert(first.generation != second.generation);
        assert(xg_render_resource_capability_validate(
                   &restored, metadata.owner_kind, 77u, NULL) ==
               XG_RENDER_RESOURCE_CAPABILITY_OK);
        assert(xg_render_resource_release(first) == XG_RENDER_RESOURCE_OK);
        assert(xg_render_resource_capability_validate(
                   &restored, metadata.owner_kind, 77u, NULL) ==
               XG_RENDER_RESOURCE_CAPABILITY_NOT_FOUND);
        assert(xg_render_resource_capability_revoke(replacement) ==
               XG_RENDER_RESOURCE_CAPABILITY_OK);
    }
    xg_render_resource_repository_diagnostics(&diagnostics);
    assert(diagnostics.live_capabilities == 0u);
    assert(diagnostics.capability_slots == 0u);
    assert(xg_render_resource_capability_validate(
               &original, metadata.owner_kind, metadata.owner_generation,
               NULL) != XG_RENDER_RESOURCE_CAPABILITY_OK);

    for (uint64_t index = 0u;
         index < (uint64_t)XG_RENDER_RESOURCE_REPOSITORY_CAPACITY * 3u;
         ++index) {
        XgRenderResourceProvenance transient;
        metadata.receipt = UINT64_C(0x10000000) + index;
        metadata.owner_generation = index + 1u;
        metadata.source.range_content_digest = metadata.receipt;
        assert(xg_render_resource_capability_register(&metadata, &transient) ==
               XG_RENDER_RESOURCE_CAPABILITY_OK);
        assert(transient.capability != original.capability);
        assert(xg_render_resource_capability_revoke(transient) ==
               XG_RENDER_RESOURCE_CAPABILITY_OK);
    }
    xg_render_resource_repository_diagnostics(&diagnostics);
    assert(diagnostics.live_capabilities == 0u);
    assert(diagnostics.capability_slots == 0u);

    {
        XgRenderResourceProvenance
            live[XG_RENDER_RESOURCE_REPOSITORY_CAPACITY];
        XgRenderResourceProvenance overflow;

        for (size_t index = 0u;
             index < XG_RENDER_RESOURCE_REPOSITORY_CAPACITY; ++index) {
            metadata.receipt = UINT64_C(0x20000000) + index;
            metadata.owner_generation = index + 1u;
            metadata.source.range_content_digest = metadata.receipt;
            assert(xg_render_resource_capability_register(
                       &metadata, &live[index]) ==
                   XG_RENDER_RESOURCE_CAPABILITY_OK);
        }
        metadata.receipt = UINT64_C(0x30000000);
        metadata.owner_generation++;
        metadata.source.range_content_digest = metadata.receipt;
        assert(xg_render_resource_capability_register(&metadata, &overflow) ==
               XG_RENDER_RESOURCE_CAPABILITY_CAPACITY_EXCEEDED);
        xg_render_resource_repository_diagnostics(&diagnostics);
        assert(diagnostics.live_capabilities ==
               XG_RENDER_RESOURCE_REPOSITORY_CAPACITY);
        assert(diagnostics.capability_slots ==
               XG_RENDER_RESOURCE_REPOSITORY_CAPACITY);
        for (size_t index = 0u;
             index < XG_RENDER_RESOURCE_REPOSITORY_CAPACITY; ++index)
            assert(xg_render_resource_capability_revoke(live[index]) ==
                   XG_RENDER_RESOURCE_CAPABILITY_OK);
    }
    xg_render_resource_repository_diagnostics(&diagnostics);
    assert(diagnostics.live_capabilities == 0u);
    assert(diagnostics.capability_slots == 0u);
}

int main(void) {
    uint8_t first_bytes[] = { 1u, 2u, 3u, 4u };
    uint8_t second_bytes[] = { 5u, 6u, 7u, 8u };
    XgRenderResourceImport import = {
        .resource_id = 10u,
        .kind = XG_RENDER_RESOURCE_TEXTURE,
        .owner_kind = XG_RENDER_RESOURCE_OWNER_SCENE,
        .owner_generation = 1u,
        .state = XG_RENDER_RESOURCE_NATIVE_OWNED,
        .provenance = {
            .kind = XG_RENDER_RESOURCE_PROVENANCE_NONE,
            .synthetic = true,
        },
        .bytes = first_bytes,
        .byte_count = sizeof(first_bytes),
    };
    XgRenderResourceHandle first;
    XgRenderResourceHandle second;
    XgRenderResourceView view;
    XgRenderResourceResult resource_result;
    XgRenderResourceDiagnostics resource_diagnostics;
    XgRenderResourceHandle static_resource;
    XgRenderResourceHandle reset_resource;
    XgRenderSourceBuilder builder;
    XgRenderSourceCommitHandle commit;
    const XgPresentationIdentity identity = {
        .presentation_epoch = 1u,
        .source_sequence = 1u,
        .guest_vblank_sequence = 1u,
        .scene_generation = 1u,
    };
    const XgSemanticSceneIdentity scene = {
        .disc_id = 1u,
        .executable_identity = 1u,
        .module = XG_SEMANTIC_MODULE_FIELD,
    };
    const XgSemanticDisplayState display = {
        .width = 320u,
        .height = 240u,
        .aspect_num = 4u,
        .aspect_den = 3u,
    };
    const XgSemanticPassRecord pass = {
        .pass_id = 1u,
    };
    XgRenderVramTransferDescription description = {
        .operation = XG_RENDER_VRAM_UPLOAD,
        .guest_cycle = 100u,
        .source_interval = 2u,
        .x = 10u,
        .y = 20u,
        .width = 2u,
        .height = 1u,
        .payload_size = 4u,
    };
    XgRenderVramTransferHandle transfer;
    XgRenderVramMutation mutation;
    XgRenderVramJournalSnapshot journal;
    const uint8_t frame_resource_bytes[] = {9u, 10u, 11u, 12u};
    const uint8_t replacement_resource_bytes[] = {13u, 14u, 15u, 16u};
    XgRenderResourceImport frame_resource_import = {
        .resource_id = 12u,
        .kind = XG_RENDER_RESOURCE_TEXTURE,
        .owner_kind = XG_RENDER_RESOURCE_OWNER_SCENE,
        .owner_generation = 2u,
        .state = XG_RENDER_RESOURCE_NATIVE_OWNED,
        .provenance = {
            .kind = XG_RENDER_RESOURCE_PROVENANCE_NONE,
            .synthetic = true,
        },
        .bytes = frame_resource_bytes,
        .byte_count = sizeof(frame_resource_bytes),
    };
    XgRenderResourceHandle frame_resource;
    XgRenderResourceHandle replacement_resource;
    XgSemanticResourceRef frame_resource_ref;
    XgRenderSourceFrameSnapshot frame_snapshot;
    uint64_t closed_epoch;

    const XgRenderSourceFrameDescription frame_description = {
        .scene = {
            .disc_id = 1u,
            .executable_identity = 1u,
            .module = XG_SEMANTIC_MODULE_FIELD,
        },
        .display = {
            .width = 320u,
            .height = 240u,
            .aspect_num = 4u,
            .aspect_den = 3u,
        },
        .scene_generation = 2u,
        .temporally_eligible = true,
    };

    xg_render_resource_repository_reset();
    import.content_digest = xg_render_resource_digest(first_bytes,
                                                       sizeof(first_bytes));
    assert(xg_render_resource_import(&import, &first) == XG_RENDER_RESOURCE_OK);
    assert(first.generation == 1u);
    assert(xg_render_resource_retain(first) == XG_RENDER_RESOURCE_OK);
    first_bytes[0] = 99u;
    assert(xg_render_resource_view(first, &view) == XG_RENDER_RESOURCE_OK);
    assert(((const uint8_t *)view.bytes)[0] == 1u);

    import.bytes = second_bytes;
    import.content_digest = xg_render_resource_digest(second_bytes,
                                                       sizeof(second_bytes));
    assert(xg_render_resource_import(&import, &second) == XG_RENDER_RESOURCE_OK);
    assert(second.generation == 2u);
    assert(xg_render_resource_view(first, &view) == XG_RENDER_RESOURCE_OK);
    assert(!view.current);
    assert(xg_render_resource_release(first) == XG_RENDER_RESOURCE_OK);
    assert(xg_render_resource_view(first, &view) == XG_RENDER_RESOURCE_NOT_FOUND);
    xg_render_resource_invalidate_owner(XG_RENDER_RESOURCE_OWNER_SCENE, 1u);
    assert(xg_render_resource_view(second, &view) == XG_RENDER_RESOURCE_NOT_FOUND);
    xg_render_resource_repository_diagnostics(&resource_diagnostics);
    assert(resource_diagnostics.imports == 2u);
    assert(resource_diagnostics.invalidations == 1u);
    assert(resource_diagnostics.live_resources == 0u);

    test_immutable_resource_descriptors();
    xg_render_resource_repository_reset();
    import.resource_id = 11u;
    import.owner_kind = XG_RENDER_RESOURCE_OWNER_STATIC;
    import.owner_generation = 1u;
    resource_result = xg_render_resource_import(&import, &static_resource);
    assert(resource_result == XG_RENDER_RESOURCE_OK);
    xg_render_resource_invalidate_transient();
    assert(xg_render_resource_view(static_resource, &view) ==
           XG_RENDER_RESOURCE_OK);
    assert(view.current);

    xg_render_source_commit_reset();
    test_concurrent_builder_cancellation(&identity, &scene, &display,
                                         static_resource,
                                         import.content_digest);
    assert(xg_render_source_commit_begin(&identity, &scene, &display, 1u,
                                         false, false, &builder) ==
           XG_RENDER_SOURCE_COMMIT_OK);
    assert(xg_render_source_commit_append_pass(builder, &pass) ==
           XG_RENDER_SOURCE_COMMIT_OK);
    assert(xg_render_source_commit_append_resource(builder,
               &(XgSemanticResourceRef){
                   static_resource.resource_id,
                   static_resource.generation,
                   import.content_digest,
               }) == XG_RENDER_SOURCE_COMMIT_OK);
    assert(xg_render_source_commit_seal(builder, &commit) ==
           XG_RENDER_SOURCE_COMMIT_OK);
    xg_render_resource_invalidate_owner(XG_RENDER_RESOURCE_OWNER_STATIC, 1u);
    assert(xg_render_resource_view(static_resource, &view) ==
           XG_RENDER_RESOURCE_OK);
    assert(!view.current && view.retain_count == 1u);
    assert(xg_render_source_commit_retire(commit) ==
           XG_RENDER_SOURCE_COMMIT_OK);
    assert(xg_render_resource_view(static_resource, &view) ==
           XG_RENDER_RESOURCE_NOT_FOUND);

    import.resource_id = 10u;
    import.owner_kind = XG_RENDER_RESOURCE_OWNER_SCENE;
    xg_render_resource_repository_reset();
    assert(xg_render_resource_import(&import, &reset_resource) ==
           XG_RENDER_RESOURCE_OK);
    assert(reset_resource.generation != first.generation);

    xg_render_vram_journal_reset();
    assert(xg_render_vram_transfer_begin(&description, &transfer) ==
           XG_RENDER_VRAM_OK);
    assert(xg_render_vram_transfer_write(transfer, 0u, second_bytes, 2u) ==
           XG_RENDER_VRAM_OK);
    assert(xg_render_vram_transfer_complete(transfer, &mutation) ==
           XG_RENDER_VRAM_INCOMPLETE_TRANSFER);
    assert(xg_render_vram_transfer_write(transfer, 0u, second_bytes, 1u) ==
           XG_RENDER_VRAM_OVERLAPPING_WRITE);
    assert(xg_render_vram_transfer_write(transfer, 2u, second_bytes + 2u, 2u) ==
           XG_RENDER_VRAM_OK);
    assert(xg_render_vram_transfer_complete(transfer, &mutation) ==
           XG_RENDER_VRAM_OK);
    assert(mutation.serial == 1u);
    assert(mutation.event_serial == 1u);
    assert(mutation.source_generation == 0u);
    assert(mutation.direction == XG_RENDER_VRAM_CPU_TO_VRAM);
    assert(mutation.content_digest ==
           xg_render_resource_digest(second_bytes, sizeof(second_bytes)));
    description.operation = XG_RENDER_VRAM_READBACK;
    assert(xg_render_vram_publish_digest(
               &description, UINT64_C(0x123456789abcdef0), &mutation) ==
           XG_RENDER_VRAM_OK);
    assert(mutation.serial == 1u);
    assert(mutation.event_serial == 2u);
    assert(mutation.source_generation == 1u);
    assert(mutation.direction == XG_RENDER_VRAM_VRAM_TO_CPU);
    assert(mutation.content_digest == UINT64_C(0x123456789abcdef0));
    description.operation = XG_RENDER_VRAM_MOVE;
    description.source_x = 1u;
    description.source_y = 2u;
    description.required_source_generation = 1u;
    assert(xg_render_vram_publish_digest(
               &description, UINT64_C(0x223456789abcdef0), &mutation) ==
           XG_RENDER_VRAM_OK);
    assert(mutation.event_serial == 3u);
    assert(mutation.serial == 2u);
    assert(mutation.source_generation == 1u);
    assert(mutation.direction == XG_RENDER_VRAM_VRAM_TO_VRAM);
    description.operation = XG_RENDER_VRAM_SCANOUT;
    description.required_source_generation = 1u;
    assert(xg_render_vram_publish_digest(
               &description, UINT64_C(0x323456789abcdef0), &mutation) ==
           XG_RENDER_VRAM_STALE_SOURCE_GENERATION);
    description.required_source_generation = 2u;
    assert(xg_render_vram_publish_digest(
               &description, UINT64_C(0x323456789abcdef0), &mutation) ==
           XG_RENDER_VRAM_OK);
    assert(mutation.event_serial == 4u);
    assert(mutation.serial == 2u);
    assert(mutation.source_generation == 2u);
    assert(mutation.direction == XG_RENDER_VRAM_VRAM_TO_SCANOUT);
    xg_render_vram_journal_snapshot(&journal);
    assert(journal.event_serial == 4u);
    assert(journal.mutation_serial == 2u);
    assert(journal.completed_transfers == 4u);
    assert(journal.readback_transfers == 1u);
    assert(journal.scanouts == 1u);
    assert(journal.ownership_violations == 1u);
    assert(journal.partial_publish_attempts == 1u);
    assert(journal.active_transfers == 0u);
    xg_render_vram_journal_reset();
    xg_render_vram_journal_snapshot(&journal);
    assert(journal.event_serial == 4u);
    assert(journal.mutation_serial == 2u);
    assert(journal.completed_transfers == 0u);
    description.operation = XG_RENDER_VRAM_RESTORE;
    description.required_source_generation = 0u;
    description.source_x = 0u;
    description.source_y = 0u;
    description.x = 0u;
    description.y = 0u;
    description.width = 1024u;
    description.height = 512u;
    description.payload_size = 1024u * 512u * sizeof(uint16_t);
    assert(xg_render_vram_publish_digest(
               &description, 0u, &mutation) == XG_RENDER_VRAM_OK);
    assert(mutation.event_serial == 5u);
    assert(mutation.serial == 3u);
    xg_render_vram_journal_snapshot(&journal);
    assert(journal.restorations == 1u);

    xg_render_semantic_presentation_reset();
    assert(xg_render_presentation_lifecycle_claim_open(TEST_PRESENTER_OWNER) ==
           XG_RENDER_PRESENTATION_LIFECYCLE_OK);
    xg_render_source_frame_reset();
    xg_render_resource_repository_reset();
    frame_resource_import.content_digest = xg_render_resource_digest(
        frame_resource_bytes, sizeof(frame_resource_bytes));
    assert(xg_render_resource_import(
               &frame_resource_import, &frame_resource) ==
           XG_RENDER_RESOURCE_OK);
    frame_resource_ref = (XgSemanticResourceRef){
        .resource_id = frame_resource.resource_id,
        .generation = frame_resource.generation,
        .content_digest = frame_resource_import.content_digest,
    };
    assert(xg_render_source_frame_begin(&frame_description) ==
           XG_RENDER_SOURCE_FRAME_OK);
    assert(xg_render_source_frame_append_pass(&pass) ==
           XG_RENDER_SOURCE_FRAME_OK);
    assert(xg_render_source_frame_append_resource(&frame_resource_ref) ==
           XG_RENDER_SOURCE_FRAME_OK);
    assert(xg_render_source_frame_append_resource(&frame_resource_ref) ==
           XG_RENDER_SOURCE_FRAME_OK);
    xg_render_source_frame_snapshot(&frame_snapshot);
    assert(frame_snapshot.resource_count == 1u);
    assert(xg_render_resource_view(frame_resource, &view) ==
           XG_RENDER_RESOURCE_OK);
    assert(view.current && view.retain_count == 1u);

    frame_resource_import.bytes = replacement_resource_bytes;
    frame_resource_import.byte_count = sizeof(replacement_resource_bytes);
    frame_resource_import.content_digest = xg_render_resource_digest(
        replacement_resource_bytes, sizeof(replacement_resource_bytes));
    assert(xg_render_resource_import(
               &frame_resource_import, &replacement_resource) ==
           XG_RENDER_RESOURCE_OK);
    assert(xg_render_resource_view(frame_resource, &view) ==
           XG_RENDER_RESOURCE_OK);
    assert(!view.current && view.retain_count == 1u);
    assert(xg_render_resource_acquire_current(
               frame_resource, frame_resource_ref.content_digest) ==
           XG_RENDER_RESOURCE_STALE);
    assert(xg_render_source_frame_complete() == XG_RENDER_SOURCE_FRAME_OK);
    assert(xg_render_timeline_source_boundary(1u, 100u) ==
           XG_RENDER_TIMELINE_OK);
    assert(xg_render_source_frame_publish_boundary() ==
           XG_RENDER_SOURCE_FRAME_REJECTED);
    assert(xg_render_resource_view(frame_resource, &view) ==
           XG_RENDER_RESOURCE_NOT_FOUND);
    assert(xg_render_presentation_lifecycle_close(
               TEST_PRESENTER_OWNER, XG_RENDER_TIMELINE_RESET,
               &closed_epoch) == XG_RENDER_PRESENTATION_LIFECYCLE_OK);
    assert(closed_epoch != 0u);
    assert(xg_render_resource_view(replacement_resource, &view) ==
           XG_RENDER_RESOURCE_OK);
    assert(view.current && view.retain_count == 0u);

    frame_resource_import.resource_id = 13u;
    assert(xg_render_resource_import(
               &frame_resource_import, &replacement_resource) ==
           XG_RENDER_RESOURCE_OK);
    frame_resource_ref = (XgSemanticResourceRef){
        .resource_id = replacement_resource.resource_id,
        .generation = replacement_resource.generation,
        .content_digest = frame_resource_import.content_digest,
    };
    assert(xg_render_source_frame_begin(&frame_description) ==
           XG_RENDER_SOURCE_FRAME_OK);
    assert(xg_render_source_frame_append_resource(&frame_resource_ref) ==
           XG_RENDER_SOURCE_FRAME_OK);
    assert(xg_render_resource_view(replacement_resource, &view) ==
           XG_RENDER_RESOURCE_OK);
    assert(view.retain_count == 1u);
    xg_render_source_frame_reset();
    assert(xg_render_resource_view(replacement_resource, &view) ==
           XG_RENDER_RESOURCE_OK);
    assert(view.current && view.retain_count == 0u);
    xg_render_resource_repository_reset();
    test_typed_resource_bindings(&identity, &scene, &display);
    test_source_commit_resource_currentness(&identity, &scene, &display);
    test_native_import_transition();
    test_transactional_generation_rollback();
    test_import_validation_and_provenance();
    test_capacity_and_identity_reuse();
    test_exact_owner_invalidation();
    xg_render_resource_repository_reset();
    test_authenticated_resource_identity();
    xg_render_resource_repository_diagnostics(&resource_diagnostics);
    assert(resource_diagnostics.registered_resource_ids == 2u);
    assert(resource_diagnostics.identity_collisions == 2u);
    test_capability_checkpoint_cold_restore_and_reclamation();
    return 0;
}
