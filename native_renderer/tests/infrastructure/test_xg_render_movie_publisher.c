#include "xg_render_movie_publisher.h"

#include <assert.h>
#include <stdlib.h>
#include <string.h>

static XgRenderMovieFrameDescription description(
        uint64_t cycle, bool depth24) {
    return (XgRenderMovieFrameDescription){
        .movie_id = 700u,
        .owner_kind = XG_RENDER_MOVIE_OWNER_STANDALONE,
        .owner_receipt = 101u,
        .owner_generation = 9u,
        .guest_cycle = cycle,
        .width = 2u,
        .height = 2u,
        .expected_strips = 2u,
        .byte_count = 8u,
        .depth24 = depth24,
    };
}

static void authorize_description(XgRenderMovieFrameDescription *description) {
    XgRenderResourceCapabilityMetadata metadata = {
        .kind = XG_RENDER_RESOURCE_PROVENANCE_SOURCE,
        .receipt = description->owner_receipt,
        .lifetime = XG_RENDER_RESOURCE_CAPABILITY_SCENE,
        .owner_kind = XG_RENDER_RESOURCE_OWNER_SOURCE,
        .owner_generation = description->owner_generation,
        .source = {
            .source_class = XG_RENDER_RESOURCE_SOURCE_MOVIE_OWNER,
            .range_size = description->byte_count,
            .range_content_digest = description->owner_receipt,
        },
    };
    XgRenderResourceProvenance provenance;

    memcpy(metadata.source.identity.bytes, &description->movie_id,
           sizeof(description->movie_id));
    assert(xg_render_resource_capability_register(&metadata, &provenance) ==
           XG_RENDER_RESOURCE_CAPABILITY_OK);
    description->owner_capability = provenance.capability;
}

static XgRenderMovieFramePublication publish(
        XgRenderMovieFrameDescription *frame_description,
        const uint8_t bytes[8]) {
    XgRenderMovieFrameHandle frame;
    XgRenderMovieFramePublication publication;

    authorize_description(frame_description);
    assert(xg_render_movie_frame_begin(frame_description, &frame) ==
           XG_RENDER_MOVIE_OK);
    assert(xg_render_movie_frame_write_strip(frame, 0u, 0u, bytes, 4u) ==
           XG_RENDER_MOVIE_OK);
    assert(xg_render_movie_frame_write_strip(frame, 1u, 4u, bytes + 4u, 4u) ==
           XG_RENDER_MOVIE_OK);
    assert(xg_render_movie_frame_publish(frame, &publication) ==
           XG_RENDER_MOVIE_OK);
    return publication;
}

static void assert_surface_bytes(XgRenderResourceHandle surface,
                                 const uint8_t expected[8],
                                 uint64_t expected_receipt) {
    XgRenderResourceView view;

    assert(xg_render_resource_view(surface, &view) == XG_RENDER_RESOURCE_OK);
    assert(view.byte_count == 8u);
    assert(memcmp(view.bytes, expected, 8u) == 0);
    assert(view.provenance.kind == XG_RENDER_RESOURCE_PROVENANCE_SOURCE);
    assert(view.provenance.receipt == expected_receipt);
    assert(!view.provenance.synthetic);
}

