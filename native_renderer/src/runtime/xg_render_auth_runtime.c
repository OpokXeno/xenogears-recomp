#include "xg_render_auth_runtime_control.h"
#include "xg_render_auth_runtime_diagnostics.h"
#include "xg_render_auth_runtime_hooks.h"
#include "xg_render_auth_runtime_invalidation.h"

#include "guest_render_bridge.h"
#include "overlay_api.h"
#include "psx_xg_render_auth_hook_types.h"
#include "xg_render_auth.h"
#include "xg_field_render_services.h"
#include "xg_render_vram_resources.h"
#include "xg_render_manifest_generated.h"
#include "xg_render_runtime_variant_auth.h"
#include "xg_render_runtime_composition.h"
#include "xg_render_runtime_host_services.h"
#include "xg_render_static_auth_metadata.h"
#include "xg_render_instrumentation.h"
#include "xg_render_movie_publisher.h"
#include "xg_render_fragment_runtime.h"
#include "xg_render_resource_repository.h"
#include "xg_render_source_frame.h"
#include "xg_render_native_work.h"
#include "xg_render_battle_geometry.h"
#include "xg_render_submission.h"
#include "xg_render_surface_graph.h"
#include "xg_render_ui_resources.h"
#include "xg_render_ui_owner_catalog.h"
#include "xg_render_vram_journal.h"
#include "guest_render_native_stream.h"
#include "guest_render_transaction.h"
#include "gte_attribution.h"
#include "gpu.h"
#include "psx_cycles.h"
#include "crash_trace.h"

#include <stdbool.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

/*
 * Authentication and scene-policy runtime. Producer composition is supplied
 * through XgRenderRuntimeAuthSceneServices.
 */

enum { XG_RENDER_AUTH_ARTIFACT_CAPACITY = 16u };

typedef struct XgRenderAuthenticatedArtifact {
    PsxXgRenderAuthCandidate candidate;
    XgRenderResourceProvenance provenance;
    uint64_t generation;
    uint64_t scene_generation;
    bool static_text;
    bool occupied;
} XgRenderAuthenticatedArtifact;

typedef struct XgRenderAuthRuntimeState {
    XgRenderAuth *auth;
    PsxXgRenderAuthCandidate pending_candidate;
    XgRenderAuthenticatedArtifact
        authenticated_artifacts[XG_RENDER_AUTH_ARTIFACT_CAPACITY];
    PsxXgRenderAuthCandidate authenticated_artifact_candidate;
    PsxXgRenderAuthCandidate movie_frame_artifact_candidate;
    PsxXgRenderAuthRejectionReceipt rejection;
    uint64_t scene_generation;
    uint64_t interpolation_scene_generation;
    uint64_t pending_scene_generation;
    uint64_t authenticated_artifact_scene_generation;
    uint64_t authenticated_artifact_generation;
    XgRenderResourceProvenance authenticated_artifact_provenance;
    XgRenderResourceProvenance movie_frame_artifact_provenance;
    uint64_t authenticated_producer_scene_generation;
    uint32_t authenticated_producer_entry;
    uint32_t authenticated_artifact_primary_index;
    uint32_t authenticated_artifact_count;
    uint32_t pending_variant_entry;
    XgRenderAuthTier pending_variant_tier;
    bool armed;
    bool active;
    bool completed;
    bool pending_candidate_valid;
    bool authenticated_artifact_candidate_valid;
    bool authenticated_artifact_primary_valid;
    bool movie_frame_artifact_candidate_valid;
    bool pending_variant_sequence;
    bool pending_variant_capture_ready;
    bool candidate_matched;
    bool candidate_dispatched;
    bool gte_attribution_producer_active;
    GpuMovieOwnerKind movie_active_owner;
    GuestRenderRenderMode requested_render_mode;
    PsxXgRenderPresentationGate presentation_gate;
    void *presentation_user_data;
    NativeRenderPresentationSnapshot presentation;
    bool configured;
    bool boot_restore_timeline_preapplied;
    bool movie_publication_pending_boundary;
} XgRenderAuthRuntimeState;

static XgRenderAuthRuntimeState state = {
    .armed = true,
    .scene_generation = 1u,
    .interpolation_scene_generation = 1u,
    .requested_render_mode = GUEST_RENDER_RENDER_ORIGINAL,
};

bool g_psx_xg_render_auth_cold_enabled;
static PsxXgRenderAuthCompletedProofReceipt completed_proof;
static uint64_t completed_proof_scene_generation;
static atomic_flag completed_proof_guard = ATOMIC_FLAG_INIT;
static atomic_flag presentation_lifecycle_guard = ATOMIC_FLAG_INIT;
static atomic_uint_fast64_t movie_owner_start_attempts;
static atomic_uint_fast64_t movie_owner_start_successes;
static atomic_uint_fast64_t movie_frame_complete_attempts;
static atomic_uint_fast64_t movie_frame_complete_successes;
static atomic_uint_fast64_t movie_vram_frame_events;
static atomic_uint_fast64_t movie_publication_successes;
static atomic_uint_fast64_t movie_publication_failures;
static atomic_uint movie_owner_last_kind;
static atomic_uint movie_owner_last_expected_callback;
static atomic_uint movie_owner_last_observed_callback;
static atomic_uint movie_owner_last_start_blocker;
static atomic_uint movie_last_frame_number;
static atomic_uint movie_last_frame_width;
static atomic_uint movie_last_frame_height;
static atomic_uint movie_last_frame_callback;
static atomic_uint movie_event_artifact_base;
static atomic_uint movie_event_artifact_size;
static atomic_uint movie_publication_last_blocker;
static atomic_uint movie_publication_last_blocker_detail;
static atomic_uint movie_event_publication_count;
static atomic_uint movie_event_edge_count;
static atomic_uint_fast64_t movie_event_coverage_bytes;
static atomic_uint_fast64_t movie_mdec_surface_successes;
static atomic_uint_fast64_t movie_mdec_surface_failures;
static atomic_uint movie_mdec_surface_x;
static atomic_uint movie_mdec_surface_y;
static atomic_uint movie_mdec_surface_width;
static atomic_uint movie_mdec_surface_height;
static atomic_uint movie_scanout_x;
static atomic_uint movie_scanout_y;
static atomic_uint movie_scanout_width;
static atomic_uint movie_scanout_height;
static atomic_uint movie_source_active_after_publication;
static atomic_uint movie_source_passes_after_publication;
static atomic_uint movie_source_active_before_boundary;
static atomic_uint movie_source_passes_before_boundary;
static atomic_uint_fast64_t movie_publication_boundaries;
static atomic_uint_fast64_t movie_publication_boundaries_active;
static atomic_uint movie_publication_boundary_completion_result;
static atomic_uint movie_publication_boundary_publish_result;
static atomic_uint_fast64_t static_artifact_attempts;
static atomic_uint_fast64_t static_artifact_successes;
static atomic_uint static_artifact_last_pc;
static atomic_uint static_artifact_last_blocker;
static XgSemanticResourceRef retained_movie_surface;

/* The static loader calls note_candidate_dispatch only AFTER its selected
 * owner's code SHA and linked game/manifest identity match. Keep that input
 * authority separate from the legacy whole-scene replacement transaction. */
typedef struct MotionSourceOwner {
    uint32_t entry;
    uint32_t size;
    uint32_t instruction;
} MotionSourceOwner;

static const MotionSourceOwner motion_source_owners[] = {
    {0x800748e8u, 0x924u, 0x27bdff30u}, {0x800848f4u, 0x40cu, 0x24020800u},
    {0x801dc5c0u, 0x288u, 0x27bdffc8u}, {0x801dc848u, 0x3ecu, 0x27bdffc8u},
    {0x801dcc3cu, 0x150u, 0x27bdffc8u}, {0x801dcd8cu, 0x8cu, 0x27bdffd8u},
    {0x801dcec8u, 0xd30u, 0x27bdff20u},
    /* Disc-1 Battle articulated parts and arena model dispatchers. */
    {0x8009f5b8u, 0x150u, 0x27bdffc8u},
    {0x8009f844u, 0xff4u, 0x27bdfef0u},
    {0x800a48ecu, 0x250u, 0x27bdffc0u},
};

typedef struct MotionSourceAuthority {
    XgRenderResourceProvenance provenance;
    PsxXgRenderAuthCandidate candidate;
    uint64_t scene_generation;
    bool valid;
} MotionSourceAuthority;

static MotionSourceAuthority
    motion_source_authorities[sizeof(motion_source_owners) / sizeof(motion_source_owners[0])];
static uint64_t motion_source_receipt;
static uint64_t motion_source_epoch = 1;

static void motion_source_retire(MotionSourceAuthority *entry) {
    if (entry->provenance.capability)
        (void)xg_render_resource_capability_retire(entry->provenance);
    *entry = (MotionSourceAuthority){0};
}

static void motion_source_invalidate(uint32_t address, uint32_t size, bool all) {
    const uint64_t begin = address & 0x1fffffffu, end = begin + size;
    bool changed = all;
    for (size_t i = 0; i < sizeof(motion_source_owners) / sizeof(motion_source_owners[0]); ++i) {
        const uint64_t owner = motion_source_owners[i].entry & 0x1fffffffu;
        if (all || (size && begin < owner + motion_source_owners[i].size && owner < end)) {
            changed |= motion_source_authorities[i].valid;
            motion_source_retire(&motion_source_authorities[i]);
        }
    }
    if (changed && motion_source_epoch != UINT64_MAX)
        ++motion_source_epoch;
}

static void note_motion_source_candidate(const PsxXgRenderAuthCandidate *candidate) {
    XgRenderRuntimeHostServices host;
    if (!candidate || !xg_render_native_work_enabled())
        return;
    const uint32_t pc = (candidate->producer_entry & 0x1fffffffu) | 0x80000000u;
    size_t index = 0;
    for (; index < sizeof(motion_source_owners) / sizeof(motion_source_owners[0]); ++index)
        if (motion_source_owners[index].entry == pc)
            break;
    if (index == sizeof(motion_source_owners) / sizeof(motion_source_owners[0]))
        return;
    const MotionSourceOwner *owner = &motion_source_owners[index];
    const uint32_t base = candidate->artifact_base & 0x1fffffffu;
    const uint32_t physical = pc & 0x1fffffffu;
    MotionSourceAuthority *entry = &motion_source_authorities[index];
    xg_render_motion_note(XG_MOTION_CANDIDATE_SEEN, pc);
    /* Full-artifact range is the static AOT callback contract, not the dynamic
     * loader's per-function range. Four-byte alignment is valid for PSX images;
     * Field's authenticated load base is 0x8006faf0, NOT page aligned. */
    if (!candidate->authority_provenance || !candidate->pair_bound || !candidate->pair_id ||
        candidate->runtime_variant_bound || (base & 3u) || base >= 0x200000u ||
        !candidate->artifact_size || candidate->artifact_size > 0x200000u - base ||
        (candidate->range_start & 0x1fffffffu) != base ||
        candidate->range_size != candidate->artifact_size || physical < base ||
        owner->size > candidate->artifact_size ||
        physical - base > candidate->artifact_size - owner->size ||
        (candidate->dispatch_pc & 0x1fffffffu) < physical ||
        (candidate->dispatch_pc & 0x1fffffffu) - physical >= owner->size ||
        memcmp(candidate->identity.game_sha256, xg_render_game_identity, 32) ||
        memcmp(candidate->identity.manifest_sha256, xg_render_manifest_identity, 32) ||
        !memcmp(candidate->artifact_sha256, (uint8_t[32]){0}, 32) ||
        !xg_render_runtime_host_services(&host) || !host.read_word ||
        host.read_word(pc) != owner->instruction) {
        motion_source_retire(entry);
        xg_render_motion_note(XG_MOTION_CANDIDATE_IDENTITY_REJECT, pc);
        return;
    }
    XgRenderResourceCapabilityMetadata prior;
    if (entry->valid && entry->scene_generation == state.scene_generation &&
        entry->candidate.pair_id == candidate->pair_id &&
        entry->candidate.artifact_base == candidate->artifact_base &&
        entry->candidate.artifact_size == candidate->artifact_size &&
        !memcmp(entry->candidate.artifact_sha256, candidate->artifact_sha256, 32) &&
        xg_render_resource_capability_validate(&entry->provenance, XG_RENDER_RESOURCE_OWNER_SCENE,
                                               state.scene_generation,
                                               &prior) == XG_RENDER_RESOURCE_CAPABILITY_OK)
        return;
    if (entry->valid && motion_source_epoch != UINT64_MAX)
        ++motion_source_epoch;
    motion_source_retire(entry);
    if (motion_source_receipt >= UINT64_C(0x000000ffffffffff))
        return;
    /* Snapshot the source owner's actual code, not mutable full-image RAM. */
    uint32_t words[0x1000u / 4u];
    if (owner->size > sizeof(words)) return;
    for (uint32_t i = 0; i < owner->size / 4; ++i)
        words[i] = host.read_word(pc + i * 4);
    XgRenderResourceCapabilityMetadata metadata = {
        .kind = XG_RENDER_RESOURCE_PROVENANCE_SOURCE,
        .receipt = UINT64_C(0x58474d0000000000) | ++motion_source_receipt,
        .lifetime = XG_RENDER_RESOURCE_CAPABILITY_SCENE,
        .owner_kind = XG_RENDER_RESOURCE_OWNER_SCENE,
        .owner_generation = state.scene_generation,
        .source = {.source_class = XG_RENDER_RESOURCE_SOURCE_MODEL_RANGE,
                   .range_offset = physical - base,
                   .range_size = owner->size,
                   .range_content_digest = xg_render_resource_digest(words, owner->size),
                   .origin_artifact = {.base = candidate->artifact_base,
                                       .size = candidate->artifact_size}},
    };
    memcpy(metadata.source.identity.bytes, candidate->artifact_sha256, 32);
    memcpy(metadata.source.origin_artifact.sha256, candidate->artifact_sha256, 32);
    if (xg_render_resource_capability_register(&metadata, &entry->provenance) !=
        XG_RENDER_RESOURCE_CAPABILITY_OK) {
        xg_render_motion_note(XG_MOTION_SOURCE_CAPABILITY_REJECT, pc);
        return;
    }
    entry->candidate = *candidate;
    entry->scene_generation = state.scene_generation;
    entry->valid = true;
    xg_render_motion_note(XG_MOTION_CANDIDATE_REGISTERED, pc);
}

bool xg_render_battle_geometry_authorizes_call(uint32_t call_pc) {
    XgRenderRuntimeHostServices host;
    XgRenderResourceCapabilityMetadata metadata;
    const uint32_t pc = (call_pc & 0x1fffffffu) | 0x80000000u;
    if ((pc != 0x8009f6bcu && pc != 0x800a0064u && pc != 0x800a4ae4u) ||
        state.requested_render_mode != GUEST_RENDER_RENDER_NATIVE ||
        !xg_render_native_work_enabled() ||
        !xg_render_runtime_host_services(&host) || !host.read_word)
        return false;
    for (size_t i = 0; i < sizeof(motion_source_owners) / sizeof(motion_source_owners[0]); ++i) {
        const MotionSourceOwner *owner = &motion_source_owners[i];
        const MotionSourceAuthority *authority = &motion_source_authorities[i];
        if (pc < owner->entry || pc - owner->entry >= owner->size) continue;
        /* Reuse the full-artifact dispatch receipt, not resident-text authority
         * (which cannot authorize overlay PCs), and never query a pose/timeline. */
        return authority->valid && authority->scene_generation == state.scene_generation &&
            xg_render_resource_capability_validate(&authority->provenance,
                XG_RENDER_RESOURCE_OWNER_SCENE, state.scene_generation, &metadata) ==
                XG_RENDER_RESOURCE_CAPABILITY_OK &&
            host.read_word(owner->entry) == owner->instruction &&
            host.read_word(pc) == 0x0c00b1c0u && host.read_word(pc + 4u) == 0u;
    }
    return false;
}

bool psx_xg_render_motion_source(uint32_t pc, XgRenderMotionSource *out) {
    XgRenderPresentationDiagnostics timeline;
    XgRenderRuntimeHostServices host;
    XgRenderResourceCapabilityMetadata metadata;
    xg_render_motion_note(XG_MOTION_SOURCE_REQUEST, pc);
    if (!out || state.requested_render_mode != GUEST_RENDER_RENDER_NATIVE ||
        !xg_render_native_work_enabled() || !state.scene_generation) {
        xg_render_motion_note(XG_MOTION_SOURCE_MODE_REJECT, pc);
        return false;
    }
    const uint32_t physical = pc & 0x1fffffffu;
    size_t i = 0;
    for (; i < sizeof(motion_source_owners) / sizeof(motion_source_owners[0]); ++i) {
        const uint32_t begin = motion_source_owners[i].entry & 0x1fffffffu;
        if (physical >= begin && physical - begin < motion_source_owners[i].size)
            break;
    }
    if (i == sizeof(motion_source_owners) / sizeof(motion_source_owners[0]) ||
        !motion_source_authorities[i].valid ||
        motion_source_authorities[i].scene_generation != state.scene_generation) {
        xg_render_motion_note(XG_MOTION_SOURCE_DISPATCH_MISSING, pc);
        return false;
    }
    MotionSourceAuthority *entry = &motion_source_authorities[i];
    if (!xg_render_runtime_host_services(&host) || !host.read_word ||
        host.read_word(motion_source_owners[i].entry) != motion_source_owners[i].instruction) {
        motion_source_retire(entry);
        xg_render_motion_note(XG_MOTION_SOURCE_OPCODE_REJECT, pc);
        return false;
    }
    if (xg_render_resource_capability_validate(&entry->provenance, XG_RENDER_RESOURCE_OWNER_SCENE,
                                               state.scene_generation,
                                               &metadata) != XG_RENDER_RESOURCE_CAPABILITY_OK) {
        xg_render_motion_note(XG_MOTION_SOURCE_CAPABILITY_REJECT, pc);
        return false;
    }
    xg_render_semantic_presentation_diagnostics(&timeline);
    if (!timeline.presentation_epoch || timeline.guest_vblank_sequence == UINT64_MAX)
        return false;
    *out = (XgRenderMotionSource){.provenance = entry->provenance,
                                  .presentation_epoch = timeline.presentation_epoch,
                                  .scene_generation = state.scene_generation,
                                  .continuity_generation = motion_source_epoch,
                                  .source_update = timeline.guest_vblank_sequence + 1};
    xg_render_motion_note(XG_MOTION_SOURCE_READY, pc);
    return true;
}

static void abort_active(XgRenderAuthReason reason,
                         PsxXgRenderAuthRejectionSource source,
                         bool has_hook, PsxXgRenderAuthHook hook,
                         uint32_t pc);
static uint64_t interpolation_scene_generation(void);
static bool pending_variant_artifact_candidate_matches(uint32_t pc);
static bool artifact_authority_for_pc(
    uint32_t pc, XgRenderArtifactAuthority *out_authority);
static bool static_artifact_authority_for_cutover(
    uint32_t pc, uint32_t instruction_word,
    XgRenderArtifactAuthority *out_authority);
static bool current_artifact_is_authorized(void);
static bool movie_artifact_candidate_matches(
    const PsxXgRenderAuthCandidate *candidate);
static bool artifact_binary_identity_matches(
    const PsxXgRenderAuthCandidate *left,
    const PsxXgRenderAuthCandidate *right);
static void clear_movie_frame_artifact_candidate(void);
static bool completed_proof_matches_tier(XgRenderAuthTier tier);
static bool submission_active_auth_append(
    uint32_t packet_address, uint32_t source_primitive_index,
    uint32_t ot_bucket, uint8_t payload_word_count,
    const XgRenderIrNativePrimitive *primitive, bool force_pending_capture,
    uint32_t *out_failure_detail);
static bool submission_presentation_gate(void);
static bool composition_prepare_source_target(
    const XgRenderSourceFrameDescription *description,
    XgSemanticResourceRef *out_target);
static bool composition_standalone_source_identity(
    XgSemanticSceneIdentity *out_identity,
    uint32_t *out_scene_generation);
static bool composition_retained_movie_surface(
    XgSemanticResourceRef *out_surface);
static uint64_t timeline_invalidate_locked(
    XgRenderTimelineInvalidationReason reason);

static void release_retained_movie_surface(void) {
    if (retained_movie_surface.resource_id != 0u)
        (void)xg_render_resource_release((XgRenderResourceHandle){
            retained_movie_surface.resource_id,
            retained_movie_surface.generation,
        });
    retained_movie_surface = (XgSemanticResourceRef){0};
}

static bool retain_movie_surface(const XgSemanticResourceRef *surface) {
    if (surface == NULL || surface->resource_id == 0u ||
        surface->generation == 0u || surface->content_digest == 0u ||
        xg_render_resource_acquire_current((XgRenderResourceHandle){
            surface->resource_id, surface->generation,
        }, surface->content_digest) != XG_RENDER_RESOURCE_OK)
        return false;
    release_retained_movie_surface();
    retained_movie_surface = *surface;
    return true;
}

static void composition_query_state(
        XgRenderRuntimeAuthSceneState *out_state) {
    const uint64_t primary_generation =
        state.authenticated_artifact_primary_valid &&
        state.authenticated_artifact_primary_index <
            XG_RENDER_AUTH_ARTIFACT_CAPACITY
        ? state.authenticated_artifacts[
              state.authenticated_artifact_primary_index].generation
        : 0u;

    if (out_state == NULL) return;
    *out_state = (XgRenderRuntimeAuthSceneState){
        .render_mode = state.requested_render_mode,
        .pending_tier = state.pending_variant_tier,
        .scene_generation = state.scene_generation,
        .interpolation_generation = interpolation_scene_generation(),
        .artifact_generation = primary_generation,
        .artifact_provenance = state.authenticated_artifact_provenance,
        .pending_producer_entry = state.pending_variant_entry,
        .armed = state.armed,
        .active = state.active,
        .completed = state.completed,
        .movie_owner_active =
            state.movie_active_owner != GPU_MOVIE_OWNER_NONE,
        .pending_sequence = state.pending_variant_sequence,
        .pending_capture_ready = state.pending_variant_capture_ready,
        .candidate_matched = state.candidate_matched,
        .candidate_dispatched = state.candidate_dispatched,
    };
}

static bool composition_auth_snapshot(XgRenderAuthSnapshot *out_snapshot) {
    return out_snapshot != NULL && state.auth != NULL &&
        xg_render_auth_snapshot(state.auth, out_snapshot) == XG_RENDER_AUTH_OK;
}

static bool composition_auth_ir_item_get(
        size_t index, XgRenderIrNativeItem *out_item) {
    return out_item != NULL && state.auth != NULL &&
        xg_render_auth_native_item_get(state.auth, index, out_item) ==
            XG_RENDER_AUTH_OK;
}

static void composition_reject_auth(uint32_t blocker) {
    (void)blocker;
    abort_active(XG_RENDER_AUTH_REJECT_TRANSACTION_FAILURE,
                 PSX_XG_RENDER_AUTH_REJECTION_SOURCE_RUNTIME_HOOK,
                 false, PSX_XG_RENDER_AUTH_HOOK_CAPTURE, 0u);
}

static const XgRenderRuntimeAuthSceneServices *composition_services(void) {
    static XgRenderRuntimeAuthSceneServices services;
    XgRenderRuntimeHostServices host = { 0 };
    const bool host_configured = xg_render_runtime_host_services(&host);

    services = (XgRenderRuntimeAuthSceneServices){
        .query_state = composition_query_state,
        .auth_snapshot = composition_auth_snapshot,
        .auth_ir_item_get = composition_auth_ir_item_get,
        .append_authenticated_ir = submission_active_auth_append,
        .prepare_source_target = composition_prepare_source_target,
        .standalone_source_identity =
            composition_standalone_source_identity,
        .retained_movie_surface = composition_retained_movie_surface,
        .artifact_authority_for_pc = artifact_authority_for_pc,
        .static_artifact_authority_for_cutover =
            static_artifact_authority_for_cutover,
        .artifact_authorizes_pc = pending_variant_artifact_candidate_matches,
        .native_text_authorizes_pc =
            host_configured ? host.native_text_authorizes_pc : NULL,
        .artifact_is_authorized = current_artifact_is_authorized,
        .completed_proof_matches_tier = completed_proof_matches_tier,
        .reject_auth = composition_reject_auth,
        .presentation_gate = submission_presentation_gate,
        .frame_count = host_configured ? host.frame_count : NULL,
        .read_guest_word = host_configured ? host.read_word : NULL,
    };
    return &services;
}

static void lock_completed_proof(void) {
    while (atomic_flag_test_and_set_explicit(&completed_proof_guard,
                                             memory_order_acquire)) {}
}

static void unlock_completed_proof(void) {
    atomic_flag_clear_explicit(&completed_proof_guard, memory_order_release);
}

static void lock_presentation_lifecycle(void) {
    while (atomic_flag_test_and_set_explicit(&presentation_lifecycle_guard,
                                              memory_order_acquire)) {
    }
}

static void unlock_presentation_lifecycle(void) {
    atomic_flag_clear_explicit(&presentation_lifecycle_guard,
                               memory_order_release);
}

static void write_identity_u16(uint8_t *target, uint16_t value) {
    target[0] = (uint8_t)value;
    target[1] = (uint8_t)(value >> 8u);
}

static void write_identity_u32(uint8_t *target, uint32_t value) {
    for (uint32_t index = 0u; index < 4u; ++index)
        target[index] = (uint8_t)(value >> (index * 8u));
}

static uint64_t identity_prefix_bytes(const uint8_t *bytes) {
    uint64_t value = 0u;

    if (bytes == NULL) return 0u;
    for (uint32_t index = 0u; index < sizeof(value); ++index)
        value |= (uint64_t)bytes[index] << (index * 8u);
    return value;
}

/* Device work exists during boot and between overlays too. Artifact identity
 * is metadata here; authority for each optional authored capture is checked by
 * its producer, not fabricated from the module detector. Guest thread only. */
bool psx_xg_render_auth_describe_native_work(
        XgRenderSourceFrameDescription *description) {
    GpuDisplayInfo display;
    XgRenderRuntimeHostServices host = {0};
    const XgRenderAuthProfile profile =
        xg_render_static_auth_profile_from_metadata();
    uint32_t module = XG_SEMANTIC_MODULE_RESIDENT;

    if (description == NULL || !state.configured ||
        state.scene_generation == 0u || state.scene_generation > UINT32_MAX)
        return false;
    gpu_get_display_info(&display);
    if (display.width == 0u || display.width > 1024u ||
        display.height == 0u || display.height > 512u)
        return false;
    if (xg_render_runtime_host_services(&host) && host.semantic_module != NULL)
        (void)host.semantic_module(&module);
    if (module > XG_SEMANTIC_MODULE_MOVIE)
        module = XG_SEMANTIC_MODULE_RESIDENT;
    *description = (XgRenderSourceFrameDescription){
        .scene = {
            .disc_id = profile.disc_id,
            .executable_identity = identity_prefix_bytes(
                profile.static_game_identity.full_sha256.bytes),
            .primary_overlay_identity =
                state.authenticated_artifact_candidate_valid
                ? identity_prefix_bytes(
                    state.authenticated_artifact_candidate.artifact_sha256)
                : 0u,
            .module = (XgSemanticModuleKind)module,
        },
        .display = {
            .width = (uint16_t)display.width,
            .height = (uint16_t)display.height,
            .display_x = (uint16_t)display.display_x,
            .display_y = (uint16_t)display.display_y,
            .aspect_num = 4u, .aspect_den = 3u,
            .depth24 = display.depth24 != 0,
            .interlaced = display.interlaced != 0,
            .disabled = display.disabled != 0,
        },
        .scene_generation = (uint32_t)state.scene_generation,
        .source_interval_vblanks = 1u,
    };
    xg_render_runtime_composition_native_work_view(&description->display);
    if (state.movie_active_owner != GPU_MOVIE_OWNER_NONE) {
        /* An active movie selects canonical scanout, not a projection reset.
         * Retaining its last resource after STOP does not retain that policy. */
        description->display.aspect_num = 4u;
        description->display.aspect_den = 3u;
    }
    return true;
}

bool psx_xg_render_auth_accept_native_draw(const GpuRenderSemantic *semantic) {
    XgRenderSourceFrameDescription description;
    XgRenderSubmissionCommand command;
    if (!xg_render_native_work_enabled()) return true;
    if (semantic == NULL) return false;
    if (semantic->submission_command_id <= UINT32_C(0x001ffffc) &&
        psx_xg_render_auth_describe_native_work(&description) &&
        xg_render_submission_resolve_command(&description,
            (uint32_t)semantic->submission_command_id, semantic, &command))
        semantic = &command.semantic;
    /* Submission retains packet layout. Override it only for an authenticated
     * mask, after that merge; VIEW expands it without changing canonical XY. */
    if (xg_render_runtime_composition_is_transition_mask(semantic)) {
        if (semantic != &command.semantic) command.semantic = *semantic;
        command.semantic.screen_space_2d = GPU_RENDER_SCREEN_SPACE_2D_STRETCH;
        semantic = &command.semantic;
    }
    return xg_render_native_work_draw(semantic, psx_get_cycle_count());
}

void psx_xg_render_auth_set_native_work_mode(bool enabled) {
    xg_render_runtime_composition_set_native_work_mode(enabled);
}

