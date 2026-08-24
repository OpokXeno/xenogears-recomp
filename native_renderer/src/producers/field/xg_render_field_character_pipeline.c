#include "xg_render_field_character_pipeline.h"

#include "field_character_shadow.h"
#include "guest_render_bridge.h"
#include "gpu.h"
#include "xg_field_character_adapter.h"
#include "xg_field_character_runtime.h"
#include "xg_field_character_source_capture.h"
#include "xg_field_render_services.h"
#include "xg_render_backend.h"
#include "xg_render_manifest_generated.h"
#include "xg_render_primitive_utils.h"
#include "xg_render_submission.h"

#include <limits.h>
#include <stddef.h>
#include <string.h>

typedef struct XgRenderSourcePending {
    PsxXgRenderSourceSiteMetadata metadata;
    uint32_t pc;
    uint32_t instruction;
    uint32_t auxiliary;
    XgRenderAuthTier tier;
    bool valid;
} XgRenderSourcePending;

typedef struct XgRenderSourceState {
    PsxXgRenderSourceSnapshot aggregate;
    XgRenderSourcePending pending;
    FieldCharacterShadow collector;
    uint64_t next_auth_sequence;
} XgRenderSourceState;

enum {
    XG_RENDER_SOURCE_BLOCK_INVALID_STAGE = 1u,
    XG_RENDER_SOURCE_BLOCK_CONTEXT = 2u,
    XG_RENDER_SOURCE_BLOCK_SITE = 3u,
    XG_RENDER_SOURCE_BLOCK_AUXILIARY = 4u,
    XG_RENDER_SOURCE_BLOCK_PENDING = 5u,
    XG_RENDER_SOURCE_BLOCK_PAIR = 6u,
    XG_RENDER_SOURCE_BLOCK_COLLECTOR = 7u,
    XG_RENDER_SOURCE_BLOCK_LIFECYCLE = 8u,
};

typedef enum XgRenderFt4GeometryPhase {
    XG_RENDER_FT4_EXPECT_FIRST_PRE = 0,
    XG_RENDER_FT4_EXPECT_FIRST_COMMIT = 1,
    XG_RENDER_FT4_EXPECT_SECOND_PRE = 2,
    XG_RENDER_FT4_EXPECT_SECOND_COMMIT = 3,
} XgRenderFt4GeometryPhase;

typedef struct XgRenderFt4GeometryPending {
    uint64_t scene_generation;
    uint32_t source_return_address;
    uint32_t destinations[4];
    XgHost3dRotAverage4Input pre_transform;
    XgHost3dRotAverage4Output host_output;
    XgFieldCharacterSourceSnapshot source_snapshot;
    XgFieldCharacterRuntimeCandidate native_candidate;
    uint32_t source_capture_result;
    XgRenderAuthTier tier;
    XgRenderFt4GeometryPhase phase;
    bool source_captured;
    bool native_ready;
    bool shadow_oracle;
    bool valid;
} XgRenderFt4GeometryPending;

typedef struct XgRenderFt4GeometryState {
    PsxXgRenderFt4Geometry records[PSX_XG_RENDER_FT4_GEOMETRY_CAPACITY];
    XgRenderFt4GeometryPending pending;
    uint64_t next_sequence;
    uint64_t completed_count;
    uint64_t host_transform_count;
    uint64_t oracle_match_count;
    uint64_t oracle_mismatch_count;
    uint32_t head;
    uint32_t count;
    bool enabled;
    bool blocked;
    bool overflowed;
} XgRenderFt4GeometryState;

static XgRenderSourceState source_state = {
    .aggregate = { .next_sequence = 1u },
    .next_auth_sequence = 1u,
};
static XgRenderFt4GeometryState ft4_geometry = { .next_sequence = 1u };
static PsxXgRenderProducerFamilySnapshot producer_family;

static bool physical_address_equals(uint32_t left, uint32_t right) {
    return (left & UINT32_C(0x1fffffff)) ==
           (right & UINT32_C(0x1fffffff));
}

static void clear_source_pending(void) {
    source_state.pending = (XgRenderSourcePending){ 0 };
}

static void clear_ft4_geometry_pending(void) {
    ft4_geometry.pending = (XgRenderFt4GeometryPending){ 0 };
}

static void reset_ft4_geometry(bool preserve_enabled) {
    const bool enabled = preserve_enabled && ft4_geometry.enabled;

    ft4_geometry = (XgRenderFt4GeometryState){
        .next_sequence = 1u,
        .enabled = enabled,
    };
}

static void block_ft4_geometry(bool overflowed) {
    clear_ft4_geometry_pending();
    ft4_geometry.head = 0u;
    ft4_geometry.count = 0u;
    ft4_geometry.blocked = true;
    ft4_geometry.overflowed |= overflowed;
}

static void block_source_proof(bool overflowed, uint32_t blocker) {
    clear_source_pending();
    if (!source_state.aggregate.blocker)
        source_state.aggregate.blocker = blocker;
    source_state.aggregate.blocked = true;
    source_state.aggregate.overflowed |= overflowed;
}

static bool source_context_matches(
    XgRenderAuthTier tier,
    const XgRenderFieldCharacterPipelineServices *services) {
    uint32_t context_bits = 0u;
    const bool matches = services != NULL &&
        services->source_context_matches != NULL &&
        services->source_context_matches(tier, &context_bits);

    source_state.aggregate.context_bits = context_bits;
    return matches;
}

static bool source_auxiliary_is_valid(
    PsxXgRenderSourceStage stage,
    const PsxXgRenderSourceSiteMetadata *metadata, uint32_t auxiliary) {
    switch (metadata->auxiliary_rule) {
    case PSX_XG_RENDER_SOURCE_AUXILIARY_EFFECTIVE_ADDRESS:
        return metadata->width == 1u || metadata->width == 2u ||
               metadata->width == 4u;
    case PSX_XG_RENDER_SOURCE_AUXILIARY_NONE:
        return metadata->width == 0u && auxiliary == 0u;
    case PSX_XG_RENDER_SOURCE_AUXILIARY_RESULT_REGISTER:
        return metadata->width == 0u &&
               (stage == PSX_XG_RENDER_SOURCE_STAGE_COMMIT || auxiliary == 0u);
    }
    return false;
}