static void test_partial_cancel_and_complete_only_hold(void) {
    const uint8_t first[] = { 1u, 2u, 3u, 4u, 5u, 6u, 7u, 8u };
    const uint8_t partial[] = { 20u, 21u, 22u, 23u };
    XgRenderMovieFrameDescription frame_description = description(100u, false);
    XgRenderMovieFrameHandle frame;
    XgRenderMovieFramePublication publication;
    XgRenderMovieFramePublication repeated_publication;
    XgRenderMovieFramePublication sentinel;
    XgRenderMovieDiagnostics diagnostics;

    xg_render_resource_repository_reset();
    xg_render_movie_publisher_reset();
    assert(xg_render_movie_frame_hold(&publication) ==
           XG_RENDER_MOVIE_NO_COMPLETE_FRAME);
    authorize_description(&frame_description);
    assert(xg_render_movie_frame_begin(&frame_description, &frame) ==
           XG_RENDER_MOVIE_OK);
    assert(xg_render_movie_frame_write_strip(frame, 0u, 0u, first, 4u) ==
           XG_RENDER_MOVIE_OK);
    memset(&sentinel, 0xa5, sizeof(sentinel));
    publication = sentinel;
    assert(xg_render_movie_frame_publish(frame, &publication) ==
           XG_RENDER_MOVIE_INCOMPLETE_FRAME);
    assert(memcmp(&publication, &sentinel, sizeof(publication)) == 0);
    assert(xg_render_movie_frame_cancel(frame) == XG_RENDER_MOVIE_OK);
    assert(xg_render_movie_frame_write_strip(frame, 1u, 4u, partial, 4u) ==
           XG_RENDER_MOVIE_STALE_FRAME);

    publication = publish(&frame_description, first);
    assert(publication.owner_kind == XG_RENDER_MOVIE_OWNER_STANDALONE);
    assert(publication.owner_receipt == 101u);
    assert(!publication.depth24);
    assert(publication.frame_generation == 1u);
    assert(!publication.held);
    assert(!publication.discontinuity);
    assert_surface_bytes(publication.surface, first, 101u);

    frame_description.guest_cycle = 150u;
    frame_description.owner_kind = XG_RENDER_MOVIE_OWNER_FIELD;
    frame_description.owner_receipt = 102u;
    repeated_publication = publish(&frame_description, first);
    assert(repeated_publication.owner_kind == XG_RENDER_MOVIE_OWNER_FIELD);
    assert(repeated_publication.owner_receipt == 102u);
    assert(repeated_publication.frame_generation ==
           publication.frame_generation + 1u);

    frame_description.guest_cycle = 200u;
    assert(xg_render_movie_frame_begin(&frame_description, &frame) ==
           XG_RENDER_MOVIE_OK);
    assert(xg_render_movie_frame_write_strip(frame, 0u, 0u,
                                              partial, 4u) ==
           XG_RENDER_MOVIE_OK);
    assert(xg_render_movie_frame_hold(&publication) == XG_RENDER_MOVIE_OK);
    assert(publication.held);
    assert(publication.guest_cycle == 150u);
    assert(publication.owner_kind == XG_RENDER_MOVIE_OWNER_FIELD);
    assert(publication.owner_receipt == 102u);
    assert_surface_bytes(publication.surface, first, 102u);
    assert(xg_render_movie_frame_cancel(frame) == XG_RENDER_MOVIE_OK);

    xg_render_movie_publisher_diagnostics(&diagnostics);
    assert(diagnostics.complete_frames == 2u);
    assert(diagnostics.frame_generation == 2u);
    assert(diagnostics.partial_publish_attempts == 1u);
    assert(diagnostics.cancelled_frames == 2u);
    assert(diagnostics.held_frames == 1u);
    assert(diagnostics.complete_frame_available);
    assert(!diagnostics.frame_active);
}

static void test_invalid_owner_rejected(void) {
    XgRenderMovieFrameDescription frame_description = description(1u, false);
    XgRenderMovieFrameHandle frame = {0};

    xg_render_movie_publisher_reset();
    frame_description.owner_kind = XG_RENDER_MOVIE_OWNER_NONE;
    assert(xg_render_movie_frame_begin(&frame_description, &frame) ==
           XG_RENDER_MOVIE_INVALID_ARGUMENT);
    frame_description.owner_kind = (XgRenderMovieOwnerKind)3;
    assert(xg_render_movie_frame_begin(&frame_description, &frame) ==
           XG_RENDER_MOVIE_INVALID_ARGUMENT);
    frame_description.owner_kind = XG_RENDER_MOVIE_OWNER_STANDALONE;
    frame_description.owner_receipt = 0u;
    assert(xg_render_movie_frame_begin(&frame_description, &frame) ==
           XG_RENDER_MOVIE_INVALID_ARGUMENT);
}