static bool standalone_artifact_semantic_module(
        const PsxXgRenderAuthCandidate *candidate,
        XgSemanticModuleKind *out_module) {
    const bool field =
        xg_render_runtime_variant_artifact_candidate_matches(candidate);
    const bool movie = movie_artifact_candidate_matches(candidate);

    if (out_module == NULL || field == movie) return false;
    *out_module = field ? XG_SEMANTIC_MODULE_FIELD :
        XG_SEMANTIC_MODULE_MOVIE;
    return true;
}

static bool standalone_semantic_module(
        const PsxXgRenderAuthCandidate *candidate,
        XgSemanticModuleKind *out_module) {
    XgRenderRuntimeHostServices host = {0};
    uint32_t module = 0u;

    if (out_module == NULL) return false;
    if (state.movie_active_owner == GPU_MOVIE_OWNER_STANDALONE) {
        *out_module = XG_SEMANTIC_MODULE_MOVIE;
        return true;
    }
    if (xg_render_runtime_host_services(&host) &&
        host.semantic_module != NULL && host.semantic_module(&module) &&
        module <= XG_SEMANTIC_MODULE_MOVIE) {
        *out_module = (XgSemanticModuleKind)module;
        return true;
    }
    return standalone_artifact_semantic_module(candidate, out_module);
}

static bool standalone_artifact_frame_authorized(
        const XgRenderSourceFrameDescription *frame) {
    const PsxXgRenderAuthCandidate *candidate =
        &state.authenticated_artifact_candidate;
    const XgRenderAuthProfile profile =
        xg_render_static_auth_profile_from_metadata();
    XgSemanticModuleKind module;

    return frame != NULL && state.requested_render_mode ==
            GUEST_RENDER_RENDER_NATIVE &&
        state.authenticated_artifact_candidate_valid &&
        state.authenticated_artifact_generation != 0u &&
        state.authenticated_artifact_scene_generation ==
            state.scene_generation &&
        state.authenticated_artifact_provenance.capability != 0u &&
        current_artifact_is_authorized() &&
        standalone_semantic_module(candidate, &module) &&
        frame->scene_generation == state.scene_generation &&
        frame->scene.disc_id == profile.disc_id &&
        frame->scene.executable_identity ==
            identity_prefix_bytes(candidate->identity.game_sha256) &&
        frame->scene.primary_overlay_identity ==
            identity_prefix_bytes(candidate->artifact_sha256) &&
        frame->scene.module == module;
}

static bool composition_prepare_source_target_internal(
        const XgRenderSourceFrameDescription *frame,
        XgSemanticResourceRef *out_target,
        XgRenderSurfaceGraphPublicationRollback **out_rollback) {
    XgRenderSurfacePublicationDescription description = {0};
    XgRenderSurfacePublication publication;
    uint8_t *pixels = NULL;
    uint64_t resource_id;
    uint32_t vram_width;
    size_t pixel_size;
    size_t byte_count;
    const bool movie_frame_authorized = frame != NULL &&
        (frame->scene.module == XG_SEMANTIC_MODULE_MOVIE ||
         frame->scene.module == XG_SEMANTIC_MODULE_FIELD) &&
        state.movie_frame_artifact_candidate_valid &&
        movie_artifact_candidate_matches(
            &state.movie_frame_artifact_candidate) &&
        state.movie_frame_artifact_provenance.capability != 0u;
    const bool standalone_frame_authorized =
        standalone_artifact_frame_authorized(frame);
    const XgRenderResourceProvenance artifact_provenance =
        movie_frame_authorized ? state.movie_frame_artifact_provenance :
            state.authenticated_artifact_provenance;
    bool success = false;

    if (frame == NULL || out_target == NULL ||
        state.requested_render_mode != GUEST_RENDER_RENDER_NATIVE ||
        ((!state.active || state.completed ||
          !current_artifact_is_authorized()) && !movie_frame_authorized &&
         !standalone_frame_authorized) ||
        frame->scene_generation != state.scene_generation ||
        frame->display.disabled || frame->display.width == 0u ||
        frame->display.height == 0u ||
        frame->display.width > 1024u || frame->display.height > 512u ||
        frame->display.display_y > 512u - frame->display.height)
        return false;
    vram_width = frame->display.depth24
        ? (frame->display.width * 3u + 1u) / 2u : frame->display.width;
    if (vram_width > 1024u || frame->display.display_x > 1024u - vram_width)
        return false;

    description.identity.bytes[8] = 'R';
    description.identity.bytes[9] = 'T';
    description.identity.bytes[10] = 'G';
    description.identity.bytes[11] = '1';
    write_identity_u32(description.identity.bytes + 12u,
                       frame->scene_generation);
    write_identity_u16(description.identity.bytes + 16u,
                       frame->display.width);
    write_identity_u16(description.identity.bytes + 18u,
                       frame->display.height);
    write_identity_u16(description.identity.bytes + 20u,
                       frame->display.display_x);
    write_identity_u16(description.identity.bytes + 22u,
                       frame->display.display_y);
    write_identity_u32(description.identity.bytes + 24u,
                       frame->scene.disc_id);
    write_identity_u32(description.identity.bytes + 28u,
                       (uint32_t)frame->scene.module |
                       (frame->display.depth24 ? UINT32_C(0x100) : 0u) |
                       (frame->display.interlaced ? UINT32_C(0x200) : 0u));
    resource_id = xg_render_resource_digest(
        description.identity.bytes + sizeof(resource_id),
        sizeof(description.identity.bytes) - sizeof(resource_id));
    if (resource_id == 0u) return false;
    for (uint32_t index = 0u; index < sizeof(resource_id); ++index)
        description.identity.bytes[index] =
            (uint8_t)(resource_id >> (index * 8u));

    pixel_size = frame->display.depth24 ? 3u : 2u;
    byte_count = (size_t)frame->display.width * frame->display.height *
        pixel_size;
    pixels = (uint8_t *)calloc(byte_count, 1u);
    if (pixels == NULL) return false;
    description.kind = XG_RENDER_RESOURCE_GENERATED_SURFACE;
    description.format = frame->display.depth24
        ? XG_RENDER_SURFACE_DEPTH24 : XG_RENDER_SURFACE_VRAM16;
    description.provenance = artifact_provenance;
    description.owner_generation = frame->scene_generation;
    description.width = frame->display.width;
    description.height = frame->display.height;
    description.descriptor = (XgRenderResourceDescriptor){
        .version = XG_RENDER_RESOURCE_DESCRIPTOR_VERSION,
        .pixel_format = frame->display.depth24
            ? XG_RENDER_RESOURCE_PIXEL_FORMAT_RGB24
            : XG_RENDER_RESOURCE_PIXEL_FORMAT_RGB555,
        .width = frame->display.width,
        .height = frame->display.height,
        .row_pitch = (uint32_t)((size_t)frame->display.width * pixel_size),
        .vram_x = frame->display.display_x,
        .vram_y = frame->display.display_y,
        .vram_width = vram_width,
        .vram_height = frame->display.height,
        .flags = XG_RENDER_RESOURCE_DESCRIPTOR_HAS_VRAM_REGION,
    };
    description.bytes = pixels;
    description.byte_count = byte_count;

    lock_presentation_lifecycle();
    if (((state.active && !state.completed &&
          current_artifact_is_authorized()) || movie_frame_authorized ||
         standalone_artifact_frame_authorized(frame)) &&
        frame->scene_generation == state.scene_generation &&
        (out_rollback != NULL
            ? xg_render_surface_graph_publish_reversible(
                  &description, &publication, out_rollback)
            : xg_render_surface_graph_publish(
                  &description, &publication)) == XG_RENDER_SURFACE_GRAPH_OK) {
        *out_target = (XgSemanticResourceRef){
            .resource_id = publication.handle.resource_id,
            .generation = publication.handle.generation,
            .content_digest = publication.content_digest,
        };
        success = true;
    }
    unlock_presentation_lifecycle();
    free(pixels);
    return success;
}

static bool composition_prepare_source_target(
        const XgRenderSourceFrameDescription *frame,
        XgSemanticResourceRef *out_target) {
    return composition_prepare_source_target_internal(frame, out_target, NULL);
}

static bool composition_begin_movie_source_frame(
        const XgRenderSourceFrameDescription *description,
        XgSemanticResourceRef *out_target,
        XgRenderSurfaceGraphPublicationRollback **out_rollback) {
    XgSemanticPassRecord pass;
    XgRenderSourceFrameFragment fragment;

    if (description == NULL || out_target == NULL || out_rollback == NULL)
        return false;
    *out_rollback = NULL;
    if (!composition_prepare_source_target_internal(
            description, out_target, out_rollback))
        return false;
    pass = (XgSemanticPassRecord){
        .pass_id = 0u,
        .target_surface_id = out_target->resource_id,
        .target_generation = out_target->generation,
        .viewport_width = description->display.width,
        .viewport_height = description->display.height,
        .load_operation = description->scene.module == XG_SEMANTIC_MODULE_FIELD
            ? XG_SEMANTIC_PASS_LOAD : XG_SEMANTIC_PASS_CLEAR,
        .store = true,
        .presentation_output = true,
    };
    fragment = (XgRenderSourceFrameFragment){
        .passes = &pass,
        .resources = out_target,
        .pass_count = 1u,
        .resource_count = 1u,
    };
    return xg_render_fragment_runtime_ingress_production_fragment(
        description, &fragment) == XG_RENDER_FRAGMENT_RUNTIME_OK;
}

static bool composition_retained_movie_surface(
        XgSemanticResourceRef *out_surface) {
    XgRenderResourceView view = {0};

    if (out_surface == NULL || retained_movie_surface.resource_id == 0u ||
        xg_render_resource_view((XgRenderResourceHandle){
            retained_movie_surface.resource_id,
            retained_movie_surface.generation,
        }, &view) != XG_RENDER_RESOURCE_OK ||
        view.kind != XG_RENDER_RESOURCE_MOVIE_FRAME ||
        view.content_digest != retained_movie_surface.content_digest ||
        (!view.current && view.retain_count == 0u))
        return false;
    *out_surface = retained_movie_surface;
    return true;
}

static bool completed_proof_matches_tier(XgRenderAuthTier tier) {
    bool matches;

    lock_completed_proof();
    matches = completed_proof.available && !completed_proof.blocked &&
              completed_proof_scene_generation == state.scene_generation &&
              completed_proof.tier == tier;
    unlock_completed_proof();
    return matches;
}

static bool is_control_transfer(uint32_t instruction) {
    const uint32_t opcode = instruction >> 26u;
    const uint32_t function = instruction & 0x3fu;

    return opcode == 1u || opcode == 2u || opcode == 3u ||
           (opcode >= 4u && opcode <= 7u) ||
           (opcode >= 0x14u && opcode <= 0x17u) ||
           (opcode == 0u && (function == 8u || function == 9u));
}

static XgRenderAuthDigest codegen_identity(void) {
    XgRenderAuthDigest digest = { { 0 } };
    const uint32_t values[] = {
        PSX_OVERLAY_ABI_TAG,
        PSX_OVERLAY_CODEGEN_VER,
        PSX_OVERLAY_CODEGEN_HASH,
    };

    memcpy(digest.bytes, values, sizeof(values));
    return digest;
}

static bool physical_address_equals(uint32_t left, uint32_t right) {
    return (left & 0x1fffffffu) == (right & 0x1fffffffu);
}

static uint32_t guest_address(uint32_t pc) {
    return pc == 0u ? 0u : (pc & UINT32_C(0x1fffffff)) |
        UINT32_C(0x80000000);
}

static bool range_contains(uint32_t range_start, uint32_t range_size,
                           uint32_t value, uint32_t value_size) {
    const uint64_t start = range_start & 0x1fffffffu;
    const uint64_t end = start + range_size;
    const uint64_t physical_value = value & 0x1fffffffu;

    return range_size != 0u && value_size != 0u &&
           physical_value >= start && physical_value + value_size <= end;
}

static bool normalized_ranges_overlap(uint32_t left_start, uint32_t left_size,
                                      uint32_t right_start, uint32_t right_size) {
    const uint64_t left_begin = left_start & 0x1fffffffu;
    const uint64_t left_end = left_begin + left_size;
    const uint64_t right_begin = right_start & 0x1fffffffu;
    const uint64_t right_end = right_begin + right_size;

    return left_size != 0u && right_size != 0u &&
           left_begin < right_end && right_begin < left_end;
}

static bool field_range_contains(uint32_t pc) {
    return range_contains(xg_render_manifest_validation.field_range_start,
                          xg_render_manifest_validation.field_range_size,
                          pc, 1u);
}

static bool field_range_is_bound(void) {
    const XgRenderManifestValidation *validation =
        &xg_render_manifest_validation;

    return range_contains(validation->field_range_start,
                          validation->field_range_size,
                          validation->producer_entry, 4u) &&
           range_contains(validation->field_range_start,
                          validation->field_range_size,
                          validation->instruction_window_start,
                          validation->instruction_window_size);
}

static bool authentication_range_write_overlaps(uint32_t write_address,
                                                uint32_t write_size) {
    const XgRenderManifestValidation *validation =
        &xg_render_manifest_validation;

    return normalized_ranges_overlap(validation->producer_entry, 4u,
                                     write_address, write_size) ||
           normalized_ranges_overlap(validation->instruction_window_start,
                                     validation->instruction_window_size,
                                     write_address, write_size);
}

static bool candidate_matches_manifest(
    const PsxXgRenderAuthCandidate *candidate) {
    const XgRenderManifestValidation *validation =
        &xg_render_manifest_validation;

    return candidate != NULL &&
           candidate->authority_provenance && candidate->pair_bound &&
           candidate->pair_id != 0u &&
           memcmp(candidate->identity.game_sha256, xg_render_game_identity,
                  sizeof(candidate->identity.game_sha256)) == 0 &&
           memcmp(candidate->identity.manifest_sha256,
                  xg_render_manifest_identity,
                  sizeof(candidate->identity.manifest_sha256)) == 0 &&
           physical_address_equals(candidate->artifact_base,
                                    validation->field_range_start) &&
           candidate->artifact_size == validation->field_range_size &&
           physical_address_equals(candidate->producer_entry,
                                   validation->producer_entry) &&
           physical_address_equals(candidate->dispatch_pc,
                                   validation->producer_entry) &&
           field_range_is_bound() &&
           range_contains(validation->field_range_start,
                          validation->field_range_size,
                          candidate->range_start, candidate->range_size) &&
           range_contains(candidate->range_start, candidate->range_size,
                          validation->producer_entry, 4u) &&
           range_contains(candidate->range_start, candidate->range_size,
                          validation->instruction_window_start,
                          validation->instruction_window_size);
}

static void clear_pending_candidate(void) {
    /* Only candidate publication writes a nonzero value and sets valid. Once
     * cleared, repeated code-write invalidations have no storage left to clear. */
    if(!state.pending_candidate_valid&&!state.pending_scene_generation)return;
    memset(&state.pending_candidate, 0, sizeof(state.pending_candidate));
    state.pending_candidate_valid = false;
    state.pending_scene_generation = 0u;
}

static void clear_authenticated_artifact_primary(void) {
    memset(&state.authenticated_artifact_candidate, 0,
           sizeof(state.authenticated_artifact_candidate));
    state.authenticated_artifact_candidate_valid = false;
    state.authenticated_artifact_primary_valid = false;
    state.authenticated_artifact_primary_index = 0u;
    state.authenticated_artifact_scene_generation = 0u;
    state.authenticated_artifact_provenance =
        (XgRenderResourceProvenance){0};
}

static void select_authenticated_artifact_primary(uint32_t preferred_index) {
    const XgRenderAuthenticatedArtifact *record = NULL;
    uint32_t selected = 0u;

    if (preferred_index < XG_RENDER_AUTH_ARTIFACT_CAPACITY &&
        state.authenticated_artifacts[preferred_index].occupied) {
        selected = preferred_index;
        record = &state.authenticated_artifacts[selected];
    } else {
        for (uint32_t index = 0u;
             index < XG_RENDER_AUTH_ARTIFACT_CAPACITY; ++index) {
            const XgRenderAuthenticatedArtifact *candidate =
                &state.authenticated_artifacts[index];

            if (candidate->occupied && !candidate->static_text &&
                (record == NULL || candidate->generation > record->generation)) {
                selected = index;
                record = candidate;
            }
        }
    }
    if (record == NULL) {
        clear_authenticated_artifact_primary();
        return;
    }
    state.authenticated_artifact_candidate = record->candidate;
    state.authenticated_artifact_candidate_valid = true;
    state.authenticated_artifact_primary_valid = true;
    state.authenticated_artifact_primary_index = selected;
    state.authenticated_artifact_scene_generation = record->scene_generation;
    state.authenticated_artifact_provenance = record->provenance;
}

static void remove_authenticated_artifact(uint32_t index, bool retire) {
    XgRenderAuthenticatedArtifact *record;
    XgRenderResourceProvenance provenance;
    bool removed_primary;

    if (index >= XG_RENDER_AUTH_ARTIFACT_CAPACITY) return;
    record = &state.authenticated_artifacts[index];
    if (!record->occupied) return;
    provenance = record->provenance;
    removed_primary = state.authenticated_artifact_primary_valid &&
        state.authenticated_artifact_primary_index == index;
    if (state.movie_frame_artifact_candidate_valid &&
        artifact_binary_identity_matches(
            &record->candidate, &state.movie_frame_artifact_candidate))
        clear_movie_frame_artifact_candidate();

    /* Authority must disappear from lookup before its capability is retired. */
    memset(record, 0, sizeof(*record));
    if (state.authenticated_artifact_count != 0u)
        --state.authenticated_artifact_count;
    if (removed_primary)
        select_authenticated_artifact_primary(
            XG_RENDER_AUTH_ARTIFACT_CAPACITY);
    if (provenance.capability != 0u) {
        if (retire)
            (void)xg_render_resource_capability_retire(provenance);
        else
            (void)xg_render_resource_capability_revoke(provenance);
    }
}

static void clear_authenticated_artifact_candidate(void) {
    clear_authenticated_artifact_primary();
    for (uint32_t index = 0u;
         index < XG_RENDER_AUTH_ARTIFACT_CAPACITY; ++index)
        remove_authenticated_artifact(index, false);
}

static void clear_movie_frame_artifact_candidate(void) {
    memset(&state.movie_frame_artifact_candidate, 0,
           sizeof(state.movie_frame_artifact_candidate));
    state.movie_frame_artifact_candidate_valid = false;
    state.movie_frame_artifact_provenance =
        (XgRenderResourceProvenance){0};
}

static void clear_candidate_outcome(void) {
    state.candidate_matched = false;
    state.candidate_dispatched = false;
}

static void clear_pending_variant_sequence(void) {
    state.pending_variant_entry = 0u;
    state.pending_variant_tier = XG_RENDER_AUTH_TIER_STATIC;
    state.pending_variant_sequence = false;
    state.pending_variant_capture_ready = false;
}

static bool pending_candidate_matches(uint32_t pc) {
    return state.pending_candidate_valid &&
           state.pending_scene_generation == state.scene_generation &&
           physical_address_equals(pc,
                                   xg_render_manifest_validation.producer_entry) &&
           candidate_matches_manifest(&state.pending_candidate);
}

static bool pending_variant_candidate_matches(void) {
    return state.pending_candidate_valid &&
           state.pending_scene_generation == state.scene_generation &&
           xg_render_runtime_variant_candidate_matches(&state.pending_candidate);
}

static bool pending_candidate_matches_runtime_variant_artifact(void) {
    return state.pending_candidate_valid &&
           state.pending_scene_generation == state.scene_generation &&
           state.pending_candidate.runtime_variant_bound &&
           xg_render_runtime_variant_artifact_candidate_matches(
               &state.pending_candidate);
}

static bool pending_variant_artifact_candidate_matches(uint32_t pc) {
    if (xg_render_runtime_variant_no_gates_enabled()) return true;
    return artifact_authority_for_pc(pc, NULL);
}

static bool authenticated_variant_hook_matches(uint32_t pc) {
    return pending_variant_artifact_candidate_matches(pc);
}

static bool current_artifact_is_authorized(void) {
    const XgRenderAuthenticatedArtifact *record;

    if (xg_render_runtime_variant_no_gates_enabled()) return true;
    if (!state.authenticated_artifact_primary_valid ||
        state.authenticated_artifact_primary_index >=
            XG_RENDER_AUTH_ARTIFACT_CAPACITY)
        return false;
    record = &state.authenticated_artifacts[
        state.authenticated_artifact_primary_index];
    return record->occupied && record->generation != 0u &&
        record->scene_generation == state.scene_generation &&
        record->provenance.capability != 0u &&
        (xg_render_runtime_variant_artifact_candidate_matches(
             &record->candidate) ||
         xg_render_authoritative_overlay_artifact_candidate_matches(
              &record->candidate) ||
          movie_artifact_candidate_matches(
              &record->candidate));
}

static bool current_artifact_code_range_overlaps(uint32_t address,
                                                   uint32_t size) {
    for (uint32_t index = 0u;
         index < XG_RENDER_AUTH_ARTIFACT_CAPACITY; ++index) {
        const XgRenderAuthenticatedArtifact *record =
            &state.authenticated_artifacts[index];

        if (record->occupied && record->generation != 0u &&
            record->scene_generation == state.scene_generation &&
            normalized_ranges_overlap(
                record->candidate.range_start, record->candidate.range_size,
                address, size))
            return true;
    }
    return false;
}

static bool current_artifact_range_contains_pc(uint32_t pc) {
    return artifact_authority_for_pc(pc, NULL);
}

static bool artifact_authority_for_pc(
        uint32_t pc, XgRenderArtifactAuthority *out_authority) {
    const XgRenderAuthenticatedArtifact *selected = NULL;
    XgRenderRuntimeHostServices host = {0};
    const bool host_configured = xg_render_runtime_host_services(&host);

    if (out_authority != NULL)
        *out_authority = (XgRenderArtifactAuthority){0};
    for (uint32_t index = 0u;
         index < XG_RENDER_AUTH_ARTIFACT_CAPACITY; ++index) {
        const XgRenderAuthenticatedArtifact *record =
            &state.authenticated_artifacts[index];
        const PsxXgRenderAuthCandidate *candidate = &record->candidate;
        /* These necessary conditions precede expensive descriptor validation;
         * empty/stale slots cannot confer authority for any PC. */
        if(!record->occupied||!record->generation||
            record->scene_generation!=state.scene_generation||!record->provenance.capability)
            continue;
        const bool static_authorizes_pc = record->static_text &&
            host_configured && host.native_text_authorizes_pc != NULL &&
            range_contains(candidate->range_start, candidate->range_size,
                           pc, 4u) &&
            host.native_text_authorizes_pc(pc);
        const bool dynamic_authorizes_pc = !record->static_text &&
            (xg_render_runtime_variant_artifact_candidate_authorizes_pc(
                 candidate, pc) ||
             xg_render_authoritative_overlay_artifact_candidate_authorizes_pc(
                 candidate, pc) ||
             (movie_artifact_candidate_matches(candidate) &&
              range_contains(candidate->range_start, candidate->range_size,
                             pc, 4u)));
        const bool authorizes_pc = record->occupied &&
            record->generation != 0u &&
            record->scene_generation == state.scene_generation &&
            record->provenance.capability != 0u &&
            (static_authorizes_pc || dynamic_authorizes_pc);

        if (authorizes_pc &&
            (selected == NULL || record->generation > selected->generation))
            selected = record;
    }
    if (selected == NULL) return false;
    if (out_authority != NULL)
        *out_authority = (XgRenderArtifactAuthority){
            .generation = selected->generation,
            .provenance = selected->provenance,
        };
    return true;
}

static bool static_artifact_authority_for_cutover(
        uint32_t pc, uint32_t instruction_word,
        XgRenderArtifactAuthority *out_authority) {
    XgRenderRuntimeVariantCutover cutover;
    XgRenderRuntimeHostServices host = {0};
    XgRenderResourceCapabilityMetadata metadata;
    XgRenderResourceProvenance provenance = {0};
    bool identity_bound = false;
    bool identity_gate_passed = false;
    uint32_t free_index = XG_RENDER_AUTH_ARTIFACT_CAPACITY;

    if (out_authority != NULL)
        *out_authority = (XgRenderArtifactAuthority){0};
    atomic_fetch_add_explicit(
        &static_artifact_attempts, 1u, memory_order_relaxed);
    atomic_store_explicit(&static_artifact_last_pc, pc, memory_order_relaxed);
    if (xg_render_runtime_variant_no_gates_enabled()) {
        atomic_store_explicit(
            &static_artifact_last_blocker, 1u, memory_order_relaxed);
        return false;
    }
    if (!xg_render_runtime_variant_native_cutover_contract_lookup(
            pc, instruction_word, &cutover)) {
        atomic_store_explicit(
            &static_artifact_last_blocker, 2u, memory_order_relaxed);
        return false;
    }
    if (cutover.code_range_size == 0u) {
        atomic_store_explicit(
            &static_artifact_last_blocker, 3u, memory_order_relaxed);
        return false;
    }
    if (!xg_render_runtime_host_services(&host) ||
        host.native_text_authorizes_pc == NULL) {
        atomic_store_explicit(
            &static_artifact_last_blocker, 4u, memory_order_relaxed);
        return false;
    }
    if (!host.native_text_authorizes_pc(pc)) {
        atomic_store_explicit(
            &static_artifact_last_blocker, 5u, memory_order_relaxed);
        return false;
    }
    if (!xg_render_static_auth_metadata_is_valid()) {
        atomic_store_explicit(
            &static_artifact_last_blocker, 6u, memory_order_relaxed);
        return false;
    }
    if (!xg_render_static_auth_bind_identity(
            &identity_bound, &identity_gate_passed) ||
        !identity_bound || !identity_gate_passed) {
        atomic_store_explicit(
            &static_artifact_last_blocker, 7u, memory_order_relaxed);
        return false;
    }
    for (uint32_t index = 0u;
         index < XG_RENDER_AUTH_ARTIFACT_CAPACITY; ++index) {
        const XgRenderAuthenticatedArtifact *record =
            &state.authenticated_artifacts[index];

        if (record->occupied && record->static_text &&
            record->scene_generation == state.scene_generation &&
            physical_address_equals(record->candidate.range_start,
                                    cutover.code_range_start) &&
            record->candidate.range_size == cutover.code_range_size &&
            memcmp(record->candidate.artifact_sha256,
                   cutover.code_range_identity,
                   sizeof(record->candidate.artifact_sha256)) == 0) {
            if (out_authority != NULL)
                *out_authority = (XgRenderArtifactAuthority){
                    .generation = record->generation,
                    .provenance = record->provenance,
                };
            atomic_fetch_add_explicit(
                &static_artifact_successes, 1u, memory_order_relaxed);
            atomic_store_explicit(
                &static_artifact_last_blocker, 0u, memory_order_relaxed);
            return true;
        }
        if (!record->occupied &&
            free_index == XG_RENDER_AUTH_ARTIFACT_CAPACITY)
            free_index = index;
    }
    if (free_index == XG_RENDER_AUTH_ARTIFACT_CAPACITY) {
        atomic_store_explicit(
            &static_artifact_last_blocker, 8u, memory_order_relaxed);
        return false;
    }
    if (state.authenticated_artifact_generation == UINT64_MAX) {
        clear_authenticated_artifact_candidate();
        state.authenticated_producer_entry = 0u;
        state.authenticated_producer_scene_generation = 0u;
        atomic_store_explicit(
            &static_artifact_last_blocker, 9u, memory_order_relaxed);
        return false;
    }
    ++state.authenticated_artifact_generation;
    metadata = (XgRenderResourceCapabilityMetadata){
        .kind = XG_RENDER_RESOURCE_PROVENANCE_ARTIFACT,
        .receipt = state.authenticated_artifact_generation,
        .lifetime = XG_RENDER_RESOURCE_CAPABILITY_SCENE,
        .owner_kind = XG_RENDER_RESOURCE_OWNER_SCENE,
        .owner_generation = state.scene_generation,
        .artifact = {
            .base = cutover.code_range_start,
            .size = cutover.code_range_size,
        },
    };
    memcpy(metadata.artifact.sha256, cutover.code_range_identity,
           sizeof(metadata.artifact.sha256));
    if (xg_render_resource_capability_register(&metadata, &provenance) !=
            XG_RENDER_RESOURCE_CAPABILITY_OK) {
        atomic_store_explicit(
            &static_artifact_last_blocker, 10u, memory_order_relaxed);
        return false;
    }
    state.authenticated_artifacts[free_index] =
        (XgRenderAuthenticatedArtifact){
            .candidate = {
                .artifact_base = cutover.code_range_start,
                .artifact_size = cutover.code_range_size,
                .range_start = cutover.code_range_start,
                .range_size = cutover.code_range_size,
                .authority_provenance = true,
            },
            .provenance = provenance,
            .generation = state.authenticated_artifact_generation,
            .scene_generation = state.scene_generation,
            .static_text = true,
            .occupied = true,
        };
    memcpy(state.authenticated_artifacts[free_index].candidate.artifact_sha256,
           cutover.code_range_identity, sizeof(cutover.code_range_identity));
    ++state.authenticated_artifact_count;
    if (out_authority != NULL)
        *out_authority = (XgRenderArtifactAuthority){
            .generation = state.authenticated_artifact_generation,
            .provenance = provenance,
        };
    atomic_fetch_add_explicit(
        &static_artifact_successes, 1u, memory_order_relaxed);
    atomic_store_explicit(
        &static_artifact_last_blocker, 0u, memory_order_relaxed);
    return true;
}