bool xg_render_field_character_inner_call_matches(
    XgRenderAuthTier tier, uint32_t hook, uint32_t pc,
    uint32_t instruction_word,
    const XgRenderFieldCharacterPipelineServices *services) {
    return hook == PSX_XG_RENDER_AUTH_HOOK_INTERNAL_OBSERVATION &&
           source_state.pending.valid && source_state.pending.tier == tier &&
           source_state.pending.metadata.operation ==
               PSX_XG_RENDER_SOURCE_OPERATION_CALL &&
           physical_address_equals(pc, source_state.pending.pc) &&
           instruction_word == source_state.pending.instruction &&
           source_context_matches(tier, services);
}

static void append_source_event(
    PsxXgRenderSourceStage stage, uint32_t pc,
    const PsxXgRenderSourceSiteMetadata *metadata, uint32_t auxiliary) {
    PsxXgRenderSourceEvent *event =
        &source_state.aggregate.events[source_state.aggregate.count++];

    *event = (PsxXgRenderSourceEvent){
        source_state.aggregate.next_sequence++,
        xg_render_runtime_guest_address(pc), auxiliary,
        metadata->operation, stage, metadata->width,
    };
}

static void append_source_pair(
    uint32_t pc, const XgRenderSourcePending *pending,
    const PsxXgRenderSourceSiteMetadata *metadata, uint32_t auxiliary) {
    if (source_state.aggregate.count + 2u >
        PSX_XG_RENDER_SOURCE_EVENT_CAPACITY) {
        source_state.aggregate.overflowed = true;
        return;
    }
    append_source_event(PSX_XG_RENDER_SOURCE_STAGE_PRE, pending->pc,
                        &pending->metadata, pending->auxiliary);
    append_source_event(PSX_XG_RENDER_SOURCE_STAGE_COMMIT, pc, metadata,
                        auxiliary);
}

static bool collect_source_pair(const XgRenderSourcePending *pending) {
    const uint32_t guest_pc = xg_render_runtime_guest_address(pending->pc);
    FieldCharacterShadowAuth auth;
    FieldCharacterShadowResult result;

    if (source_state.next_auth_sequence == 0u) return false;
    auth = (FieldCharacterShadowAuth){
        .site = guest_pc,
        .sequence = source_state.next_auth_sequence++,
        .authenticated = 1u,
    };
    result = field_character_shadow_begin(&source_state.collector, auth, 0u);
    if (result != FIELD_CHARACTER_SHADOW_RESULT_OK) return false;
    switch (pending->metadata.operation) {
    case PSX_XG_RENDER_SOURCE_OPERATION_READ:
        result = field_character_shadow_observe_dynamic_access(
            &source_state.collector, guest_pc, pending->auxiliary,
            pending->metadata.width, FIELD_CHARACTER_SHADOW_ACCESS_READ);
        break;
    case PSX_XG_RENDER_SOURCE_OPERATION_WRITE:
    case PSX_XG_RENDER_SOURCE_OPERATION_SWC2:
        result = field_character_shadow_observe_dynamic_access(
            &source_state.collector, guest_pc, pending->auxiliary,
            pending->metadata.width, FIELD_CHARACTER_SHADOW_ACCESS_WRITE);
        break;
    case PSX_XG_RENDER_SOURCE_OPERATION_CALL:
        result = field_character_shadow_observe_effect(
            &source_state.collector, FIELD_CHARACTER_SHADOW_EFFECT_ALLOCATOR);
        break;
    case PSX_XG_RENDER_SOURCE_OPERATION_BUCKET:
        result = field_character_shadow_observe_effect(
            &source_state.collector, FIELD_CHARACTER_SHADOW_EFFECT_OT);
        break;
    default:
        (void)field_character_shadow_auth_lost(&source_state.collector);
        return false;
    }
    if (result != FIELD_CHARACTER_SHADOW_RESULT_OK) return false;
    return field_character_shadow_end(&source_state.collector, auth) ==
           FIELD_CHARACTER_SHADOW_RESULT_OK;
}

static bool ft4_destinations_are_valid(const uint32_t destinations[4]) {
    return (destinations[0] & 3u) == 0u &&
           destinations[1] == destinations[0] + 8u &&
           destinations[2] == destinations[0] + 16u &&
           destinations[3] == destinations[0] + 24u &&
           destinations[0] >= 8u;
}

static bool capture_ft4_destinations(
    CPUState *cpu, XgRenderFt4GeometryPending *geometry) {
    uint32_t stack_pointer;
    size_t index;

    if (cpu == NULL || geometry == NULL || cpu->read_word == NULL) return false;
    stack_pointer = cpu->gpr[29];
    if (!xg_render_runtime_stack_address_is_valid(stack_pointer)) return false;
    for (index = 0u; index < 4u; ++index)
        geometry->destinations[index] =
            cpu->read_word(stack_pointer + 0x10u + (uint32_t)index * 4u);
    return ft4_destinations_are_valid(geometry->destinations);
}