static void test_discontinuities_and_depth(void) {
    const uint8_t depth15[] = { 1u, 1u, 2u, 2u, 3u, 3u, 4u, 4u };
    const uint8_t depth24[] = { 9u, 8u, 7u, 6u, 5u, 4u, 3u, 2u };
    XgRenderMovieFrameDescription frame_description = description(10u, false);
    XgRenderMovieFrameHandle frame;
    XgRenderMovieFramePublication publication;
    XgRenderMovieDiagnostics diagnostics;

    xg_render_resource_repository_reset();
    xg_render_movie_publisher_reset();
    (void)publish(&frame_description, depth15);
    frame_description.guest_cycle = 20u;
    assert(xg_render_movie_frame_begin(&frame_description, &frame) ==
           XG_RENDER_MOVIE_OK);
    assert(xg_render_movie_frame_write_strip(frame, 0u, 0u, depth24, 4u) ==
           XG_RENDER_MOVIE_OK);
    assert(xg_render_movie_publisher_note_discontinuity(
               XG_RENDER_MOVIE_DISCONTINUITY_SKIP) == XG_RENDER_MOVIE_OK);
    assert(xg_render_movie_frame_publish(frame, &publication) ==
           XG_RENDER_MOVIE_STALE_FRAME);
    assert(xg_render_movie_frame_hold(&publication) == XG_RENDER_MOVIE_OK);
    assert(publication.held);
    assert(publication.discontinuity);
    assert(publication.discontinuity_reason ==
           XG_RENDER_MOVIE_DISCONTINUITY_SKIP);

    frame_description.guest_cycle = 30u;
    publication = publish(&frame_description, depth15);
    assert(publication.discontinuity);
    assert(publication.discontinuity_reason ==
           XG_RENDER_MOVIE_DISCONTINUITY_SKIP);
    assert(xg_render_movie_publisher_note_discontinuity(
               XG_RENDER_MOVIE_DISCONTINUITY_LOOP) == XG_RENDER_MOVIE_OK);
    assert(xg_render_movie_publisher_note_discontinuity(
               XG_RENDER_MOVIE_DISCONTINUITY_STALL) == XG_RENDER_MOVIE_OK);
    frame_description.guest_cycle = 40u;
    frame_description.depth24 = true;
    publication = publish(&frame_description, depth24);
    assert(publication.depth24);
    assert(publication.discontinuity_reason ==
           XG_RENDER_MOVIE_DISCONTINUITY_STALL);

    frame_description.guest_cycle = 50u;
    frame_description.depth24 = false;
    publication = publish(&frame_description, depth15);
    assert(publication.discontinuity);
    assert(publication.discontinuity_reason ==
           XG_RENDER_MOVIE_DISCONTINUITY_DEPTH_CHANGE);
    xg_render_movie_publisher_diagnostics(&diagnostics);
    assert(diagnostics.discontinuities == 4u);
    assert(diagnostics.cancelled_frames == 1u);
}