static bool artifact_binary_identity_matches(
        const PsxXgRenderAuthCandidate *left,
        const PsxXgRenderAuthCandidate *right) {
    return left != NULL && right != NULL &&
        left->artifact_size == right->artifact_size &&
        physical_address_equals(left->artifact_base, right->artifact_base) &&
        memcmp(left->artifact_sha256, right->artifact_sha256,
               sizeof(left->artifact_sha256)) == 0 &&
        memcmp(&left->identity, &right->identity, sizeof(left->identity)) == 0;
}

static void broadcast_resource_overlap(void) {
    const XgRenderInvalidationEvent event = {
        .kind = XG_RENDER_INVALIDATION_RESOURCE_OVERLAP,
    };
    xg_render_runtime_composition_handle_invalidation(&event);
}

static uint64_t interpolation_scene_generation(void) {
    return state.interpolation_scene_generation != 0u
        ? state.interpolation_scene_generation : 1u;
}

static void advance_interpolation_scene(void) {
    if (state.interpolation_scene_generation == 0u)
        state.interpolation_scene_generation = 1u;
    else if (state.interpolation_scene_generation != UINT64_MAX)
        ++state.interpolation_scene_generation;
    gpu_native_interpolation_scene_boundary(interpolation_scene_generation());
}

static bool submission_active_auth_append(
        uint32_t packet_address, uint32_t source_primitive_index,
        uint32_t ot_bucket, uint8_t payload_word_count,
        const XgRenderIrNativePrimitive *primitive,
        bool force_pending_capture,
        uint32_t *out_failure_detail) {
    const XgRenderAuthResult result = state.auth != NULL
        ? xg_render_auth_append_native_insertion(
            state.auth, packet_address, source_primitive_index, ot_bucket,
            payload_word_count, primitive,
            force_pending_capture ||
                (state.pending_variant_sequence &&
                 !state.pending_variant_capture_ready))
        : XG_RENDER_AUTH_INVALID_TRANSITION;

    if (out_failure_detail != NULL)
        *out_failure_detail = (uint32_t)result;
    return result == XG_RENDER_AUTH_OK;
}

static bool submission_presentation_gate(void) {
    return state.presentation_gate != NULL &&
        state.presentation_gate(state.requested_render_mode,
                                &state.presentation,
                                state.presentation_user_data);
}

static GteAttributionExecutionTier gte_attribution_tier_for(
    XgRenderAuthTier tier) {
    return tier == XG_RENDER_AUTH_TIER_WARM_NATIVE
        ? GTE_ATTRIBUTION_TIER_WARM
        : GTE_ATTRIBUTION_TIER_COLD;
}

static void end_gte_attribution_producer(void) {
    if (!state.gte_attribution_producer_active) return;
    state.gte_attribution_producer_active = false;
    (void)gte_attribution_producer_end();
}

static bool begin_gte_attribution_producer(XgRenderAuthTier tier) {
    XgRenderAuthSnapshot snapshot = { 0 };
    GteAttributionProducerContext context = { 0 };

    end_gte_attribution_producer();
    if (state.auth == NULL ||
        xg_render_auth_snapshot(state.auth, &snapshot) != XG_RENDER_AUTH_OK ||
        snapshot.logical_identity.state_id.scene_epoch == 0u ||
        snapshot.logical_identity.producer_record_id == 0u)
        return false;
    context.visual_state_id.scene_epoch =
        snapshot.logical_identity.state_id.scene_epoch;
    context.visual_state_id.state_sequence =
        snapshot.logical_identity.state_id.state_sequence;
    context.producer_id = snapshot.logical_identity.producer_record_id;
    context.tier = gte_attribution_tier_for(tier);
    if (gte_attribution_producer_begin(&context) != GTE_ATTRIBUTION_OK)
        return false;
    state.gte_attribution_producer_active = true;
    return true;
}

static void disarm(void) {
    const XgRenderInvalidationEvent event = {
        .kind = XG_RENDER_INVALIDATION_AUTHORITY_LOST,
    };

    if (xg_render_runtime_variant_no_gates_enabled()) return;
    /* Native work observes the real guest producers; failure of the unused
     * whole-scene substitution protocol must not erase their source captures.
     * Code writes, loader mismatches and scene changes still invalidate inputs. */
    if (!xg_render_native_work_enabled())
        xg_render_runtime_composition_handle_invalidation(&event);
    end_gte_attribution_producer();
    state.active = false;
    state.armed = false;
    state.completed = false;
    clear_pending_candidate();
    clear_pending_variant_sequence();
}

static void retire_completed_auth_proof(void) {
    if (!state.completed) return;
    if (state.auth == NULL ||
        xg_render_auth_scene_reset(state.auth) != XG_RENDER_AUTH_OK) {
        disarm();
        return;
    }
    end_gte_attribution_producer();
    state.active = false;
    state.armed = true;
    state.completed = false;
    state.rejection = (PsxXgRenderAuthRejectionReceipt){ 0 };
    clear_pending_candidate();
    clear_pending_variant_sequence();
    clear_candidate_outcome();
    xg_render_runtime_variant_reset();
}

static void block_completed_proof(
    XgRenderAuthReason reason,
    const PsxXgRenderAuthRejectionReceipt *rejection) {
    PsxXgRenderAuthCompletedProofReceipt local;

    lock_completed_proof();
    local = completed_proof;
    if (local.available && !local.blocked) {
        local.blocked = true;
        local.blocker_reason = reason;
        local.blocker_rejection = *rejection;
        completed_proof = local;
    }
    unlock_completed_proof();
}

static void latch_rejection(XgRenderAuthReason reason,
                            PsxXgRenderAuthRejectionSource source,
                            bool has_hook, PsxXgRenderAuthHook hook,
                            uint32_t pc) {
    PsxXgRenderAuthRejectionReceipt rejection;

    if (xg_render_runtime_variant_no_gates_enabled()) return;
    if (!state.active ||
        state.rejection.source != PSX_XG_RENDER_AUTH_REJECTION_SOURCE_NONE)
        return;
    rejection = (PsxXgRenderAuthRejectionReceipt){
        source,
        hook,
        guest_address(pc),
        has_hook,
    };
    state.rejection = rejection;
    block_completed_proof(reason, &rejection);
}

static void abort_active(XgRenderAuthReason reason,
                         PsxXgRenderAuthRejectionSource source,
                         bool has_hook, PsxXgRenderAuthHook hook,
                         uint32_t pc) {
    if (xg_render_runtime_variant_no_gates_enabled()) return;
    latch_rejection(reason, source, has_hook, hook, pc);
    if (state.auth != NULL && state.active)
        (void)xg_render_auth_abort(state.auth, reason);
    else if (!xg_render_native_work_enabled())
        guest_render_bridge_abort_scene(GUEST_RENDER_FALLBACK_FORCED_ORIGINAL);
    disarm();
}

static XgRenderAuthExecution execution_for(XgRenderAuthTier tier,
                                            XgRenderAuthHook hook) {
    XgRenderAuthExecution execution = xg_render_static_auth_execution_for(hook);

    execution.tier = tier;
    execution.cache_identity.codegen_digest = codegen_identity();
    return execution;
}

static void publish_completed_proof(XgRenderAuthTier tier) {
    XgRenderAuthSnapshot snapshot = { 0 };
    PsxXgRenderAuthCompletedProofReceipt local = { 0 };

    if (state.auth == NULL ||
        xg_render_auth_snapshot(state.auth, &snapshot) != XG_RENDER_AUTH_OK ||
        snapshot.reject_reason != XG_RENDER_AUTH_REJECT_NONE ||
        snapshot.scene_aborted || !snapshot.ir_usable ||
        snapshot.hook_count != XG_RENDER_AUTH_HOOK_STAGE_COUNT ||
        snapshot.hook_sequence[0] != XG_RENDER_AUTH_HOOK_ENTRY ||
        snapshot.hook_sequence[1] != XG_RENDER_AUTH_HOOK_CAPTURE_SITE ||
        snapshot.hook_sequence[2] != XG_RENDER_AUTH_HOOK_RETURN ||
        snapshot.logical_identity.state_id.scene_epoch == 0u ||
        (tier != XG_RENDER_AUTH_TIER_COLD_INTERPRETER &&
         tier != XG_RENDER_AUTH_TIER_WARM_NATIVE) ||
        snapshot.next_trace_sequence < XG_RENDER_AUTH_HOOK_STAGE_COUNT + 1u ||
        (tier == XG_RENDER_AUTH_TIER_WARM_NATIVE &&
         (!state.candidate_matched || !state.candidate_dispatched)))
        return;

    local.available = true;
    local.producer_record_id =
        snapshot.logical_identity.producer_record_id;
    local.site_record_id = snapshot.logical_identity.site_record_id;
    local.tuple = (PsxXgRenderAuthCompletedProofTuple){
        snapshot.logical_identity.producer_entry,
        snapshot.logical_identity.capture_site,
        snapshot.logical_identity.static_callee,
        snapshot.logical_identity.return_site,
    };
    local.tier = tier;
    local.state_id = snapshot.logical_identity.state_id;
    local.entry_event_sequence =
        snapshot.next_trace_sequence - XG_RENDER_AUTH_HOOK_STAGE_COUNT;
    local.capture_event_sequence = local.entry_event_sequence + 1u;
    local.return_event_sequence = local.capture_event_sequence + 1u;
    local.candidate_matched = state.candidate_matched;
    local.candidate_dispatched = state.candidate_dispatched;

    lock_completed_proof();
    completed_proof = local;
    completed_proof_scene_generation = state.scene_generation;
    unlock_completed_proof();
    xg_render_instrumentation_record_completed_proof();
}

static void begin_scene(XgRenderAuthTier tier, uint32_t producer_entry) {
    const GuestRenderSceneConfig config = {
        state.requested_render_mode,
    };
    GuestRenderVisualStateId state_id = { 0 };
    XgRenderAuthProfile profile;

    if (producer_entry != xg_render_manifest_validation.producer_entry)
        return;
    (void)xg_render_runtime_composition_configure(composition_services());
    xg_render_runtime_composition_prepare_authenticated_scene();
    if (state.active && state.auth != NULL)
        (void)xg_render_auth_scene_reset(state.auth);
    end_gte_attribution_producer();
    state.active = false;
    state.armed = true;
    state.completed = false;
    state.rejection = (PsxXgRenderAuthRejectionReceipt){ 0 };
    if (xg_render_auth_process_owner(&state.auth) != XG_RENDER_AUTH_OK ||
        !xg_render_static_auth_metadata_is_valid() ||
        !xg_render_static_auth_bind_identity(&(bool){ false }, &(bool){ false })) {
        abort_active(XG_RENDER_AUTH_REJECT_IDENTITY_MISMATCH,
                     PSX_XG_RENDER_AUTH_REJECTION_SOURCE_NONE, false,
                     PSX_XG_RENDER_AUTH_HOOK_ENTRY, 0u);
        return;
    }
    profile = xg_render_static_auth_profile_from_metadata();
    profile.cache_identity.codegen_digest = codegen_identity();
    if (guest_render_bridge_begin_scene(&config) != GUEST_RENDER_OK) {
        abort_active(XG_RENDER_AUTH_REJECT_TRANSACTION_FAILURE,
                     PSX_XG_RENDER_AUTH_REJECTION_SOURCE_NONE, false,
                     PSX_XG_RENDER_AUTH_HOOK_ENTRY, 0u);
        return;
    }
    guest_render_native_stream_set_enabled(
        state.requested_render_mode == GUEST_RENDER_RENDER_NATIVE);
    if (state.presentation_gate != NULL) {
        if (!state.presentation_gate(state.requested_render_mode,
                                     &state.presentation,
                                     state.presentation_user_data))
            guest_render_bridge_force_original(
                GUEST_RENDER_FALLBACK_PRESENTATION_GATE);
    } else if (state.requested_render_mode !=
               GUEST_RENDER_RENDER_ORIGINAL) {
        memset(&state.presentation, 0, sizeof(state.presentation));
        state.presentation.reason =
            NATIVE_RENDER_PRESENTATION_GATE_OPENGL_REQUIRED;
        guest_render_bridge_force_original(
            GUEST_RENDER_FALLBACK_PRESENTATION_GATE);
    }
    if (guest_render_bridge_begin_state(&state_id) != GUEST_RENDER_OK ||
        xg_render_auth_scene_begin(state.auth, state_id, &profile) != XG_RENDER_AUTH_OK) {
        abort_active(XG_RENDER_AUTH_REJECT_TRANSACTION_FAILURE,
                     PSX_XG_RENDER_AUTH_REJECTION_SOURCE_NONE, false,
                     PSX_XG_RENDER_AUTH_HOOK_ENTRY, 0u);
        return;
    }
    state.active = true;
    (void)tier;
}

static void observe_capture(XgRenderAuthTier tier, uint32_t pc,
                            uint32_t instruction_word, uint32_t delay_slot_word,
                            uint32_t return_address) {
    XgRenderAuthDecision decision = { 0 };
    XgRenderAuthExecution execution;

    if (!state.active) return;
    if (!physical_address_equals(return_address, pc + 8u)) {
        abort_active(XG_RENDER_AUTH_REJECT_VALIDATION_MISMATCH,
                     PSX_XG_RENDER_AUTH_REJECTION_SOURCE_RUNTIME_HOOK, true,
                     PSX_XG_RENDER_AUTH_HOOK_CAPTURE, pc);
        return;
    }
    execution = execution_for(tier, XG_RENDER_AUTH_HOOK_CAPTURE_SITE);
    execution.validation.caller_site = pc;
    execution.validation.callee_entry =
        (pc & 0xf0000000u) | ((instruction_word & 0x03ffffffu) << 2u);
    execution.validation.return_site = pc + 8u;
    execution.validation.required_jal_opcode = instruction_word >> 26u;
    execution.validation.jal_target = execution.validation.callee_entry;
    execution.validation.delay_slot_complete = true;
    execution.validation.delay_slot_is_control_transfer =
        is_control_transfer(delay_slot_word);
    if (xg_render_auth_observe_hook(state.auth, &execution, &decision) !=
        XG_RENDER_AUTH_OK) {
        latch_rejection(decision.reject_reason,
                        PSX_XG_RENDER_AUTH_REJECTION_SOURCE_RUNTIME_HOOK,
                        true, PSX_XG_RENDER_AUTH_HOOK_CAPTURE, pc);
        disarm();
    }
}

static void observe_return(XgRenderAuthTier tier, uint32_t return_site,
                           uint32_t return_address) {
    XgRenderAuthDecision decision = { 0 };
    XgRenderAuthExecution execution;

    if (!state.active) return;
    if (!physical_address_equals(return_address, return_site)) {
        abort_active(XG_RENDER_AUTH_REJECT_VALIDATION_MISMATCH,
                     PSX_XG_RENDER_AUTH_REJECTION_SOURCE_RUNTIME_HOOK, true,
                     PSX_XG_RENDER_AUTH_HOOK_RETURN, return_site);
        return;
    }
    execution = execution_for(tier, XG_RENDER_AUTH_HOOK_RETURN);
    execution.validation.return_site = return_site;
    if (xg_render_auth_observe_hook(state.auth, &execution, &decision) !=
        XG_RENDER_AUTH_OK) {
        latch_rejection(decision.reject_reason,
                        PSX_XG_RENDER_AUTH_REJECTION_SOURCE_RUNTIME_HOOK,
                     true, PSX_XG_RENDER_AUTH_HOOK_RETURN, return_site);
        disarm();
    } else if (state.requested_render_mode == GUEST_RENDER_RENDER_NATIVE &&
               !xg_render_runtime_composition_flush_authenticated_ir()) {
        abort_active(XG_RENDER_AUTH_REJECT_TRANSACTION_FAILURE,
                     PSX_XG_RENDER_AUTH_REJECTION_SOURCE_RUNTIME_HOOK, true,
                     PSX_XG_RENDER_AUTH_HOOK_RETURN, return_site);
    } else {
        publish_completed_proof(tier);
        state.completed = true;
        end_gte_attribution_producer();
    }
}

static void observe_entry(XgRenderAuthTier tier, uint32_t pc, bool warm,
                          bool candidate_matched) {
    clear_pending_candidate();
    clear_candidate_outcome();
    begin_scene(tier, xg_render_manifest_validation.producer_entry);
    if (warm && !candidate_matched) {
        abort_active(XG_RENDER_AUTH_REJECT_VALIDATION_MISMATCH,
                     PSX_XG_RENDER_AUTH_REJECTION_SOURCE_VARIANT_HOOK,
                     true, PSX_XG_RENDER_AUTH_HOOK_ENTRY, pc);
        if (!xg_render_runtime_variant_no_gates_enabled()) return;
    }
    if (warm && candidate_matched) state.candidate_matched = true;
    if (state.active) {
        XgRenderAuthDecision decision = { 0 };
        XgRenderAuthExecution execution =
            execution_for(tier, XG_RENDER_AUTH_HOOK_ENTRY);

        execution.observed_producer_entry =
            xg_render_manifest_validation.producer_entry;
        if (xg_render_auth_observe_hook(state.auth, &execution, &decision) !=
            XG_RENDER_AUTH_OK) {
            latch_rejection(decision.reject_reason,
                            PSX_XG_RENDER_AUTH_REJECTION_SOURCE_RUNTIME_HOOK,
                            true, PSX_XG_RENDER_AUTH_HOOK_ENTRY, pc);
            disarm();
        } else if (!begin_gte_attribution_producer(tier)) {
            abort_active(XG_RENDER_AUTH_REJECT_TRANSACTION_FAILURE,
                         PSX_XG_RENDER_AUTH_REJECTION_SOURCE_RUNTIME_HOOK,
                         true, PSX_XG_RENDER_AUTH_HOOK_ENTRY, pc);
        } else if (!xg_render_runtime_composition_flush_pre_scene()) {
            abort_active(XG_RENDER_AUTH_REJECT_TRANSACTION_FAILURE,
                         PSX_XG_RENDER_AUTH_REJECTION_SOURCE_RUNTIME_HOOK,
                         true, PSX_XG_RENDER_AUTH_HOOK_ENTRY, pc);
        } else if (warm && candidate_matched) {
            state.candidate_dispatched = true;
        }
    }
}

static bool active_scene_is_canonical_entry_only(void) {
    XgRenderAuthSnapshot snapshot = { 0 };

    return state.active && !state.completed && state.auth != NULL &&
           xg_render_auth_snapshot(state.auth, &snapshot) == XG_RENDER_AUTH_OK &&
           !snapshot.scene_aborted &&
           snapshot.reject_reason == XG_RENDER_AUTH_REJECT_NONE &&
           snapshot.producer_begin_count == 1u &&
           snapshot.native_item_count == 0u &&
           snapshot.hook_count == 1u &&
           snapshot.hook_sequence[0] == XG_RENDER_AUTH_HOOK_ENTRY;
}

static void observe_hook(CPUState *cpu, XgRenderAuthTier tier, uint32_t hook, uint32_t pc,
    uint32_t instruction_word, uint32_t delay_slot_word,
    uint32_t return_address) {
    if (xg_render_runtime_composition_observe_auth_hook(
            cpu, tier, hook, pc, instruction_word, delay_slot_word))
        return;
    const XgRenderRuntimeVariantEvent variant_event =
        xg_render_runtime_variant_observe(hook, pc, instruction_word,
                                           delay_slot_word, return_address,
                                           state.scene_generation);

    xg_render_instrumentation_record_variant_progress(
        variant_event, xg_render_runtime_variant_event_is_exact(variant_event, pc));

    if (variant_event == XG_RENDER_RUNTIME_VARIANT_CONSUMED)
        return;
    if (tier == XG_RENDER_AUTH_TIER_WARM_NATIVE &&
        state.pending_candidate_valid && !state.pending_variant_sequence &&
        variant_event != XG_RENDER_RUNTIME_VARIANT_ACTIVATED &&
        variant_event != XG_RENDER_RUNTIME_VARIANT_ENTRY &&
        (hook != PSX_XG_RENDER_AUTH_HOOK_ENTRY ||
         !physical_address_equals(pc,
                                  xg_render_manifest_validation.producer_entry)))
        clear_pending_candidate();
    if (variant_event == XG_RENDER_RUNTIME_VARIANT_ACTIVATED) {
        clear_pending_variant_sequence();
        if (!state.active) state.armed = true;
        if (!state.armed ||
            (state.active && !state.completed &&
             !active_scene_is_canonical_entry_only()))
            abort_active(XG_RENDER_AUTH_REJECT_VALIDATION_MISMATCH,
                         PSX_XG_RENDER_AUTH_REJECTION_SOURCE_VARIANT_HOOK,
                         true, (PsxXgRenderAuthHook)hook, pc);
        return;
    }
    if (variant_event == XG_RENDER_RUNTIME_VARIANT_REJECT) {
        abort_active(XG_RENDER_AUTH_REJECT_VALIDATION_MISMATCH,
                     PSX_XG_RENDER_AUTH_REJECTION_SOURCE_VARIANT_HOOK, true,
                     (PsxXgRenderAuthHook)hook, pc);
        if (!xg_render_runtime_variant_no_gates_enabled()) return;
    }
    if (variant_event == XG_RENDER_RUNTIME_VARIANT_ENTRY) {
        const bool warm = tier == XG_RENDER_AUTH_TIER_WARM_NATIVE;
        const bool candidate_matched = !warm ||
            pending_variant_candidate_matches();

        state.pending_variant_entry = pc;
        state.pending_variant_tier = tier;
        state.pending_variant_sequence = true;
        state.pending_variant_capture_ready = false;
        observe_entry(tier, pc, warm, candidate_matched);
        return;
    }
    if (variant_event == XG_RENDER_RUNTIME_VARIANT_CAPTURE) {
        if (!state.pending_variant_sequence ||
            state.pending_variant_tier != tier) {
            abort_active(XG_RENDER_AUTH_REJECT_VALIDATION_MISMATCH,
                         PSX_XG_RENDER_AUTH_REJECTION_SOURCE_VARIANT_HOOK,
                         true, (PsxXgRenderAuthHook)hook, pc);
            return;
        }
        state.pending_variant_capture_ready = true;
        observe_capture(tier, xg_render_manifest_validation.caller_site,
                        instruction_word, delay_slot_word,
                        xg_render_manifest_validation.return_site);
        return;
    }
    if (variant_event == XG_RENDER_RUNTIME_VARIANT_RETURN) {
        if (!state.pending_variant_sequence ||
            !state.pending_variant_capture_ready ||
            state.pending_variant_tier != tier) {
            abort_active(XG_RENDER_AUTH_REJECT_VALIDATION_MISMATCH,
                         PSX_XG_RENDER_AUTH_REJECTION_SOURCE_VARIANT_HOOK,
                         true, (PsxXgRenderAuthHook)hook, pc);
            return;
        }
        observe_return(tier, xg_render_manifest_validation.return_site,
                       xg_render_manifest_validation.return_site);
        if (state.completed) {
            state.authenticated_producer_entry = state.pending_variant_entry;
            state.authenticated_producer_scene_generation =
                state.scene_generation;
        }
        clear_pending_variant_sequence();
        xg_render_runtime_variant_reset();
        return;
    }
    if (hook == PSX_XG_RENDER_AUTH_HOOK_ENTRY) {
        const bool warm = tier == XG_RENDER_AUTH_TIER_WARM_NATIVE;
        const bool candidate_matched = warm && pending_candidate_matches(pc);

        if (!physical_address_equals(pc,
                                     xg_render_manifest_validation.producer_entry))
            return;
        if (warm && !candidate_matched &&
            pending_candidate_matches_runtime_variant_artifact())
            return;
        observe_entry(tier, pc, warm, candidate_matched);
    } else if (hook == PSX_XG_RENDER_AUTH_HOOK_CAPTURE) {
        if (!physical_address_equals(pc,
                                     xg_render_manifest_validation.caller_site))
            return;
        observe_capture(tier, xg_render_manifest_validation.caller_site,
                        instruction_word, delay_slot_word, return_address);
    } else if (hook == PSX_XG_RENDER_AUTH_HOOK_RETURN) {
        if (!physical_address_equals(pc,
                                     xg_render_manifest_validation.return_site))
            return;
        observe_return(tier, xg_render_manifest_validation.return_site,
                       return_address);
    } else if (hook == PSX_XG_RENDER_AUTH_HOOK_FOREIGN_INTERIOR &&
               field_range_contains(pc)) {
        abort_active(XG_RENDER_AUTH_REJECT_FOREIGN_INTERIOR,
                     PSX_XG_RENDER_AUTH_REJECTION_SOURCE_RUNTIME_HOOK, true,
                     PSX_XG_RENDER_AUTH_HOOK_FOREIGN_INTERIOR, pc);
    }
}

bool psx_xg_render_auth_configure(
    GuestRenderRenderMode requested_render_mode,
    PsxXgRenderPresentationGate presentation_gate,
    void *presentation_user_data) {
    const bool render_valid =
        requested_render_mode == GUEST_RENDER_RENDER_ORIGINAL ||
        requested_render_mode == GUEST_RENDER_RENDER_SHADOW ||
        requested_render_mode == GUEST_RENDER_RENDER_NATIVE;
    const GuestRenderRenderMode render_mode = render_valid
        ? requested_render_mode : GUEST_RENDER_RENDER_ORIGINAL;
    const bool preserve_producer_family =
        xg_render_runtime_composition_producer_family_enabled();
    const XgRenderAuthRuntimeState previous_state = state;

    if (state.configured)
        return state.requested_render_mode == render_mode &&
            state.presentation_gate == presentation_gate &&
            state.presentation_user_data == presentation_user_data;
    state.requested_render_mode = render_mode;
    state.presentation_gate = presentation_gate;
    state.presentation_user_data = presentation_user_data;
    if (!xg_render_runtime_composition_configure(composition_services())) {
        state = previous_state;
        return false;
    }
    if (state.interpolation_scene_generation == 0u)
        state.interpolation_scene_generation = 1u;
    gpu_native_interpolation_scene_boundary(interpolation_scene_generation());
    g_psx_xg_render_auth_cold_enabled =
        state.requested_render_mode != GUEST_RENDER_RENDER_ORIGINAL;
    xg_render_runtime_composition_enable_producer_family(
        preserve_producer_family ||
        state.requested_render_mode != GUEST_RENDER_RENDER_ORIGINAL);
    state.configured = true;
    return true;
}

bool psx_xg_render_auth_configure_native_view(
    bool enabled, uint16_t aspect_num, uint16_t aspect_den,
    uint16_t canonical_width, uint16_t canonical_height) {
    return xg_render_runtime_composition_configure_native_view(
        enabled, aspect_num, aspect_den, canonical_width, canonical_height);
}

void psx_xg_render_auth_register_code_watches(
    void (*set_range)(uint32_t physical_address, uint32_t size)) {
    xg_render_runtime_composition_register_code_watches(set_range);
}

void psx_xg_render_auth_cold_enable(bool enabled) {
    const XgRenderInvalidationEvent event = {
        .kind = XG_RENDER_INVALIDATION_DISABLE,
    };

    g_psx_xg_render_auth_cold_enabled = enabled;
    if (!enabled) {
        xg_render_runtime_composition_disable();
        xg_render_runtime_composition_handle_invalidation(&event);
    }
}

bool psx_xg_render_auth_cold_enabled(void) {
    return g_psx_xg_render_auth_cold_enabled;
}

bool psx_xg_render_auth_movie_owner_active(void) {
    bool active;

    lock_presentation_lifecycle();
    active = state.movie_active_owner != GPU_MOVIE_OWNER_NONE;
    unlock_presentation_lifecycle();
    return active;
}

static void scene_boundary_locked(void) {
    const XgRenderInvalidationEvent event = {
        .kind = XG_RENDER_INVALIDATION_SCENE_BOUNDARY,
    };

    motion_source_invalidate(0,0,true);
    clear_authenticated_artifact_candidate();
    clear_movie_frame_artifact_candidate();
    if (xg_render_auth_process_owner(&state.auth) == XG_RENDER_AUTH_OK)
        (void)xg_render_auth_scene_reset(state.auth);
    xg_render_runtime_composition_handle_invalidation(&event);
    end_gte_attribution_producer();
    state.active = false;
    state.armed = true;
    state.completed = false;
    state.authenticated_producer_entry = 0u;
    state.authenticated_producer_scene_generation = 0u;
    state.rejection = (PsxXgRenderAuthRejectionReceipt){ 0 };
    if (state.scene_generation != UINT64_MAX) {
        ++state.scene_generation;
        xg_render_runtime_composition_scene_boundary(true);
    } else {
        xg_render_runtime_composition_scene_boundary(false);
    }
    clear_pending_candidate();
    clear_pending_variant_sequence();
    clear_candidate_outcome();
}

void psx_xg_render_auth_scene_boundary(void) {
    lock_presentation_lifecycle();
    clear_authenticated_artifact_candidate();
    clear_movie_frame_artifact_candidate();
    (void)timeline_invalidate_locked(XG_RENDER_TIMELINE_SCENE_CHANGE);
    scene_boundary_locked();
    unlock_presentation_lifecycle();
}

void psx_xg_render_auth_scene_boundary_after_timeline_invalidation(void) {
    lock_presentation_lifecycle();
    scene_boundary_locked();
    unlock_presentation_lifecycle();
}