static bool capture_shadow_pre_transform(
    CPUState *cpu, XgRenderFt4GeometryPending *geometry) {
    uint32_t vector_addresses[4];
    uint32_t stack_pointer;
    size_t index;

    if (cpu == NULL || cpu->read_word == NULL) return false;
    stack_pointer = cpu->gpr[29];
    if (!xg_render_runtime_stack_address_is_valid(stack_pointer)) return false;
    vector_addresses[0] = cpu->gpr[4];
    vector_addresses[1] = cpu->gpr[5];
    vector_addresses[2] = cpu->gpr[6];
    if (cpu->gpr[7] == cpu->gpr[4])
        vector_addresses[3] = cpu->gpr[7] + 0x18u;
    else if (cpu->gpr[7] == cpu->gpr[4] + 0x18u)
        vector_addresses[3] = cpu->gpr[7];
    else
        return false;
    for (index = 0u; index < 4u; ++index) {
        uint32_t xy;
        uint32_t z_pad;

        if (!xg_render_runtime_vector_address_is_valid(vector_addresses[index]))
            return false;
        xy = cpu->read_word(vector_addresses[index]);
        z_pad = cpu->read_word(vector_addresses[index] + 4u);
        geometry->pre_transform.vertices[index] = (XgHost3dVector){
            xg_render_runtime_low_s16(xy),
            xg_render_runtime_low_s16(xy >> 16u),
            xg_render_runtime_low_s16(z_pad),
            (uint16_t)(z_pad >> 16u),
        };
        geometry->destinations[index] =
            cpu->read_word(stack_pointer + 0x10u + (uint32_t)index * 4u);
    }
    if (!ft4_destinations_are_valid(geometry->destinations)) return false;
    xg_render_runtime_capture_shadow_projection(
        cpu, &geometry->pre_transform.projection);
    return xg_host_3d_rot_average4(&geometry->pre_transform,
                                   &geometry->host_output) != 0;
}

static bool read_source_u16(void *context, uint32_t address,
                            uint16_t *out_value) {
    CPUState *cpu = (CPUState *)context;

    if (cpu == NULL || cpu->read_half == NULL || out_value == NULL) return false;
    *out_value = cpu->read_half(address);
    return true;
}

static bool read_source_u32(void *context, uint32_t address,
                            uint32_t *out_value) {
    CPUState *cpu = (CPUState *)context;

    if (cpu == NULL || cpu->read_word == NULL || out_value == NULL) return false;
    *out_value = cpu->read_word(address);
    return true;
}

static bool capture_source_values(
    CPUState *cpu, XgRenderFt4GeometryPending *geometry,
    uint32_t model_address, uint32_t ft4_index,
    const XgRenderFieldCharacterPipelineServices *services) {
    XgFieldCharacterSourceCaptureRequest request = { 0 };
    XgFieldCharacterAuthenticatedReader reader;
    GpuDrawState draw = { 0 };

    if (cpu == NULL || geometry == NULL || services == NULL ||
        services->capture_auth_context == NULL || cpu->read_half == NULL ||
        cpu->read_word == NULL || gpu_render_vram_mutation_overflowed())
        return false;
    memcpy(request.game_sha256, xg_render_game_identity,
           sizeof(request.game_sha256));
    memcpy(request.manifest_sha256, xg_render_manifest_identity,
           sizeof(request.manifest_sha256));
    request.source_generation = ft4_geometry.next_sequence;
    request.actor_index = cpu->gpr[21];
    request.actor_record_address = cpu->gpr[18];
    request.model_address = model_address;
    request.producer_stack_pointer = cpu->gpr[29];
    request.producer_ft4_index = ft4_index;
    request.vram_mutation_serial = gpu_render_vram_mutation_serial();
    if (!services->capture_auth_context(&request)) return false;
    gpu_get_draw_state(&draw);
    request.raster.draw_area_left = draw.left;
    request.raster.draw_area_top = draw.top;
    request.raster.draw_area_right = draw.right;
    request.raster.draw_area_bottom = draw.bottom;
    request.raster.draw_offset_x = draw.offset_x;
    request.raster.draw_offset_y = draw.offset_y;
    request.raster.texture_window_mask_x = draw.texture_window_mask_x;
    request.raster.texture_window_mask_y = draw.texture_window_mask_y;
    request.raster.texture_window_offset_x = draw.texture_window_offset_x;
    request.raster.texture_window_offset_y = draw.texture_window_offset_y;
    request.raster.dither = draw.dither;
    request.raster.mask_set = draw.mask_set;
    request.raster.mask_check = draw.mask_check;
    reader = (XgFieldCharacterAuthenticatedReader){
        cpu,
        read_source_u16,
        read_source_u32,
        request.source_generation,
        1u,
    };
    geometry->source_capture_result =
        (uint32_t)xg_field_character_source_capture(
            &request, &reader, &geometry->source_snapshot);
    geometry->source_captured = geometry->source_capture_result ==
        XG_FIELD_CHARACTER_SOURCE_CAPTURE_OK;
    return geometry->source_captured;
}

static void publish_ft4_geometry(void) {
    PsxXgRenderFt4Geometry *record;
    uint32_t index;

    if (ft4_geometry.count == PSX_XG_RENDER_FT4_GEOMETRY_CAPACITY ||
        ft4_geometry.next_sequence == 0u ||
        ft4_geometry.completed_count == UINT64_MAX) {
        block_ft4_geometry(true);
        return;
    }
    record = &ft4_geometry.records[
        (ft4_geometry.head + ft4_geometry.count) %
        PSX_XG_RENDER_FT4_GEOMETRY_CAPACITY];
    memset(record, 0, sizeof(*record));
    record->sequence = ft4_geometry.next_sequence++;
    record->semantic_template = PSX_XG_RENDER_FT4_SEMANTIC_DYNAMIC_ACTOR;
    record->tier = ft4_geometry.pending.tier;
    record->source_snapshot = ft4_geometry.pending.source_snapshot;
    record->source_capture_result = ft4_geometry.pending.source_capture_result;
    record->source_captured = ft4_geometry.pending.source_captured;
    record->packet_guest_address = xg_render_runtime_guest_address(
        ft4_geometry.pending.destinations[0] - 8u);
    if (ft4_geometry.pending.shadow_oracle) {
        record->pre_transform = ft4_geometry.pending.pre_transform;
        record->ordering_depth = ft4_geometry.pending.host_output.ordering_depth;
        record->depth_cue = ft4_geometry.pending.host_output.depth_cue;
        record->projection_flags =
            ft4_geometry.pending.host_output.projection_flags;
        record->host_transformed = true;
        for (index = 0u; index < 4u; ++index) {
            record->x[index] =
                ft4_geometry.pending.host_output.vertices[index].x;
            record->y[index] =
                ft4_geometry.pending.host_output.vertices[index].y;
        }
    }
    ++ft4_geometry.count;
    ++ft4_geometry.completed_count;
    clear_ft4_geometry_pending();
}

