#include "xg_render_surface_graph.h"

#include <assert.h>
#include <stdlib.h>
#include <string.h>

static XgRenderResourceIdentity identity(uint8_t seed) {
    XgRenderResourceIdentity result;
    for (uint32_t index = 0u; index < sizeof(result.bytes); ++index)
        result.bytes[index] = (uint8_t)(seed + index);
    return result;
}

static XgRenderResourceIdentity identity_for_id(uint64_t resource_id) {
    XgRenderResourceIdentity result = {0};
    for (uint32_t index = 0u; index < sizeof(resource_id); ++index)
        result.bytes[index] =
            (uint8_t)(resource_id >> (index * 8u));
    return result;
}

static XgRenderResourceProvenance authorize_source(
        uint64_t receipt, XgRenderResourceOwnerKind owner_kind,
        uint64_t owner_generation, XgRenderResourceIdentity identity_value) {
    XgRenderResourceCapabilityMetadata metadata = {
        .kind = XG_RENDER_RESOURCE_PROVENANCE_SOURCE,
        .receipt = receipt,
        .lifetime = XG_RENDER_RESOURCE_CAPABILITY_SCENE,
        .owner_kind = owner_kind,
        .owner_generation = owner_generation,
        .source = {
            .source_class = owner_kind == XG_RENDER_RESOURCE_OWNER_SOURCE
                ? XG_RENDER_RESOURCE_SOURCE_MOVIE_OWNER
                : XG_RENDER_RESOURCE_SOURCE_GPU_SCANOUT,
            .identity = identity_value,
            .range_size = 1u,
            .range_content_digest = receipt,
        },
    };
    XgRenderResourceProvenance provenance;

    assert(xg_render_resource_capability_register(&metadata, &provenance) ==
           XG_RENDER_RESOURCE_CAPABILITY_OK);
    return provenance;
}

static void assert_same_snapshot(
        const XgRenderSurfaceGraphSnapshot *left,
        const XgRenderSurfaceGraphSnapshot *right) {
    assert(memcmp(left, right, sizeof(*left)) == 0);
}

