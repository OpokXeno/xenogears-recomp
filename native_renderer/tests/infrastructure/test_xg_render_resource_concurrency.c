#include "xg_render_resource_repository.h"
#include "xg_render_semantic_presentation.h"
#include "xg_render_source_commit.h"
#include "xg_render_source_frame.h"
#include "xg_render_vram_journal.h"

#include <assert.h>
#include <stdatomic.h>
#include <string.h>
#include <threads.h>

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

enum { CONCURRENT_REFERENCE_THREADS = 4, CONCURRENT_REFERENCE_COUNT = 4000 };
typedef struct ConcurrentReferenceContext {
    XgRenderResourceHandle handle;
    uint64_t digest;
    atomic_uint ready;
    atomic_bool go;
    bool release;
} ConcurrentReferenceContext;

static int change_references_concurrently(void *user_data) {
    ConcurrentReferenceContext *context = user_data;
    atomic_fetch_add_explicit(&context->ready, 1u, memory_order_release);
    while (!atomic_load_explicit(&context->go, memory_order_acquire)) thrd_yield();
    for (unsigned int i = 0u; i < CONCURRENT_REFERENCE_COUNT; ++i) {
        const XgRenderResourceResult result = context->release
            ? xg_render_resource_release(context->handle)
            : xg_render_resource_acquire_current(context->handle, context->digest);
        if (result != XG_RENDER_RESOURCE_OK) return 1;
        if ((i & 127u) == 0u) {
            XgRenderResourceView view;
            if (xg_render_resource_view(context->handle, &view) != XG_RENDER_RESOURCE_OK ||
                !view.current || view.content_digest != context->digest) return 2;
        }
    }
    return 0;
}

static void test_concurrent_resource_references(void) {
    static const uint8_t bytes[] = { 17u, 43u, 91u, 255u };
    xg_render_resource_repository_reset();
    const XgSemanticResourceRef resource = import_test_resource(
        20u, XG_RENDER_RESOURCE_TEXTURE, bytes, sizeof(bytes));
    ConcurrentReferenceContext context = {
        .handle = { resource.resource_id, resource.generation },
        .digest = resource.content_digest,
    };
    atomic_init(&context.ready, 0u);
    atomic_init(&context.go, false);
    for (unsigned int phase = 0u; phase < 2u; ++phase) {
        thrd_t workers[CONCURRENT_REFERENCE_THREADS];
        context.release = phase != 0u;
        atomic_store_explicit(&context.ready, 0u, memory_order_relaxed);
        atomic_store_explicit(&context.go, false, memory_order_relaxed);
        for (unsigned int i = 0u; i < CONCURRENT_REFERENCE_THREADS; ++i)
            assert(thrd_create(&workers[i], change_references_concurrently, &context) == thrd_success);
        while (atomic_load_explicit(&context.ready, memory_order_acquire) != CONCURRENT_REFERENCE_THREADS)
            thrd_yield();
        atomic_store_explicit(&context.go, true, memory_order_release);
        for (unsigned int i = 0u; i < CONCURRENT_REFERENCE_THREADS; ++i) {
            int result;
            assert(thrd_join(workers[i], &result) == thrd_success);
            assert(result == 0);
        }
        XgRenderResourceView view;
        assert(xg_render_resource_view(context.handle, &view) == XG_RENDER_RESOURCE_OK);
        assert(view.current && view.retain_count == (phase ? 0u :
            CONCURRENT_REFERENCE_THREADS * CONCURRENT_REFERENCE_COUNT));
        assert(view.byte_count == sizeof(bytes) && !memcmp(view.bytes, bytes, sizeof(bytes)));
    }
    xg_render_resource_repository_reset();
}

static int read_retained_generation(void *user_data) {
    ConcurrentReferenceContext *context = user_data;
    static const uint8_t expected[] = { 17u, 43u, 91u, 255u };
    atomic_fetch_add_explicit(&context->ready, 1u, memory_order_release);
    while (!atomic_load_explicit(&context->go, memory_order_acquire)) thrd_yield();
    for (unsigned i = 0u; i < 16000u; ++i) {
        XgRenderResourceView view;
        if (xg_render_resource_view(context->handle, &view) != XG_RENDER_RESOURCE_OK ||
            view.handle.resource_id != context->handle.resource_id ||
            view.handle.generation != context->handle.generation ||
            view.content_digest != context->digest || view.retain_count != 1u ||
            view.byte_count != sizeof(expected) || memcmp(view.bytes, expected, sizeof(expected)))
            return 1;
    }
    return 0;
}

static int replace_generations(void *user_data) {
    ConcurrentReferenceContext *context = user_data;
    atomic_fetch_add_explicit(&context->ready, 1u, memory_order_release);
    while (!atomic_load_explicit(&context->go, memory_order_acquire)) thrd_yield();
    for (unsigned i = 0u; i < 2000u; ++i) {
        const uint8_t bytes[] = { (uint8_t)i, (uint8_t)(i >> 8u), 37u, 64u };
        (void)import_test_resource(context->handle.resource_id,
            XG_RENDER_RESOURCE_TEXTURE, bytes, sizeof(bytes));
        /* Force repository growth while readers keep a retained generation. */
        if (i < 256u) (void)import_test_resource(1000u + i,
            XG_RENDER_RESOURCE_TEXTURE, bytes, sizeof(bytes));
    }
    return 0;
}

static void test_views_during_publication(void) {
    static const uint8_t bytes[] = { 17u, 43u, 91u, 255u };
    xg_render_resource_repository_reset();
    const XgSemanticResourceRef resource = import_test_resource(
        20u, XG_RENDER_RESOURCE_TEXTURE, bytes, sizeof(bytes));
    ConcurrentReferenceContext context = {
        .handle = { resource.resource_id, resource.generation },
        .digest = resource.content_digest,
    };
    atomic_init(&context.ready, 0u);
    atomic_init(&context.go, false);
    assert(xg_render_resource_acquire_current(context.handle, context.digest) == XG_RENDER_RESOURCE_OK);
    thrd_t workers[5];
    for (unsigned i = 0u; i < 5u; ++i)
        assert(thrd_create(&workers[i], i == 4u ? replace_generations : read_retained_generation,
            &context) == thrd_success);
    while (atomic_load_explicit(&context.ready, memory_order_acquire) != 5u) thrd_yield();
    atomic_store_explicit(&context.go, true, memory_order_release);
    for (unsigned i = 0u; i < 5u; ++i) {
        int result;
        assert(thrd_join(workers[i], &result) == thrd_success && result == 0);
    }
    assert(xg_render_resource_release(context.handle) == XG_RENDER_RESOURCE_OK);
    XgRenderResourceView stale;
    assert(xg_render_resource_view(context.handle, &stale) == XG_RENDER_RESOURCE_NOT_FOUND);
    xg_render_resource_repository_reset();
}

int main(void) {
    test_concurrent_resource_references();
    test_views_during_publication();
    return 0;
}