static void arm_ft4_geometry(
    CPUState *cpu, const XgRenderSourcePending *pending,
    const XgRenderFieldCharacterPipelineServices *services) {
    GuestRenderBridgeSnapshot bridge_snapshot = { 0 };

    if (!ft4_geometry.enabled || ft4_geometry.blocked ||
        pending->metadata.operation != PSX_XG_RENDER_SOURCE_OPERATION_CALL)
        return;
    if (ft4_geometry.pending.valid) {
        block_ft4_geometry(false);
        return;
    }
    if (services == NULL || services->scene_generation == NULL) {
        block_ft4_geometry(false);
        return;
    }
    ft4_geometry.pending = (XgRenderFt4GeometryPending){
        .scene_generation = services->scene_generation(),
        .source_return_address = pending->pc + 8u,
        .tier = pending->tier,
        .phase = XG_RENDER_FT4_EXPECT_FIRST_PRE,
        .valid = true,
    };
    if (guest_render_bridge_snapshot(&bridge_snapshot) != GUEST_RENDER_OK) {
        block_ft4_geometry(false);
        return;
    }
    if (!capture_ft4_destinations(cpu, &ft4_geometry.pending)) {
        block_ft4_geometry(false);
        return;
    }
    (void)capture_source_values(cpu, &ft4_geometry.pending, cpu->gpr[4],
                                cpu->gpr[8], services);
    if (bridge_snapshot.modes.effective_render_mode ==
        GUEST_RENDER_RENDER_NATIVE) {
        if (ft4_geometry.pending.source_captured &&
            xg_field_character_runtime_build_candidate(
                &ft4_geometry.pending.source_snapshot,
                &ft4_geometry.pending.native_candidate) ==
                XG_FIELD_CHARACTER_RUNTIME_OK) {
            ft4_geometry.pending.host_output =
                ft4_geometry.pending.native_candidate.source_derived.projection;
            ft4_geometry.pending.native_ready = true;
            ++ft4_geometry.host_transform_count;
        }
        return;
    }
    if (bridge_snapshot.modes.effective_render_mode !=
        GUEST_RENDER_RENDER_SHADOW) {
        clear_ft4_geometry_pending();
        return;
    }
    ft4_geometry.pending.shadow_oracle = true;
    if (!capture_shadow_pre_transform(cpu, &ft4_geometry.pending)) {
        block_ft4_geometry(false);
        return;
    }
    ++ft4_geometry.host_transform_count;
}

void xg_render_field_character_reject(
    uint32_t blocker,
    const XgRenderFieldCharacterPipelineServices *services) {
    producer_family.blocked = true;
    if (producer_family.blocker == 0u) producer_family.blocker = blocker;
    xg_render_submission_reject_producer();
    if (services != NULL && services->reject_auth != NULL)
        services->reject_auth();
}

bool xg_render_field_character_ft4_geometry_pop(
    PsxXgRenderFt4Geometry *out_geometry) {
    if (out_geometry == NULL || ft4_geometry.blocked ||
        ft4_geometry.count == 0u)
        return false;
    *out_geometry = ft4_geometry.records[ft4_geometry.head];
    memset(&ft4_geometry.records[ft4_geometry.head], 0,
           sizeof(ft4_geometry.records[ft4_geometry.head]));
    ft4_geometry.head =
        (ft4_geometry.head + 1u) % PSX_XG_RENDER_FT4_GEOMETRY_CAPACITY;
    --ft4_geometry.count;
    return true;
}