static void test_transaction_commit_fail_closed(void) {
    const uint8_t source_bytes[8] = {
        1u, 2u, 3u, 4u, 5u, 6u, 7u, 8u,
    };
    const uint8_t target_bytes[8] = {
        11u, 12u, 13u, 14u, 15u, 16u, 17u, 18u,
    };
    const uint8_t replacement_bytes[8] = {
        21u, 22u, 23u, 24u, 25u, 26u, 27u, 28u,
    };
    XgRenderSurfacePublicationDescription source_description = {
        .identity = identity(181u),
        .kind = XG_RENDER_RESOURCE_FRAMEBUFFER,
        .format = XG_RENDER_SURFACE_VRAM16,
        .provenance = {
            .kind = XG_RENDER_RESOURCE_PROVENANCE_NONE,
            .synthetic = true,
        },
        .owner_generation = 17u,
        .width = 2u,
        .height = 2u,
        .descriptor = {
            .version = XG_RENDER_RESOURCE_DESCRIPTOR_VERSION,
            .pixel_format = XG_RENDER_RESOURCE_PIXEL_FORMAT_RGB555,
            .width = 2u,
            .height = 2u,
            .row_pitch = 4u,
        },
        .bytes = source_bytes,
        .byte_count = sizeof(source_bytes),
    };
    XgRenderSurfacePublicationDescription target_description = {
        .identity = identity(211u),
        .kind = XG_RENDER_RESOURCE_GENERATED_SURFACE,
        .format = XG_RENDER_SURFACE_VRAM16,
        .provenance = {
            .kind = XG_RENDER_RESOURCE_PROVENANCE_NONE,
            .synthetic = true,
        },
        .owner_generation = 17u,
        .width = 2u,
        .height = 2u,
        .descriptor = {
            .version = XG_RENDER_RESOURCE_DESCRIPTOR_VERSION,
            .pixel_format = XG_RENDER_RESOURCE_PIXEL_FORMAT_RGB555,
            .width = 2u,
            .height = 2u,
            .row_pitch = 4u,
        },
        .bytes = target_bytes,
        .byte_count = sizeof(target_bytes),
    };
    XgRenderSurfacePublicationDescription replacement = target_description;
    XgRenderSurfacePublication source;
    XgRenderSurfacePublication target;
    XgRenderSurfacePublication current;
    XgRenderSurfacePublication staged;
    XgRenderSurfaceGraphTransaction *transaction;
    XgRenderSurfaceGraphSnapshot before;
    XgRenderSurfaceGraphSnapshot after;
    XgRenderResourceDiagnostics resources_before;
    XgRenderResourceDiagnostics resources_after;
    XgRenderResourceView staged_view;
    XgSemanticSurfaceEdge edge;

    xg_render_resource_repository_reset();
    xg_render_surface_graph_reset();
    assert(xg_render_surface_graph_publish(&source_description, &source) ==
           XG_RENDER_SURFACE_GRAPH_OK);
    assert(xg_render_surface_graph_publish(&target_description, &target) ==
           XG_RENDER_SURFACE_GRAPH_OK);
    replacement.bytes = replacement_bytes;
    xg_render_surface_graph_snapshot(&before);
    xg_render_resource_repository_diagnostics(&resources_before);

    assert(xg_render_surface_graph_transaction_begin(
               &replacement, &transaction, &staged) ==
           XG_RENDER_SURFACE_GRAPH_OK);
    xg_render_surface_graph_transaction_fault_inject(
        XG_RENDER_SURFACE_GRAPH_TRANSACTION_FAULT_RESOURCE);
    assert(xg_render_surface_graph_transaction_commit(transaction) ==
           XG_RENDER_SURFACE_GRAPH_RESOURCE_FAILED);
    xg_render_surface_graph_snapshot(&after);
    assert_same_snapshot(&before, &after);
    assert(xg_render_surface_graph_lookup(target.handle.resource_id, &current) ==
           XG_RENDER_SURFACE_GRAPH_OK);
    assert(current.handle.generation == target.handle.generation);
    assert(xg_render_resource_view(staged.handle, &staged_view) ==
           XG_RENDER_RESOURCE_NOT_FOUND);

    assert(xg_render_surface_graph_transaction_begin(
               &replacement, &transaction, &staged) ==
           XG_RENDER_SURFACE_GRAPH_OK);
    xg_render_surface_graph_transaction_fault_inject(
        XG_RENDER_SURFACE_GRAPH_TRANSACTION_FAULT_CAPACITY);
    assert(xg_render_surface_graph_transaction_commit(transaction) ==
           XG_RENDER_SURFACE_GRAPH_CAPACITY_EXCEEDED);
    xg_render_surface_graph_snapshot(&after);
    assert_same_snapshot(&before, &after);
    assert(xg_render_resource_view(staged.handle, &staged_view) ==
           XG_RENDER_RESOURCE_NOT_FOUND);

    assert(xg_render_surface_graph_transaction_begin(
               &replacement, &transaction, &staged) ==
           XG_RENDER_SURFACE_GRAPH_OK);
    edge = (XgSemanticSurfaceEdge){
        .source_surface_id = source.handle.resource_id,
        .source_generation = source.handle.generation,
        .target_surface_id = target.handle.resource_id,
        .target_generation = target.handle.generation,
        .kind = XG_SEMANTIC_SURFACE_SAMPLE,
        .width = 1u,
        .height = 1u,
    };
    assert(xg_render_surface_graph_append_edge(&edge) ==
           XG_RENDER_SURFACE_GRAPH_OK);
    xg_render_surface_graph_snapshot(&before);
    assert(xg_render_surface_graph_transaction_commit(transaction) ==
           XG_RENDER_SURFACE_GRAPH_STALE);
    xg_render_surface_graph_snapshot(&after);
    assert_same_snapshot(&before, &after);
    assert(xg_render_resource_view(staged.handle, &staged_view) ==
           XG_RENDER_RESOURCE_NOT_FOUND);

    assert(xg_render_surface_graph_transaction_begin(
               &target_description, &transaction, &staged) ==
           XG_RENDER_SURFACE_GRAPH_OK);
    assert(staged.handle.generation == target.handle.generation);
    assert(xg_render_resource_retire_current(target.handle) ==
           XG_RENDER_RESOURCE_OK);
    xg_render_surface_graph_snapshot(&before);
    assert(xg_render_surface_graph_transaction_commit(transaction) ==
           XG_RENDER_SURFACE_GRAPH_STALE);
    xg_render_surface_graph_snapshot(&after);
    assert_same_snapshot(&before, &after);

    xg_render_resource_repository_diagnostics(&resources_after);
    assert(resources_after.live_resources ==
           resources_before.live_resources - 1u);
    assert(resources_after.retained_resources ==
           resources_before.retained_resources);
    xg_render_surface_graph_reset();
    xg_render_resource_repository_reset();
}