bool psx_xg_render_auth_cold_hook_relevant(uint32_t hook, uint32_t pc,
                                           uint32_t instruction_word) {
    const XgRenderHookRouteKind route =
        xg_render_runtime_composition_hook_route_kind(
            hook, pc, instruction_word);
    bool canonical = false;

    if (!g_psx_xg_render_auth_cold_enabled) return false;
    if (route == XG_RENDER_HOOK_ROUTE_CANONICAL_ENTRY ||
        route == XG_RENDER_HOOK_ROUTE_CANONICAL_CAPTURE)
        canonical = authenticated_variant_hook_matches(pc);
    else if (route == XG_RENDER_HOOK_ROUTE_CANONICAL_RETURN)
        canonical = state.active && authenticated_variant_hook_matches(pc);
    else if (route == XG_RENDER_HOOK_ROUTE_UI_DRAW_OT)
        canonical = true;
    return canonical || xg_render_runtime_variant_hook_relevant(hook, pc);
}

void psx_xg_render_auth_cold_hook(CPUState *cpu, uint32_t hook, uint32_t pc,
                                  uint32_t instruction_word,
                                  uint32_t delay_slot_word) {
    if (!g_psx_xg_render_auth_cold_enabled) return;
    xg_render_instrumentation_record_cold_hook();
    observe_hook(cpu, XG_RENDER_AUTH_TIER_COLD_INTERPRETER, hook, pc,
                 instruction_word, delay_slot_word,
                 cpu != NULL ? cpu->gpr[31] : 0u);
}

void psx_xg_render_auth_warm_hook(CPUState *cpu, uint32_t hook, uint32_t pc,
                                  uint32_t instruction_word,
                                  uint32_t delay_slot_word) {
    observe_hook(cpu, XG_RENDER_AUTH_TIER_WARM_NATIVE, hook, pc,
                 instruction_word, delay_slot_word,
                 cpu != NULL ? cpu->gpr[31] : 0u);
}

bool psx_xg_render_auth_source_site_lookup(
    uint32_t pc, uint32_t instruction_word,
    PsxXgRenderSourceSiteMetadata *out_metadata) {
    if (!g_psx_xg_render_auth_cold_enabled ||
        !authenticated_variant_hook_matches(pc))
        return false;
    return xg_render_runtime_composition_source_site_lookup(
        pc, instruction_word, out_metadata);
}

bool psx_xg_render_auth_cold_source_pc_relevant(uint32_t pc) {
    return g_psx_xg_render_auth_cold_enabled &&
           authenticated_variant_hook_matches(pc) &&
           xg_render_runtime_composition_source_pc_relevant(pc);
}

uint32_t psx_xg_render_auth_cold_instruction_flags(
    uint32_t pc, uint32_t instruction_word) {
    enum { INVARIANT_CACHE_CAPACITY = 4096u };
    typedef struct {
        uint32_t pc;
        uint32_t instruction_word;
        uint32_t flags;
        bool valid;
    } XgRenderColdInvariantCacheEntry;
    static XgRenderColdInvariantCacheEntry
        invariant_cache[INVARIANT_CACHE_CAPACITY];
    XgRenderColdInvariantCacheEntry *cached;
    uint32_t flags;

    if (!g_psx_xg_render_auth_cold_enabled) return 0u;
    cached = &invariant_cache[(pc >> 2u) & (INVARIANT_CACHE_CAPACITY - 1u)];
    if (cached->valid && cached->pc == pc &&
        cached->instruction_word == instruction_word) {
        flags = cached->flags;
    } else {
        flags = 0u;
        if (psx_xg_render_auth_native_cutover_pc_relevant(pc))
            flags |= PSX_XG_RENDER_COLD_NATIVE_PRE;
        if (psx_xg_render_auth_native_cutover_post_pc_relevant(pc))
            flags |= PSX_XG_RENDER_COLD_NATIVE_POST;
        if (psx_xg_render_auth_overlay_cutover_relevant(pc, instruction_word))
            flags |= PSX_XG_RENDER_COLD_OVERLAY;
        *cached = (XgRenderColdInvariantCacheEntry){
            .pc = pc,
            .instruction_word = instruction_word,
            .flags = flags,
            .valid = true,
        };
    }
    if (psx_xg_render_auth_cold_hook_relevant(
            PSX_XG_RENDER_AUTH_HOOK_ENTRY, pc, instruction_word))
        flags |= PSX_XG_RENDER_COLD_ENTRY;
    if (instruction_word >> 26u == 3u &&
        psx_xg_render_auth_cold_hook_relevant(
            PSX_XG_RENDER_AUTH_HOOK_CAPTURE, pc, instruction_word))
        flags |= PSX_XG_RENDER_COLD_CAPTURE;
    if (psx_xg_render_auth_cold_source_pc_relevant(pc))
        flags |= PSX_XG_RENDER_COLD_SOURCE;
    return flags;
}

bool psx_xg_render_auth_cold_source_observe(
    PsxXgRenderSourceStage stage, uint32_t pc, uint32_t instruction_word,
    uint32_t auxiliary) {
    if (!g_psx_xg_render_auth_cold_enabled) return false;
    return xg_render_runtime_composition_source_observe(
        NULL, XG_RENDER_AUTH_TIER_COLD_INTERPRETER, stage, pc,
        instruction_word, auxiliary);
}

bool psx_xg_render_auth_cold_source_observe_cpu(
    CPUState *cpu, PsxXgRenderSourceStage stage, uint32_t pc,
    uint32_t instruction_word, uint32_t auxiliary) {
    if (!g_psx_xg_render_auth_cold_enabled) return false;
    return xg_render_runtime_composition_source_observe(
        cpu, XG_RENDER_AUTH_TIER_COLD_INTERPRETER, stage, pc,
        instruction_word, auxiliary);
}

bool psx_xg_render_auth_native_ft4_bypass(
        CPUState *cpu, uint32_t pc, uint32_t instruction_word) {
    return xg_render_runtime_composition_observe_dispatch(
        cpu, pc, instruction_word) == XG_RENDER_RUNTIME_COMPOSITION_BYPASS;
}

static const XgRenderAuthenticatedArtifact *current_movie_artifact(
        uint32_t artifact_base, uint32_t artifact_size) {
    for (uint32_t index = 0u;
         index < XG_RENDER_AUTH_ARTIFACT_CAPACITY; ++index) {
        const XgRenderAuthenticatedArtifact *record =
            &state.authenticated_artifacts[index];
        const PsxXgRenderAuthCandidate *candidate = &record->candidate;

        if (record->occupied && record->generation != 0u &&
            record->scene_generation == state.scene_generation &&
            record->provenance.capability != 0u &&
            movie_artifact_candidate_matches(candidate) &&
            physical_address_equals(candidate->artifact_base, artifact_base) &&
            candidate->artifact_size == artifact_size)
            return record;
    }
    return NULL;
}

static bool current_movie_artifact_matches(
        uint32_t artifact_base, uint32_t artifact_size) {
    return current_movie_artifact(artifact_base, artifact_size) != NULL;
}

static bool movie_artifact_candidate_matches(
        const PsxXgRenderAuthCandidate *candidate) {
    static const struct {
        uint32_t base;
        uint32_t size;
        uint8_t sha256[32];
    } artifacts[] = {
        { UINT32_C(0x8006faf0), 29779u,
          {0x50,0xe1,0xa9,0xd9,0xe0,0x8b,0x90,0xee,0xd0,0xc2,0xda,0x1c,0x28,0x95,0x07,0xe7,
           0x1c,0xbf,0x51,0x74,0x98,0x93,0xa9,0x2f,0x45,0x7c,0xe7,0x9a,0x59,0x70,0x1f,0x9a} },
        { UINT32_C(0x8006faf0), 260862u,
          {0x38,0xa1,0xce,0x82,0x9a,0x6f,0x09,0x4c,0x50,0x5f,0x67,0x14,0x3d,0x6a,0xce,0x2d,
           0x32,0x84,0x18,0xc6,0x54,0x25,0xa7,0x38,0x39,0x91,0x17,0x94,0x67,0xe1,0xfd,0xfc} },
        { UINT32_C(0x801d3000), 90112u,
          {0x2a,0x84,0x69,0x09,0x5f,0xd3,0x3d,0xae,0xf6,0x1d,0xbb,0xf0,0x9d,0x4f,0x10,0x6b,
           0xa1,0xd7,0x29,0x04,0xa5,0x02,0x76,0x3c,0xb8,0x90,0x57,0xcb,0xb6,0x15,0xa4,0x40} },
    };

    if (candidate == NULL || !candidate->authority_provenance ||
        !candidate->pair_bound || candidate->pair_id == 0u ||
        candidate->runtime_variant_bound ||
        memcmp(candidate->identity.game_sha256, xg_render_game_identity,
               sizeof(candidate->identity.game_sha256)) != 0 ||
        memcmp(candidate->identity.manifest_sha256,
               xg_render_manifest_identity,
               sizeof(candidate->identity.manifest_sha256)) != 0 ||
        !physical_address_equals(candidate->range_start,
                                 candidate->artifact_base) ||
        candidate->range_size != candidate->artifact_size ||
        !range_contains(candidate->range_start, candidate->range_size,
                        candidate->producer_entry, 4u) ||
        !range_contains(candidate->range_start, candidate->range_size,
                        candidate->dispatch_pc, 4u))
        return false;
    for (size_t index = 0u; index < sizeof(artifacts) / sizeof(artifacts[0]);
         ++index)
        if (physical_address_equals(candidate->artifact_base,
                                    artifacts[index].base) &&
            candidate->artifact_size == artifacts[index].size &&
            memcmp(candidate->artifact_sha256, artifacts[index].sha256,
                   sizeof(candidate->artifact_sha256)) == 0)
            return true;
    return false;
}

static bool movie_owner_start(
        CPUState *cpu, GpuMovieOwnerKind owner,
        uint32_t artifact_size,
        uint32_t expected_callback) {
    uint32_t callback_address = 0u;
    uint32_t callback_target = 0u;
    PsxXgRenderMovieOwnerStartBlocker blocker =
        PSX_XG_RENDER_MOVIE_OWNER_START_OK;
    bool started = false;

    atomic_fetch_add_explicit(
        &movie_owner_start_attempts, 1u, memory_order_relaxed);
    clear_movie_frame_artifact_candidate();
    if (!current_movie_artifact_matches(
            UINT32_C(0x8006faf0), artifact_size)) {
        blocker = PSX_XG_RENDER_MOVIE_OWNER_START_ARTIFACT;
    } else if (cpu == NULL || cpu->read_word == NULL) {
        blocker = PSX_XG_RENDER_MOVIE_OWNER_START_CPU;
    } else if (!xg_render_runtime_stack_address_is_valid(cpu->gpr[29]) ||
               cpu->gpr[29] > UINT32_MAX - UINT32_C(0x30)) {
        blocker = PSX_XG_RENDER_MOVIE_OWNER_START_STACK;
    } else {
        callback_address = cpu->gpr[29] + UINT32_C(0x30);
        if (!xg_render_runtime_word_address_is_valid(callback_address)) {
            blocker = PSX_XG_RENDER_MOVIE_OWNER_START_CALLBACK_ADDRESS;
        } else {
            callback_target = cpu->read_word(callback_address);
            if (callback_target != expected_callback) {
                blocker = PSX_XG_RENDER_MOVIE_OWNER_START_CALLBACK_TARGET;
            } else if (!gpu_note_movie_owner_start(owner, callback_target)) {
                blocker = PSX_XG_RENDER_MOVIE_OWNER_START_GPU;
            } else {
                started = true;
                state.movie_active_owner = owner;
                atomic_fetch_add_explicit(
                    &movie_owner_start_successes, 1u, memory_order_relaxed);
            }
        }
    }
    atomic_store_explicit(
        &movie_owner_last_kind, (unsigned)owner, memory_order_relaxed);
    atomic_store_explicit(
        &movie_owner_last_expected_callback, expected_callback,
        memory_order_relaxed);
    atomic_store_explicit(
        &movie_owner_last_observed_callback, callback_target,
        memory_order_relaxed);
    atomic_store_explicit(
        &movie_owner_last_start_blocker, (unsigned)blocker,
        memory_order_release);
    return started;
}

void psx_xg_render_auth_movie_owner_diagnostics(
        PsxXgRenderMovieOwnerDiagnostics *out_diagnostics) {
    if (out_diagnostics == NULL) return;
    *out_diagnostics = (PsxXgRenderMovieOwnerDiagnostics){
        .start_attempts = atomic_load_explicit(
            &movie_owner_start_attempts, memory_order_relaxed),
        .start_successes = atomic_load_explicit(
            &movie_owner_start_successes, memory_order_relaxed),
        .frame_complete_attempts = atomic_load_explicit(
            &movie_frame_complete_attempts, memory_order_relaxed),
        .frame_complete_successes = atomic_load_explicit(
            &movie_frame_complete_successes, memory_order_relaxed),
        .vram_frame_events = atomic_load_explicit(
            &movie_vram_frame_events, memory_order_relaxed),
        .publication_successes = atomic_load_explicit(
            &movie_publication_successes, memory_order_relaxed),
        .publication_failures = atomic_load_explicit(
            &movie_publication_failures, memory_order_relaxed),
        .owner_kind = atomic_load_explicit(
            &movie_owner_last_kind, memory_order_relaxed),
        .expected_callback = atomic_load_explicit(
            &movie_owner_last_expected_callback, memory_order_relaxed),
        .observed_callback = atomic_load_explicit(
            &movie_owner_last_observed_callback, memory_order_relaxed),
        .frame_number = atomic_load_explicit(
            &movie_last_frame_number, memory_order_relaxed),
        .frame_width = atomic_load_explicit(
            &movie_last_frame_width, memory_order_relaxed),
        .frame_height = atomic_load_explicit(
            &movie_last_frame_height, memory_order_relaxed),
        .frame_callback = atomic_load_explicit(
            &movie_last_frame_callback, memory_order_relaxed),
        .event_artifact_base = atomic_load_explicit(
            &movie_event_artifact_base, memory_order_relaxed),
        .event_artifact_size = atomic_load_explicit(
            &movie_event_artifact_size, memory_order_relaxed),
        .publication_blocker = atomic_load_explicit(
            &movie_publication_last_blocker, memory_order_relaxed),
        .publication_blocker_detail = atomic_load_explicit(
            &movie_publication_last_blocker_detail, memory_order_relaxed),
        .event_publication_count = atomic_load_explicit(
            &movie_event_publication_count, memory_order_relaxed),
        .event_movie_edge_count = atomic_load_explicit(
            &movie_event_edge_count, memory_order_relaxed),
        .event_coverage_bytes = atomic_load_explicit(
            &movie_event_coverage_bytes, memory_order_relaxed),
        .mdec_surface_successes = atomic_load_explicit(
            &movie_mdec_surface_successes, memory_order_relaxed),
        .mdec_surface_failures = atomic_load_explicit(
            &movie_mdec_surface_failures, memory_order_relaxed),
        .mdec_surface_x = atomic_load_explicit(
            &movie_mdec_surface_x, memory_order_relaxed),
        .mdec_surface_y = atomic_load_explicit(
            &movie_mdec_surface_y, memory_order_relaxed),
        .mdec_surface_width = atomic_load_explicit(
            &movie_mdec_surface_width, memory_order_relaxed),
        .mdec_surface_height = atomic_load_explicit(
            &movie_mdec_surface_height, memory_order_relaxed),
        .scanout_x = atomic_load_explicit(
            &movie_scanout_x, memory_order_relaxed),
        .scanout_y = atomic_load_explicit(
            &movie_scanout_y, memory_order_relaxed),
        .scanout_width = atomic_load_explicit(
            &movie_scanout_width, memory_order_relaxed),
        .scanout_height = atomic_load_explicit(
            &movie_scanout_height, memory_order_relaxed),
        .source_active_after_publication = atomic_load_explicit(
            &movie_source_active_after_publication, memory_order_relaxed),
        .source_passes_after_publication = atomic_load_explicit(
            &movie_source_passes_after_publication, memory_order_relaxed),
        .source_active_before_boundary = atomic_load_explicit(
            &movie_source_active_before_boundary, memory_order_relaxed),
        .source_passes_before_boundary = atomic_load_explicit(
            &movie_source_passes_before_boundary, memory_order_relaxed),
        .publication_boundaries = atomic_load_explicit(
            &movie_publication_boundaries, memory_order_relaxed),
        .publication_boundaries_active = atomic_load_explicit(
            &movie_publication_boundaries_active, memory_order_relaxed),
        .publication_boundary_completion_result = atomic_load_explicit(
            &movie_publication_boundary_completion_result,
            memory_order_relaxed),
        .publication_boundary_publish_result = atomic_load_explicit(
            &movie_publication_boundary_publish_result,
            memory_order_relaxed),
        .start_blocker = (PsxXgRenderMovieOwnerStartBlocker)
            atomic_load_explicit(
                &movie_owner_last_start_blocker, memory_order_acquire),
    };
}

static bool movie_owner_stop(
        GpuMovieOwnerKind owner, uint32_t artifact_size) {
    const bool stopped = current_movie_artifact_matches(
            UINT32_C(0x8006faf0), artifact_size) &&
        gpu_note_movie_owner_stop(owner);

    if (stopped) {
        state.movie_active_owner = GPU_MOVIE_OWNER_NONE;
        clear_movie_frame_artifact_candidate();
    }
    return stopped;
}

bool psx_xg_render_auth_movie_standalone_start(CPUState *cpu) {
    return movie_owner_start(
        cpu, GPU_MOVIE_OWNER_STANDALONE, 29779u,
        UINT32_C(0x800768d8));
}

bool psx_xg_render_auth_movie_standalone_stop(void) {
    return movie_owner_stop(
        GPU_MOVIE_OWNER_STANDALONE, 29779u);
}

bool psx_xg_render_auth_movie_field_start(CPUState *cpu) {
    return movie_owner_start(
        cpu, GPU_MOVIE_OWNER_FIELD, 260862u,
        UINT32_C(0x800a7120));
}

bool psx_xg_render_auth_movie_field_stop(void) {
    return movie_owner_stop(
        GPU_MOVIE_OWNER_FIELD, 260862u);
}

bool psx_xg_render_auth_movie_frame_complete(CPUState *cpu) {
    GpuDisplayInfo display = {0};
    const XgRenderAuthenticatedArtifact *movie_artifact;
    bool completed;

    if (cpu != NULL) gpu_get_display_info(&display);
    atomic_fetch_add_explicit(
        &movie_frame_complete_attempts, 1u, memory_order_relaxed);
    atomic_store_explicit(&movie_last_frame_number,
        cpu != NULL ? cpu->gpr[4] : 0u, memory_order_relaxed);
    atomic_store_explicit(&movie_last_frame_width,
        display.width, memory_order_relaxed);
    atomic_store_explicit(&movie_last_frame_height,
        display.height, memory_order_relaxed);
    atomic_store_explicit(&movie_last_frame_callback,
        cpu != NULL ? cpu->gpr[7] : 0u, memory_order_relaxed);
    movie_artifact = current_movie_artifact(
        UINT32_C(0x801d3000), 90112u);
    if (cpu == NULL || movie_artifact == NULL ||
        display.width == 0u || display.width > UINT16_MAX ||
        display.height == 0u || display.height > UINT16_MAX)
        return false;
    state.movie_frame_artifact_candidate = movie_artifact->candidate;
    state.movie_frame_artifact_candidate_valid = true;
    completed = gpu_note_movie_frame_complete(
        cpu->gpr[4], cpu->gpr[7]);
    if (completed)
        atomic_fetch_add_explicit(
            &movie_frame_complete_successes, 1u, memory_order_relaxed);
    if (!completed) clear_movie_frame_artifact_candidate();
    return completed;
}

void psx_xg_render_auth_capture_model_ft3_link(CPUState *cpu) {
    xg_render_runtime_composition_capture_model_ft3_link(cpu);
}

bool psx_xg_render_auth_native_cutover_pc_relevant(uint32_t pc) {
    return xg_render_runtime_composition_cutover_pc_relevant(pc);
}

bool psx_xg_render_auth_overlay_cutover_relevant(
        uint32_t pc, uint32_t instruction_word) {
    return xg_render_runtime_composition_overlay_relevant(
        pc, instruction_word);
}

bool psx_xg_render_auth_native_cutover_post_pc_relevant(uint32_t pc) {
    return xg_render_runtime_composition_cutover_post_pc_relevant(pc);
}

static void invalidate_authenticated_authority(void) {
    clear_authenticated_artifact_candidate();
    state.authenticated_producer_entry = 0u;
    state.authenticated_producer_scene_generation = 0u;
}

static void invalidate_authenticated_authority_overlapping(
        uint32_t address, uint32_t size) {
    bool removed = false;

    for (uint32_t index = 0u;
         index < XG_RENDER_AUTH_ARTIFACT_CAPACITY; ++index) {
        const XgRenderAuthenticatedArtifact *record =
            &state.authenticated_artifacts[index];

        if (record->occupied && normalized_ranges_overlap(
                record->candidate.range_start, record->candidate.range_size,
                address, size)) {
            remove_authenticated_artifact(index, false);
            removed = true;
        }
    }
    if (removed) {
        state.authenticated_producer_entry = 0u;
        state.authenticated_producer_scene_generation = 0u;
    }
}

void psx_xg_render_auth_note_code_write(uint64_t previous_generation,
                                           uint64_t current_generation,
                                           uint32_t guest_pc,
                                           uint32_t write_size) {
    XgRenderMutationClassification classification;
    XgRenderMutationContext mutation_context;

    motion_source_invalidate(guest_pc,write_size,false);
    if (xg_render_runtime_variant_no_gates_enabled()) return;
    mutation_context = (XgRenderMutationContext){
        .artifact_mutation =
            current_artifact_code_range_overlaps(guest_pc, write_size),
        .authentication_range_mutation =
            authentication_range_write_overlaps(guest_pc, write_size),
        .completed_authorization = state.completed,
    };
    xg_render_runtime_composition_classify_code_write(
        guest_pc, write_size, &mutation_context, &classification);
    const XgRenderInvalidationEvent event = {
        .kind = XG_RENDER_INVALIDATION_CODE_WRITE,
        .address = guest_pc,
        .size = write_size,
        .code_write_mask = classification.code_write_mask,
        .mutation = classification.properties,
    };

    if (event.mutation.authentication_mutation)
        invalidate_authenticated_authority();
    else if (event.mutation.authority_loss)
        invalidate_authenticated_authority_overlapping(guest_pc, write_size);
    if (event.mutation.interpolation_reset)
        advance_interpolation_scene();
    xg_render_runtime_composition_handle_invalidation(&event);

    if (!event.mutation.watched_range_mutation &&
        !event.mutation.artifact_mutation &&
        !event.mutation.authentication_mutation &&
        !event.mutation.resource_mutation) {
        if (state.completed) retire_completed_auth_proof();
        return;
    }

    if (state.completed) {
        retire_completed_auth_proof();
        return;
    }
    if (!state.active) {
        if (!event.mutation.runtime_variant_mutation)
            return;
        if (state.pending_variant_sequence) {
            begin_scene(state.pending_variant_tier,
                        xg_render_manifest_validation.producer_entry);
            abort_active(XG_RENDER_AUTH_REJECT_CODE_PAGE_MUTATION,
                         PSX_XG_RENDER_AUTH_REJECTION_SOURCE_CODE_PAGE_MUTATION,
                         false, PSX_XG_RENDER_AUTH_HOOK_CAPTURE, guest_pc);
        } else {
            clear_pending_candidate();
            clear_pending_variant_sequence();
        }
        return;
    }
    if (!event.mutation.authentication_mutation) return;
    if (state.auth == NULL) {
        disarm();
        return;
    }
    if (xg_render_auth_note_code_page_mutation(state.auth, previous_generation,
                                               current_generation) !=
        XG_RENDER_AUTH_OK) {
        latch_rejection(XG_RENDER_AUTH_REJECT_CODE_PAGE_MUTATION,
                        PSX_XG_RENDER_AUTH_REJECTION_SOURCE_CODE_PAGE_MUTATION,
                        false, PSX_XG_RENDER_AUTH_HOOK_CAPTURE, guest_pc);
        disarm();
    }
}

void psx_xg_render_auth_loader_mismatch(uint32_t pc) {
    motion_source_invalidate(pc,4,false);
    const bool artifact_authorized = current_artifact_range_contains_pc(pc);

    if (xg_render_runtime_variant_no_gates_enabled()) return;
    if (field_range_contains(pc) || artifact_authorized ||
        xg_render_runtime_composition_authority_authorizes_pc(pc) ||
        xg_render_runtime_composition_pending_authorizes_pc(pc)) {
        const XgRenderInvalidationEvent event = {
            .kind = XG_RENDER_INVALIDATION_LOADER_MISMATCH,
            .address = pc,
        };

        advance_interpolation_scene();
        if (artifact_authorized)
            invalidate_authenticated_authority_overlapping(pc, 4u);
        else if (field_range_contains(pc))
            invalidate_authenticated_authority();
        xg_render_runtime_composition_handle_invalidation(&event);
        /* A rehash miss demotes compiled host code to authoritative guest
         * execution. It aborts the active proof, not resources observed from
         * that guest execution; code writes own their invalidation separately. */
        if (state.active || state.completed) {
            abort_active(XG_RENDER_AUTH_REJECT_VALIDATION_MISMATCH,
                         PSX_XG_RENDER_AUTH_REJECTION_SOURCE_LOADER_MISMATCH,
                         false, PSX_XG_RENDER_AUTH_HOOK_ENTRY, pc);
        } else {
            clear_pending_candidate();
            clear_pending_variant_sequence();
        }
    }
}

void psx_xg_render_auth_native_bad_entry(uint32_t owner, uint32_t pc) {
    if (xg_render_runtime_variant_no_gates_enabled()) return;
    if (physical_address_equals(owner,
                                xg_render_manifest_validation.producer_entry) ||
        field_range_contains(pc))
        abort_active(XG_RENDER_AUTH_REJECT_FOREIGN_INTERIOR,
                     PSX_XG_RENDER_AUTH_REJECTION_SOURCE_NATIVE_BAD_ENTRY,
                     false, PSX_XG_RENDER_AUTH_HOOK_ENTRY, pc);
}

static void note_artifact_candidate_locked(
        const PsxXgRenderAuthCandidate *candidate) {
    bool candidate_matches;
    bool overlap_removed = false;
    uint32_t free_index = XG_RENDER_AUTH_ARTIFACT_CAPACITY;
    XgRenderResourceProvenance provenance = {0};

    if (candidate == NULL) return;
    candidate_matches =
        xg_render_runtime_variant_artifact_candidate_matches(candidate) ||
        (!candidate->runtime_variant_bound &&
         (xg_render_authoritative_overlay_artifact_candidate_matches(candidate) ||
           movie_artifact_candidate_matches(candidate)));
    if (candidate_matches) {
        for (uint32_t index = 0u;
             index < XG_RENDER_AUTH_ARTIFACT_CAPACITY; ++index) {
            XgRenderAuthenticatedArtifact *record =
                &state.authenticated_artifacts[index];

            if (record->occupied && !record->static_text &&
                record->scene_generation == state.scene_generation &&
                artifact_binary_identity_matches(
                    &record->candidate, candidate)) {
                record->candidate = *candidate;
                select_authenticated_artifact_primary(index);
                return;
            }
        }
    }
    if (!candidate_matches) {
        for (uint32_t index = 0u;
             index < XG_RENDER_AUTH_ARTIFACT_CAPACITY; ++index) {
            const XgRenderAuthenticatedArtifact *record =
                &state.authenticated_artifacts[index];

            if (record->occupied && normalized_ranges_overlap(
                    record->candidate.artifact_base,
                    record->candidate.artifact_size,
                    candidate->dispatch_pc, 4u)) {
                remove_authenticated_artifact(index, true);
                overlap_removed = true;
            }
        }
        if (overlap_removed) {
            broadcast_resource_overlap();
            state.authenticated_producer_entry = 0u;
            state.authenticated_producer_scene_generation = 0u;
        }
        return;
    }

    for (uint32_t index = 0u;
         index < XG_RENDER_AUTH_ARTIFACT_CAPACITY; ++index) {
        const XgRenderAuthenticatedArtifact *record =
            &state.authenticated_artifacts[index];

        if (record->occupied && normalized_ranges_overlap(
                record->candidate.artifact_base,
                record->candidate.artifact_size,
                candidate->artifact_base, candidate->artifact_size)) {
            remove_authenticated_artifact(index, true);
            overlap_removed = true;
        }
    }
    if (overlap_removed) {
        broadcast_resource_overlap();
        state.authenticated_producer_entry = 0u;
        state.authenticated_producer_scene_generation = 0u;
    }

    for (uint32_t index = 0u;
         index < XG_RENDER_AUTH_ARTIFACT_CAPACITY; ++index)
        if (!state.authenticated_artifacts[index].occupied) {
            free_index = index;
            break;
        }
    /* The set is bounded and never evicts an unrelated authority record. */
    if (free_index == XG_RENDER_AUTH_ARTIFACT_CAPACITY) return;
    if (state.authenticated_artifact_generation == UINT64_MAX) {
        invalidate_authenticated_authority();
        return;
    }

    ++state.authenticated_artifact_generation;
    {
        XgRenderResourceCapabilityMetadata metadata = {
            .kind = XG_RENDER_RESOURCE_PROVENANCE_ARTIFACT,
            .receipt = state.authenticated_artifact_generation,
            .lifetime = XG_RENDER_RESOURCE_CAPABILITY_SCENE,
            .owner_kind = XG_RENDER_RESOURCE_OWNER_SCENE,
            .owner_generation = state.scene_generation,
            .artifact = {
                .base = candidate->artifact_base,
                .size = candidate->artifact_size,
            },
        };

        memcpy(metadata.artifact.sha256, candidate->artifact_sha256,
               sizeof(metadata.artifact.sha256));
        (void)xg_render_ui_owner_catalog_artifact_identity(
            candidate->artifact_base, candidate->artifact_size,
            candidate->artifact_sha256, &metadata.artifact);
        if (xg_render_resource_capability_register(
                &metadata, &provenance) !=
                XG_RENDER_RESOURCE_CAPABILITY_OK)
            return;
    }
    state.authenticated_artifacts[free_index] =
        (XgRenderAuthenticatedArtifact){
            .candidate = *candidate,
            .provenance = provenance,
            .generation = state.authenticated_artifact_generation,
            .scene_generation = state.scene_generation,
            .occupied = true,
        };
    ++state.authenticated_artifact_count;
    select_authenticated_artifact_primary(free_index);
}