static void test_restore_active_assembly(void) {
    const uint8_t complete[] = { 5u, 6u, 7u, 8u, 9u, 10u, 11u, 12u };
    const uint8_t restored[] = { 30u, 31u, 32u, 33u,
                                 40u, 41u, 42u, 43u };
    XgRenderMovieFrameDescription frame_description = description(70u, true);
    XgRenderMovieFrameHandle frame;
    XgRenderMovieFrameHandle restored_frame;
    XgRenderMovieFramePublication publication;
    XgRenderMoviePublisherSnapshot *snapshot;
    XgRenderMovieDiagnostics diagnostics;
    size_t snapshot_size;

    xg_render_resource_repository_reset();
    xg_render_movie_publisher_reset();
    (void)publish(&frame_description, complete);
    frame_description.guest_cycle = 80u;
    frame_description.depth24 = false;
    frame_description.owner_kind = XG_RENDER_MOVIE_OWNER_FIELD;
    frame_description.owner_receipt = 202u;
    authorize_description(&frame_description);
    assert(xg_render_movie_frame_begin(&frame_description, &frame) ==
           XG_RENDER_MOVIE_OK);
    assert(xg_render_movie_frame_write_strip(frame, 0u, 0u, restored, 4u) ==
           XG_RENDER_MOVIE_OK);
    assert(xg_render_movie_publisher_snapshot_size(&snapshot_size) ==
           XG_RENDER_MOVIE_OK);
    snapshot = (XgRenderMoviePublisherSnapshot *)malloc(snapshot_size);
    assert(snapshot != NULL);
    assert(xg_render_movie_publisher_snapshot(snapshot, snapshot_size) ==
           XG_RENDER_MOVIE_OK);
    assert(snapshot->active);
    assert(snapshot->completed_strip_mask == 1u);
    assert(snapshot->active_description.owner_kind ==
           XG_RENDER_MOVIE_OWNER_FIELD);
    assert(snapshot->active_description.owner_receipt == 202u);
    assert(snapshot->complete_description.owner_kind ==
           XG_RENDER_MOVIE_OWNER_STANDALONE);
    assert(snapshot->complete_description.owner_receipt == 101u);
    assert(snapshot->complete_publication.owner_kind ==
           XG_RENDER_MOVIE_OWNER_STANDALONE);
    assert(snapshot->complete_publication.owner_receipt == 101u);

    assert(xg_render_movie_frame_cancel(frame) == XG_RENDER_MOVIE_OK);
    xg_render_resource_repository_reset();
    xg_render_movie_publisher_reset();
    snapshot->payload[snapshot->active_byte_count] = 2u;
    assert(xg_render_movie_publisher_restore(snapshot, snapshot_size,
                                              &restored_frame) ==
           XG_RENDER_MOVIE_INVALID_SNAPSHOT);
    snapshot->payload[snapshot->active_byte_count] = 1u;
    snapshot->complete_publication.owner_kind = XG_RENDER_MOVIE_OWNER_FIELD;
    assert(xg_render_movie_publisher_restore(snapshot, snapshot_size,
                                              &restored_frame) ==
           XG_RENDER_MOVIE_INVALID_SNAPSHOT);
    snapshot->complete_publication.owner_kind =
        XG_RENDER_MOVIE_OWNER_STANDALONE;
    snapshot->complete_publication.owner_receipt = 999u;
    assert(xg_render_movie_publisher_restore(snapshot, snapshot_size,
                                              &restored_frame) ==
           XG_RENDER_MOVIE_INVALID_SNAPSHOT);
    snapshot->complete_publication.owner_receipt = 101u;
    snapshot->active_authority.bytes[20] ^= 1u;
    assert(xg_render_movie_publisher_restore(snapshot, snapshot_size,
                                              &restored_frame) ==
           XG_RENDER_MOVIE_INVALID_SNAPSHOT);
    snapshot->active_authority.bytes[20] ^= 1u;
    assert(xg_render_movie_publisher_restore(snapshot, snapshot_size,
                                               &restored_frame) ==
           XG_RENDER_MOVIE_OK);
    assert(restored_frame.generation != snapshot->assembly_generation);
    assert(restored_frame.generation != frame.generation);
    assert(xg_render_movie_frame_hold(&publication) == XG_RENDER_MOVIE_OK);
    assert(publication.held);
    assert(publication.depth24);
    assert(publication.owner_kind == XG_RENDER_MOVIE_OWNER_STANDALONE);
    assert(publication.owner_receipt == 101u);
    assert(publication.discontinuity);
    assert(publication.discontinuity_reason ==
           XG_RENDER_MOVIE_DISCONTINUITY_RESTORE);
    assert_surface_bytes(publication.surface, complete, 101u);
    assert(xg_render_movie_frame_write_strip(restored_frame, 1u, 4u,
                                              restored + 4u, 4u) ==
           XG_RENDER_MOVIE_OK);
    assert(xg_render_movie_frame_publish(restored_frame, &publication) ==
           XG_RENDER_MOVIE_OK);
    assert(!publication.depth24);
    assert(publication.owner_kind == XG_RENDER_MOVIE_OWNER_FIELD);
    assert(publication.owner_receipt == 202u);
    assert(publication.discontinuity);
    assert(publication.discontinuity_reason ==
           XG_RENDER_MOVIE_DISCONTINUITY_RESTORE);
    assert_surface_bytes(publication.surface, restored, 202u);
    xg_render_movie_publisher_diagnostics(&diagnostics);
    assert(diagnostics.complete_frames == 2u);
    assert(diagnostics.discontinuities == 1u);
    assert(!diagnostics.frame_active);
    free(snapshot);
}