int main(void) {
    const uint8_t generated_bytes[16] = {
        1u, 2u, 3u, 4u, 5u, 6u, 7u, 8u,
        9u, 10u, 11u, 12u, 13u, 14u, 15u, 16u,
    };
    const uint8_t future_bytes[16] = {
        21u, 22u, 23u, 24u, 25u, 26u, 27u, 28u,
        29u, 30u, 31u, 32u, 33u, 34u, 35u, 36u,
    };
    const uint8_t framebuffer_bytes[8] = {
        40u, 41u, 42u, 43u, 44u, 45u, 46u, 47u,
    };
    const uint8_t future_movie_bytes[8] = {
        50u, 51u, 52u, 53u, 54u, 55u, 56u, 57u,
    };
    XgRenderSurfacePublicationDescription generated_description = {
        .identity = identity(1u),
        .kind = XG_RENDER_RESOURCE_GENERATED_SURFACE,
        .format = XG_RENDER_SURFACE_RGBA8,
        .provenance = {
            .kind = XG_RENDER_RESOURCE_PROVENANCE_SOURCE,
            .receipt = 101u,
        },
        .owner_generation = 7u,
        .width = 2u,
        .height = 2u,
        .descriptor = {
            .version = XG_RENDER_RESOURCE_DESCRIPTOR_VERSION,
            .pixel_format = XG_RENDER_RESOURCE_PIXEL_FORMAT_RGBA8,
            .width = 2u,
            .height = 2u,
            .row_pitch = 8u,
        },
        .bytes = generated_bytes,
        .byte_count = sizeof(generated_bytes),
    };
    XgRenderSurfacePublicationDescription framebuffer_description = {
        .identity = identity(91u),
        .kind = XG_RENDER_RESOURCE_FRAMEBUFFER,
        .format = XG_RENDER_SURFACE_VRAM16,
        .provenance = {
            .kind = XG_RENDER_RESOURCE_PROVENANCE_SOURCE,
            .receipt = 102u,
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
        .bytes = framebuffer_bytes,
        .byte_count = sizeof(framebuffer_bytes),
    };
    XgRenderSurfacePublication generated;
    XgRenderSurfacePublication framebuffer;
    XgRenderSurfacePublication movie;
    XgRenderSurfacePublication restored_movie;
    XgRenderSurfacePublication future;
    XgRenderSurfacePublication restored_generated;
    XgRenderSurfacePublication restored_framebuffer;
    XgSemanticSurfaceEdge edge;
    XgSemanticSurfaceEdge movie_edge;
    XgSemanticSurfaceEdge restored_edges[2];
    XgRenderResourceHandle movie_handle;
    XgRenderResourceHandle future_movie_handle;
    XgRenderResourceView view;
    XgRenderSurfaceGraphSnapshot snapshot;
    uint8_t *checkpoint;
    size_t checkpoint_size;
    size_t edge_count;
    uint64_t checkpoint_capability;
    XgRenderResourceImport movie_import = {
        .resource_id = 700u,
        .kind = XG_RENDER_RESOURCE_MOVIE_FRAME,
        .owner_kind = XG_RENDER_RESOURCE_OWNER_SOURCE,
        .owner_generation = 7u,
        .state = XG_RENDER_RESOURCE_NATIVE_OWNED,
        .provenance = {
            .kind = XG_RENDER_RESOURCE_PROVENANCE_NONE,
            .synthetic = true,
        },
        .bytes = framebuffer_bytes,
        .byte_count = sizeof(framebuffer_bytes),
        .descriptor = {
            .version = XG_RENDER_RESOURCE_DESCRIPTOR_VERSION,
            .pixel_format = XG_RENDER_RESOURCE_PIXEL_FORMAT_RGB555,
            .width = 2u,
            .height = 2u,
            .row_pitch = 4u,
        },
    };
    XgRenderSurfaceAttachmentDescription movie_attachment;
    XgRenderSurfacePublicationDescription unauthenticated_description =
        generated_description;

    test_transaction_commit_fail_closed();
    xg_render_resource_repository_reset();
    xg_render_surface_graph_reset();
    generated_description.provenance = authorize_source(
        101u, XG_RENDER_RESOURCE_OWNER_SCENE, 7u,
        generated_description.identity);
    framebuffer_description.provenance = authorize_source(
        102u, XG_RENDER_RESOURCE_OWNER_SCENE, 7u,
        framebuffer_description.identity);
    movie_import.identity = identity_for_id(movie_import.resource_id);
    movie_import.content_digest = xg_render_resource_digest(
        movie_import.bytes, movie_import.byte_count);
    memset(&unauthenticated_description.provenance, 0,
           sizeof(unauthenticated_description.provenance));
    assert(xg_render_surface_graph_publish(
               &unauthenticated_description, &future) ==
           XG_RENDER_SURFACE_GRAPH_INVALID_ARGUMENT);
    assert(xg_render_surface_graph_publish(
               &generated_description, &generated) ==
           XG_RENDER_SURFACE_GRAPH_OK);
    assert(xg_render_surface_graph_publish(
               &framebuffer_description, &framebuffer) ==
           XG_RENDER_SURFACE_GRAPH_OK);
    assert(generated.provenance.kind == XG_RENDER_RESOURCE_PROVENANCE_SOURCE);
    assert(generated.provenance.receipt == 101u);
    assert(framebuffer.provenance.receipt == 102u);
    edge = (XgSemanticSurfaceEdge){
        .source_surface_id = generated.handle.resource_id,
        .source_generation = generated.handle.generation,
        .target_surface_id = framebuffer.handle.resource_id,
        .target_generation = framebuffer.handle.generation,
        .kind = XG_SEMANTIC_SURFACE_SAMPLE,
        .width = 2u,
        .height = 2u,
    };
    assert(xg_render_surface_graph_append_edge(&edge) ==
           XG_RENDER_SURFACE_GRAPH_OK);
    assert(xg_render_resource_import_native(&movie_import, &movie_handle) ==
           XG_RENDER_RESOURCE_OK);
    movie_attachment = (XgRenderSurfaceAttachmentDescription){
        .handle = movie_handle,
        .kind = XG_RENDER_RESOURCE_MOVIE_FRAME,
        .owner_kind = XG_RENDER_RESOURCE_OWNER_SOURCE,
        .format = XG_RENDER_SURFACE_VRAM16,
        .provenance = movie_import.provenance,
        .owner_generation = movie_import.owner_generation,
        .content_digest = movie_import.content_digest,
        .width = 2u,
        .height = 2u,
        .byte_count = movie_import.byte_count,
    };
    movie_edge = (XgSemanticSurfaceEdge){
        .source_surface_id = framebuffer.handle.resource_id,
        .source_generation = framebuffer.handle.generation,
        .target_surface_id = movie_handle.resource_id,
        .target_generation = movie_handle.generation,
        .kind = XG_SEMANTIC_SURFACE_MOVIE,
        .width = 2u,
        .height = 2u,
    };
    movie_attachment.owner_kind = XG_RENDER_RESOURCE_OWNER_SCENE;
    assert(xg_render_surface_graph_attach_resource_with_edge(
               &movie_attachment, &movie_edge, &movie) ==
           XG_RENDER_SURFACE_GRAPH_INVALID_ARGUMENT);
    movie_attachment.owner_kind = XG_RENDER_RESOURCE_OWNER_SOURCE;
    movie_attachment.content_digest ^= 1u;
    assert(xg_render_surface_graph_attach_resource_with_edge(
               &movie_attachment, &movie_edge, &movie) ==
           XG_RENDER_SURFACE_GRAPH_STALE);
    movie_attachment.content_digest ^= 1u;
    xg_render_surface_graph_snapshot(&snapshot);
    assert(snapshot.node_count == 2u && snapshot.edge_count == 1u);
    {
        XgRenderSurfacePublicationDescription future_description =
            framebuffer_description;
        XgRenderSurfaceGraphTransaction *transaction = NULL;
        XgRenderSurfacePublication prepared_framebuffer;
        XgRenderSurfacePublication current_framebuffer;
        XgRenderSurfaceGraphSnapshot before_transaction;
        XgRenderSurfaceGraphSnapshot after_transaction;
        XgRenderResourceDiagnostics resources_before;
        XgRenderResourceDiagnostics resources_after;
        XgSemanticSurfaceEdge prepared_edge;
        uint8_t *checkpoint_before;
        uint8_t *checkpoint_after;
        size_t transaction_checkpoint_size;

        future_description.bytes = future_movie_bytes;
        xg_render_surface_graph_snapshot(&before_transaction);
        xg_render_resource_repository_diagnostics(&resources_before);
        transaction_checkpoint_size =
            xg_render_surface_graph_checkpoint_size();
        checkpoint_before = (uint8_t *)malloc(transaction_checkpoint_size);
        checkpoint_after = (uint8_t *)malloc(transaction_checkpoint_size);
        assert(checkpoint_before != NULL && checkpoint_after != NULL);
        assert(xg_render_surface_graph_checkpoint_write(
                   checkpoint_before, transaction_checkpoint_size) ==
               XG_RENDER_SURFACE_GRAPH_OK);
        assert(xg_render_surface_graph_transaction_begin(
                   &future_description, &transaction,
                   &prepared_framebuffer) == XG_RENDER_SURFACE_GRAPH_OK);
        assert(prepared_framebuffer.handle.generation !=
               framebuffer.handle.generation);
        prepared_edge = edge;
        prepared_edge.target_generation =
            prepared_framebuffer.handle.generation;
        assert(xg_render_surface_graph_transaction_append_edge(
                   transaction, &prepared_edge) ==
               XG_RENDER_SURFACE_GRAPH_OK);
        xg_render_surface_graph_snapshot(&after_transaction);
        assert(memcmp(&before_transaction, &after_transaction,
                      sizeof(before_transaction)) == 0);
        assert(xg_render_surface_graph_lookup(
                   framebuffer.handle.resource_id, &current_framebuffer) ==
               XG_RENDER_SURFACE_GRAPH_OK);
        assert(current_framebuffer.handle.generation ==
               framebuffer.handle.generation);
        xg_render_surface_graph_transaction_cancel(transaction);
        assert(xg_render_surface_graph_checkpoint_size() ==
               transaction_checkpoint_size);
        assert(xg_render_surface_graph_checkpoint_write(
                   checkpoint_after, transaction_checkpoint_size) ==
               XG_RENDER_SURFACE_GRAPH_OK);
        assert(memcmp(checkpoint_before, checkpoint_after,
                      transaction_checkpoint_size) == 0);
        xg_render_resource_repository_diagnostics(&resources_after);
        assert(resources_after.live_resources ==
               resources_before.live_resources);
        assert(resources_after.retained_resources ==
               resources_before.retained_resources);
        free(checkpoint_after);
        free(checkpoint_before);
    }
    assert(xg_render_surface_graph_attach_resource_with_edge(
               &movie_attachment, &movie_edge, &movie) ==
           XG_RENDER_SURFACE_GRAPH_OK);
    assert(movie.handle.resource_id == movie_handle.resource_id);
    assert(movie.handle.generation == movie_handle.generation);
    assert(movie.owner_kind == XG_RENDER_RESOURCE_OWNER_SOURCE);
    assert(movie.owner_generation == 7u);
    assert(movie.provenance.synthetic && movie.provenance.receipt == 0u);
    assert(xg_render_surface_graph_attach_resource_with_edge(
               &movie_attachment, &movie_edge, &movie) ==
           XG_RENDER_SURFACE_GRAPH_STALE);
    xg_render_surface_graph_snapshot(&snapshot);
    assert(snapshot.node_count == 3u && snapshot.edge_count == 2u);
    checkpoint_size = xg_render_surface_graph_checkpoint_size();
    checkpoint = (uint8_t *)malloc(checkpoint_size);
    assert(checkpoint != NULL);
    assert(xg_render_surface_graph_checkpoint_write(
               checkpoint, checkpoint_size) == XG_RENDER_SURFACE_GRAPH_OK);
    checkpoint_capability = generated.provenance.capability;

    movie_import.bytes = future_movie_bytes;
    movie_import.content_digest = xg_render_resource_digest(
        movie_import.bytes, movie_import.byte_count);
    assert(xg_render_resource_import_native(
               &movie_import, &future_movie_handle) == XG_RENDER_RESOURCE_OK);
    assert(future_movie_handle.generation != movie_handle.generation);
    assert(xg_render_surface_graph_attach_resource_with_edge(
               &movie_attachment, &movie_edge, &movie) ==
           XG_RENDER_SURFACE_GRAPH_STALE);

    assert(xg_render_resource_retain(generated.handle) == XG_RENDER_RESOURCE_OK);
    generated_description.bytes = future_bytes;
    assert(xg_render_surface_graph_publish(
               &generated_description, &future) ==
           XG_RENDER_SURFACE_GRAPH_OK);
    assert(future.handle.generation != generated.handle.generation);
    xg_render_resource_repository_reset();
    xg_render_surface_graph_reset();
    assert(xg_render_surface_graph_checkpoint_restore(
               checkpoint, checkpoint_size, 9u) ==
           XG_RENDER_SURFACE_GRAPH_OK);
    assert(xg_render_surface_graph_lookup(
               generated.handle.resource_id, &restored_generated) ==
           XG_RENDER_SURFACE_GRAPH_OK);
    assert(xg_render_surface_graph_lookup(
               framebuffer.handle.resource_id, &restored_framebuffer) ==
           XG_RENDER_SURFACE_GRAPH_OK);
    assert(xg_render_surface_graph_lookup(
               movie_handle.resource_id, &restored_movie) ==
           XG_RENDER_SURFACE_GRAPH_OK);
    assert(restored_generated.handle.generation != generated.handle.generation);
    assert(restored_generated.handle.generation != future.handle.generation);
    assert(restored_framebuffer.handle.generation != framebuffer.handle.generation);
    assert(restored_movie.handle.generation != movie_handle.generation);
    assert(restored_movie.handle.generation != future_movie_handle.generation);
    assert(restored_generated.owner_generation == 9u);
    assert(restored_generated.provenance.kind ==
           XG_RENDER_RESOURCE_PROVENANCE_SOURCE);
    assert(restored_generated.provenance.receipt == 101u);
    assert(restored_generated.provenance.capability != checkpoint_capability);
    assert(restored_framebuffer.provenance.receipt == 102u);
    assert(restored_movie.owner_kind == XG_RENDER_RESOURCE_OWNER_SOURCE);
    assert(restored_movie.provenance.synthetic &&
           restored_movie.provenance.receipt == 0u);
    assert(xg_render_resource_view(restored_generated.handle, &view) ==
           XG_RENDER_RESOURCE_OK);
    assert(view.provenance.kind == XG_RENDER_RESOURCE_PROVENANCE_SOURCE);
    assert(view.provenance.receipt == 101u && !view.provenance.synthetic);
    assert(view.descriptor.version == XG_RENDER_RESOURCE_DESCRIPTOR_VERSION);
    assert(view.descriptor.pixel_format ==
           XG_RENDER_RESOURCE_PIXEL_FORMAT_RGBA8);
    assert(view.descriptor.width == 2u && view.descriptor.height == 2u);
    assert(view.descriptor.row_pitch == 8u);
    assert(memcmp(view.bytes, generated_bytes, sizeof(generated_bytes)) == 0);
    assert(xg_render_surface_graph_copy_edges(
               restored_edges, 2u, &edge_count) ==
           XG_RENDER_SURFACE_GRAPH_OK);
    assert(edge_count == 2u);
    assert(restored_edges[0].source_generation ==
           restored_generated.handle.generation);
    assert(restored_edges[0].target_generation ==
           restored_framebuffer.handle.generation);
    assert(restored_edges[1].source_generation ==
           restored_framebuffer.handle.generation);
    assert(restored_edges[1].target_generation ==
           restored_movie.handle.generation);
    assert(restored_edges[1].kind == XG_SEMANTIC_SURFACE_MOVIE);
    xg_render_surface_graph_snapshot(&snapshot);
    assert(snapshot.node_count == 3u && snapshot.edge_count == 2u);
    assert(snapshot.restorations == 1u);

    xg_render_resource_repository_reset();
    xg_render_surface_graph_reset();
    checkpoint[32u + 156u] ^= 1u;
    assert(xg_render_surface_graph_checkpoint_restore(
               checkpoint, checkpoint_size, 10u) ==
           XG_RENDER_SURFACE_GRAPH_RESOURCE_FAILED);
    checkpoint[32u + 156u] ^= 1u;
    checkpoint[checkpoint_size - 1u] ^= 1u;
    assert(xg_render_surface_graph_checkpoint_restore(
               checkpoint, checkpoint_size, 10u) ==
           XG_RENDER_SURFACE_GRAPH_INVALID_CHECKPOINT);
    assert(xg_render_surface_graph_lookup(
               generated.handle.resource_id, &restored_generated) ==
           XG_RENDER_SURFACE_GRAPH_NOT_FOUND);
    assert(xg_render_resource_release(generated.handle) ==
           XG_RENDER_RESOURCE_NOT_FOUND);
    free(checkpoint);
    return 0;
}