static void process_producer_family_candidate(
    CPUState *cpu, uint32_t ot_bucket,
    const XgRenderFieldCharacterPipelineServices *services) {
    PsxXgRenderFt4Geometry geometry;
    XgFieldCharacterRuntimeCandidate candidate;
    XgRenderIrNativePrimitive primitive;
    GpuRenderSemantic semantic;
    GpuRenderTransactionId visual_id;
    XgFieldCharacterRuntimeResult result;
    XgRenderFieldCharacterStageResult stage_result;
    uint32_t compare_result = 0u;
    uint32_t mismatch_word = UINT32_MAX;
    uint32_t mismatch_byte = UINT32_MAX;
    uint32_t packet_address;
    uint32_t source_primitive_index;
    GuestRenderBridgeSnapshot bridge_snapshot = { 0 };

    if (!producer_family.enabled || producer_family.blocked) return;
    if (guest_render_bridge_snapshot(&bridge_snapshot) != GUEST_RENDER_OK) {
        xg_render_field_character_reject(9u, services);
        return;
    }
    if (bridge_snapshot.modes.effective_render_mode ==
        GUEST_RENDER_RENDER_ORIGINAL)
        return;
    if (cpu == NULL ||
        !xg_render_field_character_ft4_geometry_pop(&geometry)) {
        xg_render_field_character_reject(1u, services);
        return;
    }
    ++producer_family.geometry_count;
    if (bridge_snapshot.modes.effective_render_mode ==
        GUEST_RENDER_RENDER_NATIVE) {
        if (!geometry.source_captured) {
            producer_family.last_runtime_result =
                XG_FIELD_CHARACTER_RUNTIME_INCOMPLETE_SOURCE;
            ++producer_family.source_incomplete_count;
            xg_render_field_character_reject(10u, services);
            return;
        }
        ++producer_family.source_capture_count;
        result = xg_field_character_runtime_build_candidate(
            &geometry.source_snapshot, &candidate);
        producer_family.last_runtime_result = (uint32_t)result;
        if (result != XG_FIELD_CHARACTER_RUNTIME_OK) {
            ++producer_family.source_incomplete_count;
            xg_render_field_character_reject(11u, services);
            return;
        }
        candidate.packet_guest_address = geometry.packet_guest_address;
        packet_address = geometry.packet_guest_address & UINT32_C(0x001ffffc);
        if (geometry.source_snapshot.identity.actor_index >
            (UINT32_MAX - geometry.source_snapshot.ordering.ft4_index) /
                XG_FIELD_CHARACTER_SOURCE_DYNAMIC_MODEL_FT4_COUNT) {
            xg_render_field_character_reject(12u, services);
            return;
        }
        source_primitive_index =
            geometry.source_snapshot.identity.actor_index *
                XG_FIELD_CHARACTER_SOURCE_DYNAMIC_MODEL_FT4_COUNT +
            geometry.source_snapshot.ordering.ft4_index;
        producer_family.last_ot_bucket =
            candidate.source_derived.ordering_bucket;
        if (candidate.source_derived.ordering_bucket != ot_bucket ||
            xg_field_character_adapter_build_primitive(
                &candidate.candidate, &primitive) !=
                XG_FIELD_CHARACTER_ADAPTER_OK) {
            xg_render_field_character_reject(12u, services);
            return;
        }
        xg_render_primitive_apply_projected_quad_positions(
            &primitive, candidate.source_derived.projection.vertices);
        if (xg_render_backend_translate_primitive(&primitive, &semantic) !=
            XG_RENDER_BACKEND_OK) {
            xg_render_field_character_reject(12u, services);
            return;
        }
        if (services == NULL ||
            services->interpolation_scene_generation == NULL ||
            services->stage_candidate == NULL) {
            xg_render_field_character_reject(13u, services);
            return;
        }
        xg_render_semantic_set_interpolation_identity(
            &semantic, services->interpolation_scene_generation(),
            geometry.source_snapshot.identity.producer_record_id,
            source_primitive_index);
        visual_id = (GpuRenderTransactionId){
            geometry.source_snapshot.generation.visual_state.scene_epoch,
            geometry.source_snapshot.generation.visual_state.state_sequence,
        };
        stage_result = services->stage_candidate(
            &primitive, &semantic, packet_address, source_primitive_index,
            candidate.source_derived.ordering_bucket, visual_id);
        if (stage_result == XG_RENDER_FIELD_CHARACTER_STAGE_AUTH_FAILED) {
            xg_render_field_character_reject(13u, services);
            return;
        }
        if (stage_result != XG_RENDER_FIELD_CHARACTER_STAGE_OK) {
            xg_render_field_character_reject(14u, services);
            return;
        }
        ++producer_family.candidate_count;
        return;
    }
    if (bridge_snapshot.modes.effective_render_mode !=
        GUEST_RENDER_RENDER_SHADOW)
        return;
    if (!geometry.source_captured) {
        producer_family.last_runtime_result =
            XG_FIELD_CHARACTER_RUNTIME_INCOMPLETE_SOURCE;
        ++producer_family.source_incomplete_count;
        xg_render_field_character_reject(2u, services);
        return;
    }
    ++producer_family.source_capture_count;
    result = xg_field_character_runtime_build_candidate(
        &geometry.source_snapshot, &candidate);
    producer_family.last_runtime_result = (uint32_t)result;
    if (result != XG_FIELD_CHARACTER_RUNTIME_OK) {
        xg_render_field_character_reject(2u, services);
        return;
    }
    candidate.packet_guest_address = geometry.packet_guest_address;
    ++producer_family.candidate_count;
    producer_family.last_ot_bucket = candidate.source_derived.ordering_bucket;
    result = xg_field_character_runtime_compare_original(
        cpu, &candidate, ot_bucket, candidate.source_derived.ordering_bucket,
        &primitive, &compare_result, &mismatch_word, &mismatch_byte);
    producer_family.last_runtime_result = (uint32_t)result;
    producer_family.last_compare_result = compare_result;
    producer_family.first_mismatch_word = mismatch_word;
    producer_family.first_mismatch_byte = mismatch_byte;
    if (result != XG_FIELD_CHARACTER_RUNTIME_OK) {
        ++producer_family.mismatch_count;
        xg_render_field_character_reject(3u, services);
        return;
    }
    ++producer_family.match_count;
}

bool xg_render_field_character_observe_source(
    CPUState *cpu, XgRenderAuthTier tier, PsxXgRenderSourceStage stage,
    uint32_t pc, uint32_t instruction_word, uint32_t auxiliary,
    const XgRenderFieldCharacterPipelineServices *services) {
    PsxXgRenderSourceSiteMetadata metadata = { 0 };

    if (source_state.aggregate.blocked) return false;
    if (stage != PSX_XG_RENDER_SOURCE_STAGE_PRE &&
        stage != PSX_XG_RENDER_SOURCE_STAGE_COMMIT) {
        block_source_proof(false, XG_RENDER_SOURCE_BLOCK_INVALID_STAGE);
        return false;
    }
    if (!source_context_matches(tier, services)) {
        block_source_proof(false, XG_RENDER_SOURCE_BLOCK_CONTEXT);
        return false;
    }
    if (services == NULL || services->source_site_lookup == NULL ||
        !services->source_site_lookup(pc, instruction_word, &metadata)) {
        block_source_proof(false, XG_RENDER_SOURCE_BLOCK_SITE);
        return false;
    }
    if (!source_auxiliary_is_valid(stage, &metadata, auxiliary)) {
        block_source_proof(false, XG_RENDER_SOURCE_BLOCK_AUXILIARY);
        return false;
    }
    if (stage == PSX_XG_RENDER_SOURCE_STAGE_PRE) {
        if (source_state.pending.valid) {
            block_source_proof(false, XG_RENDER_SOURCE_BLOCK_PENDING);
            return false;
        }
        source_state.pending = (XgRenderSourcePending){
            .metadata = metadata,
            .pc = xg_render_runtime_guest_address(pc),
            .instruction = instruction_word,
            .auxiliary = auxiliary,
            .tier = tier,
            .valid = true,
        };
        arm_ft4_geometry(cpu, &source_state.pending, services);
        return true;
    }
    if (!source_state.pending.valid ||
        !physical_address_equals(pc, source_state.pending.pc) ||
        instruction_word != source_state.pending.instruction ||
        metadata.operation != source_state.pending.metadata.operation ||
        metadata.width != source_state.pending.metadata.width ||
        metadata.auxiliary_rule !=
            source_state.pending.metadata.auxiliary_rule ||
        (metadata.auxiliary_rule ==
             PSX_XG_RENDER_SOURCE_AUXILIARY_EFFECTIVE_ADDRESS &&
         auxiliary != source_state.pending.auxiliary)) {
        block_source_proof(false, XG_RENDER_SOURCE_BLOCK_PAIR);
        return false;
    }
    if (!collect_source_pair(&source_state.pending)) {
        block_source_proof(false, XG_RENDER_SOURCE_BLOCK_COLLECTOR);
        return false;
    }
    append_source_pair(xg_render_runtime_guest_address(pc),
                       &source_state.pending, &metadata, auxiliary);
    if (source_state.pending.metadata.operation ==
        PSX_XG_RENDER_SOURCE_OPERATION_BUCKET)
        process_producer_family_candidate(cpu, auxiliary, services);
    clear_source_pending();
    return true;
}