void psx_xg_render_auth_note_artifact_candidate(
        const PsxXgRenderAuthCandidate *candidate) {
    if (candidate == NULL) return;
    lock_presentation_lifecycle();
    note_artifact_candidate_locked(candidate);
    unlock_presentation_lifecycle();
}

bool psx_xg_render_auth_authenticated_producer_entry(
    uint32_t *out_producer_entry) {
    if (out_producer_entry == NULL ||
        state.authenticated_producer_entry == 0u ||
        state.authenticated_producer_scene_generation != state.scene_generation)
        return false;
    *out_producer_entry = state.authenticated_producer_entry;
    return true;
}

void psx_xg_render_auth_note_candidate_dispatch(
    const PsxXgRenderAuthCandidate *candidate) {
    bool candidate_matches;

    note_motion_source_candidate(candidate);
    psx_xg_render_auth_note_artifact_candidate(candidate);
    clear_pending_candidate();
    if (candidate == NULL || !state.armed)
        return;
    if (state.completed) retire_completed_auth_proof();
    if (state.active || !state.armed) return;
    candidate_matches = candidate->runtime_variant_bound
        ? xg_render_runtime_variant_candidate_matches(candidate) ||
              xg_render_runtime_variant_artifact_candidate_matches(candidate)
        : candidate_matches_manifest(candidate);
    if (!candidate_matches)
        return;
    state.pending_candidate = *candidate;
    state.pending_scene_generation = state.scene_generation;
    state.pending_candidate_valid = true;
}

void psx_xg_render_auth_provenance_snapshot(
    PsxXgRenderAuthProvenance *out_provenance) {
    bool identity_bound = false;
    bool identity_gate_passed = false;

    if (out_provenance == NULL) return;
    out_provenance->manifest_bound =
        xg_render_static_auth_metadata_is_valid() &&
        xg_render_static_auth_bind_identity(&identity_bound,
                                            &identity_gate_passed);
    out_provenance->range_bound = field_range_is_bound();
    out_provenance->candidate_matched = state.candidate_matched;
    out_provenance->candidate_dispatched = state.candidate_dispatched;
}

void psx_xg_render_auth_static_artifact_diagnostics(
        uint64_t *out_attempts, uint64_t *out_successes,
        uint32_t *out_last_pc, uint32_t *out_last_blocker) {
    if (out_attempts != NULL)
        *out_attempts = atomic_load_explicit(
            &static_artifact_attempts, memory_order_relaxed);
    if (out_successes != NULL)
        *out_successes = atomic_load_explicit(
            &static_artifact_successes, memory_order_relaxed);
    if (out_last_pc != NULL)
        *out_last_pc = atomic_load_explicit(
            &static_artifact_last_pc, memory_order_relaxed);
    if (out_last_blocker != NULL)
        *out_last_blocker = atomic_load_explicit(
            &static_artifact_last_blocker, memory_order_relaxed);
}

void psx_xg_render_auth_rejection_snapshot(
    PsxXgRenderAuthRejectionReceipt *out_receipt) {
    if (out_receipt != NULL) *out_receipt = state.rejection;
}

void psx_xg_render_auth_completed_proof_snapshot(
    PsxXgRenderAuthCompletedProofReceipt *out_receipt) {
    if (out_receipt == NULL) return;
    lock_completed_proof();
    *out_receipt = completed_proof;
    unlock_completed_proof();
}

void psx_xg_render_auth_instrumentation_snapshot(
    PsxXgRenderAuthInstrumentation *out_instrumentation) {
    xg_render_instrumentation_snapshot(out_instrumentation);
}

void psx_xg_render_auth_mode_snapshot(
    PsxXgRenderModeSnapshot *out_snapshot) {
    GuestRenderBridgeSnapshot bridge_snapshot = { 0 };
    GuestRenderBridgeSnapshot completed_bridge = { 0 };
    GuestRenderCompletedState completed = { 0 };
    GuestRenderTransactionSnapshot transaction_snapshot = { 0 };
    bool completed_authority;

    if (out_snapshot == NULL) return;
    memset(out_snapshot, 0, sizeof(*out_snapshot));
    out_snapshot->modes.requested_render_mode = state.requested_render_mode;
    out_snapshot->modes.effective_render_mode = state.requested_render_mode;
    out_snapshot->presentation = state.presentation;
    lock_completed_proof();
    completed_authority = completed_proof.available && !completed_proof.blocked &&
        completed_proof_scene_generation == state.scene_generation;
    unlock_completed_proof();
    if (guest_render_bridge_snapshot(&bridge_snapshot) == GUEST_RENDER_OK) {
        out_snapshot->modes = bridge_snapshot.modes;
        out_snapshot->fallback_reason = bridge_snapshot.fallback_reason;
        out_snapshot->fallback_count = bridge_snapshot.fallback_count;
        if (!bridge_snapshot.state_open && !bridge_snapshot.producer_open &&
            bridge_snapshot.fallback_reason == GUEST_RENDER_FALLBACK_NONE &&
            bridge_snapshot.modes.effective_render_mode ==
                GUEST_RENDER_RENDER_ORIGINAL &&
            state.requested_render_mode != GUEST_RENDER_RENDER_ORIGINAL &&
            guest_render_bridge_last_completed(&completed_bridge, &completed) ==
                GUEST_RENDER_OK &&
            completed_bridge.fallback_reason == GUEST_RENDER_FALLBACK_NONE &&
            completed_bridge.modes.requested_render_mode ==
                state.requested_render_mode) {
            out_snapshot->modes = completed_bridge.modes;
        } else if (!bridge_snapshot.state_open &&
                   !bridge_snapshot.producer_open &&
                   bridge_snapshot.fallback_reason ==
                       GUEST_RENDER_FALLBACK_NONE &&
                   completed_authority) {
            out_snapshot->modes.requested_render_mode =
                state.requested_render_mode;
            out_snapshot->modes.effective_render_mode =
                state.requested_render_mode;
        }
    }
    if (guest_render_transaction_snapshot(&transaction_snapshot) ==
        GUEST_RENDER_TRANSACTION_OK) {
        out_snapshot->transaction_count =
            transaction_snapshot.published_transaction_count;
        out_snapshot->substitution_count =
            transaction_snapshot.published_substitution_count;
    }
}

void psx_xg_render_auth_reset(void) {
    const XgRenderInvalidationEvent event = {
        .kind = XG_RENDER_INVALIDATION_RESET,
    };

    motion_source_invalidate(0,0,true);
    clear_authenticated_artifact_candidate();
    clear_movie_frame_artifact_candidate();
    (void)psx_xg_render_auth_timeline_invalidate(XG_RENDER_TIMELINE_RESET);
    guest_render_native_stream_set_enabled(false);
    end_gte_attribution_producer();
    if (state.auth != NULL) (void)xg_render_auth_scene_reset(state.auth);
    state = (XgRenderAuthRuntimeState){
        .armed = true,
        .scene_generation = 1u,
        .interpolation_scene_generation = 1u,
        .requested_render_mode = GUEST_RENDER_RENDER_ORIGINAL,
    };
    g_psx_xg_render_auth_cold_enabled = false;
    gpu_native_interpolation_scene_boundary(interpolation_scene_generation());
    (void)xg_render_runtime_composition_configure(composition_services());
    xg_render_runtime_composition_configure_invalidation();
    xg_render_runtime_composition_handle_invalidation(&event);
    xg_render_runtime_composition_reset();
    completed_proof = (PsxXgRenderAuthCompletedProofReceipt){ 0 };
    completed_proof_scene_generation = 0u;
}

void psx_xg_render_auth_runtime_snapshot(
        PsxXgRenderAuthRuntimeSnapshot *out_snapshot) {
    PsxXgRenderUiOtSnapshot ui_ot = {0};
    XgRenderResourceView movie_view = {0};
    const uint64_t primary_generation =
        state.authenticated_artifact_primary_valid &&
        state.authenticated_artifact_primary_index <
            XG_RENDER_AUTH_ARTIFACT_CAPACITY
        ? state.authenticated_artifacts[
              state.authenticated_artifact_primary_index].generation
        : 0u;

    if (out_snapshot == NULL) return;
    xg_render_runtime_composition_ui_ot_snapshot(&ui_ot);
    *out_snapshot = (PsxXgRenderAuthRuntimeSnapshot){
        .interpolation_scene_generation = interpolation_scene_generation(),
        .authenticated_artifact_generation =
            primary_generation,
        .authenticated_artifact_active = current_artifact_is_authorized(),
        .active = state.active,
        .armed = state.armed,
        .completed = state.completed,
        .artifact_candidate_valid =
            state.authenticated_artifact_candidate_valid,
        .artifact_scene_generation =
            state.authenticated_artifact_scene_generation,
        .ui_ot_completed_count = ui_ot.completed_count,
        .ui_ot_staged_count = ui_ot.staged_count,
        .ui_ot_blocked = ui_ot.blocked,
    };
    out_snapshot->retained_movie_resource_id =
        retained_movie_surface.resource_id;
    out_snapshot->retained_movie_generation =
        retained_movie_surface.generation;
    out_snapshot->retained_movie_available =
        retained_movie_surface.resource_id != 0u &&
        xg_render_resource_view((XgRenderResourceHandle){
            retained_movie_surface.resource_id,
            retained_movie_surface.generation,
        }, &movie_view) == XG_RENDER_RESOURCE_OK;
    if (out_snapshot->retained_movie_available) {
        out_snapshot->retained_movie_kind = movie_view.kind;
        out_snapshot->retained_movie_width = movie_view.descriptor.width;
        out_snapshot->retained_movie_height = movie_view.descriptor.height;
        out_snapshot->retained_movie_retain_count = movie_view.retain_count;
        out_snapshot->retained_movie_current = movie_view.current;
    }
    xg_render_runtime_composition_invalidation_counts(
        out_snapshot->invalidation_kind_counts,
        sizeof(out_snapshot->invalidation_kind_counts) /
            sizeof(out_snapshot->invalidation_kind_counts[0]),
        out_snapshot->invalidation_mutation_counts,
        sizeof(out_snapshot->invalidation_mutation_counts) /
            sizeof(out_snapshot->invalidation_mutation_counts[0]));
    xg_render_runtime_composition_pre_scene_status(
        &out_snapshot->pre_scene_count, &out_snapshot->pre_scene_blocker);
    xg_render_runtime_composition_title_restage_status(
        &out_snapshot->title_restage_attempts,
        &out_snapshot->title_restage_last_result,
        &out_snapshot->title_restage_last_detail,
        &out_snapshot->title_restage_fail_index,
        &out_snapshot->title_restage_tpage,
        &out_snapshot->title_restage_clut);
}

void psx_xg_render_auth_resident_text_snapshot(
        PsxXgRenderResidentTextSnapshot *out_snapshot) {
    xg_render_runtime_composition_resident_text_snapshot(out_snapshot);
}

const char *psx_xg_render_auth_rejection_source_name(uint32_t source) {
    switch (source) {
    case PSX_XG_RENDER_AUTH_REJECTION_SOURCE_NONE: return "none";
    case PSX_XG_RENDER_AUTH_REJECTION_SOURCE_RUNTIME_HOOK: return "runtime_hook";
    case PSX_XG_RENDER_AUTH_REJECTION_SOURCE_VARIANT_HOOK: return "variant_hook";
    case PSX_XG_RENDER_AUTH_REJECTION_SOURCE_LOADER_MISMATCH:
        return "loader_mismatch";
    case PSX_XG_RENDER_AUTH_REJECTION_SOURCE_NATIVE_BAD_ENTRY:
        return "native_bad_entry";
    case PSX_XG_RENDER_AUTH_REJECTION_SOURCE_CODE_PAGE_MUTATION:
        return "code_page_mutation";
    }
    return "none";
}

const char *psx_xg_render_auth_hook_name(uint32_t hook) {
    switch (hook) {
    case PSX_XG_RENDER_AUTH_HOOK_PRODUCER_ENTRY: return "producer_entry";
    case PSX_XG_RENDER_AUTH_HOOK_INTERNAL_OBSERVATION:
        return "internal_observation";
    case PSX_XG_RENDER_AUTH_HOOK_CONTINUATION: return "continuation";
    case PSX_XG_RENDER_AUTH_HOOK_PRODUCER_EXIT: return "producer_exit";
    case PSX_XG_RENDER_AUTH_HOOK_FOREIGN_INTERIOR: return "foreign_interior";
    case PSX_XG_RENDER_AUTH_HOOK_SOURCE_PRE: return "source_pre";
    case PSX_XG_RENDER_AUTH_HOOK_SOURCE_COMMIT: return "source_commit";
    }
    return "none";
}

void psx_xg_render_auth_set_terrain_temporal_coverage(bool enabled) {
    xg_render_runtime_composition_set_terrain_temporal_coverage(enabled);
}

void psx_xg_render_auth_set_exec_phase_exchange(
        PsxXgRenderExecPhaseExchange exchange) {
    xg_render_runtime_composition_set_exec_phase_exchange(exchange);
}

void psx_xg_render_auth_before_gpu_submission(void) {
    xg_render_runtime_composition_before_gpu_submission();
}

void psx_xg_render_auth_note_gpu_semantic_current(
        const GpuRenderSemantic *semantic) {
    xg_render_runtime_composition_note_gpu_semantic_current(semantic);
}

void psx_xg_render_auth_complete_gpu_source_frame(void) {
    (void)xg_render_runtime_composition_complete_gpu_source_frame();
}

static bool vram_upload_matches_authenticated_resource(
        XgRenderResourceKind kind, uint16_t x, uint16_t y,
        uint16_t width, uint16_t height,
        const uint16_t *pixels, size_t pixel_count) {
    XgSemanticResourceRef resource;
    XgRenderResourceCapabilityMetadata authority;
    XgRenderResourceView view;

    if (!xg_render_vram_resources_lookup(
            kind, x, y, width, height, &resource) ||
        xg_render_resource_view((XgRenderResourceHandle){
            resource.resource_id, resource.generation,
        }, &view) != XG_RENDER_RESOURCE_OK ||
        !view.current || view.bytes == NULL || !view.has_identity ||
        view.byte_count != pixel_count * sizeof(*pixels) ||
        view.descriptor.vram_x != x || view.descriptor.vram_y != y ||
        view.descriptor.vram_width != width ||
        view.descriptor.vram_height != height ||
        xg_render_resource_capability_validate(
            &view.provenance, XG_RENDER_RESOURCE_OWNER_SCENE,
            state.scene_generation, &authority) !=
                XG_RENDER_RESOURCE_CAPABILITY_OK)
        return false;
    for (size_t index = 0u; index < pixel_count; ++index) {
        const uint8_t *encoded = (const uint8_t *)view.bytes + index * 2u;

        if (pixels[index] != ((uint16_t)encoded[0] |
                              (uint16_t)encoded[1] << 8u))
            return false;
    }
    return true;
}

static bool deterministic_vram_clear(
        const GpuVramEvent *event,
        const uint16_t *pixels, size_t pixel_count) {
    uint16_t expected;

    if (event == NULL || !event->command_context_valid ||
        event->command_opcode != 0x02u || event->command_word_count < 3u)
        return false;
    expected = (uint16_t)(((event->command_words[0] & 0xf8u) >> 3u) |
        ((event->command_words[0] & 0xf800u) >> 6u) |
        ((event->command_words[0] & 0xf80000u) >> 9u));
    for (size_t index = 0u; index < pixel_count; ++index)
        if (pixels[index] != expected) return false;
    return true;
}

static bool note_vram_rect_transfer(
        XgRenderVramOperation operation,
        uint64_t guest_vblank_sequence, uint64_t guest_cycle,
        uint64_t device_mutation_serial,
        uint16_t source_x, uint16_t source_y,
        uint16_t x, uint16_t y, uint16_t width, uint16_t height,
        const uint16_t *pixels, size_t pixel_count,
        const GpuVramEvent *event) {
    enum { ENCODED_CHUNK_SIZE = 2048u };
    uint8_t encoded[ENCODED_CHUNK_SIZE];
    XgRenderVramTransferDescription description;
    XgRenderVramTransferHandle transfer;
    XgRenderVramMutation mutation;
    XgRenderResourceIdentity move_source_identity = {0};
    XgRenderResourceProvenance move_source_provenance = {0};
    uint16_t *move_source_pixels = NULL;
    const uint16_t *transfer_pixels = pixels;
    size_t payload_size;
    size_t pixel_offset = 0u;
    bool move_source_capability_created = false;
    bool move_source_authenticated = false;
    bool payload_authenticated = false;
    bool success = false;

    if (pixels == NULL || width == 0u || height == 0u ||
        pixel_count != (size_t)width * height ||
        pixel_count > SIZE_MAX / sizeof(*pixels))
        return false;
    payload_size = pixel_count * sizeof(*pixels);
    description = (XgRenderVramTransferDescription){
        .operation = operation,
        .guest_cycle = guest_cycle,
        .source_interval = guest_vblank_sequence,
        .device_mutation_serial = device_mutation_serial,
        .source_x = source_x,
        .source_y = source_y,
        .x = x,
        .y = y,
        .width = width,
        .height = height,
        .payload_size = payload_size,
        .payload_source_receipt = event != NULL
            ? event->payload_source_receipt : 0u,
        .payload_source = event != NULL ? (uint32_t)event->payload_source : 0u,
        .payload_format = event != NULL ? event->payload_format : 0u,
        .command_source_address = event != NULL
            ? event->command_source_address : 0u,
        .command_pc = event != NULL ? event->command_pc : 0u,
        .command_function = event != NULL ? event->command_function : 0u,
        .command_return_address = event != NULL
            ? event->command_return_address : 0u,
        .command_source_kind = event != NULL
            ? event->command_source_kind : 0u,
        .command_opcode = event != NULL ? event->command_opcode : 0u,
        .command_word_count = event != NULL ? event->command_word_count : 0u,
        .command_context_valid = event != NULL && event->command_context_valid,
    };
    if (event != NULL) {
        size_t command_word_count = event->command_word_count;

        if (command_word_count > 4u) command_word_count = 4u;
        memcpy(description.command_words, event->command_words,
               command_word_count * sizeof(*event->command_words));
    }

    lock_presentation_lifecycle();
    payload_authenticated =
        (operation == XG_RENDER_VRAM_CLEAR &&
         deterministic_vram_clear(event, pixels, pixel_count)) ||
        (operation == XG_RENDER_VRAM_UPLOAD && event != NULL &&
         event->payload_source == GPU_VRAM_PAYLOAD_SOURCE_MDEC_DMA1 &&
         event->payload_source_receipt != 0u &&
         (event->payload_format == 2u || event->payload_format == 3u)) ||
        (operation == XG_RENDER_VRAM_UPLOAD &&
         (vram_upload_matches_authenticated_resource(
              XG_RENDER_RESOURCE_TEXTURE, x, y, width, height,
              pixels, pixel_count) ||
          vram_upload_matches_authenticated_resource(
              XG_RENDER_RESOURCE_CLUT, x, y, width, height,
              pixels, pixel_count)));
    if (operation == XG_RENDER_VRAM_MOVE &&
        xg_render_vram_move_command_matches(
            source_x, source_y, x, y, width, height,
            event != NULL ? event->command_opcode : 0u,
            event != NULL ? event->command_words : NULL,
            event != NULL ? event->command_word_count : 0u,
            event != NULL && event->command_context_valid)) {
        XgRenderSurfacePublication
            publications[XG_RENDER_SURFACE_GRAPH_NODE_CAPACITY];
        size_t publication_count = 0u;
        uint64_t source_generation = 0u;
        bool found = false;

        if ((uint32_t)source_x + width <= 1024u &&
            (uint32_t)source_y + height <= 512u)
            move_source_pixels = (uint16_t *)malloc(payload_size);
        if (move_source_pixels != NULL &&
            xg_render_vram_journal_copy_authoritative_rect(
                source_x, source_y, width, height, move_source_pixels,
                pixel_count, &source_generation)) {
            const uint64_t digest = xg_render_resource_digest(
                move_source_pixels, payload_size);
            const uint64_t identity_words[4] = {
                UINT64_C(0x78672d7672616d31),
                source_generation,
                (uint64_t)source_x | (uint64_t)source_y << 16u |
                    (uint64_t)width << 32u | (uint64_t)height << 48u,
                digest,
            };
            XgRenderResourceCapabilityMetadata metadata = {
                .kind = XG_RENDER_RESOURCE_PROVENANCE_SOURCE,
                .receipt = source_generation,
                .lifetime = XG_RENDER_RESOURCE_CAPABILITY_SCENE,
                .owner_kind = XG_RENDER_RESOURCE_OWNER_SCENE,
                .owner_generation = state.scene_generation,
                .source = {
                    .source_class = XG_RENDER_RESOURCE_SOURCE_VRAM_TRANSFER,
                    .range_offset = (uint64_t)source_y * 1024u + source_x,
                    .range_size = payload_size,
                    .range_content_digest = digest,
                },
            };

            memcpy(move_source_identity.bytes, identity_words,
                   sizeof(move_source_identity.bytes));
            metadata.source.identity = move_source_identity;
            found = xg_render_resource_capability_register_tracked(
                &metadata, &move_source_provenance,
                &move_source_capability_created) ==
                    XG_RENDER_RESOURCE_CAPABILITY_OK;
            if (found) description.required_source_generation = source_generation;
        }
        if (move_source_pixels != NULL &&
            !found &&
            xg_render_surface_graph_copy_publications(
                publications, XG_RENDER_SURFACE_GRAPH_NODE_CAPACITY,
                &publication_count) == XG_RENDER_SURFACE_GRAPH_OK)
        for (size_t index = 0u; index < publication_count; ++index) {
            const XgRenderSurfacePublication *publication =
                &publications[index];
            const XgRenderResourceDescriptor *descriptor =
                &publication->descriptor;
            XgRenderResourceCapabilityMetadata authority;
            XgRenderResourceView view;

            if (publication->format != XG_RENDER_SURFACE_VRAM16 ||
                (descriptor->flags &
                 XG_RENDER_RESOURCE_DESCRIPTOR_HAS_VRAM_REGION) == 0u ||
                descriptor->pixel_format !=
                    XG_RENDER_RESOURCE_PIXEL_FORMAT_RGB555 ||
                descriptor->vram_x > source_x ||
                descriptor->vram_y > source_y ||
                (uint64_t)source_x + width >
                    (uint64_t)descriptor->vram_x + descriptor->vram_width ||
                (uint64_t)source_y + height >
                    (uint64_t)descriptor->vram_y + descriptor->vram_height)
                continue;
            if (found ||
                xg_render_resource_view(publication->handle, &view) !=
                    XG_RENDER_RESOURCE_OK || !view.current ||
                !view.has_identity || view.bytes == NULL ||
                view.content_digest != publication->content_digest ||
                view.byte_count != publication->byte_count ||
                memcmp(view.identity.bytes, publication->identity.bytes,
                       sizeof(view.identity.bytes)) != 0 ||
                xg_render_resource_capability_validate(
                    &view.provenance, XG_RENDER_RESOURCE_OWNER_SCENE,
                    state.scene_generation, &authority) !=
                        XG_RENDER_RESOURCE_CAPABILITY_OK ||
                authority.kind != XG_RENDER_RESOURCE_PROVENANCE_SOURCE ||
                memcmp(authority.source.identity.bytes,
                       publication->identity.bytes,
                       sizeof(publication->identity.bytes)) != 0) {
                found = false;
                break;
            }
            for (uint32_t row = 0u; row < height; ++row) {
                const size_t source_offset =
                    ((size_t)(source_y - descriptor->vram_y + row) *
                         publication->width +
                     (source_x - descriptor->vram_x)) * 2u;

                for (uint32_t column = 0u; column < width; ++column) {
                    const uint8_t *source =
                        (const uint8_t *)view.bytes + source_offset +
                        (size_t)column * 2u;
                    move_source_pixels[(size_t)row * width + column] =
                        (uint16_t)source[0] | (uint16_t)source[1] << 8u;
                }
            }
            move_source_identity = publication->identity;
            move_source_provenance = publication->provenance;
            found = true;
        }
        if (found && memcmp(move_source_pixels, pixels, payload_size) == 0) {
            move_source_authenticated = true;
            payload_authenticated = true;
        } else {
            free(move_source_pixels);
            move_source_pixels = NULL;
        }
    }
    description.payload_authenticated = payload_authenticated;
    if (xg_render_vram_transfer_begin(&description, &transfer) !=
            XG_RENDER_VRAM_OK)
        goto done;
    while (pixel_offset < pixel_count) {
        const size_t chunk_pixels = pixel_count - pixel_offset <
                ENCODED_CHUNK_SIZE / 2u
            ? pixel_count - pixel_offset : ENCODED_CHUNK_SIZE / 2u;

        for (size_t index = 0u; index < chunk_pixels; ++index) {
            const uint16_t pixel = transfer_pixels[pixel_offset + index];

            encoded[index * 2u] = (uint8_t)pixel;
            encoded[index * 2u + 1u] = (uint8_t)(pixel >> 8u);
        }
        if (xg_render_vram_transfer_write(
                transfer, pixel_offset * 2u, encoded,
                chunk_pixels * 2u) != XG_RENDER_VRAM_OK) {
            (void)xg_render_vram_transfer_cancel(transfer);
            goto done;
        }
        pixel_offset += chunk_pixels;
    }
    if (xg_render_vram_transfer_complete(transfer, &mutation) !=
            XG_RENDER_VRAM_OK) {
        (void)xg_render_vram_transfer_cancel(transfer);
        goto done;
    }
    if (operation != XG_RENDER_VRAM_READBACK)
        xg_render_vram_resources_note_vram_mutation_context(
            mutation.source_x, mutation.source_y,
            x, y, width, height, mutation.source_generation,
            mutation.serial, mutation.content_digest,
            (uint32_t)mutation.operation, (uint32_t)mutation.direction,
            event != NULL ? event->command_source_address : 0u,
            event != NULL ? event->command_pc : 0u,
            event != NULL ? event->command_function : 0u,
            event != NULL ? event->command_return_address : 0u,
            event != NULL ? event->command_source_kind : 0u,
            event != NULL ? event->command_opcode : 0u,
            event != NULL ? event->command_words : NULL,
            event != NULL ? event->command_word_count : 0u,
            event != NULL && event->command_context_valid,
            move_source_authenticated ? &move_source_identity : NULL,
            move_source_authenticated
                ? &move_source_provenance : NULL,
            transfer_pixels, pixel_count,
            operation == XG_RENDER_VRAM_UPLOAD);
    success = true;

done:
    if (move_source_capability_created)
        (void)xg_render_resource_capability_revoke(move_source_provenance);
    unlock_presentation_lifecycle();
    free(move_source_pixels);
    return success;
}