static void test_portable_wire_restore(void) {
    const uint8_t complete[] = { 2u, 4u, 6u, 8u, 10u, 12u, 14u, 16u };
    const uint8_t active[] = { 31u, 32u, 33u, 34u,
                               41u, 42u, 43u, 44u };
    XgRenderMovieFrameDescription frame_description = description(90u, true);
    XgRenderMovieFrameHandle frame;
    XgRenderMovieFrameHandle restored_frame;
    XgRenderMovieFramePublication publication;
    XgRenderMovieDiagnostics checkpoint_diagnostics;
    XgRenderMovieDiagnostics restored_diagnostics;
    XgRenderResourceView view;
    uint64_t checkpoint_capability;
    uint8_t *wire;
    size_t wire_size;

    xg_render_resource_repository_reset();
    xg_render_movie_publisher_reset();
    assert(xg_render_movie_publisher_note_discontinuity(
               XG_RENDER_MOVIE_DISCONTINUITY_LOOP) == XG_RENDER_MOVIE_OK);
    (void)publish(&frame_description, complete);
    assert(xg_render_movie_frame_hold(&publication) == XG_RENDER_MOVIE_OK);
    frame_description.guest_cycle = 95u;
    assert(xg_render_movie_frame_begin(&frame_description, &frame) ==
           XG_RENDER_MOVIE_OK);
    assert(xg_render_movie_frame_cancel(frame) == XG_RENDER_MOVIE_OK);
    frame_description.guest_cycle = 100u;
    frame_description.depth24 = false;
    frame_description.owner_kind = XG_RENDER_MOVIE_OWNER_FIELD;
    frame_description.owner_receipt = 302u;
    authorize_description(&frame_description);
    assert(xg_render_movie_frame_begin(&frame_description, &frame) ==
           XG_RENDER_MOVIE_OK);
    assert(xg_render_movie_frame_write_strip(frame, 0u, 0u, active, 4u) ==
           XG_RENDER_MOVIE_OK);
    assert(xg_render_movie_frame_publish(frame, &publication) ==
           XG_RENDER_MOVIE_INCOMPLETE_FRAME);
    xg_render_movie_publisher_diagnostics(&checkpoint_diagnostics);
    assert(xg_render_movie_publisher_wire_size(&wire_size) ==
           XG_RENDER_MOVIE_OK);
    assert(wire_size == 332u +
           2u * XG_RENDER_RESOURCE_CAPABILITY_CHECKPOINT_SIZE +
           sizeof(complete) + sizeof(active) * 2u);
    wire = (uint8_t *)malloc(wire_size);
    assert(wire != NULL);
    assert(xg_render_movie_publisher_wire_write(wire, wire_size) ==
           XG_RENDER_MOVIE_OK);
    checkpoint_capability = frame_description.owner_capability;
    assert(xg_render_movie_publisher_wire_validate(wire, wire_size, 17u) ==
           XG_RENDER_MOVIE_OK);
    wire[280u] ^= 1u;
    assert(xg_render_movie_publisher_wire_validate(wire, wire_size, 17u) ==
           XG_RENDER_MOVIE_INVALID_SNAPSHOT);
    wire[280u] ^= 1u;
    wire[324u] |= 8u;
    assert(xg_render_movie_publisher_wire_validate(wire, wire_size, 17u) ==
           XG_RENDER_MOVIE_INVALID_SNAPSHOT);
    wire[324u] &= (uint8_t)~8u;
    wire[332u] ^= 1u;
    assert(xg_render_movie_publisher_wire_validate(wire, wire_size, 17u) ==
           XG_RENDER_MOVIE_INVALID_SNAPSHOT);
    wire[332u] ^= 1u;

    xg_render_resource_repository_reset();
    xg_render_movie_publisher_reset();
    wire[wire_size - 1u] ^= 1u;
    assert(xg_render_movie_publisher_wire_restore(
               wire, wire_size, 17u, &restored_frame) ==
           XG_RENDER_MOVIE_INVALID_SNAPSHOT);
    wire[wire_size - 1u] ^= 1u;
    assert(xg_render_movie_publisher_wire_restore(
               wire, wire_size, 17u, &restored_frame) ==
           XG_RENDER_MOVIE_OK);
    xg_render_movie_publisher_diagnostics(&restored_diagnostics);
    assert(restored_diagnostics.complete_frames ==
           checkpoint_diagnostics.complete_frames);
    assert(restored_diagnostics.frame_generation ==
           checkpoint_diagnostics.frame_generation);
    assert(restored_diagnostics.partial_publish_attempts ==
           checkpoint_diagnostics.partial_publish_attempts);
    assert(restored_diagnostics.cancelled_frames ==
           checkpoint_diagnostics.cancelled_frames);
    assert(restored_diagnostics.held_frames ==
           checkpoint_diagnostics.held_frames);
    assert(restored_diagnostics.discontinuities ==
           checkpoint_diagnostics.discontinuities + 1u);
    assert(restored_diagnostics.completed_strip_mask ==
           checkpoint_diagnostics.completed_strip_mask);
    assert(restored_diagnostics.frame_active ==
           checkpoint_diagnostics.frame_active);
    assert(restored_diagnostics.complete_frame_available ==
           checkpoint_diagnostics.complete_frame_available);
    assert(restored_diagnostics.discontinuity_pending);
    assert(restored_diagnostics.discontinuity_reason ==
           XG_RENDER_MOVIE_DISCONTINUITY_RESTORE);
    assert(restored_frame.generation != frame.generation);
    assert(xg_render_movie_frame_hold(&publication) == XG_RENDER_MOVIE_OK);
    assert(publication.owner_kind == XG_RENDER_MOVIE_OWNER_STANDALONE);
    assert(publication.owner_receipt == 101u);
    assert(xg_render_resource_view(publication.surface, &view) ==
           XG_RENDER_RESOURCE_OK);
    assert(view.owner_generation == 17u);
    assert(view.provenance.kind == XG_RENDER_RESOURCE_PROVENANCE_SOURCE);
    assert(view.provenance.receipt == 101u && !view.provenance.synthetic);
    assert(memcmp(view.bytes, complete, sizeof(complete)) == 0);
    assert(xg_render_movie_frame_write_strip(
               restored_frame, 1u, 4u, active + 4u, 4u) ==
           XG_RENDER_MOVIE_OK);
    assert(xg_render_movie_frame_publish(restored_frame, &publication) ==
           XG_RENDER_MOVIE_OK);
    assert(publication.owner_kind == XG_RENDER_MOVIE_OWNER_FIELD);
    assert(publication.owner_receipt == 302u);
    assert(xg_render_resource_view(publication.surface, &view) ==
           XG_RENDER_RESOURCE_OK);
    assert(view.owner_generation == 17u);
    assert(view.provenance.kind == XG_RENDER_RESOURCE_PROVENANCE_SOURCE);
    assert(view.provenance.receipt == 302u && !view.provenance.synthetic);
    assert(view.provenance.capability != checkpoint_capability);
    assert(memcmp(view.bytes, active, sizeof(active)) == 0);
    free(wire);
}