static bool ft4_context_matches(
    const CPUState *cpu,
    const XgRenderFieldCharacterPipelineServices *services) {
    return cpu != NULL && ft4_geometry.pending.valid &&
           !ft4_geometry.blocked && services != NULL &&
           services->scene_generation != NULL &&
           ft4_geometry.pending.scene_generation ==
               services->scene_generation() &&
           source_context_matches(ft4_geometry.pending.tier, services) &&
           physical_address_equals(
               cpu->gpr[31], ft4_geometry.pending.source_return_address);
}

bool xg_render_field_character_native_actor_cutover(
    CPUState *cpu, uint32_t continuation, XgRenderAuthTier tier,
    const XgRenderFieldCharacterPipelineServices *services) {
    GuestRenderBridgeSnapshot bridge = { 0 };
    XgRenderFt4GeometryPending *pending = &ft4_geometry.pending;
    uint32_t actor_address;
    uint32_t model_address;
    uint32_t ft4_index;
    uint32_t packet_address;
    uint32_t packet_offset;
    uint32_t ot_base;
    uint32_t ot_bucket;
    uint32_t ot_address;
    uint32_t packet_tag;
    uint32_t previous_head;
    uint32_t index;

    if (cpu == NULL || cpu->read_word == NULL || cpu->write_word == NULL ||
        guest_render_bridge_snapshot(&bridge) != GUEST_RENDER_OK ||
        bridge.modes.effective_render_mode != GUEST_RENDER_RENDER_NATIVE)
        return false;
    if (!producer_family.enabled || producer_family.blocked) return false;
    if (!ft4_geometry.enabled || ft4_geometry.blocked || pending->valid ||
        !source_context_matches(tier, services) ||
        !xg_render_runtime_stack_address_is_valid(cpu->gpr[29])) {
        xg_render_field_character_reject(17u, services);
        return false;
    }
    actor_address = cpu->gpr[18];
    if (actor_address > UINT32_MAX - 8u ||
        !xg_render_runtime_word_address_is_valid(actor_address + 8u)) {
        xg_render_field_character_reject(18u, services);
        return false;
    }
    model_address = cpu->read_word(actor_address + 8u);
    ft4_index = cpu->read_word(cpu->gpr[29] + 0xd8u);
    if (ft4_index >= XG_FIELD_CHARACTER_SOURCE_DYNAMIC_MODEL_FT4_COUNT ||
        ft4_index > (UINT32_MAX - 0x40u) / 0x28u) {
        xg_render_field_character_reject(18u, services);
        return false;
    }
    packet_offset = ft4_index * 0x28u + 0x20u;
    if (model_address > UINT32_MAX - packet_offset - 0x20u) {
        xg_render_field_character_reject(18u, services);
        return false;
    }
    packet_address = model_address + packet_offset;
    if (services == NULL || services->scene_generation == NULL) {
        xg_render_field_character_reject(19u, services);
        return false;
    }
    *pending = (XgRenderFt4GeometryPending){
        .scene_generation = services->scene_generation(),
        .tier = tier,
        .phase = XG_RENDER_FT4_EXPECT_FIRST_PRE,
        .valid = true,
    };
    for (index = 0u; index < 4u; ++index)
        pending->destinations[index] = packet_address + 8u + index * 8u;
    if (!ft4_destinations_are_valid(pending->destinations) ||
        !capture_source_values(cpu, pending, model_address, ft4_index,
                               services) ||
        xg_field_character_runtime_build_candidate(
            &pending->source_snapshot, &pending->native_candidate) !=
            XG_FIELD_CHARACTER_RUNTIME_OK) {
        block_ft4_geometry(false);
        xg_render_field_character_reject(19u, services);
        return false;
    }
    pending->host_output = pending->native_candidate.source_derived.projection;
    pending->native_ready = true;
    ++ft4_geometry.host_transform_count;

    ot_base = cpu->read_word(cpu->gpr[29] + 0xd0u);
    ot_bucket = pending->native_candidate.source_derived.ordering_bucket;
    if (ot_bucket > (UINT32_MAX - ot_base) / 4u ||
        !xg_render_runtime_word_address_is_valid(packet_address) ||
        !xg_render_runtime_word_address_is_valid(ot_base + ot_bucket * 4u) ||
        ft4_geometry.count == PSX_XG_RENDER_FT4_GEOMETRY_CAPACITY ||
        ft4_geometry.next_sequence == 0u ||
        ft4_geometry.completed_count == UINT64_MAX) {
        block_ft4_geometry(
            ft4_geometry.count == PSX_XG_RENDER_FT4_GEOMETRY_CAPACITY);
        xg_render_field_character_reject(20u, services);
        return false;
    }
    ot_address = ot_base + ot_bucket * 4u;
    packet_tag = cpu->read_word(packet_address);
    previous_head = cpu->read_word(ot_address);
    for (index = 0u; index < 4u; ++index) {
        const XgHost3dProjectedVertex *vertex =
            &pending->host_output.vertices[index];
        const uint32_t xy = (uint16_t)vertex->x |
            ((uint32_t)(uint16_t)vertex->y << 16u);

        psx_store_cycle_barrier();
        cpu->write_word(pending->destinations[index], xy);
    }
    psx_store_cycle_barrier();
    cpu->write_word(packet_address,
                    (packet_tag & UINT32_C(0xff000000)) |
                    (previous_head & UINT32_C(0x00ffffff)));
    psx_store_cycle_barrier();
    cpu->write_word(ot_address,
                    (previous_head & UINT32_C(0xff000000)) |
                    (packet_address & UINT32_C(0x00ffffff)));
    cpu->gpr[2] = pending->host_output.ordering_depth;
    publish_ft4_geometry();
    if (!ft4_geometry.blocked)
        process_producer_family_candidate(cpu, ot_bucket, services);
    cpu->pc = continuation;
    return true;
}