static bool publish_mdec_generated_surface(const GpuVramEvent *event) {
    XgRenderSurfacePublicationDescription description = {0};
    XgRenderSurfacePublication publication;
    XgRenderSurfacePublication superseded_publication;
    XgRenderResourceProvenance registered_provenance = {0};
    uint8_t identity_payload[12];
    uint8_t *encoded;
    uint64_t resource_id = UINT64_C(1469598103934665603);
    uint32_t surface_width;
    size_t byte_count;
    bool capability_created = false;
    bool has_superseded_publication = false;
    bool success = false;

    if (event == NULL ||
        event->payload_source != GPU_VRAM_PAYLOAD_SOURCE_MDEC_DMA1 ||
        event->payload_source_receipt == 0u || event->pixels == NULL ||
        (event->payload_format != 2u && event->payload_format != 3u))
        return false;
    if (event->payload_format == 2u) {
        if (((uint32_t)event->width * 2u) % 3u != 0u) return false;
        surface_width = (uint32_t)event->width * 2u / 3u;
        description.format = XG_RENDER_SURFACE_DEPTH24;
    } else {
        surface_width = event->width;
        description.format = XG_RENDER_SURFACE_VRAM16;
    }
    if (surface_width == 0u || event->pixel_count > SIZE_MAX / 2u)
        return false;
    atomic_store_explicit(
        &movie_mdec_surface_x, event->destination_x, memory_order_relaxed);
    atomic_store_explicit(
        &movie_mdec_surface_y, event->destination_y, memory_order_relaxed);
    atomic_store_explicit(
        &movie_mdec_surface_width, surface_width, memory_order_relaxed);
    atomic_store_explicit(
        &movie_mdec_surface_height, event->height, memory_order_relaxed);
    byte_count = event->pixel_count * 2u;
    encoded = (uint8_t *)malloc(byte_count);
    if (encoded == NULL) return false;
    for (size_t index = 0u; index < event->pixel_count; ++index) {
        encoded[index * 2u] = (uint8_t)event->pixels[index];
        encoded[index * 2u + 1u] = (uint8_t)(event->pixels[index] >> 8u);
    }
    identity_payload[0] = (uint8_t)event->destination_x;
    identity_payload[1] = (uint8_t)(event->destination_x >> 8u);
    identity_payload[2] = (uint8_t)event->destination_y;
    identity_payload[3] = (uint8_t)(event->destination_y >> 8u);
    identity_payload[4] = (uint8_t)event->width;
    identity_payload[5] = (uint8_t)(event->width >> 8u);
    identity_payload[6] = (uint8_t)event->height;
    identity_payload[7] = (uint8_t)(event->height >> 8u);
    identity_payload[8] = (uint8_t)event->payload_format;
    identity_payload[9] = 'M';
    identity_payload[10] = 'D';
    identity_payload[11] = '1';
    for (size_t index = 0u; index < sizeof(identity_payload); ++index) {
        resource_id ^= identity_payload[index];
        resource_id *= UINT64_C(1099511628211);
    }
    if (resource_id == 0u) resource_id = 1u;
    for (size_t index = 0u; index < sizeof(resource_id); ++index)
        description.identity.bytes[index] =
            (uint8_t)(resource_id >> (index * 8u));
    memcpy(description.identity.bytes + sizeof(resource_id),
           identity_payload, sizeof(identity_payload));
    description.kind = XG_RENDER_RESOURCE_GENERATED_SURFACE;
    description.owner_generation = state.scene_generation;
    description.width = surface_width;
    description.height = event->height;
    description.descriptor = (XgRenderResourceDescriptor){
        .version = XG_RENDER_RESOURCE_DESCRIPTOR_VERSION,
        .pixel_format = event->payload_format == 2u
            ? XG_RENDER_RESOURCE_PIXEL_FORMAT_RGB24
            : XG_RENDER_RESOURCE_PIXEL_FORMAT_RGB555,
        .width = surface_width,
        .height = event->height,
        .row_pitch = (uint32_t)((size_t)surface_width *
            (event->payload_format == 2u ? 3u : 2u)),
        .vram_x = event->payload_format == 3u ? event->destination_x : 0u,
        .vram_y = event->payload_format == 3u ? event->destination_y : 0u,
        .vram_width = event->payload_format == 3u ? surface_width : 0u,
        .vram_height = event->payload_format == 3u ? event->height : 0u,
        .flags = event->payload_format == 3u
            ? XG_RENDER_RESOURCE_DESCRIPTOR_HAS_VRAM_REGION : 0u,
    };
    description.bytes = encoded;
    description.byte_count = byte_count;
    lock_presentation_lifecycle();
    {
        XgRenderResourceCapabilityMetadata metadata = {
            .kind = XG_RENDER_RESOURCE_PROVENANCE_SOURCE,
            .receipt = event->payload_source_receipt,
            .lifetime = XG_RENDER_RESOURCE_CAPABILITY_SCENE,
            .owner_kind = XG_RENDER_RESOURCE_OWNER_SCENE,
            .owner_generation = state.scene_generation,
            .source = {
                .source_class = XG_RENDER_RESOURCE_SOURCE_MDEC_DMA_RAM_GPU,
                .range_size = byte_count,
                .range_content_digest =
                    xg_render_resource_digest(encoded, byte_count),
            },
        };
        XgRenderResourceCapabilityMetadata artifact_metadata;

        metadata.source.identity = description.identity;
        if (xg_render_resource_capability_validate(
                &state.authenticated_artifact_provenance,
                XG_RENDER_RESOURCE_OWNER_SCENE, state.scene_generation,
                &artifact_metadata) == XG_RENDER_RESOURCE_CAPABILITY_OK)
            metadata.source.origin_artifact = artifact_metadata.artifact;
        if (xg_render_resource_capability_register_tracked(
                &metadata, &registered_provenance, &capability_created) !=
                XG_RENDER_RESOURCE_CAPABILITY_OK)
            goto done;
        description.provenance = registered_provenance;
    }
    has_superseded_publication = xg_render_surface_graph_lookup(
        resource_id, &superseded_publication) == XG_RENDER_SURFACE_GRAPH_OK;
    if (description.owner_generation == 0u ||
        xg_render_surface_graph_publish(&description, &publication) !=
            XG_RENDER_SURFACE_GRAPH_OK)
        goto done;
    success = true;
    if (has_superseded_publication &&
        superseded_publication.provenance.capability !=
            description.provenance.capability)
        (void)xg_render_resource_capability_retire(
            superseded_publication.provenance);

done:
    atomic_fetch_add_explicit(success ? &movie_mdec_surface_successes :
        &movie_mdec_surface_failures, 1u, memory_order_relaxed);
    if (!success && capability_created)
        (void)xg_render_resource_capability_revoke(registered_provenance);
    unlock_presentation_lifecycle();
    free(encoded);
    return success;
}

#ifdef XG_RENDER_RUNTIME_COMPOSITION_TESTING
static uint32_t movie_publication_failure_after = UINT32_MAX;

void psx_xg_render_auth_runtime_test_fail_movie_publication_after(
        uint32_t successful_preparations) {
    movie_publication_failure_after = successful_preparations;
}

static bool movie_publication_failure_injected(void) {
    if (movie_publication_failure_after == UINT32_MAX) return false;
    if (movie_publication_failure_after != 0u) {
        movie_publication_failure_after--;
        return false;
    }
    movie_publication_failure_after = UINT32_MAX;
    return true;
}
#else
static bool movie_publication_failure_injected(void) {
    return false;
}
#endif

static bool composition_standalone_source_identity(
        XgSemanticSceneIdentity *out_identity,
        uint32_t *out_scene_generation) {
    const PsxXgRenderAuthCandidate *candidate =
        &state.authenticated_artifact_candidate;
    const XgRenderAuthProfile profile =
        xg_render_static_auth_profile_from_metadata();
    const uint64_t executable_identity =
        identity_prefix_bytes(candidate->identity.game_sha256);
    const uint64_t overlay_identity =
        identity_prefix_bytes(candidate->artifact_sha256);
    XgSemanticModuleKind module;

    if (out_identity == NULL || out_scene_generation == NULL ||
        state.requested_render_mode != GUEST_RENDER_RENDER_NATIVE ||
        state.scene_generation == 0u || state.scene_generation > UINT32_MAX ||
        !xg_render_static_auth_metadata_is_valid() || profile.disc_id == 0u ||
        !current_artifact_is_authorized() ||
        !standalone_semantic_module(candidate, &module) ||
        state.authenticated_artifact_provenance.capability == 0u ||
        executable_identity == 0u || overlay_identity == 0u)
        return false;
    *out_identity = (XgSemanticSceneIdentity){
        .disc_id = profile.disc_id,
        .executable_identity = executable_identity,
        .primary_overlay_identity = overlay_identity,
        .module = module,
    };
    *out_scene_generation = (uint32_t)state.scene_generation;
    return true;
}

static bool movie_source_frame_description(
        const GpuVramEvent *event, uint32_t visual_width,
        XgRenderSourceFrameDescription *out_description) {
    const PsxXgRenderAuthCandidate *candidate =
        &state.movie_frame_artifact_candidate;
    const uint64_t executable_identity =
        identity_prefix_bytes(candidate->identity.game_sha256);
    uint64_t artifact_identity;

    if (event == NULL || out_description == NULL || visual_width == 0u ||
        visual_width > UINT16_MAX || event->height == 0u ||
        state.requested_render_mode != GUEST_RENDER_RENDER_NATIVE ||
        state.scene_generation == 0u || state.scene_generation > UINT32_MAX ||
        !state.movie_frame_artifact_candidate_valid ||
        !movie_artifact_candidate_matches(candidate) ||
        !event->movie_frame_complete || event->movie_owner_receipt == 0u ||
        (event->movie_owner_kind != GPU_MOVIE_OWNER_STANDALONE &&
         event->movie_owner_kind != GPU_MOVIE_OWNER_FIELD) ||
        executable_identity == 0u)
        return false;
    artifact_identity = identity_prefix_bytes(candidate->artifact_sha256);
    if (artifact_identity == 0u) return false;
    *out_description = (XgRenderSourceFrameDescription){
        .scene = {
            .disc_id = 1u,
            .executable_identity = executable_identity,
            .primary_overlay_identity = artifact_identity,
            .companion_set_identity = event->movie_owner_kind ==
                    GPU_MOVIE_OWNER_FIELD
                ? event->movie_owner_receipt : 0u,
            .module = event->movie_owner_kind == GPU_MOVIE_OWNER_FIELD
                ? XG_SEMANTIC_MODULE_FIELD : XG_SEMANTIC_MODULE_MOVIE,
        },
        .display = {
            .width = (uint16_t)visual_width,
            .height = event->height,
            .display_x = event->source_x,
            .display_y = event->source_y,
            .aspect_num = 4u,
            .aspect_den = 3u,
            .depth24 = event->payload_format == 2u,
        },
        .scene_generation = (uint32_t)state.scene_generation,
        .source_interval_vblanks = event->movie_owner_kind ==
                GPU_MOVIE_OWNER_STANDALONE
            ? 8u : 0u,
        .temporally_eligible = false,
    };
    return true;
}

static bool movie_frame_artifact_identity(
        XgRenderArtifactIdentity *out_identity) {
    const PsxXgRenderAuthCandidate *candidate =
        &state.movie_frame_artifact_candidate;

    if (out_identity == NULL ||
        !state.movie_frame_artifact_candidate_valid ||
        !movie_artifact_candidate_matches(candidate))
        return false;
    *out_identity = (XgRenderArtifactIdentity){
        .base = candidate->artifact_base,
        .size = candidate->artifact_size,
    };
    memcpy(out_identity->sha256, candidate->artifact_sha256,
           sizeof(out_identity->sha256));
    (void)xg_render_ui_owner_catalog_artifact_identity(
        candidate->artifact_base, candidate->artifact_size,
        candidate->artifact_sha256, out_identity);
    return true;
}

static bool movie_frame_artifact_capability_prepare(
        const GpuVramEvent *event, bool *out_created) {
    const PsxXgRenderAuthCandidate *candidate =
        &state.movie_frame_artifact_candidate;
    XgRenderResourceCapabilityMetadata metadata;

    if (out_created == NULL) return false;
    *out_created = false;
    if (event == NULL || event->movie_owner_receipt == 0u ||
        !state.movie_frame_artifact_candidate_valid ||
        !movie_artifact_candidate_matches(candidate))
        return false;
    metadata = (XgRenderResourceCapabilityMetadata){
        .kind = XG_RENDER_RESOURCE_PROVENANCE_ARTIFACT,
        .receipt = event->movie_owner_receipt,
        .lifetime = XG_RENDER_RESOURCE_CAPABILITY_SCENE,
        .owner_kind = XG_RENDER_RESOURCE_OWNER_SCENE,
        .owner_generation = state.scene_generation,
        .artifact = {
            .base = candidate->artifact_base,
            .size = candidate->artifact_size,
        },
    };
    memcpy(metadata.artifact.sha256, candidate->artifact_sha256,
           sizeof(metadata.artifact.sha256));
    (void)xg_render_ui_owner_catalog_artifact_identity(
        candidate->artifact_base, candidate->artifact_size,
        candidate->artifact_sha256, &metadata.artifact);
    return xg_render_resource_capability_register_tracked(
        &metadata, &state.movie_frame_artifact_provenance, out_created) ==
            XG_RENDER_RESOURCE_CAPABILITY_OK;
}

static bool publish_mdec_framebuffer(
        const GpuVramEvent *event, uint64_t guest_cycle) {
    XgRenderSurfacePublicationDescription description = {0};
    XgRenderSurfacePublication framebuffer;
    XgRenderSurfacePublication
        publications[XG_RENDER_SURFACE_GRAPH_NODE_CAPACITY];
    XgSemanticSurfaceEdge
        movie_edges[XG_RENDER_SURFACE_GRAPH_NODE_CAPACITY];
    XgRenderSurfaceGraphTransaction *graph_transaction = NULL;
    XgRenderSurfaceGraphPublicationRollback *source_target_rollback = NULL;
    XgRenderMovieFrameTransaction *movie_transaction = NULL;
    XgRenderFragmentRuntimeTransaction *fragment_transaction = NULL;
    XgRenderMoviePublisherSnapshot *publisher_before = NULL;
    XgRenderMovieFramePublication movie_publication = {0};
    XgRenderResourceHandle movie_resource = {0};
    XgRenderResourceHandle previous_movie_surface = {0};
    XgSemanticResourceRef source_target = {0};
    XgRenderResourceProvenance framebuffer_provenance = {0};
    XgRenderResourceProvenance movie_owner_provenance = {0};
    XgRenderResourceProvenance superseded_framebuffer_provenance = {0};
    XgRenderResourceProvenance superseded_movie_provenance = {0};
    XgRenderSurfaceGraphResult graph_result;
    XgRenderSourceFrameDescription source_frame_description;
    uint8_t identity_payload[12];
    uint8_t *encoded;
    uint64_t resource_id = UINT64_C(1469598103934665603);
    XgRenderMovieOwnerKind movie_owner_kind = XG_RENDER_MOVIE_OWNER_NONE;
    uint32_t visual_width;
    size_t byte_count;
    uint8_t *coverage = NULL;
    size_t publication_count = 0u;
    size_t movie_edge_count = 0u;
    size_t coverage_byte_count = 0u;
    size_t publisher_snapshot_size = 0u;
    bool framebuffer_capability_created = false;
    bool movie_capability_created = false;
    bool movie_artifact_capability_created = false;
    bool has_superseded_framebuffer = false;
    bool has_superseded_movie = false;
    bool complete_movie_coverage = false;
    bool movie_transaction_committed = false;
    bool movie_resource_owned = false;
    bool previous_movie_surface_retained = false;
    bool new_movie_surface_retained = false;
    bool presentation_locked = false;
    bool success = false;

    if (event == NULL || event->operation != GPU_VRAM_EVENT_SCANOUT ||
        event->mutation_serial == 0u ||
        event->pixels == NULL ||
        (event->payload_format != 2u && event->payload_format != 3u) ||
        event->pixel_count > SIZE_MAX / 2u)
        return false;
    atomic_store_explicit(
        &movie_publication_last_blocker_detail, 0u, memory_order_relaxed);
    if (event->payload_format == 2u) {
        if (((uint32_t)event->width * 2u) % 3u != 0u) return false;
        visual_width = (uint32_t)event->width * 2u / 3u;
        description.format = XG_RENDER_SURFACE_DEPTH24;
    } else {
        visual_width = event->width;
        description.format = XG_RENDER_SURFACE_VRAM16;
    }
    if (visual_width == 0u) return false;
    atomic_store_explicit(&movie_scanout_x,
        event->source_x, memory_order_relaxed);
    atomic_store_explicit(&movie_scanout_y,
        event->source_y, memory_order_relaxed);
    atomic_store_explicit(&movie_scanout_width,
        visual_width, memory_order_relaxed);
    atomic_store_explicit(&movie_scanout_height,
        event->height, memory_order_relaxed);
    byte_count = event->pixel_count * 2u;
    encoded = (uint8_t *)malloc(byte_count);
    if (encoded == NULL) return false;
    for (size_t index = 0u; index < event->pixel_count; ++index) {
        encoded[index * 2u] = (uint8_t)event->pixels[index];
        encoded[index * 2u + 1u] = (uint8_t)(event->pixels[index] >> 8u);
    }
    switch (event->movie_owner_kind) {
        case GPU_MOVIE_OWNER_STANDALONE:
            movie_owner_kind = XG_RENDER_MOVIE_OWNER_STANDALONE;
            break;
        case GPU_MOVIE_OWNER_FIELD:
            movie_owner_kind = XG_RENDER_MOVIE_OWNER_FIELD;
            break;
        case GPU_MOVIE_OWNER_NONE:
        default:
            break;
    }
    if (event->movie_frame_complete &&
        event->movie_frame_width == visual_width &&
        event->movie_frame_height == event->height &&
        movie_owner_kind != XG_RENDER_MOVIE_OWNER_NONE &&
        event->movie_owner_receipt != 0u) {
        if (!movie_frame_artifact_capability_prepare(
                event, &movie_artifact_capability_created)) {
            atomic_store_explicit(
                &movie_publication_last_blocker, 1u, memory_order_relaxed);
            goto done;
        }
        coverage = (uint8_t *)calloc(byte_count, 1u);
        if (coverage == NULL) goto done;
        complete_movie_coverage = true;
        if (!movie_source_frame_description(
                event, visual_width, &source_frame_description)) {
            atomic_store_explicit(
                &movie_publication_last_blocker, 2u, memory_order_relaxed);
            goto done;
        }
        if (xg_render_fragment_runtime_transaction_begin(&fragment_transaction) !=
                XG_RENDER_FRAGMENT_RUNTIME_OK ||
            !composition_begin_movie_source_frame(
                &source_frame_description, &source_target,
                &source_target_rollback)) {
            atomic_store_explicit(
                &movie_publication_last_blocker, 3u, memory_order_relaxed);
            goto done;
        }
        atomic_store_explicit(
            &movie_publication_last_blocker, 4u, memory_order_relaxed);
    }
    identity_payload[0] = (uint8_t)event->source_x;
    identity_payload[1] = (uint8_t)(event->source_x >> 8u);
    identity_payload[2] = (uint8_t)event->source_y;
    identity_payload[3] = (uint8_t)(event->source_y >> 8u);
    identity_payload[4] = (uint8_t)event->width;
    identity_payload[5] = (uint8_t)(event->width >> 8u);
    identity_payload[6] = (uint8_t)event->height;
    identity_payload[7] = (uint8_t)(event->height >> 8u);
    identity_payload[8] = (uint8_t)event->payload_format;
    identity_payload[9] = 'F';
    identity_payload[10] = 'B';
    identity_payload[11] = '1';
    for (size_t index = 0u; index < sizeof(identity_payload); ++index) {
        resource_id ^= identity_payload[index];
        resource_id *= UINT64_C(1099511628211);
    }
    if (resource_id == 0u) resource_id = 1u;
    for (size_t index = 0u; index < sizeof(resource_id); ++index)
        description.identity.bytes[index] =
            (uint8_t)(resource_id >> (index * 8u));
    memcpy(description.identity.bytes + sizeof(resource_id),
           identity_payload, sizeof(identity_payload));
    description.kind = XG_RENDER_RESOURCE_FRAMEBUFFER;
    description.owner_generation = state.scene_generation;
    description.width = visual_width;
    description.height = event->height;
    description.descriptor = (XgRenderResourceDescriptor){
        .version = XG_RENDER_RESOURCE_DESCRIPTOR_VERSION,
        .pixel_format = event->payload_format == 2u
            ? XG_RENDER_RESOURCE_PIXEL_FORMAT_RGB24
            : XG_RENDER_RESOURCE_PIXEL_FORMAT_RGB555,
        .width = visual_width,
        .height = event->height,
        .row_pitch = (uint32_t)((size_t)visual_width *
            (event->payload_format == 2u ? 3u : 2u)),
    };
    description.bytes = encoded;
    description.byte_count = byte_count;
    lock_presentation_lifecycle();
    presentation_locked = true;
    {
        XgRenderResourceCapabilityMetadata metadata = {
            .kind = XG_RENDER_RESOURCE_PROVENANCE_SOURCE,
            .receipt = event->mutation_serial,
            .lifetime = XG_RENDER_RESOURCE_CAPABILITY_SCENE,
            .owner_kind = XG_RENDER_RESOURCE_OWNER_SCENE,
            .owner_generation = state.scene_generation,
            .source = {
                .source_class = XG_RENDER_RESOURCE_SOURCE_GPU_SCANOUT,
                .range_size = byte_count,
                .range_content_digest =
                    xg_render_resource_digest(encoded, byte_count),
            },
        };
        XgRenderResourceCapabilityMetadata artifact_metadata;

        metadata.source.identity = description.identity;
        if (!(event->movie_frame_complete && movie_frame_artifact_identity(
                  &metadata.source.origin_artifact)) &&
            xg_render_resource_capability_validate(
                &state.authenticated_artifact_provenance,
                XG_RENDER_RESOURCE_OWNER_SCENE, state.scene_generation,
                &artifact_metadata) == XG_RENDER_RESOURCE_CAPABILITY_OK)
            metadata.source.origin_artifact = artifact_metadata.artifact;
        if (xg_render_resource_capability_register_tracked(
                &metadata, &framebuffer_provenance,
                &framebuffer_capability_created) !=
                XG_RENDER_RESOURCE_CAPABILITY_OK) {
            atomic_store_explicit(
                &movie_publication_last_blocker, 5u, memory_order_relaxed);
            goto done;
        }
        description.provenance = framebuffer_provenance;
    }

    graph_result = description.owner_generation == 0u
        ? XG_RENDER_SURFACE_GRAPH_INVALID_ARGUMENT
        : xg_render_surface_graph_transaction_begin(
            &description, &graph_transaction, &framebuffer);
    if (graph_result != XG_RENDER_SURFACE_GRAPH_OK) {
        atomic_store_explicit(
            &movie_publication_last_blocker, 6u, memory_order_relaxed);
        atomic_store_explicit(&movie_publication_last_blocker_detail,
            UINT32_C(0x10000) | (uint32_t)graph_result,
            memory_order_relaxed);
        goto done;
    }
    if (movie_publication_failure_injected()) {
        atomic_store_explicit(
            &movie_publication_last_blocker, 6u, memory_order_relaxed);
        atomic_store_explicit(&movie_publication_last_blocker_detail,
            UINT32_C(0x20000), memory_order_relaxed);
        goto done;
    }
    graph_result = xg_render_surface_graph_copy_publications(
        publications, XG_RENDER_SURFACE_GRAPH_NODE_CAPACITY,
        &publication_count);
    if (graph_result != XG_RENDER_SURFACE_GRAPH_OK) {
        atomic_store_explicit(
            &movie_publication_last_blocker, 6u, memory_order_relaxed);
        atomic_store_explicit(&movie_publication_last_blocker_detail,
            UINT32_C(0x30000) | (uint32_t)graph_result,
            memory_order_relaxed);
        goto done;
    }
    atomic_store_explicit(&movie_event_publication_count,
        (unsigned)publication_count, memory_order_relaxed);
    for (size_t index = 0u; index < publication_count; ++index) {
        const XgRenderSurfacePublication *surface = &publications[index];
        const uint8_t *identity = surface->identity.bytes + sizeof(uint64_t);
        XgRenderResourceView view;
        uint32_t x;
        uint32_t y;
        uint32_t raw_width;
        uint32_t height;
        uint32_t clip_x0;
        uint32_t clip_y0;
        uint32_t clip_x1;
        uint32_t clip_y1;
        uint32_t clip_width;
        uint32_t clip_height;
        uint32_t edge_width;
        uint32_t edge_source_x;
        uint32_t edge_destination_x;
        bool matches = true;

        if (surface->handle.resource_id == framebuffer.handle.resource_id) {
            superseded_framebuffer_provenance = surface->provenance;
            has_superseded_framebuffer = true;
        }
        if (surface->kind != XG_RENDER_RESOURCE_GENERATED_SURFACE ||
            surface->provenance.kind !=
                XG_RENDER_RESOURCE_PROVENANCE_SOURCE ||
            surface->provenance.receipt == 0u ||
            surface->provenance.synthetic ||
            identity[9] != 'M' || identity[10] != 'D' || identity[11] != '1' ||
            identity[8] != event->payload_format)
            continue;
        x = (uint32_t)identity[0] | (uint32_t)identity[1] << 8u;
        y = (uint32_t)identity[2] | (uint32_t)identity[3] << 8u;
        raw_width = (uint32_t)identity[4] | (uint32_t)identity[5] << 8u;
        height = (uint32_t)identity[6] | (uint32_t)identity[7] << 8u;
        if (raw_width == 0u || height == 0u ||
            x > UINT32_MAX - raw_width || y > UINT32_MAX - height ||
            event->source_x > UINT32_MAX - event->width ||
            event->source_y > UINT32_MAX - event->height)
            continue;
        clip_x0 = x > event->source_x ? x : event->source_x;
        clip_y0 = y > event->source_y ? y : event->source_y;
        clip_x1 = x + raw_width < event->source_x + event->width
            ? x + raw_width : event->source_x + event->width;
        clip_y1 = y + height < event->source_y + event->height
            ? y + height : event->source_y + event->height;
        if (clip_x0 >= clip_x1 || clip_y0 >= clip_y1 ||
            xg_render_resource_view(surface->handle, &view) !=
                XG_RENDER_RESOURCE_OK ||
            view.byte_count != (size_t)raw_width * height * 2u)
            continue;
        clip_width = clip_x1 - clip_x0;
        clip_height = clip_y1 - clip_y0;
        edge_width = event->payload_format == 2u
            ? clip_width * 2u / 3u : clip_width;
        edge_source_x = clip_x0 - x;
        edge_destination_x = clip_x0 - event->source_x;
        if (event->payload_format == 2u) {
            if ((edge_source_x * 2u) % 3u != 0u ||
                (edge_destination_x * 2u) % 3u != 0u)
                continue;
            edge_source_x = edge_source_x * 2u / 3u;
            edge_destination_x = edge_destination_x * 2u / 3u;
        }
        if (edge_width == 0u || edge_width > UINT16_MAX ||
            clip_height > UINT16_MAX)
            continue;
        for (uint32_t row = 0u; row < clip_height; ++row) {
            const size_t surface_offset =
                ((size_t)(clip_y0 - y + row) * raw_width + clip_x0 - x) * 2u;
            const size_t framebuffer_offset =
                ((size_t)(clip_y0 - event->source_y + row) * event->width +
                 (clip_x0 - event->source_x)) * 2u;
            if (memcmp((const uint8_t *)view.bytes + surface_offset,
                       encoded + framebuffer_offset,
                       (size_t)clip_width * 2u) != 0) {
                matches = false;
                break;
            }
        }
        if (matches)
            movie_edges[movie_edge_count++] = (XgSemanticSurfaceEdge){
                .source_surface_id = surface->handle.resource_id,
                .source_generation = surface->handle.generation,
                .target_surface_id = framebuffer.handle.resource_id,
                .target_generation = framebuffer.handle.generation,
                .kind = XG_SEMANTIC_SURFACE_MOVIE,
                .order = {.pass_id = 0u},
                .source = {
                    .x = (int32_t)edge_source_x,
                    .y = (int32_t)(clip_y0 - y),
                    .width = edge_width,
                    .height = clip_height,
                },
                .destination = {
                    .x = (int32_t)edge_destination_x,
                    .y = (int32_t)(clip_y0 - event->source_y),
                    .width = edge_width,
                    .height = clip_height,
                },
                .sample = {
                    .sampler = XG_RENDER_RESOURCE_SAMPLER_NEAREST,
                    .wrap_u = XG_RENDER_RESOURCE_WRAP_CLAMP,
                    .wrap_v = XG_RENDER_RESOURCE_WRAP_CLAMP,
                },
            };
        if (matches && complete_movie_coverage) {
            for (uint32_t row = 0u; row < clip_height; ++row) {
                const size_t framebuffer_offset =
                    ((size_t)(clip_y0 - event->source_y + row) * event->width +
                     (clip_x0 - event->source_x)) * 2u;
                const size_t row_bytes = (size_t)clip_width * 2u;

                for (size_t byte = 0u; byte < row_bytes; ++byte) {
                    if (coverage[framebuffer_offset + byte] != 0u) {
                        complete_movie_coverage = false;
                        break;
                    }
                    coverage[framebuffer_offset + byte] = 1u;
                    coverage_byte_count++;
                }
                if (!complete_movie_coverage) break;
            }
        }
    }
    atomic_store_explicit(&movie_event_edge_count,
        (unsigned)movie_edge_count, memory_order_relaxed);
    atomic_store_explicit(&movie_event_coverage_bytes,
        coverage_byte_count, memory_order_relaxed);
    for (size_t index = 0u; index < movie_edge_count; ++index)
        if (xg_render_surface_graph_transaction_append_edge(
                graph_transaction, &movie_edges[index]) !=
                XG_RENDER_SURFACE_GRAPH_OK ||
            movie_publication_failure_injected()) {
            atomic_store_explicit(
                &movie_publication_last_blocker, 7u, memory_order_relaxed);
            goto done;
        }
    if (complete_movie_coverage) {
        XgRenderMovieFrameDescription movie_description;
        XgRenderSurfaceAttachmentDescription movie_attachment;
        XgRenderSurfacePublication movie_surface;
        XgSemanticSurfaceEdge framebuffer_movie_edge;
        XgRenderResourceView movie_view;
        uint64_t movie_id = UINT64_C(0x4d4f56494546524d);

        for (size_t byte = 0u; byte < byte_count; ++byte)
            if (coverage[byte] == 0u) {
                complete_movie_coverage = false;
                break;
            }
        for (size_t byte = 0u; byte < sizeof(event->movie_owner_receipt);
             ++byte) {
            movie_id ^= (uint8_t)(event->movie_owner_receipt >> (byte * 8u));
            movie_id *= UINT64_C(1099511628211);
        }
        movie_id ^= (uint8_t)movie_owner_kind;
        movie_id *= UINT64_C(1099511628211);
        if (movie_id == 0u) movie_id = 1u;
        if (complete_movie_coverage) {
            XgRenderResourceCapabilityMetadata metadata = {
                .kind = XG_RENDER_RESOURCE_PROVENANCE_SOURCE,
                .receipt = event->movie_owner_receipt,
                .lifetime = XG_RENDER_RESOURCE_CAPABILITY_SCENE,
                .owner_kind = XG_RENDER_RESOURCE_OWNER_SOURCE,
                .owner_generation = state.scene_generation,
                .source = {
                    .source_class = XG_RENDER_RESOURCE_SOURCE_MOVIE_OWNER,
                    .range_size = byte_count,
                    .range_content_digest =
                        xg_render_resource_digest(encoded, byte_count),
                },
            };
            XgRenderResourceCapabilityMetadata artifact_metadata;

            memcpy(metadata.source.identity.bytes, &movie_id,
                   sizeof(movie_id));
            metadata.source.identity.bytes[sizeof(movie_id)] =
                (uint8_t)movie_owner_kind;
            if (!movie_frame_artifact_identity(
                    &metadata.source.origin_artifact) &&
                xg_render_resource_capability_validate(
                    &state.authenticated_artifact_provenance,
                    XG_RENDER_RESOURCE_OWNER_SCENE, state.scene_generation,
                    &artifact_metadata) == XG_RENDER_RESOURCE_CAPABILITY_OK)
                metadata.source.origin_artifact = artifact_metadata.artifact;
            if (xg_render_resource_capability_register_tracked(
                    &metadata, &movie_owner_provenance,
                    &movie_capability_created) !=
                    XG_RENDER_RESOURCE_CAPABILITY_OK) {
                atomic_store_explicit(
                    &movie_publication_last_blocker, 9u,
                    memory_order_relaxed);
                goto done;
            }
            for (size_t index = 0u; index < publication_count; ++index) {
                if (publications[index].kind == XG_RENDER_RESOURCE_MOVIE_FRAME &&
                    publications[index].handle.resource_id == movie_id) {
                    superseded_movie_provenance =
                        publications[index].provenance;
                    has_superseded_movie = true;
                    break;
                }
            }
            movie_description = (XgRenderMovieFrameDescription){
                .movie_id = movie_id,
                .owner_kind = movie_owner_kind,
                .owner_receipt = movie_owner_provenance.receipt,
                .owner_capability = movie_owner_provenance.capability,
                .owner_generation = state.scene_generation,
                .guest_cycle = guest_cycle,
                .width = event->movie_frame_width,
                .height = event->movie_frame_height,
                .expected_strips = 1u,
                .byte_count = byte_count,
                .depth24 = event->payload_format == 2u,
            };
            if (xg_render_movie_frame_transaction_prepare(
                    &movie_description, encoded, byte_count,
                    &movie_transaction, &movie_publication) !=
                    XG_RENDER_MOVIE_OK ||
                movie_publication_failure_injected() ||
                xg_render_resource_view(movie_publication.surface,
                    &movie_view) != XG_RENDER_RESOURCE_OK) {
                atomic_store_explicit(
                    &movie_publication_last_blocker, 10u,
                    memory_order_relaxed);
                goto done;
            }
            movie_resource = movie_publication.surface;
            movie_resource_owned = !movie_view.current &&
                movie_view.state == XG_RENDER_RESOURCE_IMPORTING_NATIVE;
            movie_attachment = (XgRenderSurfaceAttachmentDescription){
                .handle = movie_publication.surface,
                .kind = XG_RENDER_RESOURCE_MOVIE_FRAME,
                .owner_kind = XG_RENDER_RESOURCE_OWNER_SOURCE,
                .format = description.format,
                .provenance = movie_view.provenance,
                .owner_generation = movie_view.owner_generation,
                .content_digest = movie_view.content_digest,
                .width = movie_publication.width,
                .height = movie_publication.height,
                .byte_count = movie_view.byte_count,
            };
            framebuffer_movie_edge = (XgSemanticSurfaceEdge){
                .source_surface_id = framebuffer.handle.resource_id,
                .source_generation = framebuffer.handle.generation,
                .target_surface_id = movie_publication.surface.resource_id,
                .target_generation = movie_publication.surface.generation,
                .kind = XG_SEMANTIC_SURFACE_MOVIE,
                .order = {.pass_id = 0u},
                .source = {
                    .width = movie_publication.width,
                    .height = movie_publication.height,
                },
                .destination = {
                    .width = movie_publication.width,
                    .height = movie_publication.height,
                },
                .sample = {
                    .sampler = XG_RENDER_RESOURCE_SAMPLER_NEAREST,
                    .wrap_u = XG_RENDER_RESOURCE_WRAP_CLAMP,
                    .wrap_v = XG_RENDER_RESOURCE_WRAP_CLAMP,
                },
            };
            if (movie_publication.width > UINT16_MAX ||
                movie_publication.height > UINT16_MAX ||
                xg_render_surface_graph_transaction_attach_resource_with_edge(
                    graph_transaction,
                    &movie_attachment, &framebuffer_movie_edge,
                    &movie_surface) != XG_RENDER_SURFACE_GRAPH_OK ||
                movie_publication_failure_injected()) {
                atomic_store_explicit(
                    &movie_publication_last_blocker, 11u,
                    memory_order_relaxed);
                goto done;
            }
        }
    }
    if (event->movie_frame_complete && !complete_movie_coverage) {
        atomic_store_explicit(
            &movie_publication_last_blocker, 8u, memory_order_relaxed);
        goto done;
    }
    if (movie_transaction != NULL) {
        if (xg_render_movie_publisher_snapshot_size(&publisher_snapshot_size) !=
                XG_RENDER_MOVIE_OK ||
            (publisher_before = (XgRenderMoviePublisherSnapshot *)malloc(
                 publisher_snapshot_size)) == NULL ||
            xg_render_movie_publisher_snapshot(
                publisher_before, publisher_snapshot_size) !=
                    XG_RENDER_MOVIE_OK) {
            atomic_store_explicit(
                &movie_publication_last_blocker, 12u, memory_order_relaxed);
            goto done;
        }
        if (publisher_before->complete_frame_available) {
            XgRenderResourceView previous_view;

            previous_movie_surface =
                publisher_before->complete_publication.surface;
            if (xg_render_resource_view(
                    previous_movie_surface, &previous_view) !=
                        XG_RENDER_RESOURCE_OK ||
                !previous_view.current ||
                xg_render_resource_acquire_current(
                    previous_movie_surface, previous_view.content_digest) !=
                        XG_RENDER_RESOURCE_OK) {
                atomic_store_explicit(
                    &movie_publication_last_blocker, 12u,
                    memory_order_relaxed);
                goto done;
            }
            previous_movie_surface_retained = true;
        }
        if (xg_render_movie_frame_transaction_commit(movie_transaction) !=
                XG_RENDER_MOVIE_OK) {
            movie_transaction = NULL;
            atomic_store_explicit(
                &movie_publication_last_blocker, 12u, memory_order_relaxed);
            goto done;
        }
        movie_transaction = NULL;
        movie_transaction_committed = true;
    }
    if (event->movie_frame_complete) {
        XgRenderResourceView movie_view;
        XgSemanticResourceRef movie_reference;
        XgSemanticSurfaceEdge source_edge;
        XgRenderSourceFrameFragment fragment;
        XgRenderSemanticOwnerKey semantic_owner;
        XgRenderSemanticOwnerAuthority semantic_authority;

        if (movie_resource.resource_id == 0u ||
            xg_render_resource_view(movie_resource, &movie_view) !=
                XG_RENDER_RESOURCE_OK || !movie_view.current ||
            xg_render_resource_acquire_current(
                movie_resource, movie_view.content_digest) !=
                    XG_RENDER_RESOURCE_OK) {
            atomic_store_explicit(
                &movie_publication_last_blocker, 13u, memory_order_relaxed);
            goto done;
        }
        new_movie_surface_retained = true;
        movie_reference = (XgSemanticResourceRef){
            .resource_id = movie_resource.resource_id,
            .generation = movie_resource.generation,
            .content_digest = movie_view.content_digest,
        };
        source_edge = (XgSemanticSurfaceEdge){
            .source_surface_id = movie_reference.resource_id,
            .source_generation = movie_reference.generation,
            .target_surface_id = source_target.resource_id,
            .target_generation = source_target.generation,
            .kind = XG_SEMANTIC_SURFACE_MOVIE,
            .order = {.pass_id = 0u},
            .source = {.width = visual_width, .height = event->height},
            .destination = {.width = visual_width, .height = event->height},
            .sample = {
                .sampler = XG_RENDER_RESOURCE_SAMPLER_NEAREST,
                .wrap_u = XG_RENDER_RESOURCE_WRAP_CLAMP,
                .wrap_v = XG_RENDER_RESOURCE_WRAP_CLAMP,
            },
        };
        fragment = (XgRenderSourceFrameFragment){
            .resources = &movie_reference,
            .surface_edges = &source_edge,
            .resource_count = 1u,
            .surface_edge_count = 1u,
        };
        semantic_owner = (XgRenderSemanticOwnerKey){
            .module = source_frame_description.scene.module,
            .domain = XG_RENDER_SEMANTIC_OWNER_MOVIE,
            .primary_root = event->movie_owner_receipt,
            .secondary_root = (uint64_t)movie_owner_kind + 1u,
        };
        semantic_authority = (XgRenderSemanticOwnerAuthority){
            .scene_generation = source_frame_description.scene_generation,
            .owner_generation = state.scene_generation,
            .receipt = event->movie_owner_receipt,
            .proof = movie_owner_provenance.capability,
        };
        source_frame_description.discontinuity =
            source_frame_description.discontinuity ||
            movie_publication.discontinuity;
        if (xg_render_fragment_runtime_ingress_authenticated_fragment(
                &source_frame_description, &semantic_owner,
                &semantic_authority, &fragment) !=
                XG_RENDER_FRAGMENT_RUNTIME_OK) {
            atomic_store_explicit(
                &movie_publication_last_blocker, 14u, memory_order_relaxed);
            goto done;
        }
        if (xg_render_surface_graph_transaction_commit(graph_transaction) !=
                XG_RENDER_SURFACE_GRAPH_OK) {
            graph_transaction = NULL;
            atomic_store_explicit(
                &movie_publication_last_blocker, 15u,
                memory_order_relaxed);
            goto done;
        }
        graph_transaction = NULL;
        xg_render_fragment_runtime_transaction_commit(fragment_transaction);
        fragment_transaction = NULL;
        xg_render_surface_graph_publication_accept(source_target_rollback);
        source_target_rollback = NULL;
        release_retained_movie_surface();
        retained_movie_surface = movie_reference;
        new_movie_surface_retained = false;
    } else {
        if (xg_render_surface_graph_transaction_commit(graph_transaction) !=
                XG_RENDER_SURFACE_GRAPH_OK) {
            graph_transaction = NULL;
            atomic_store_explicit(
                &movie_publication_last_blocker, 15u,
                memory_order_relaxed);
            goto done;
        }
        graph_transaction = NULL;
    }
    success = true;
    if (event->movie_frame_complete) {
        XgRenderFragmentRuntimeDiagnostics fragment_snapshot = {0};

        xg_render_fragment_runtime_diagnostics(&fragment_snapshot);
        atomic_store_explicit(&movie_source_active_after_publication,
            fragment_snapshot.frame_owned ? 1u : 0u, memory_order_relaxed);
        atomic_store_explicit(&movie_source_passes_after_publication,
            fragment_snapshot.frame_owned ? 1u : 0u, memory_order_relaxed);
        state.movie_publication_pending_boundary = true;
        atomic_store_explicit(
            &movie_publication_last_blocker, 0u, memory_order_relaxed);
        atomic_store_explicit(
            &movie_publication_last_blocker_detail, 0u,
            memory_order_relaxed);
    }
    if (has_superseded_movie &&
        superseded_movie_provenance.capability !=
            movie_owner_provenance.capability)
        (void)xg_render_resource_capability_retire(
            superseded_movie_provenance);
    if (has_superseded_framebuffer &&
        superseded_framebuffer_provenance.capability !=
            framebuffer_provenance.capability)
        (void)xg_render_resource_capability_retire(
            superseded_framebuffer_provenance);

done:
    if (!success)
        xg_render_fragment_runtime_transaction_rollback(fragment_transaction);
    else
        xg_render_fragment_runtime_transaction_commit(fragment_transaction);
    fragment_transaction = NULL;
    if (!success) {
        if (xg_render_surface_graph_publication_rollback(
                source_target_rollback) != XG_RENDER_SURFACE_GRAPH_OK &&
            source_target_rollback != NULL)
            xg_render_surface_graph_reset();
    } else {
        xg_render_surface_graph_publication_accept(source_target_rollback);
    }
    source_target_rollback = NULL;
    if (new_movie_surface_retained)
        (void)xg_render_resource_release(movie_resource);
    xg_render_movie_frame_transaction_cancel(movie_transaction);
    xg_render_surface_graph_transaction_cancel(graph_transaction);
    if (!success && movie_transaction_committed &&
        xg_render_movie_publisher_transaction_rollback(
            publisher_before, publisher_snapshot_size, movie_resource,
            previous_movie_surface, movie_resource_owned) !=
                XG_RENDER_MOVIE_OK) {
        (void)xg_render_resource_retire_current(movie_resource);
        xg_render_movie_publisher_reset();
    }
    if (previous_movie_surface_retained)
        (void)xg_render_resource_release(previous_movie_surface);
    free(publisher_before);
    if (!success && movie_capability_created)
        (void)xg_render_resource_capability_revoke(movie_owner_provenance);
    if (!success && framebuffer_capability_created)
        (void)xg_render_resource_capability_revoke(framebuffer_provenance);
    if (!success && movie_artifact_capability_created) {
        (void)xg_render_resource_capability_revoke(
            state.movie_frame_artifact_provenance);
        state.movie_frame_artifact_provenance =
            (XgRenderResourceProvenance){0};
    }
    if (presentation_locked) unlock_presentation_lifecycle();
    free(coverage);
    free(encoded);
    return success;
}