static void test_complete_frame_transaction_cancel_and_commit(void) {
    const uint8_t bytes[] = { 7u, 6u, 5u, 4u, 3u, 2u, 1u, 9u };
    XgRenderMovieFrameDescription frame_description = description(500u, false);
    XgRenderMovieFrameTransaction *transaction = NULL;
    XgRenderMovieFramePublication prepared;
    XgRenderMovieFramePublication held;
    XgRenderMovieDiagnostics before;
    XgRenderMovieDiagnostics after;
    XgRenderResourceDiagnostics resources_before;
    XgRenderResourceDiagnostics resources_after;
    uint8_t *wire_before;
    uint8_t *wire_after;
    size_t wire_size;

    xg_render_resource_repository_reset();
    xg_render_movie_publisher_reset();
    frame_description.expected_strips = 1u;
    authorize_description(&frame_description);
    assert(xg_render_movie_publisher_wire_size(&wire_size) ==
           XG_RENDER_MOVIE_OK);
    wire_before = (uint8_t *)malloc(wire_size);
    wire_after = (uint8_t *)malloc(wire_size);
    assert(wire_before != NULL && wire_after != NULL);
    assert(xg_render_movie_publisher_wire_write(wire_before, wire_size) ==
           XG_RENDER_MOVIE_OK);
    xg_render_movie_publisher_diagnostics(&before);
    xg_render_resource_repository_diagnostics(&resources_before);

    assert(xg_render_movie_frame_transaction_prepare(
               &frame_description, bytes, sizeof(bytes),
               &transaction, &prepared) == XG_RENDER_MOVIE_OK);
    assert(transaction != NULL);
    xg_render_movie_publisher_diagnostics(&after);
    assert(memcmp(&after, &before, sizeof(after)) == 0);
    assert(xg_render_movie_publisher_wire_write(wire_after, wire_size) ==
           XG_RENDER_MOVIE_OK);
    assert(memcmp(wire_before, wire_after, wire_size) == 0);
    xg_render_movie_frame_transaction_cancel(transaction);
    transaction = NULL;
    xg_render_resource_repository_diagnostics(&resources_after);
    assert(resources_after.live_resources == resources_before.live_resources);
    assert(resources_after.retained_resources ==
           resources_before.retained_resources);
    assert(xg_render_movie_frame_hold(&held) ==
           XG_RENDER_MOVIE_NO_COMPLETE_FRAME);

    assert(xg_render_movie_frame_transaction_prepare(
               &frame_description, bytes, sizeof(bytes),
               &transaction, &prepared) == XG_RENDER_MOVIE_OK);
    xg_render_movie_frame_transaction_commit(transaction);
    assert(xg_render_movie_frame_hold(&held) == XG_RENDER_MOVIE_OK);
    assert(held.surface.resource_id == prepared.surface.resource_id);
    assert(held.surface.generation == prepared.surface.generation);
    assert_surface_bytes(held.surface, bytes, frame_description.owner_receipt);
    xg_render_movie_publisher_diagnostics(&after);
    assert(after.complete_frames == 1u);
    assert(after.frame_generation == 1u);
    free(wire_after);
    free(wire_before);
}