bool xg_render_field_character_native_bypass(
    CPUState *cpu, uint32_t pc, uint32_t instruction_word,
    const XgRenderFieldCharacterPipelineServices *services) {
    GuestRenderBridgeSnapshot bridge = { 0 };
    XgRenderFt4GeometryPending *pending = &ft4_geometry.pending;
    PsxXgRenderSourceSiteMetadata source_metadata = { 0 };
    uint32_t depth_cue_address;
    uint32_t flags_address;
    uint32_t index;

    if (cpu == NULL || !pending->valid || services == NULL ||
        services->source_site_lookup == NULL ||
        !services->source_site_lookup(pc, instruction_word, &source_metadata) ||
        source_metadata.operation != PSX_XG_RENDER_SOURCE_OPERATION_CALL ||
        guest_render_bridge_snapshot(&bridge) != GUEST_RENDER_OK ||
        bridge.modes.effective_render_mode != GUEST_RENDER_RENDER_NATIVE)
        return false;
    if (!pending->native_ready || !pending->source_captured ||
        !source_context_matches(pending->tier, services) ||
        cpu->read_word == NULL || cpu->write_word == NULL ||
        !ft4_destinations_are_valid(pending->destinations)) {
        xg_render_field_character_reject(15u, services);
        return false;
    }
    depth_cue_address = cpu->read_word(cpu->gpr[29] + 0x20u);
    flags_address = cpu->read_word(cpu->gpr[29] + 0x24u);
    if (!xg_render_runtime_word_address_is_valid(depth_cue_address) ||
        !xg_render_runtime_word_address_is_valid(flags_address)) {
        xg_render_field_character_reject(16u, services);
        return false;
    }
    for (index = 0u; index < 4u; ++index) {
        const XgHost3dProjectedVertex *vertex =
            &pending->host_output.vertices[index];
        const uint32_t xy = (uint16_t)vertex->x |
            ((uint32_t)(uint16_t)vertex->y << 16u);

        psx_store_cycle_barrier();
        cpu->write_word(pending->destinations[index], xy);
    }
    psx_store_cycle_barrier();
    cpu->write_word(depth_cue_address,
                    (uint32_t)(int32_t)pending->host_output.depth_cue);
    psx_store_cycle_barrier();
    cpu->write_word(flags_address, pending->host_output.projection_flags);
    cpu->gpr[2] = pending->host_output.ordering_depth;
    publish_ft4_geometry();
    return !ft4_geometry.blocked;
}