bool psx_xg_render_auth_note_vram_event(
        uint64_t guest_vblank_sequence, uint64_t guest_cycle,
        const GpuVramEvent *event) {
    XgRenderVramOperation operation;
    XgRenderVramTransferDescription description;
    XgRenderVramMutation mutation;
    bool restore_journal_preapplied = false;
    bool success = false;

    if (xg_render_native_work_enabled()) {
        if (event == NULL) return false;
        if (event->operation == GPU_VRAM_EVENT_RESTORE) {
            lock_presentation_lifecycle();
            if (state.boot_restore_timeline_preapplied)
                state.boot_restore_timeline_preapplied = false;
            else
                (void)timeline_invalidate_locked(XG_RENDER_TIMELINE_RESTORE);
            unlock_presentation_lifecycle();
        }
        return xg_render_native_work_vram_event(event, guest_cycle);
    }

    if (event == NULL || event->width == 0u || event->height == 0u ||
        event->pixel_count != (size_t)event->width * event->height)
        return false;
    switch (event->operation) {
        case GPU_VRAM_EVENT_UPLOAD:
            operation = XG_RENDER_VRAM_UPLOAD;
            break;
        case GPU_VRAM_EVENT_READBACK:
            operation = XG_RENDER_VRAM_READBACK;
            break;
        case GPU_VRAM_EVENT_MOVE:
            operation = XG_RENDER_VRAM_MOVE;
            break;
        case GPU_VRAM_EVENT_CLEAR:
            operation = XG_RENDER_VRAM_CLEAR;
            break;
        case GPU_VRAM_EVENT_RENDER_TARGET_WRITE:
            operation = XG_RENDER_VRAM_RENDER_TARGET_WRITE;
            break;
        case GPU_VRAM_EVENT_RESTORE:
            operation = XG_RENDER_VRAM_RESTORE;
            break;
        case GPU_VRAM_EVENT_SCANOUT:
            operation = XG_RENDER_VRAM_SCANOUT;
            break;
        default:
            return false;
    }
    if (operation != XG_RENDER_VRAM_READBACK &&
        operation != XG_RENDER_VRAM_RENDER_TARGET_WRITE &&
        operation != XG_RENDER_VRAM_RESTORE &&
        operation != XG_RENDER_VRAM_SCANOUT) {
        success = note_vram_rect_transfer(
            operation, guest_vblank_sequence, guest_cycle,
            event->mutation_serial, event->source_x, event->source_y,
            event->destination_x, event->destination_y,
            event->width, event->height, event->pixels, event->pixel_count,
            event);
        if (success && operation == XG_RENDER_VRAM_UPLOAD &&
            event->payload_source == GPU_VRAM_PAYLOAD_SOURCE_MDEC_DMA1)
            success = publish_mdec_generated_surface(event);
        return success;
    }

    description = (XgRenderVramTransferDescription){
        .operation = operation,
        .guest_cycle = guest_cycle,
        .source_interval = guest_vblank_sequence,
        .device_mutation_serial = event->mutation_serial,
        .source_x = event->source_x,
        .source_y = event->source_y,
        .x = (operation == XG_RENDER_VRAM_RENDER_TARGET_WRITE ||
              operation == XG_RENDER_VRAM_RESTORE)
            ? event->destination_x : event->source_x,
        .y = (operation == XG_RENDER_VRAM_RENDER_TARGET_WRITE ||
              operation == XG_RENDER_VRAM_RESTORE)
            ? event->destination_y : event->source_y,
        .width = event->width,
        .height = event->height,
        .payload_size = event->pixel_count * sizeof(uint16_t),
        .payload_source_receipt = event->payload_source_receipt,
        .payload_source = (uint32_t)event->payload_source,
        .payload_format = event->payload_format,
        .command_source_address = event->command_source_address,
        .command_pc = event->command_pc,
        .command_function = event->command_function,
        .command_return_address = event->command_return_address,
        .command_source_kind = event->command_source_kind,
        .command_opcode = event->command_opcode,
        .command_word_count = event->command_word_count,
        .command_context_valid = event->command_context_valid,
    };
    {
        size_t command_word_count = event->command_word_count;

        if (command_word_count > 4u) command_word_count = 4u;
        memcpy(description.command_words, event->command_words,
               command_word_count * sizeof(*event->command_words));
    }
    lock_presentation_lifecycle();
    if (operation == XG_RENDER_VRAM_RESTORE) {
        if (state.boot_restore_timeline_preapplied) {
            state.boot_restore_timeline_preapplied = false;
            restore_journal_preapplied = true;
        } else {
            invalidate_authenticated_authority();
            clear_movie_frame_artifact_candidate();
            (void)timeline_invalidate_locked(XG_RENDER_TIMELINE_RESTORE);
        }
    }
    if (restore_journal_preapplied ||
        xg_render_vram_publish_digest(
            &description, event->content_digest, &mutation) ==
                XG_RENDER_VRAM_OK) {
        if (operation == XG_RENDER_VRAM_RENDER_TARGET_WRITE)
            xg_render_vram_resources_note_vram_mutation_context(
                mutation.source_x, mutation.source_y,
                description.x, description.y,
                description.width, description.height,
                mutation.source_generation, mutation.serial,
                mutation.content_digest, (uint32_t)mutation.operation,
                (uint32_t)mutation.direction,
                event->command_source_address, event->command_pc,
                event->command_function, event->command_return_address,
                event->command_source_kind, event->command_opcode,
                event->command_words, event->command_word_count,
                event->command_context_valid, NULL, NULL, NULL, 0u, false);
        success = true;
    }
    unlock_presentation_lifecycle();
    if (success && operation == XG_RENDER_VRAM_SCANOUT && event->pixels != NULL) {
        if (event->movie_frame_complete) {
            atomic_fetch_add_explicit(
                &movie_vram_frame_events, 1u, memory_order_relaxed);
            atomic_store_explicit(&movie_event_artifact_base,
                state.movie_frame_artifact_candidate.artifact_base,
                memory_order_relaxed);
            atomic_store_explicit(&movie_event_artifact_size,
                state.movie_frame_artifact_candidate.artifact_size,
                memory_order_relaxed);
        }
        success = publish_mdec_framebuffer(event, guest_cycle);
        if (event->movie_frame_complete)
            atomic_fetch_add_explicit(success ? &movie_publication_successes :
                &movie_publication_failures, 1u, memory_order_relaxed);
        if (event->movie_frame_complete)
            clear_movie_frame_artifact_candidate();
    }
    return success;
}

enum {
    XG_RENDER_CHECKPOINT_MAGIC = 0x50434758u,
    XG_RENDER_CHECKPOINT_VERSION = 6u,
    XG_RENDER_CHECKPOINT_HEADER_SIZE = 64u,
    XG_RENDER_CHECKPOINT_FLAG_RETAINED_MOVIE = 1u,
};

enum {
    XG_RENDER_CHECKPOINT_RESTORE_FAULT_NONE = 0u,
    XG_RENDER_CHECKPOINT_RESTORE_FAULT_VRAM_RESOURCES = 1u,
    XG_RENDER_CHECKPOINT_RESTORE_FAULT_MOVIE = 2u,
    XG_RENDER_CHECKPOINT_RESTORE_FAULT_GRAPH = 3u,
    XG_RENDER_CHECKPOINT_RESTORE_FAULT_UI = 4u,
    XG_RENDER_CHECKPOINT_RESTORE_FAULT_REPOSITORY_ALLOCATION = 5u,
    XG_RENDER_CHECKPOINT_RESTORE_FAULT_REPOSITORY_CAPACITY = 6u,
};

static uint32_t checkpoint_restore_fault;
static uint32_t checkpoint_failure_stage;

uint32_t psx_xg_render_auth_checkpoint_failure_stage(void) {
    return checkpoint_failure_stage;
}

void psx_xg_render_auth_runtime_test_fail_checkpoint_restore(uint32_t fault) {
    checkpoint_restore_fault = fault;
    if (fault == XG_RENDER_CHECKPOINT_RESTORE_FAULT_REPOSITORY_ALLOCATION)
        xg_render_resource_restore_fault_inject(
            XG_RENDER_RESOURCE_RESTORE_FAULT_ALLOCATION, 0u);
    else if (fault == XG_RENDER_CHECKPOINT_RESTORE_FAULT_REPOSITORY_CAPACITY)
        xg_render_resource_restore_fault_inject(
            XG_RENDER_RESOURCE_RESTORE_FAULT_CAPACITY, 0u);
}

static bool checkpoint_restore_fault_after(uint32_t stage) {
    if (checkpoint_restore_fault != stage) return false;
    checkpoint_restore_fault = XG_RENDER_CHECKPOINT_RESTORE_FAULT_NONE;
    return true;
}

static void checkpoint_write_u32(uint8_t **cursor, uint32_t value) {
    for (uint32_t index = 0u; index < 4u; ++index)
        (*cursor)[index] = (uint8_t)(value >> (index * 8u));
    *cursor += 4u;
}

static void checkpoint_write_u64(uint8_t **cursor, uint64_t value) {
    for (uint32_t index = 0u; index < 8u; ++index)
        (*cursor)[index] = (uint8_t)(value >> (index * 8u));
    *cursor += 8u;
}

static uint32_t checkpoint_read_u32(const uint8_t **cursor) {
    uint32_t value = 0u;
    for (uint32_t index = 0u; index < 4u; ++index)
        value |= (uint32_t)(*cursor)[index] << (index * 8u);
    *cursor += 4u;
    return value;
}

static uint64_t checkpoint_read_u64(const uint8_t **cursor) {
    uint64_t value = 0u;
    for (uint32_t index = 0u; index < 8u; ++index)
        value |= (uint64_t)(*cursor)[index] << (index * 8u);
    *cursor += 8u;
    return value;
}

size_t psx_xg_render_auth_checkpoint_size(void) {
    size_t journal_size;
    size_t vram_resource_size;
    size_t movie_size;
    size_t surface_size;
    size_t ui_size;
    size_t size = 0u;

    lock_presentation_lifecycle();
    journal_size = xg_render_vram_journal_checkpoint_size();
    vram_resource_size = xg_render_vram_resources_checkpoint_size();
    surface_size = xg_render_surface_graph_checkpoint_size();
    ui_size = xg_render_ui_resources_checkpoint_size();
    if (journal_size != 0u &&
        vram_resource_size != 0u &&
        surface_size != 0u &&
        ui_size != 0u &&
        xg_render_movie_publisher_wire_size(&movie_size) ==
            XG_RENDER_MOVIE_OK &&
        journal_size <= SIZE_MAX - XG_RENDER_CHECKPOINT_HEADER_SIZE &&
        vram_resource_size <= SIZE_MAX - XG_RENDER_CHECKPOINT_HEADER_SIZE -
            journal_size &&
        movie_size <= SIZE_MAX - XG_RENDER_CHECKPOINT_HEADER_SIZE -
            journal_size - vram_resource_size &&
        surface_size <= SIZE_MAX - XG_RENDER_CHECKPOINT_HEADER_SIZE -
            journal_size - vram_resource_size - movie_size &&
        ui_size <= SIZE_MAX - XG_RENDER_CHECKPOINT_HEADER_SIZE -
            journal_size - vram_resource_size - movie_size - surface_size)
        size = XG_RENDER_CHECKPOINT_HEADER_SIZE + journal_size +
            vram_resource_size + movie_size + surface_size + ui_size;
    unlock_presentation_lifecycle();
    return size;
}

bool psx_xg_render_auth_checkpoint_write(
        void *out_checkpoint, size_t checkpoint_size) {
    XgRenderVramJournalSnapshot journal;
    uint8_t *cursor = (uint8_t *)out_checkpoint;
    size_t journal_size;
    size_t vram_resource_size;
    size_t movie_size;
    size_t surface_size;
    size_t ui_size;
    XgSemanticResourceRef retained_checkpoint_surface = {0};
    uint64_t checkpoint_flags = 0u;
    bool success;

    lock_presentation_lifecycle();
    journal_size = xg_render_vram_journal_checkpoint_size();
    vram_resource_size = xg_render_vram_resources_checkpoint_size();
    surface_size = xg_render_surface_graph_checkpoint_size();
    ui_size = xg_render_ui_resources_checkpoint_size();
    success = journal_size != 0u &&
        vram_resource_size != 0u &&
        surface_size != 0u &&
        ui_size != 0u &&
        xg_render_movie_publisher_wire_size(&movie_size) ==
            XG_RENDER_MOVIE_OK &&
        journal_size <= SIZE_MAX - XG_RENDER_CHECKPOINT_HEADER_SIZE &&
        vram_resource_size <= SIZE_MAX - XG_RENDER_CHECKPOINT_HEADER_SIZE -
            journal_size &&
        movie_size <= SIZE_MAX - XG_RENDER_CHECKPOINT_HEADER_SIZE -
            journal_size - vram_resource_size &&
        surface_size <= SIZE_MAX - XG_RENDER_CHECKPOINT_HEADER_SIZE -
            journal_size - vram_resource_size - movie_size &&
        ui_size <= SIZE_MAX - XG_RENDER_CHECKPOINT_HEADER_SIZE -
            journal_size - vram_resource_size - movie_size - surface_size &&
        checkpoint_size == XG_RENDER_CHECKPOINT_HEADER_SIZE +
            journal_size + vram_resource_size + movie_size + surface_size +
                ui_size &&
        out_checkpoint != NULL;
    if (!success) {
        unlock_presentation_lifecycle();
        return false;
    }
    if (composition_retained_movie_surface(&retained_checkpoint_surface))
        checkpoint_flags |= XG_RENDER_CHECKPOINT_FLAG_RETAINED_MOVIE;
    checkpoint_write_u32(&cursor, XG_RENDER_CHECKPOINT_MAGIC);
    checkpoint_write_u32(&cursor, XG_RENDER_CHECKPOINT_VERSION);
    checkpoint_write_u64(&cursor, checkpoint_size);
    checkpoint_write_u64(&cursor, journal_size);
    checkpoint_write_u64(&cursor, vram_resource_size);
    checkpoint_write_u64(&cursor, movie_size);
    checkpoint_write_u64(&cursor, surface_size);
    checkpoint_write_u64(&cursor, ui_size);
    checkpoint_write_u64(&cursor, checkpoint_flags);
    xg_render_vram_journal_snapshot(&journal);
    success = xg_render_vram_journal_checkpoint_write(
        cursor, journal_size);
    cursor += journal_size;
    if (success)
        success = xg_render_vram_resources_checkpoint_write(
        journal.mutation_serial, cursor, vram_resource_size) ==
            XG_RENDER_VRAM_RESOURCE_OK;
    cursor += vram_resource_size;
    if (success)
        success = xg_render_movie_publisher_wire_write(cursor, movie_size) ==
            XG_RENDER_MOVIE_OK;
    cursor += movie_size;
    if (success)
        success = xg_render_surface_graph_checkpoint_write(
            cursor, surface_size) == XG_RENDER_SURFACE_GRAPH_OK;
    cursor += surface_size;
    if (success)
        success = xg_render_ui_resources_checkpoint_write(cursor, ui_size) ==
            XG_RENDER_UI_OK;
    unlock_presentation_lifecycle();
    return success;
}

struct PsxXgRenderCheckpointRestore {
    XgRenderVramJournalCheckpointRestore *journal;
    XgRenderVramResourceCheckpointRestore *vram_resources;
    XgRenderMovieCheckpointRestore *movie;
    XgRenderSurfaceGraphCheckpointRestore *surface;
    XgRenderUiCheckpointRestore *ui;
    XgRenderResourceHandle movie_resource;
    size_t movie_resource_count;
    uint64_t restored_vram_generation;
    bool retain_movie_surface;
};