static void test_committed_transaction_exact_rollback(void) {
    const uint8_t old_bytes[] = {1u, 2u, 3u, 4u, 5u, 6u, 7u, 8u};
    const uint8_t new_bytes[] = {8u, 7u, 6u, 5u, 4u, 3u, 2u, 1u};
    XgRenderMovieFrameDescription frame_description = description(600u, false);
    XgRenderMovieFramePublication old_publication;
    XgRenderMovieFramePublication new_publication;
    XgRenderMovieFramePublication held;
    XgRenderMovieFrameTransaction *transaction = NULL;
    XgRenderMoviePublisherSnapshot *snapshot;
    XgRenderResourceView view;
    size_t snapshot_size;

    xg_render_resource_repository_reset();
    xg_render_movie_publisher_reset();
    old_publication = publish(&frame_description, old_bytes);
    assert(xg_render_movie_publisher_snapshot_size(&snapshot_size) ==
           XG_RENDER_MOVIE_OK);
    snapshot = (XgRenderMoviePublisherSnapshot *)malloc(snapshot_size);
    assert(snapshot != NULL);
    assert(xg_render_movie_publisher_snapshot(snapshot, snapshot_size) ==
           XG_RENDER_MOVIE_OK);
    assert(xg_render_resource_view(old_publication.surface, &view) ==
           XG_RENDER_RESOURCE_OK);
    assert(xg_render_resource_acquire_current(
               old_publication.surface, view.content_digest) ==
           XG_RENDER_RESOURCE_OK);

    frame_description.expected_strips = 1u;
    frame_description.guest_cycle++;
    assert(xg_render_movie_frame_transaction_prepare(
               &frame_description, new_bytes, sizeof(new_bytes),
               &transaction, &new_publication) == XG_RENDER_MOVIE_OK);
    xg_render_movie_frame_transaction_commit(transaction);
    assert(xg_render_movie_publisher_transaction_rollback(
               snapshot, snapshot_size, new_publication.surface,
               old_publication.surface) == XG_RENDER_MOVIE_OK);
    assert(xg_render_movie_frame_hold(&held) == XG_RENDER_MOVIE_OK);
    assert(held.surface.resource_id == old_publication.surface.resource_id);
    assert(held.surface.generation == old_publication.surface.generation);
    assert(held.frame_generation == old_publication.frame_generation);
    assert_surface_bytes(held.surface, old_bytes,
                         frame_description.owner_receipt);
    assert(xg_render_resource_view(new_publication.surface, &view) ==
           XG_RENDER_RESOURCE_NOT_FOUND);
    assert(xg_render_resource_release(old_publication.surface) ==
           XG_RENDER_RESOURCE_OK);
    assert(xg_render_resource_view(old_publication.surface, &view) ==
           XG_RENDER_RESOURCE_OK);
    assert(view.current && view.retain_count == 0u);
    free(snapshot);
}

int main(void) {
    test_invalid_owner_rejected();
    test_partial_cancel_and_complete_only_hold();
    test_discontinuities_and_depth();
    test_restore_active_assembly();
    test_portable_wire_restore();
    test_complete_frame_transaction_cancel_and_commit();
    test_committed_transaction_exact_rollback();
    xg_render_movie_publisher_reset();
    xg_render_resource_repository_reset();
    return 0;
}