bool xg_render_field_character_resident_ft4_observe(
    CPUState *cpu, uint32_t stage, uint32_t pc, uint32_t instruction_word,
    const XgRenderFieldCharacterPipelineServices *services) {
    XgRenderFt4GeometryPending *pending = &ft4_geometry.pending;

    if (!ft4_geometry.enabled || !pending->valid) return false;
    if (!ft4_context_matches(cpu, services) ||
        stage > PSX_XG_RENDER_SOURCE_STAGE_COMMIT) {
        block_ft4_geometry(false);
        return false;
    }
    switch (pending->phase) {
    case XG_RENDER_FT4_EXPECT_FIRST_PRE:
        if (stage != PSX_XG_RENDER_SOURCE_STAGE_PRE ||
            !physical_address_equals(pc, UINT32_C(0x8004a7e8)) ||
            instruction_word != UINT32_C(0xe90c0000))
            break;
        if (!physical_address_equals(pending->destinations[0], cpu->gpr[8]) ||
            !physical_address_equals(pending->destinations[1], cpu->gpr[9]) ||
            !physical_address_equals(pending->destinations[2], cpu->gpr[10]) ||
            (uint32_t)(uint16_t)pending->host_output.vertices[0].x !=
                (cpu->gte_data[12] & 0xffffu) ||
            (uint32_t)(uint16_t)pending->host_output.vertices[0].y !=
                (cpu->gte_data[12] >> 16u) ||
            (uint32_t)(uint16_t)pending->host_output.vertices[1].x !=
                (cpu->gte_data[13] & 0xffffu) ||
            (uint32_t)(uint16_t)pending->host_output.vertices[1].y !=
                (cpu->gte_data[13] >> 16u) ||
            (uint32_t)(uint16_t)pending->host_output.vertices[2].x !=
                (cpu->gte_data[14] & 0xffffu) ||
            (uint32_t)(uint16_t)pending->host_output.vertices[2].y !=
                (cpu->gte_data[14] >> 16u) ||
            pending->host_output.rtpt_flags != cpu->gte_ctrl[31]) {
            ++ft4_geometry.oracle_mismatch_count;
            break;
        }
        pending->phase = XG_RENDER_FT4_EXPECT_FIRST_COMMIT;
        return true;
    case XG_RENDER_FT4_EXPECT_FIRST_COMMIT:
        if (stage == PSX_XG_RENDER_SOURCE_STAGE_COMMIT &&
            physical_address_equals(pc, UINT32_C(0x8004a7e8)) &&
            instruction_word == UINT32_C(0xe90c0000)) {
            pending->phase = XG_RENDER_FT4_EXPECT_SECOND_PRE;
            return true;
        }
        break;
    case XG_RENDER_FT4_EXPECT_SECOND_PRE:
        if (stage == PSX_XG_RENDER_SOURCE_STAGE_PRE &&
            physical_address_equals(pc, UINT32_C(0x8004a814)) &&
            instruction_word == UINT32_C(0xe90e0000)) {
            if (!physical_address_equals(pending->destinations[3],
                                         cpu->gpr[8]) ||
                (uint32_t)(uint16_t)pending->host_output.vertices[3].x !=
                    (cpu->gte_data[14] & 0xffffu) ||
                (uint32_t)(uint16_t)pending->host_output.vertices[3].y !=
                    (cpu->gte_data[14] >> 16u) ||
                pending->host_output.rtps_flags != cpu->gte_ctrl[31]) {
                ++ft4_geometry.oracle_mismatch_count;
                break;
            }
            pending->phase = XG_RENDER_FT4_EXPECT_SECOND_COMMIT;
            return true;
        }
        break;
    case XG_RENDER_FT4_EXPECT_SECOND_COMMIT:
        if (stage == PSX_XG_RENDER_SOURCE_STAGE_COMMIT &&
            physical_address_equals(pc, UINT32_C(0x8004a814)) &&
            instruction_word == UINT32_C(0xe90e0000)) {
            ++ft4_geometry.oracle_match_count;
            publish_ft4_geometry();
            return !ft4_geometry.blocked;
        }
        break;
    }
    block_ft4_geometry(false);
    return false;
}

void xg_render_field_character_disarm(void) {
    block_source_proof(false, XG_RENDER_SOURCE_BLOCK_LIFECYCLE);
    if (ft4_geometry.enabled) block_ft4_geometry(false);
    if (producer_family.enabled) {
        producer_family.blocked = true;
        if (!producer_family.blocker) producer_family.blocker = 4u;
    }
}

void xg_render_field_character_scene_boundary(void) {
    if (source_state.pending.valid)
        block_source_proof(false, XG_RENDER_SOURCE_BLOCK_LIFECYCLE);
    reset_ft4_geometry(true);
}

void xg_render_field_character_source_reset(void) {
    source_state = (XgRenderSourceState){
        .aggregate = { .next_sequence = 1u },
        .next_auth_sequence = 1u,
    };
    field_character_shadow_init(&source_state.collector);
    reset_ft4_geometry(false);
    producer_family = (PsxXgRenderProducerFamilySnapshot){ 0 };
}

void xg_render_field_character_reset(void) {
    xg_render_field_character_source_reset();
}

void xg_render_field_character_handle_invalidation(
        const XgRenderInvalidationEvent *event,
        const XgRenderInvalidationServices *services) {
    (void)services;
    if (event->kind == XG_RENDER_INVALIDATION_SCENE_BOUNDARY)
        xg_render_field_character_scene_boundary();
    else if (event->kind == XG_RENDER_INVALIDATION_AUTHORITY_LOST)
        xg_render_field_character_disarm();
    else if (event->kind == XG_RENDER_INVALIDATION_RESET)
        xg_render_field_character_reset();
}

void xg_render_field_character_ft4_geometry_enable(bool enabled) {
    reset_ft4_geometry(false);
    ft4_geometry.enabled = enabled;
}

void xg_render_field_character_producer_family_enable(bool enabled) {
    xg_render_field_character_source_reset();
    producer_family = (PsxXgRenderProducerFamilySnapshot){
        .enabled = enabled,
    };
    ft4_geometry.enabled = enabled;
}

void xg_render_field_character_note_pre_scene_blocker(uint32_t blocker) {
    if (!producer_family.blocker)
        producer_family.blocker = 30u + blocker;
}

bool xg_render_field_character_source_blocked(void) {
    return source_state.aggregate.blocked;
}

bool xg_render_field_character_producer_family_enabled(void) {
    return producer_family.enabled;
}

void xg_render_field_character_source_snapshot(
    PsxXgRenderSourceSnapshot *out_snapshot) {
    if (out_snapshot != NULL) *out_snapshot = source_state.aggregate;
}

void xg_render_field_character_source_collector_snapshot(
    FieldCharacterShadowSummary *out_summary) {
    if (out_summary != NULL)
        (void)field_character_shadow_snapshot(&source_state.collector,
                                              out_summary);
}

void xg_render_field_character_ft4_geometry_snapshot(
    PsxXgRenderFt4GeometrySnapshot *out_snapshot) {
    if (out_snapshot == NULL) return;
    *out_snapshot = (PsxXgRenderFt4GeometrySnapshot){
        .completed_count = ft4_geometry.completed_count,
        .host_transform_count = ft4_geometry.host_transform_count,
        .oracle_match_count = ft4_geometry.oracle_match_count,
        .oracle_mismatch_count = ft4_geometry.oracle_mismatch_count,
        .queued_count = ft4_geometry.count,
        .enabled = ft4_geometry.enabled,
        .pending = ft4_geometry.pending.valid,
        .blocked = ft4_geometry.blocked,
        .overflowed = ft4_geometry.overflowed,
    };
}

void xg_render_field_character_producer_family_snapshot(
    PsxXgRenderProducerFamilySnapshot *out_snapshot) {
    if (out_snapshot != NULL) *out_snapshot = producer_family;
}