bool psx_xg_render_auth_checkpoint_prepare(
        const void *checkpoint, size_t checkpoint_size,
        PsxXgRenderCheckpointRestore **out_restore) {
    XgRenderVramJournalSnapshot journal;
    const uint8_t *cursor = (const uint8_t *)checkpoint;
    const uint8_t *journal_checkpoint;
    const uint8_t *vram_resource_checkpoint;
    const uint8_t *movie_checkpoint;
    const uint8_t *surface_checkpoint;
    const uint8_t *ui_checkpoint;
    uint64_t declared_size;
    uint64_t journal_size;
    uint64_t vram_resource_size;
    uint64_t movie_size;
    uint64_t surface_size;
    uint64_t ui_size;
    uint64_t checkpoint_flags;
    uint64_t saved_journal_generation;
    uint64_t saved_resource_generation;
    uint64_t validation_owner_generation;
    PsxXgRenderCheckpointRestore *restore;
    bool capability_restore_started = false;
    bool success;

    checkpoint_failure_stage = 0u;
    if (out_restore == NULL) return false;
    *out_restore = NULL;
    if (checkpoint == NULL ||
        checkpoint_size < XG_RENDER_CHECKPOINT_HEADER_SIZE ||
        checkpoint_read_u32(&cursor) != XG_RENDER_CHECKPOINT_MAGIC ||
        checkpoint_read_u32(&cursor) != XG_RENDER_CHECKPOINT_VERSION) {
        checkpoint_failure_stage = 1u;
        return false;
    }
    declared_size = checkpoint_read_u64(&cursor);
    journal_size = checkpoint_read_u64(&cursor);
    vram_resource_size = checkpoint_read_u64(&cursor);
    movie_size = checkpoint_read_u64(&cursor);
    surface_size = checkpoint_read_u64(&cursor);
    ui_size = checkpoint_read_u64(&cursor);
    checkpoint_flags = checkpoint_read_u64(&cursor);
    if (declared_size != checkpoint_size || journal_size > SIZE_MAX ||
        vram_resource_size > SIZE_MAX ||
        movie_size > SIZE_MAX || surface_size > SIZE_MAX || ui_size > SIZE_MAX ||
        (checkpoint_flags & ~XG_RENDER_CHECKPOINT_FLAG_RETAINED_MOVIE) != 0u ||
        journal_size > checkpoint_size - XG_RENDER_CHECKPOINT_HEADER_SIZE ||
        vram_resource_size > checkpoint_size - XG_RENDER_CHECKPOINT_HEADER_SIZE -
            (size_t)journal_size ||
        movie_size > checkpoint_size - XG_RENDER_CHECKPOINT_HEADER_SIZE -
            (size_t)journal_size - (size_t)vram_resource_size ||
        surface_size > checkpoint_size - XG_RENDER_CHECKPOINT_HEADER_SIZE -
            (size_t)journal_size - (size_t)vram_resource_size -
                (size_t)movie_size ||
        ui_size != checkpoint_size - XG_RENDER_CHECKPOINT_HEADER_SIZE -
            (size_t)journal_size - (size_t)vram_resource_size -
                (size_t)movie_size - (size_t)surface_size) {
        checkpoint_failure_stage = 2u;
        return false;
    }
    journal_checkpoint = cursor;
    vram_resource_checkpoint = journal_checkpoint + (size_t)journal_size;
    movie_checkpoint = vram_resource_checkpoint + (size_t)vram_resource_size;
    surface_checkpoint = movie_checkpoint + (size_t)movie_size;
    ui_checkpoint = surface_checkpoint + (size_t)surface_size;
    restore = (PsxXgRenderCheckpointRestore *)calloc(1u, sizeof(*restore));
    if (restore == NULL) {
        checkpoint_failure_stage = 3u;
        return false;
    }
    lock_presentation_lifecycle();
    if (state.scene_generation == UINT64_MAX) {
        unlock_presentation_lifecycle();
        free(restore);
        checkpoint_failure_stage = 4u;
        return false;
    }
    xg_render_vram_journal_snapshot(&journal);
    if (journal.mutation_serial == UINT64_MAX) {
        unlock_presentation_lifecycle();
        free(restore);
        checkpoint_failure_stage = 5u;
        return false;
    }
    validation_owner_generation = state.scene_generation + 1u;
    restore->restored_vram_generation = journal.mutation_serial + 1u;
    restore->retain_movie_surface =
        (checkpoint_flags & XG_RENDER_CHECKPOINT_FLAG_RETAINED_MOVIE) != 0u;
    success = xg_render_vram_journal_checkpoint_prepare(
        journal_checkpoint, (size_t)journal_size, &restore->journal,
        &saved_journal_generation);
    if (!success) checkpoint_failure_stage = 6u;
    if (success) {
        success = xg_render_vram_resources_checkpoint_source_generation(
                vram_resource_checkpoint, (size_t)vram_resource_size,
                &saved_resource_generation) &&
            saved_resource_generation == saved_journal_generation;
        if (!success) checkpoint_failure_stage = 7u;
    }
    if (success) {
        capability_restore_started =
            xg_render_resource_capability_restore_begin();
        success = capability_restore_started;
        if (!success) checkpoint_failure_stage = 8u;
    }
    if (success) {
        success = xg_render_vram_resources_checkpoint_prepare(
                vram_resource_checkpoint, (size_t)vram_resource_size,
                restore->restored_vram_generation,
                validation_owner_generation,
                &restore->vram_resources) == XG_RENDER_VRAM_RESOURCE_OK &&
            !checkpoint_restore_fault_after(
                XG_RENDER_CHECKPOINT_RESTORE_FAULT_VRAM_RESOURCES);
        if (!success) checkpoint_failure_stage = 9u;
    }
    if (success)
        success = xg_render_movie_publisher_wire_prepare(
                      movie_checkpoint, (size_t)movie_size,
                      validation_owner_generation, &restore->movie) ==
                  XG_RENDER_MOVIE_OK &&
            !checkpoint_restore_fault_after(
                XG_RENDER_CHECKPOINT_RESTORE_FAULT_MOVIE);
    if (!success && checkpoint_failure_stage == 0u)
        checkpoint_failure_stage = 10u;
    if (success) {
        const XgRenderMovieResult movie_resource_result =
            xg_render_movie_publisher_checkpoint_resource(
                restore->movie, &restore->movie_resource);

        restore->movie_resource_count =
            movie_resource_result == XG_RENDER_MOVIE_OK ? 1u : 0u;
        success = movie_resource_result == XG_RENDER_MOVIE_OK ||
            movie_resource_result == XG_RENDER_MOVIE_NO_COMPLETE_FRAME;
        if (success && restore->retain_movie_surface)
            success = movie_resource_result == XG_RENDER_MOVIE_OK;
        if (!success && checkpoint_failure_stage == 0u)
            checkpoint_failure_stage = 11u;
    }
    if (success)
        success =
            xg_render_surface_graph_checkpoint_prepare_with_shared_resources(
                surface_checkpoint, (size_t)surface_size,
                validation_owner_generation,
                restore->movie_resource_count != 0u
                    ? &restore->movie_resource : NULL,
                restore->movie_resource_count, &restore->surface) ==
                XG_RENDER_SURFACE_GRAPH_OK &&
            !checkpoint_restore_fault_after(
                XG_RENDER_CHECKPOINT_RESTORE_FAULT_GRAPH);
    if (!success && checkpoint_failure_stage == 0u)
        checkpoint_failure_stage = 12u;
    if (success)
        success = xg_render_ui_resources_checkpoint_prepare(
                      ui_checkpoint, (size_t)ui_size,
                      validation_owner_generation, &restore->ui) ==
                  XG_RENDER_UI_OK &&
            !checkpoint_restore_fault_after(
                XG_RENDER_CHECKPOINT_RESTORE_FAULT_UI);
    if (!success && checkpoint_failure_stage == 0u)
        checkpoint_failure_stage = 13u;
    if (!success) {
        xg_render_ui_resources_checkpoint_cancel(restore->ui);
        xg_render_surface_graph_checkpoint_cancel(restore->surface);
        xg_render_movie_publisher_wire_cancel(restore->movie);
        xg_render_vram_resources_checkpoint_cancel(restore->vram_resources);
        if (capability_restore_started)
            xg_render_resource_capability_restore_cancel();
        xg_render_vram_journal_checkpoint_cancel(restore->journal);
        checkpoint_restore_fault = XG_RENDER_CHECKPOINT_RESTORE_FAULT_NONE;
        xg_render_resource_restore_fault_inject(
            XG_RENDER_RESOURCE_RESTORE_FAULT_NONE, 0u);
        unlock_presentation_lifecycle();
        free(restore);
        return false;
    }
    *out_restore = restore;
    checkpoint_failure_stage = 0u;
    return true;
}

static void checkpoint_commit(
        PsxXgRenderCheckpointRestore *restore, bool boot_restore) {
    if (restore == NULL) return;
    if (boot_restore)
        (void)timeline_invalidate_locked(XG_RENDER_TIMELINE_RESTORE);
    release_retained_movie_surface();
    scene_boundary_locked();
    xg_render_resource_capability_restore_commit();
    xg_render_vram_journal_checkpoint_commit(
        restore->journal, restore->restored_vram_generation);
    xg_render_vram_resources_checkpoint_commit(restore->vram_resources);
    xg_render_movie_publisher_wire_commit(restore->movie, NULL);
    if (restore->retain_movie_surface) {
        XgRenderResourceView movie_view = {0};

        if (xg_render_resource_view(restore->movie_resource, &movie_view) ==
                XG_RENDER_RESOURCE_OK &&
            movie_view.kind == XG_RENDER_RESOURCE_MOVIE_FRAME)
            (void)retain_movie_surface(&(XgSemanticResourceRef){
                .resource_id = restore->movie_resource.resource_id,
                .generation = restore->movie_resource.generation,
                .content_digest = movie_view.content_digest,
            });
    }
    xg_render_surface_graph_checkpoint_commit(restore->surface);
    xg_render_ui_resources_checkpoint_commit(restore->ui);
    checkpoint_restore_fault = XG_RENDER_CHECKPOINT_RESTORE_FAULT_NONE;
    checkpoint_failure_stage = 0u;
    xg_render_resource_restore_fault_inject(
        XG_RENDER_RESOURCE_RESTORE_FAULT_NONE, 0u);
    state.boot_restore_timeline_preapplied = boot_restore;
    unlock_presentation_lifecycle();
    free(restore);
}

void psx_xg_render_auth_checkpoint_commit(
        PsxXgRenderCheckpointRestore *restore) {
    checkpoint_commit(restore, false);
}

void psx_xg_render_auth_checkpoint_commit_boot_restore(
        PsxXgRenderCheckpointRestore *restore) {
    checkpoint_commit(restore, true);
}

void psx_xg_render_auth_checkpoint_cancel(
        PsxXgRenderCheckpointRestore *restore) {
    if (restore == NULL) return;
    xg_render_ui_resources_checkpoint_cancel(restore->ui);
    xg_render_surface_graph_checkpoint_cancel(restore->surface);
    xg_render_movie_publisher_wire_cancel(restore->movie);
    xg_render_vram_resources_checkpoint_cancel(restore->vram_resources);
    xg_render_resource_capability_restore_cancel();
    xg_render_vram_journal_checkpoint_cancel(restore->journal);
    checkpoint_restore_fault = XG_RENDER_CHECKPOINT_RESTORE_FAULT_NONE;
    xg_render_resource_restore_fault_inject(
        XG_RENDER_RESOURCE_RESTORE_FAULT_NONE, 0u);
    unlock_presentation_lifecycle();
    free(restore);
}

bool psx_xg_render_auth_checkpoint_restore(
        const void *checkpoint, size_t checkpoint_size) {
    PsxXgRenderCheckpointRestore *restore = NULL;

    if (!psx_xg_render_auth_checkpoint_prepare(
            checkpoint, checkpoint_size, &restore))
        return false;
    psx_xg_render_auth_checkpoint_commit(restore);
    return true;
}

bool psx_xg_render_auth_note_vram_upload(
        uint64_t guest_vblank_sequence, uint64_t guest_cycle,
        uint16_t x, uint16_t y, uint16_t width, uint16_t height,
        const uint16_t *pixels, size_t pixel_count) {
    return note_vram_rect_transfer(
        XG_RENDER_VRAM_UPLOAD, guest_vblank_sequence, guest_cycle,
        0u, 0u, 0u,
        x, y, width, height, pixels, pixel_count, NULL);
}

bool psx_xg_render_auth_note_vram_readback(
        uint64_t guest_vblank_sequence, uint64_t guest_cycle,
        uint16_t x, uint16_t y, uint16_t width, uint16_t height,
        const uint16_t *pixels, size_t pixel_count) {
    return note_vram_rect_transfer(
        XG_RENDER_VRAM_READBACK, guest_vblank_sequence, guest_cycle,
        0u, x, y,
        x, y, width, height, pixels, pixel_count, NULL);
}

bool psx_xg_render_auth_note_vram_readback_digest(
        uint64_t guest_vblank_sequence, uint64_t guest_cycle,
        uint16_t x, uint16_t y, uint16_t width, uint16_t height,
        size_t pixel_count, uint64_t content_digest) {
    XgRenderVramTransferDescription description;
    XgRenderVramMutation mutation;
    bool success = false;

    if (width == 0u || height == 0u ||
        pixel_count != (size_t)width * (size_t)height)
        return false;
    description = (XgRenderVramTransferDescription){
        .operation = XG_RENDER_VRAM_READBACK,
        .guest_cycle = guest_cycle,
        .source_interval = guest_vblank_sequence,
        .x = x,
        .y = y,
        .width = width,
        .height = height,
        .payload_size = pixel_count * sizeof(uint16_t),
    };
    lock_presentation_lifecycle();
    if (xg_render_vram_publish_digest(
            &description, content_digest, &mutation) == XG_RENDER_VRAM_OK)
        success = true;
    unlock_presentation_lifecycle();
    return success;
}

bool psx_xg_render_auth_note_vram_move(
        uint64_t guest_vblank_sequence, uint64_t guest_cycle,
        uint16_t x, uint16_t y, uint16_t width, uint16_t height,
        const uint16_t *pixels, size_t pixel_count) {
    return note_vram_rect_transfer(
        XG_RENDER_VRAM_MOVE, guest_vblank_sequence, guest_cycle,
        0u, 0u, 0u,
        x, y, width, height, pixels, pixel_count, NULL);
}

bool psx_xg_render_auth_note_vram_clear(
        uint64_t guest_vblank_sequence, uint64_t guest_cycle,
        uint16_t x, uint16_t y, uint16_t width, uint16_t height,
        const uint16_t *pixels, size_t pixel_count) {
    return note_vram_rect_transfer(
        XG_RENDER_VRAM_CLEAR, guest_vblank_sequence, guest_cycle,
        0u, 0u, 0u,
        x, y, width, height, pixels, pixel_count, NULL);
}

bool psx_xg_render_auth_source_boundary(uint64_t guest_vblank_sequence,
                                          uint64_t guest_cycle) {
    XgRenderTimelineResult result;
    XgRenderSourceFrameResult completion_result;
    XgRenderSourceFrameResult frame_result;
    bool success;
    XgRenderSourceFrameSnapshot source_snapshot = {0};
    const bool movie_boundary = state.movie_publication_pending_boundary;

    if (xg_render_native_work_enabled())
        return xg_render_timeline_source_boundary(guest_vblank_sequence,
                   guest_cycle) == XG_RENDER_TIMELINE_OK &&
            xg_render_native_work_flush(true, guest_cycle);

    if (state.configured)
        xg_render_runtime_composition_prepare_gpu_source_boundary();
    lock_presentation_lifecycle();
    xg_render_source_frame_snapshot(&source_snapshot);
    atomic_store_explicit(&movie_source_active_before_boundary,
        source_snapshot.active ? 1u : 0u, memory_order_relaxed);
    atomic_store_explicit(&movie_source_passes_before_boundary,
        source_snapshot.pass_count, memory_order_relaxed);
    if (movie_boundary) {
        atomic_fetch_add_explicit(
            &movie_publication_boundaries, 1u, memory_order_relaxed);
        if (source_snapshot.active)
            atomic_fetch_add_explicit(
                &movie_publication_boundaries_active, 1u,
                memory_order_relaxed);
        state.movie_publication_pending_boundary = false;
    }
    result = xg_render_timeline_source_boundary(
        guest_vblank_sequence, guest_cycle);
    if (result != XG_RENDER_TIMELINE_OK) {
        unlock_presentation_lifecycle();
        return false;
    }
    completion_result = state.configured
        ? xg_render_runtime_composition_complete_gpu_source_frame()
        : xg_render_fragment_runtime_finalize_source_frame();
    if (completion_result != XG_RENDER_SOURCE_FRAME_OK &&
        completion_result != XG_RENDER_SOURCE_FRAME_EMPTY) {
        xg_render_timeline_note_rejection();
        xg_render_fragment_runtime_finish_source_frame(false);
        xg_render_fragment_runtime_boundary_end();
        unlock_presentation_lifecycle();
        return false;
    }
    frame_result = xg_render_source_frame_publish_boundary();
    if (movie_boundary) {
        atomic_store_explicit(&movie_publication_boundary_completion_result,
            (unsigned)completion_result, memory_order_relaxed);
        atomic_store_explicit(&movie_publication_boundary_publish_result,
            (unsigned)frame_result, memory_order_relaxed);
    }
    success = completion_result == XG_RENDER_SOURCE_FRAME_EMPTY
        ? frame_result == XG_RENDER_SOURCE_FRAME_EMPTY
        : completion_result == XG_RENDER_SOURCE_FRAME_OK &&
          frame_result == XG_RENDER_SOURCE_FRAME_OK;
    if (completion_result == XG_RENDER_SOURCE_FRAME_EMPTY &&
        frame_result == XG_RENDER_SOURCE_FRAME_EMPTY)
        xg_render_timeline_note_hold();
    else if (!success)
        xg_render_timeline_note_rejection();
    xg_render_fragment_runtime_finish_source_frame(success);
    xg_render_fragment_runtime_boundary_end();
    unlock_presentation_lifecycle();
    return success;
}

static uint64_t timeline_invalidate_locked(
        XgRenderTimelineInvalidationReason reason) {
    uint64_t epoch;
    XgRenderResourceOwnerKind first_owner = XG_RENDER_RESOURCE_OWNER_MODULE;

    if (xg_render_native_work_enabled()) {
        if (reason == XG_RENDER_TIMELINE_RESTORE ||
            reason == XG_RENDER_TIMELINE_RESET) {
            xg_render_native_work_cancel_pending();
        } else {
            /* The cooperative host service may render diagnostics. Never keep
             * an auth lock on the suspended guest stack during that service. */
            unlock_presentation_lifecycle();
            const bool drained = xg_render_native_work_drain(psx_get_cycle_count());
            lock_presentation_lifecycle();
            if (!drained) {
                psx_fatal_halt("Native visual work failed before timeline invalidation");
                return 0u;
            }
        }
    }

    if (reason == XG_RENDER_TIMELINE_ARTIFACT_CHANGE &&
        state.movie_active_owner != GPU_MOVIE_OWNER_NONE) {
        XgRenderPresentationDiagnostics diagnostics;

        xg_render_semantic_presentation_diagnostics(&diagnostics);
        return diagnostics.presentation_epoch;
    }

    epoch = xg_render_timeline_invalidate(reason);
    if (xg_render_native_work_enabled())
        xg_render_native_work_cancel_pending();
    if (reason != XG_RENDER_TIMELINE_SCENE_CHANGE &&
        reason != XG_RENDER_TIMELINE_ARTIFACT_CHANGE) {
        release_retained_movie_surface();
        xg_render_runtime_composition_source_reset();
    } else if (reason == XG_RENDER_TIMELINE_SCENE_CHANGE ||
             reason == XG_RENDER_TIMELINE_ARTIFACT_CHANGE)
        xg_render_surface_graph_reset();
    xg_render_fragment_runtime_invalidate();
    xg_render_source_frame_reset();
    if (reason == XG_RENDER_TIMELINE_DISC_CHANGE)
        first_owner = XG_RENDER_RESOURCE_OWNER_DISC;
    else if (reason == XG_RENDER_TIMELINE_ROLLBACK ||
             reason == XG_RENDER_TIMELINE_SCENE_CHANGE ||
             reason == XG_RENDER_TIMELINE_ARTIFACT_CHANGE)
        first_owner = XG_RENDER_RESOURCE_OWNER_SCENE;
    xg_render_resource_invalidate_from_owner(first_owner);
    if (reason == XG_RENDER_TIMELINE_SCENE_CHANGE ||
        reason == XG_RENDER_TIMELINE_ARTIFACT_CHANGE)
        xg_render_vram_journal_cancel_transfers();
    else
        xg_render_vram_journal_reset();
    if (first_owner <= XG_RENDER_RESOURCE_OWNER_MODULE)
        xg_render_ui_resources_reset();
    if (reason == XG_RENDER_TIMELINE_SCENE_CHANGE ||
        reason == XG_RENDER_TIMELINE_ARTIFACT_CHANGE)
        xg_render_movie_publisher_scene_boundary();
    else
        xg_render_movie_publisher_reset();
    return epoch;
}

uint64_t psx_xg_render_auth_timeline_invalidate(
        XgRenderTimelineInvalidationReason reason) {
    uint64_t epoch;

    lock_presentation_lifecycle();
    epoch = timeline_invalidate_locked(reason);
    unlock_presentation_lifecycle();
    return epoch;
}

void psx_xg_render_auth_presentation_snapshot(
        XgRenderPresentationDiagnostics *out_snapshot) {
    xg_render_semantic_presentation_diagnostics(out_snapshot);
}

bool psx_xg_render_auth_prepare_ui_ot(uint32_t start_addr) {
    return xg_render_runtime_composition_prepare_ui_ot(start_addr);
}

void psx_xg_render_auth_complete_ordering_table(
        uint32_t start_addr, uint32_t transferred_words) {
    xg_render_runtime_composition_complete_ordering_table(
        start_addr, transferred_words);
}

void psx_xg_render_auth_ui_ot_snapshot(
        PsxXgRenderUiOtSnapshot *out_snapshot) {
    xg_render_runtime_composition_ui_ot_snapshot(out_snapshot);
}

void psx_xg_render_auth_source_snapshot(
        PsxXgRenderSourceSnapshot *out_snapshot) {
    xg_render_runtime_composition_source_snapshot(out_snapshot);
}

void psx_xg_render_auth_source_collector_snapshot(
        FieldCharacterShadowSummary *out_summary) {
    xg_render_runtime_composition_source_collector_snapshot(out_summary);
}

void psx_xg_render_auth_source_reset(void) {
    xg_render_runtime_composition_source_reset();
}

void psx_xg_render_auth_ft4_geometry_enable(bool enabled) {
    xg_render_runtime_composition_ft4_geometry_enable(enabled);
}

void psx_xg_render_auth_capture_clear_tile(CPUState *cpu) {
    xg_render_runtime_composition_capture_clear_tile(cpu);
}

void psx_xg_render_auth_capture_logo_sprite(
        uint32_t command_address, uint8_t color) {
    xg_render_runtime_composition_capture_logo_sprite(command_address, color);
}

void psx_xg_render_auth_capture_tile_write(
        CPUState *cpu, uint32_t command_address, uint32_t writer_pc,
        uint8_t color) {
    xg_render_runtime_composition_capture_tile_write(
        cpu, command_address, writer_pc, color);
}

bool psx_xg_render_auth_resident_ft4_observe(
        CPUState *cpu, uint32_t stage, uint32_t pc,
        uint32_t instruction_word) {
    return xg_render_runtime_composition_resident_ft4_observe(
        cpu, stage, pc, instruction_word);
}

bool psx_xg_render_auth_ft4_geometry_pop(
        PsxXgRenderFt4Geometry *out_geometry) {
    return xg_render_runtime_composition_ft4_geometry_pop(out_geometry);
}

void psx_xg_render_auth_ft4_geometry_snapshot(
        PsxXgRenderFt4GeometrySnapshot *out_snapshot) {
    xg_render_runtime_composition_ft4_geometry_snapshot(out_snapshot);
}

void psx_xg_render_auth_tim_route_diagnostics(
        PsxXgRenderTimRouteDiagnostics *out_diagnostics) {
    xg_render_runtime_composition_tim_route_diagnostics(out_diagnostics);
}

void psx_xg_render_auth_zoom_template_contract_snapshot(
        PsxXgRenderZoomTemplateContractSnapshot *out_snapshot) {
    xg_render_runtime_composition_zoom_template_contract_snapshot(out_snapshot);
}

size_t psx_xg_render_auth_pre_scene_snapshot(
        PsxXgRenderPreScenePrimitiveSnapshot *out_snapshots,
        size_t capacity) {
    return xg_render_runtime_composition_pre_scene_snapshot(
        out_snapshots, capacity);
}

size_t psx_xg_render_auth_field_fragment_snapshot(
        PsxXgRenderPreScenePrimitiveSnapshot *out_snapshots,
        size_t capacity) {
    return xg_render_runtime_composition_field_fragment_snapshot(
        out_snapshots, capacity);
}

void psx_xg_render_auth_overlay_ft4_snapshot(
        PsxXgRenderOverlayFt4Snapshot *out_snapshot) {
    xg_render_runtime_composition_overlay_ft4_snapshot(out_snapshot);
}

void psx_xg_render_auth_producer_family_enable(bool enabled) {
    xg_render_runtime_composition_enable_producer_family(enabled);
}

void psx_xg_render_auth_producer_family_snapshot(
        PsxXgRenderProducerFamilySnapshot *out_snapshot) {
    xg_render_runtime_composition_producer_family_snapshot(out_snapshot);
}

void psx_xg_render_auth_projected_lifecycle_snapshot(
        PsxXgRenderProjectedLifecycleSnapshot *out_snapshot) {
    xg_render_runtime_composition_projected_lifecycle_snapshot(out_snapshot);
}

void psx_xg_render_auth_model_ft4_shadow_snapshot(
        PsxXgRenderModelFt4ShadowSnapshot *out_snapshot) {
    xg_render_runtime_composition_model_ft4_shadow_snapshot(out_snapshot);
}

void psx_xg_render_auth_model_ft3_shadow_snapshot(
        PsxXgRenderModelFt3ShadowSnapshot *out_snapshot) {
    xg_render_runtime_composition_model_ft3_shadow_snapshot(out_snapshot);
}

void psx_xg_render_auth_sprite_ft4_shadow_snapshot(
        PsxXgRenderSpriteFt4ShadowSnapshot *out_snapshot) {
    xg_render_runtime_composition_sprite_ft4_shadow_snapshot(out_snapshot);
}

void psx_xg_render_auth_field_polyline_snapshot(
        PsxXgRenderFieldPolylineSnapshot *out_snapshot) {
    xg_render_runtime_composition_field_polyline_snapshot(out_snapshot);
}

void psx_xg_render_auth_world_horizon_shadow_snapshot(
        PsxXgRenderWorldHorizonShadowSnapshot *out_snapshot) {
    xg_render_runtime_composition_world_horizon_shadow_snapshot(out_snapshot);
}

void psx_xg_render_auth_world_effects_shadow_snapshot(
        PsxXgRenderWorldEffectsShadowSnapshot *out_snapshot) {
    xg_render_runtime_composition_world_effects_shadow_snapshot(out_snapshot);
}

void psx_xg_render_auth_world_terrain_water_shadow_snapshot(
        PsxXgRenderWorldTerrainWaterShadowSnapshot *out_snapshot) {
    xg_render_runtime_composition_world_terrain_water_shadow_snapshot(
        out_snapshot);
}

void psx_xg_render_auth_world_entity_shadows_shadow_snapshot(
        PsxXgRenderWorldEntityShadowsShadowSnapshot *out_snapshot) {
    xg_render_runtime_composition_world_entity_shadows_shadow_snapshot(
        out_snapshot);
}

void psx_xg_render_auth_world_decorations_shadow_snapshot(
        PsxXgRenderWorldDecorationsShadowSnapshot *out_snapshot) {
    xg_render_runtime_composition_world_decorations_shadow_snapshot(
        out_snapshot);
}

void psx_xg_render_auth_world_clouds_shadow_snapshot(
        PsxXgRenderWorldCloudsShadowSnapshot *out_snapshot) {
    xg_render_runtime_composition_world_clouds_shadow_snapshot(out_snapshot);
}

void psx_xg_render_auth_world_minimap_shadow_snapshot(
        PsxXgRenderWorldMinimapShadowSnapshot *out_snapshot) {
    xg_render_runtime_composition_world_minimap_shadow_snapshot(out_snapshot);
}

void psx_xg_render_auth_world_models_native_snapshot(
        PsxXgRenderWorldNativeSnapshot *out_snapshot) {
    xg_render_runtime_composition_world_models_native_snapshot(out_snapshot);
}

void psx_xg_render_auth_world_actor_sprites_native_snapshot(
        PsxXgRenderWorldNativeSnapshot *out_snapshot) {
    xg_render_runtime_composition_world_actor_sprites_native_snapshot(
        out_snapshot);
}

void psx_xg_render_auth_world_sky_native_snapshot(
        PsxXgRenderWorldNativeSnapshot *out_snapshot) {
    xg_render_runtime_composition_world_sky_native_snapshot(out_snapshot);
}

void psx_xg_render_auth_world_execution_snapshot(
        PsxXgRenderWorldExecutionSnapshot *out_snapshot) {
    xg_render_runtime_composition_world_execution_snapshot(out_snapshot);
}
