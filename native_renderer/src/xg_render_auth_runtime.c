#include "xg_render_auth_runtime.h"

#include "xg_render_auth.h"
#include "xg_render_manifest_generated.h"
#include "xg_render_runtime_variant_auth.h"
#include "xg_render_static_auth_metadata.h"
#include "xg_field_character_runtime.h"
#include "xg_field_character_source_capture.h"
#include "xg_field_compass.h"
#include "xg_field_render_services.h"
#include "xg_field_particles.h"
#include "xg_field_projected.h"
#include "xg_field_zoom.h"
#include "xg_host_3d.h"
#include "xg_model_ft4_raw.h"
#include "xg_render_backend.h"
#include "xg_render_quad_builder.h"
#include "xg_sprite_ft4.h"
#include "xg_world_effects.h"
#include "xg_world_effects_source_capture.h"
#include "xg_world_horizon.h"
#include "xg_world_horizon_source_capture.h"
#include "xg_world_actor_sprites_source_capture.h"
#include "xg_world_clouds_source_capture.h"
#include "xg_world_decorations_source_capture.h"
#include "xg_world_entity_shadows_source_capture.h"
#include "xg_world_minimap.h"
#include "xg_world_minimap_source_capture.h"
#include "xg_world_models_native.h"
#include "xg_world_sky.h"
#include "xg_world_terrain_water_source_capture.h"
#include "guest_render_native_stream.h"
#include "guest_render_transaction.h"
#include "gte_attribution.h"
#include "gpu.h"

#include <stdbool.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

#ifndef PSX_XG_RENDER_AUTH_RUNTIME_TESTING
extern uint32_t psx_read_word(uint32_t address);
extern uint64_t s_frame_count;
#endif

typedef struct XgRenderAuthRuntimeState {
    XgRenderAuth *auth;
    PsxXgRenderAuthCandidate pending_candidate;
    PsxXgRenderAuthCandidate authenticated_artifact_candidate;
    PsxXgRenderAuthRejectionReceipt rejection;
    uint64_t scene_generation;
    uint64_t interpolation_scene_generation;
    uint64_t pending_scene_generation;
    uint64_t authenticated_artifact_scene_generation;
    uint64_t authenticated_artifact_generation;
    uint64_t authenticated_producer_scene_generation;
    uint32_t authenticated_producer_entry;
    uint32_t pending_variant_entry;
    uint32_t pending_variant_instruction;
    uint32_t pending_variant_delay_slot;
    XgRenderAuthTier pending_variant_tier;
    bool armed;
    bool active;
    bool completed;
    bool pending_candidate_valid;
    bool authenticated_artifact_candidate_valid;
    bool pending_variant_sequence;
    bool pending_variant_capture_ready;
    bool candidate_matched;
    bool candidate_dispatched;
    bool gte_attribution_producer_active;
    GuestRenderTimingMode requested_timing_mode;
    GuestRenderRenderMode requested_render_mode;
    PsxXgRenderPresentationGate presentation_gate;
    void *presentation_user_data;
    NativeRenderPresentationSnapshot presentation;
    bool configured;
} XgRenderAuthRuntimeState;

typedef struct XgRenderProducerLifecycle {
    uint64_t artifact_generation;
    uint64_t resource_generation;
    uint64_t scene_generation;
    uint32_t producer_pc;
    /* Resident resources survive scene changes and are invalidated by writes;
     * overlay resources are additionally bound to one authenticated scene.
     * No-gates shared resources are scene-scoped without requiring an artifact. */
    uint8_t scene_resource;
} XgRenderProducerLifecycle;

static void (*producer_resource_watch)(uint32_t physical_address,
                                       uint32_t size);

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

enum {
    XG_RENDER_PRE_SCENE_PRIMITIVE_CAPACITY = XG_RENDER_IR_ITEM_CAPACITY,
    XG_RENDER_SPRITE_FT4_NATIVE_CAPACITY = 64u,
    XG_RENDER_MODEL_FT4_SHADOW_CAPACITY = XG_RENDER_IR_ITEM_CAPACITY,
    XG_RENDER_WORLD_MODEL_RECORD_CAPACITY = 256u,
    XG_RENDER_WORLD_MODEL_NODE_CAPACITY = 1024u,
    XG_RENDER_WORLD_MODEL_DISPATCH_CAPACITY = 256u,
    XG_RENDER_WORLD_MODEL_PRIMITIVE_CAPACITY = 4096u,
    XG_RENDER_WORLD_MODEL_ANCHOR_CAPACITY =
        XG_RENDER_WORLD_MODEL_PRIMITIVE_CAPACITY * 4u,
    XG_RENDER_WORLD_MODEL_TEMPLATE_CAPACITY = 16384u,
    XG_RENDER_WORLD_MODEL_COLOR_WRITE_CAPACITY = 16384u,
    XG_RENDER_CANONICAL_ENTRY_INSTRUCTION = UINT32_C(0x27bdff18),
    XG_RENDER_CANONICAL_CAPTURE_INSTRUCTION = UINT32_C(0x0c012d53),
    XG_RENDER_CANONICAL_RETURN_INSTRUCTION = UINT32_C(0x0c01d1c0),
};

enum {
    XG_RENDER_MODEL_PRECONDITION_CONTEXT = 1u << 0,
    XG_RENDER_MODEL_PRECONDITION_CPU = 1u << 1,
    XG_RENDER_MODEL_PRECONDITION_CALLBACKS = 1u << 2,
    XG_RENDER_MODEL_PRECONDITION_RETURN = 1u << 3,
    XG_RENDER_MODEL_PRECONDITION_TARGET_ZERO = 1u << 4,
    XG_RENDER_MODEL_PRECONDITION_TARGET_CAPACITY = 1u << 5,
    XG_RENDER_MODEL_PRECONDITION_VERTEX_BASE = 1u << 6,
    XG_RENDER_MODEL_PRECONDITION_OT_BASE = 1u << 7,
};

enum {
    XG_RENDER_WORLD_MODELS_PREPARE_FAILURE_PRECONDITION = 1u,
    XG_RENDER_WORLD_MODELS_PREPARE_FAILURE_CALLER_RETURN,
    XG_RENDER_WORLD_MODELS_PREPARE_FAILURE_CONTRACT,
    XG_RENDER_WORLD_MODELS_PREPARE_FAILURE_GLOBAL_RANGE,
    XG_RENDER_WORLD_MODELS_PREPARE_FAILURE_NODE_RANGE,
    XG_RENDER_WORLD_MODELS_PREPARE_FAILURE_CULLED_DISPATCH,
    XG_RENDER_WORLD_MODELS_PREPARE_FAILURE_DISPATCH_RANGE,
    XG_RENDER_WORLD_MODELS_PREPARE_FAILURE_SOURCE_IDENTITY,
    XG_RENDER_WORLD_MODELS_PREPARE_FAILURE_TEMPLATE_MISSING,
    XG_RENDER_WORLD_MODELS_PREPARE_FAILURE_TEMPLATE_INACTIVE,
    XG_RENDER_WORLD_MODELS_PREPARE_FAILURE_TEMPLATE_OWNER,
    XG_RENDER_WORLD_MODELS_PREPARE_FAILURE_TEMPLATE_EPOCH,
    XG_RENDER_WORLD_MODELS_PREPARE_FAILURE_TEMPLATE_MODEL,
    XG_RENDER_WORLD_MODELS_PREPARE_FAILURE_TEMPLATE_PACKET_BASE,
    XG_RENDER_WORLD_MODELS_PREPARE_FAILURE_TEMPLATE_ORDINAL,
    XG_RENDER_WORLD_MODELS_PREPARE_FAILURE_TEMPLATE_ATTRIBUTE,
    XG_RENDER_WORLD_MODELS_PREPARE_FAILURE_TEMPLATE_FAMILY,
    XG_RENDER_WORLD_MODELS_PREPARE_FAILURE_TEMPLATE_WORD_COUNT,
    XG_RENDER_WORLD_MODELS_PREPARE_FAILURE_OUTPUT_PACKET,
    XG_RENDER_WORLD_MODELS_PREPARE_FAILURE_OUTPUT_WORD_COUNT,
    XG_RENDER_WORLD_MODELS_PREPARE_FAILURE_OUTPUT_COUNTER,
    XG_RENDER_WORLD_MODELS_PREPARE_FAILURE_PACKET_RANGE,
    XG_RENDER_WORLD_MODELS_PREPARE_FAILURE_OUTPUT_MASK,
    XG_RENDER_WORLD_MODELS_PREPARE_FAILURE_OUTPUT_ACCEPTED,
    XG_RENDER_WORLD_MODELS_PREPARE_FAILURE_OUTPUT_TAG,
    XG_RENDER_WORLD_MODELS_PREPARE_FAILURE_PACKET_WORD,
    XG_RENDER_WORLD_MODELS_PREPARE_FAILURE_OT_RANGE,
    XG_RENDER_WORLD_MODELS_PREPARE_FAILURE_PRIMITIVE_TOTAL,
    XG_RENDER_WORLD_MODELS_PREPARE_FAILURE_COMMIT_OVERFLOW,
    XG_RENDER_WORLD_MODELS_PREPARE_FAILURE_COMMIT_TOTAL,
    XG_RENDER_WORLD_MODELS_PREPARE_FAILURE_NATIVE_RESULT_BASE = 0x100u,
    XG_RENDER_WORLD_MODELS_PREPARE_FAILURE_BUILD_RESULT_BASE = 0x200u,
    XG_RENDER_WORLD_MODELS_PREPARE_FAILURE_TEMPLATE_READ_BASE = 0x300u,
};

enum {
    XG_RENDER_WORLD_MODEL_TEMPLATE_READ_INVALID_ARGUMENT = 1u,
    XG_RENDER_WORLD_MODEL_TEMPLATE_READ_MISSING,
    XG_RENDER_WORLD_MODEL_TEMPLATE_READ_INACTIVE,
    XG_RENDER_WORLD_MODEL_TEMPLATE_READ_EPOCH,
    XG_RENDER_WORLD_MODEL_TEMPLATE_READ_OWNER,
    XG_RENDER_WORLD_MODEL_TEMPLATE_READ_MODEL,
    XG_RENDER_WORLD_MODEL_TEMPLATE_READ_PACKET_BASE,
    XG_RENDER_WORLD_MODEL_TEMPLATE_READ_FAMILY,
    XG_RENDER_WORLD_MODEL_TEMPLATE_READ_WORD_COUNT,
    XG_RENDER_WORLD_MODEL_TEMPLATE_READ_ATTRIBUTE,
};

typedef struct XgRenderPreScenePrimitive {
    XgRenderIrNativePrimitive primitive;
    uint32_t packet_address;
    uint32_t source_primitive_index;
    uint32_t ot_bucket;
    uint32_t interpolation_producer_id;
    uint32_t interpolation_primitive_id;
    uint8_t payload_word_count;
    bool interpolation_identity_valid;
    bool temporal_only;
    GpuRenderTemporalCullPolicy temporal_cull;
} XgRenderPreScenePrimitive;

typedef struct XgRenderPreSceneState {
    XgRenderPreScenePrimitive records[XG_RENDER_PRE_SCENE_PRIMITIVE_CAPACITY];
    uint32_t count;
    uint32_t blocker;
    bool blocked;
} XgRenderPreSceneState;

typedef struct XgRenderStandaloneSubmissionState {
    GuestRenderVisualStateId visual_id;
    GuestRenderProducerHandle producer;
    bool open;
} XgRenderStandaloneSubmissionState;

typedef enum XgRenderOrderingDomain {
    XG_RENDER_ORDERING_DOMAIN_UNKNOWN = 0,
    XG_RENDER_ORDERING_DOMAIN_FIELD,
    XG_RENDER_ORDERING_DOMAIN_BATTLE,
} XgRenderOrderingDomain;

typedef struct XgRenderModelFt4ShadowContext {
    XgHost3dProjection projection;
    uint32_t instance_address;
    uint32_t model_address;
    uint32_t vertex_base;
    uint32_t topology_base;
    uint32_t material_base;
    uint32_t packet_base;
    uint32_t ot_base;
    uint16_t tpage;
    uint16_t clut;
    uint32_t caller_window_start;
    uint8_t dispatch_mode;
    uint8_t caller_contract;
    bool resident_dispatch;
    bool valid;
} XgRenderModelFt4ShadowContext;

typedef struct XgRenderModelFt4ShadowRecord {
    XgModelFt4RawRecord native;
    XgRenderProducerLifecycle lifecycle;
    uint32_t observed_xy[4];
    uint32_t packet_address;
    uint32_t attribute_address;
    uint32_t expected_tag;
    uint32_t material_word;
    uint32_t interpolation_producer_id;
    uint32_t interpolation_primitive_id;
    uint32_t source_vertex_indices[4];
    uint16_t uv[4];
    uint16_t tpage;
    uint16_t clut;
    bool interpolation_identity_valid;
    bool output_validated;
    bool guest_observed_passed_screen_cull;
    bool guest_observed_accepted;
} XgRenderModelFt4ShadowRecord;

typedef struct XgRenderModelFt4ShadowState {
    XgRenderModelFt4ShadowRecord records[
        XG_RENDER_MODEL_FT4_SHADOW_CAPACITY];
    XgRenderModelFt4ShadowContext context;
    PsxXgRenderModelFt4ShadowSnapshot snapshot;
    uint32_t initial_packet_cursor;
    uint32_t initial_counter;
    uint32_t expected_counter_delta;
    uint32_t descriptor_base;
    uint32_t count;
} XgRenderModelFt4ShadowState;

typedef struct XgRenderModelFt3ShadowRecord {
    XgRenderIrNativePrimitive primitive;
    XgRenderProducerLifecycle lifecycle;
    XgHost3dProjectedVertex vertices[3];
    uint32_t observed_xy[3];
    uint32_t packet_address;
    uint32_t attribute_address;
    uint32_t expected_tag;
    uint32_t material_word;
    uint32_t interpolation_producer_id;
    uint32_t interpolation_primitive_id;
    uint32_t source_vertex_indices[3];
    uint16_t uv[3];
    uint16_t tpage;
    uint16_t clut;
    uint16_t ordering_bucket;
    bool interpolation_identity_valid;
    bool output_validated;
    bool nclip_positive;
    bool guest_screen_accepted;
    bool guest_vertical_accepted;
    bool guest_horizontal_accepted;
    bool projection_flag_negative;
    bool guest_passed_screen_cull;
    bool guest_accepted;
    bool guest_observed_passed_screen_cull;
    bool guest_observed_accepted;
    bool passed_screen_cull;
    bool accepted;
} XgRenderModelFt3ShadowRecord;

typedef struct XgRenderModelFt3ShadowState {
    XgRenderModelFt3ShadowRecord records[
        XG_RENDER_MODEL_FT4_SHADOW_CAPACITY];
    PsxXgRenderModelFt3ShadowSnapshot snapshot;
    uint32_t initial_packet_cursor;
    uint32_t initial_counter;
    uint32_t expected_counter_delta;
    uint32_t descriptor_base;
    uint32_t count;
} XgRenderModelFt3ShadowState;

#define XG_RENDER_MODEL_FT3_SOURCE_CAPACITY XG_RENDER_IR_ITEM_CAPACITY

typedef struct XgRenderModelFt3SourceRecord {
    XgRenderIrNativePrimitive primitive;
    GpuRenderSemantic semantic;
    XgRenderProducerLifecycle lifecycle;
    uint32_t source_id;
    uint32_t interpolation_producer_id;
    uint32_t interpolation_primitive_id;
    bool interpolation_identity_valid;
    bool semantic_ready;
    bool valid;
} XgRenderModelFt3SourceRecord;

static XgRenderModelFt3SourceRecord model_ft3_sources[
    XG_RENDER_MODEL_FT3_SOURCE_CAPACITY];
static uint32_t model_ft3_source_count;

#define XG_RENDER_MODEL_FT4_SOURCE_CAPACITY XG_RENDER_IR_ITEM_CAPACITY

typedef struct XgRenderModelFt4SourceRecord {
    XgRenderIrNativePrimitive primitive;
    GpuRenderSemantic semantic;
    XgRenderProducerLifecycle lifecycle;
    uint32_t source_id;
    uint32_t interpolation_producer_id;
    uint32_t interpolation_primitive_id;
    uint8_t opcode;
    bool interpolation_identity_valid;
    bool semantic_ready;
    bool valid;
} XgRenderModelFt4SourceRecord;

static XgRenderModelFt4SourceRecord model_ft4_sources[
    XG_RENDER_MODEL_FT4_SOURCE_CAPACITY];
static uint32_t model_ft4_source_count;

#define XG_RENDER_LOOKUP_WORD_CAPACITY (0x200000u / 4u)
#define XG_RENDER_RESOURCE_WATCH_BITMAP_WORDS \
    (XG_RENDER_LOOKUP_WORD_CAPACITY / 32u)

typedef struct XgRenderAddressLookupSlot {
    uint16_t index;
    uint16_t epoch;
} XgRenderAddressLookupSlot;

static XgRenderAddressLookupSlot model_ft3_source_lookup[
    XG_RENDER_LOOKUP_WORD_CAPACITY];
static XgRenderAddressLookupSlot model_ft4_source_lookup[
    XG_RENDER_LOOKUP_WORD_CAPACITY];
static XgRenderAddressLookupSlot field_sprite_template_lookup[
    XG_RENDER_LOOKUP_WORD_CAPACITY];
static XgRenderAddressLookupSlot residual_template_lookup[
    XG_RENDER_LOOKUP_WORD_CAPACITY];
static XgRenderAddressLookupSlot f4_source_lookup[
    XG_RENDER_LOOKUP_WORD_CAPACITY];
static uint16_t model_ft3_source_lookup_epoch = 1u;
static uint16_t model_ft4_source_lookup_epoch = 1u;
static uint16_t field_sprite_template_lookup_epoch = 1u;
static uint16_t residual_template_lookup_epoch = 1u;
static uint16_t f4_source_lookup_epoch = 1u;
static uint32_t producer_resource_watch_bitmap[
    XG_RENDER_RESOURCE_WATCH_BITMAP_WORDS];
static uint8_t producer_resource_invalidated_byte_masks[
    XG_RENDER_LOOKUP_WORD_CAPACITY];

static bool xg_render_lookup_key(uint32_t address, uint32_t *out_key) {
    const uint32_t physical = address & UINT32_C(0x1fffffff);

    if (out_key == NULL || physical >= UINT32_C(0x200000) ||
        (physical & 3u) != 0u)
        return false;
    *out_key = physical >> 2u;
    return true;
}

static uint32_t xg_render_lookup_find(
        const XgRenderAddressLookupSlot *lookup, uint16_t epoch,
        uint32_t address, uint32_t count) {
    uint32_t key;

    if (!xg_render_lookup_key(address, &key)) return UINT32_MAX;
    if (lookup[key].epoch != epoch || lookup[key].index >= count)
        return UINT32_MAX;
    return lookup[key].index;
}

static void xg_render_lookup_put(
        XgRenderAddressLookupSlot *lookup, uint16_t epoch,
        uint32_t address, uint32_t index) {
    uint32_t key;

    if (index > UINT16_MAX || !xg_render_lookup_key(address, &key)) return;
    lookup[key].index = (uint16_t)index;
    lookup[key].epoch = epoch;
}

static void xg_render_lookup_remove(
        XgRenderAddressLookupSlot *lookup, uint16_t epoch,
        uint32_t address, uint32_t index) {
    uint32_t key;

    if (index > UINT16_MAX || !xg_render_lookup_key(address, &key)) return;
    if (lookup[key].epoch == epoch && lookup[key].index == index)
        lookup[key].epoch = 0u;
}

static void xg_render_lookup_reset(
        XgRenderAddressLookupSlot *lookup, uint16_t *epoch) {
    if (*epoch == UINT16_MAX) {
        memset(lookup, 0, XG_RENDER_LOOKUP_WORD_CAPACITY * sizeof(*lookup));
        *epoch = 1u;
    } else {
        ++*epoch;
    }
}

#define XG_RENDER_MODEL_FT4_TEMPLATE_CAPACITY 4096u

typedef struct XgRenderModelFt4Template {
    XgRenderProducerLifecycle lifecycle;
    uint32_t packet_address;
    uint32_t descriptor_address;
    uint32_t material_word;
    uint16_t uv[4];
    uint16_t tpage;
    uint16_t clut;
    uint16_t descriptor_next;
    uint32_t table_epoch;
    bool valid;
} XgRenderModelFt4Template;

static XgRenderModelFt4Template model_ft4_templates[
    XG_RENDER_MODEL_FT4_TEMPLATE_CAPACITY];
static XgRenderModelFt4Template model_ft4_descriptor_templates[
    XG_RENDER_MODEL_FT4_TEMPLATE_CAPACITY];
static XgRenderAddressLookupSlot model_ft4_packet_lookup[
    XG_RENDER_LOOKUP_WORD_CAPACITY];
static XgRenderAddressLookupSlot model_ft4_descriptor_lookup[
    XG_RENDER_LOOKUP_WORD_CAPACITY];
static uint16_t model_ft4_packet_lookup_epoch = 1u;
static uint16_t model_ft4_descriptor_lookup_epoch = 1u;
static uint16_t model_ft4_descriptor_packet_heads[
    XG_RENDER_MODEL_FT4_TEMPLATE_CAPACITY];
static uint32_t model_ft4_table_epoch = 1u;
static uint32_t model_ft4_template_count;
static uint32_t model_ft4_descriptor_template_count;

static bool model_ft4_template_is_current(
        const XgRenderModelFt4Template *entry) {
    return entry != NULL && entry->valid &&
        entry->table_epoch == model_ft4_table_epoch;
}

static void invalidate_model_ft4_templates_overlapping(uint32_t address,
                                                        uint32_t size);

static void invalidate_model_ft4_templates(void) {
    model_ft4_template_count = 0u;
    model_ft4_descriptor_template_count = 0u;
    xg_render_lookup_reset(model_ft4_packet_lookup,
                           &model_ft4_packet_lookup_epoch);
    xg_render_lookup_reset(model_ft4_descriptor_lookup,
                           &model_ft4_descriptor_lookup_epoch);
    memset(model_ft4_descriptor_packet_heads, 0,
           sizeof(model_ft4_descriptor_packet_heads));
    if (model_ft4_table_epoch == UINT32_MAX) {
        memset(model_ft4_templates, 0, sizeof(model_ft4_templates));
        memset(model_ft4_descriptor_templates, 0,
               sizeof(model_ft4_descriptor_templates));
        model_ft4_table_epoch = 1u;
        return;
    }
    ++model_ft4_table_epoch;
}

typedef struct XgRenderWorldModelTemplate {
    uint32_t words[XG_WORLD_MODELS_MAX_PACKET_WORD_COUNT];
    uint64_t resource_epoch;
    uint32_t table_epoch;
    CPUState *owner_cpu;
    uint32_t model_address;
    uint32_t packet_base;
    uint32_t primitive_ordinal;
    uint32_t packet_address;
    uint32_t attribute_address;
    uint8_t primitive_family;
    uint8_t word_count;
    bool active;
    bool valid;
} XgRenderWorldModelTemplate;

typedef struct XgRenderWorldModelColorWrite {
    uint64_t resource_epoch;
    CPUState *owner_cpu;
    uint32_t model_address;
    uint32_t packet_address;
    uint32_t address;
    uint32_t value;
    bool used;
    bool valid;
} XgRenderWorldModelColorWrite;

typedef struct XgRenderWorldModelInitializerReceipt {
    uint64_t resource_epoch;
    CPUState *owner_cpu;
    uint32_t model_address;
    uint32_t packet_address;
    uint32_t initializer_function;
    uint8_t primitive_family;
    bool valid;
} XgRenderWorldModelInitializerReceipt;

typedef struct XgRenderWorldModelInitializerContext {
    uint64_t resource_epoch;
    uint64_t authentication_generation;
    CPUState *owner_cpu;
    uint32_t model_address;
    uint32_t packet_base;
    uint32_t packet_capacity;
    uint32_t caller_return;
    uint32_t entry_stack_pointer;
    uint32_t dispatch_mode;
    uint32_t receipt_count;
    bool active;
} XgRenderWorldModelInitializerContext;

typedef struct XgRenderWorldModelPacketCopyContext {
    CPUState *owner_cpu;
    uint32_t destination;
    uint32_t source;
    uint32_t size;
    bool active;
} XgRenderWorldModelPacketCopyContext;

typedef struct XgRenderWorldModelPacketCopyRange {
    uint32_t destination;
    uint32_t source;
    uint32_t size;
} XgRenderWorldModelPacketCopyRange;

typedef struct XgRenderWorldModelsNativeState {
    XgWorldModelsRecordSource
        record_sources[XG_RENDER_WORLD_MODEL_RECORD_CAPACITY];
    XgWorldModelsTransformNodeSource
        transform_nodes[XG_RENDER_WORLD_MODEL_NODE_CAPACITY];
    XgWorldModelsRecordOutput records[XG_RENDER_WORLD_MODEL_RECORD_CAPACITY];
    XgWorldModelsNodeSideEffect
        node_side_effects[XG_RENDER_WORLD_MODEL_NODE_CAPACITY];
    XgWorldModelsNativeDispatch
        dispatches[XG_RENDER_WORLD_MODEL_DISPATCH_CAPACITY];
    XgWorldModelsNativeDispatchOutput
        dispatch_outputs[XG_RENDER_WORLD_MODEL_DISPATCH_CAPACITY];
    XgWorldModelsNativePrimitiveSource
        primitives[XG_RENDER_WORLD_MODEL_PRIMITIVE_CAPACITY];
    XgWorldModelsNativePrimitiveOutput
        outputs[XG_RENDER_WORLD_MODEL_PRIMITIVE_CAPACITY];
    GpuRenderInterpolationVertexAnchor
        anchors[XG_RENDER_WORLD_MODEL_ANCHOR_CAPACITY];
    uint32_t anchor_seen[UINT16_MAX / 32u + 1u];
    XgRenderWorldModelTemplate
        *templates[XG_RENDER_WORLD_MODEL_PRIMITIVE_CAPACITY];
    XgWorldModelsNativePreparation preparation;
    XgWorldModelsNativeCommit expected_commit;
    uint32_t ot_heads[XG_WORLD_MODELS_OT_BUCKET_COUNT];
    bool ot_touched[XG_WORLD_MODELS_OT_BUCKET_COUNT];
    uint64_t authentication_generation;
    uint32_t entry_stack_pointer;
    uint32_t accepted_count;
    CPUState *owner_cpu;
    bool valid;
} XgRenderWorldModelsNativeState;

typedef struct XgRenderWorldTerrainWaterNativeState {
    struct {
        uint32_t group_id;
        int32_t canonical_x;
        int32_t canonical_y;
        int32_t native_x;
        int32_t native_y;
        uint32_t generation;
    } mesh_vertices[145u * 145u];
    uint32_t mesh_generation;
    XgWorldTerrainWaterSource source;
    XgWorldTerrainWaterRecord records[XG_WORLD_TERRAIN_WATER_RECORD_CAPACITY];
    XgWorldTerrainWaterRecord
        unculled_records[XG_WORLD_TERRAIN_WATER_UNCULLED_CAPACITY];
    XgWorldTerrainWaterAnchor anchors[XG_WORLD_TERRAIN_WATER_ANCHOR_CAPACITY];
    uint32_t packet_uv2_high[XG_WORLD_TERRAIN_WATER_RECORD_CAPACITY];
    uint32_t scratch_words[XG_WORLD_TERRAIN_WATER_NATIVE_SCRATCH_WORD_COUNT];
    uint32_t ot_heads[XG_WORLD_TERRAIN_WATER_NATIVE_OT_BUCKET_COUNT];
    uint32_t simulated_ot_heads[XG_WORLD_TERRAIN_WATER_NATIVE_OT_BUCKET_COUNT];
    bool ot_touched[XG_WORLD_TERRAIN_WATER_NATIVE_OT_BUCKET_COUNT];
} XgRenderWorldTerrainWaterNativeState;

typedef struct XgRenderWorldEntityShadowsNativeState {
    XgWorldEntityShadowRecord records[XG_WORLD_ENTITY_SHADOW_LIST_CAPACITY];
    uint32_t packet_tags[XG_WORLD_ENTITY_SHADOW_LIST_CAPACITY];
    uint32_t ot_heads[XG_WORLD_ENTITY_SHADOW_OT_BUCKET_COUNT];
    bool ot_touched[XG_WORLD_ENTITY_SHADOW_OT_BUCKET_COUNT];
} XgRenderWorldEntityShadowsNativeState;

typedef struct XgRenderWorldDecorationsNativeState {
    XgWorldDecorationsRecord records[XG_WORLD_DECORATIONS_PACKET_CAPACITY];
    XgWorldDecorationsRecord
        temporal_records[XG_WORLD_DECORATIONS_TEMPORAL_CAPACITY];
    uint32_t ot_heads[XG_WORLD_DECORATIONS_NATIVE_OT_BUCKET_COUNT];
    uint32_t simulated_ot_heads[XG_WORLD_DECORATIONS_NATIVE_OT_BUCKET_COUNT];
    bool ot_touched[XG_WORLD_DECORATIONS_NATIVE_OT_BUCKET_COUNT];
} XgRenderWorldDecorationsNativeState;

typedef struct XgRenderWorldCloudsNativePending {
    XgWorldCloudRecord records[XG_WORLD_CLOUD_PACKET_CAPACITY];
    XgWorldCloudRecord temporal_records[XG_WORLD_CLOUD_TEMPORAL_CAPACITY];
    XgWorldCloudPosition stepped_positions[XG_WORLD_CLOUD_COUNT];
    uint32_t expected_packets[XG_WORLD_CLOUD_PACKET_CAPACITY]
                             [XG_WORLD_CLOUDS_SHADOW_PACKET_WORD_COUNT];
    uint32_t expected_ot[XG_WORLD_CLOUD_OT_BUCKET_COUNT];
    uint64_t authentication_generation;
    uint32_t entry_stack_pointer;
    uint32_t position_base;
    uint32_t packet_base;
    uint32_t ot_base;
    uint32_t record_count;
    uint32_t temporal_record_count;
    uint32_t expected_attempts;
    CPUState *owner_cpu;
    uint32_t depth;
    bool poisoned;
    bool valid;
} XgRenderWorldCloudsNativePending;

typedef struct XgRenderWorldActorSpritesNativeState {
    XgWorldActorSpriteRecord
        records[XG_WORLD_ACTOR_SPRITES_NATIVE_RECORD_CAPACITY];
    XgWorldActorSpritesNativePreparation preparation;
    uint32_t payload_words[XG_WORLD_ACTOR_SPRITES_NATIVE_RECORD_CAPACITY]
                           [XG_WORLD_ACTOR_SPRITE_PACKET_PAYLOAD_WORD_COUNT];
    uint32_t packet_tags[XG_WORLD_ACTOR_SPRITES_NATIVE_RECORD_CAPACITY];
    uint32_t ot_addresses[XG_WORLD_ACTOR_SPRITES_NATIVE_RECORD_CAPACITY];
    uint32_t ot_heads[XG_WORLD_ACTOR_SPRITE_OT_BUCKET_COUNT];
    bool ot_touched[XG_WORLD_ACTOR_SPRITE_OT_BUCKET_COUNT];
    uint64_t authentication_generation;
    uint32_t entry_stack_pointer;
    uint32_t ot_base;
    CPUState *owner_cpu;
    bool valid;
} XgRenderWorldActorSpritesNativeState;

typedef struct XgRenderWorldActorContext {
    uint64_t authentication_generation;
    uint32_t entry_stack_pointer;
    uint32_t caller_return;
    CPUState *owner_cpu;
    uint32_t depth;
    bool poisoned;
    bool active;
} XgRenderWorldActorContext;

typedef enum XgRenderSpriteFt4ShadowPhase {
    XG_RENDER_SPRITE_FT4_SHADOW_IDLE = 0,
    XG_RENDER_SPRITE_FT4_SHADOW_EXPECT_XY,
    XG_RENDER_SPRITE_FT4_SHADOW_EXPECT_MATERIAL,
} XgRenderSpriteFt4ShadowPhase;

typedef struct XgRenderSpriteFt4ShadowState {
    XgSpriteFt4Record native;
    XgRenderPreScenePrimitive native_records[
        XG_RENDER_SPRITE_FT4_NATIVE_CAPACITY];
    XgRenderProducerLifecycle native_lifecycles[
        XG_RENDER_SPRITE_FT4_NATIVE_CAPACITY];
    uint8_t native_opcodes[XG_RENDER_SPRITE_FT4_NATIVE_CAPACITY];
    PsxXgRenderSpriteFt4ShadowSnapshot snapshot;
    uint32_t sprite_address;
    uint32_t packet_address;
    uint32_t descriptor_address;
    uint32_t material_word;
    uint16_t tpage;
    uint16_t clut;
    uint8_t uv[4][2];
    XgRenderSpriteFt4ShadowPhase phase;
    uint32_t native_record_count;
    bool geometry_matches;
    bool payload_matches;
    bool invocation_matches;
    bool wrapper_scope;
} XgRenderSpriteFt4ShadowState;

#define XG_RENDER_FIELD_SPRITE_BUILDER_CAPACITY 256u
#define XG_RENDER_FIELD_SPRITE_TEMPLATE_CAPACITY 1024u
#define XG_RENDER_RESIDUAL_TEMPLATE_CAPACITY 1024u
#define XG_RENDER_RESIDUAL_MAX_RESOURCE_SIZE 0x24u

typedef struct XgRenderFieldSpriteBuilderRecord {
    XgRenderIrNativePrimitive primitive;
    GpuRenderSemantic semantic;
    XgRenderProducerLifecycle lifecycle;
    uint32_t packet_address;
    uint32_t descriptor_address;
    uint32_t interpolation_producer_id;
    uint32_t interpolation_primitive_id;
    uint32_t xy[4];
    uint16_t uv[4];
    uint16_t tpage;
    uint16_t clut;
    bool interpolation_identity_valid;
    bool semantic_ready;
} XgRenderFieldSpriteBuilderRecord;

typedef struct XgRenderFieldSpriteBuilderState {
    XgRenderFieldSpriteBuilderRecord records[
        XG_RENDER_FIELD_SPRITE_BUILDER_CAPACITY];
    uint32_t count;
    uint8_t overlay_family;
} XgRenderFieldSpriteBuilderState;

typedef struct XgRenderFieldSpriteTemplateState {
    XgRenderFieldSpriteBuilderRecord records[
        XG_RENDER_FIELD_SPRITE_TEMPLATE_CAPACITY];
    uint32_t count;
} XgRenderFieldSpriteTemplateState;

typedef struct XgRenderResidualTemplate {
    XgRenderIrNativePrimitive primitive;
    GpuRenderSemantic semantic;
    XgRenderProducerLifecycle lifecycle;
    uint32_t command_address;
    uint32_t producer_seam;
    uint32_t resource_size;
    bool semantic_ready;
    bool valid;
} XgRenderResidualTemplate;

typedef struct XgRenderResidualTemplateState {
    XgRenderResidualTemplate records[XG_RENDER_RESIDUAL_TEMPLATE_CAPACITY];
    uint32_t count;
} XgRenderResidualTemplateState;

#define XG_RENDER_OVERLAY_FT4_TEMPLATE_CAPACITY XG_RENDER_IR_ITEM_CAPACITY
#define XG_RENDER_F4_SOURCE_CAPACITY 128u

typedef struct XgRenderOverlayFt4Template {
    XgRenderIrNativePrimitive primitive;
    XgRenderProducerLifecycle lifecycle;
    XgRenderIrMaterialState material;
    uint32_t packet_address;
    uint32_t source_primitive_index;
    uint32_t interpolation_producer_id;
    uint32_t interpolation_primitive_id;
    uint8_t family;
    bool interpolation_identity_valid;
    bool material_ready;
    bool valid;
} XgRenderOverlayFt4Template;

typedef struct XgRenderOverlayFt4State {
    XgRenderOverlayFt4Template templates[
        XG_RENDER_OVERLAY_FT4_TEMPLATE_CAPACITY];
    uint32_t count;
} XgRenderOverlayFt4State;

typedef struct XgRenderF4SourceRecord {
    XgRenderQuadSourceVertex vertices[XG_RENDER_QUAD_VERTEX_COUNT];
    XgRenderIrMaterialState material;
    GpuRenderSemantic semantic;
    XgRenderProducerLifecycle lifecycle;
    uint32_t source_id;
    uint32_t ot_address;
    uint8_t opcode;
    bool semantic_ready;
    bool valid;
} XgRenderF4SourceRecord;

typedef struct XgRenderF4SourceState {
    XgRenderF4SourceRecord records[XG_RENDER_F4_SOURCE_CAPACITY];
    uint32_t count;
} XgRenderF4SourceState;

#define XG_RENDER_FIELD_POLYLINE_CAPACITY 64u

typedef struct XgRenderFieldPolylineRecord {
    GpuRenderSemantic semantic;
    uint32_t packet_address;
    uint32_t command_word;
    uint32_t xy[3];
    uint32_t interpolation_primitive_id;
} XgRenderFieldPolylineRecord;

typedef struct XgRenderFieldPolylineState {
    XgRenderFieldPolylineRecord records[XG_RENDER_FIELD_POLYLINE_CAPACITY];
    PsxXgRenderFieldPolylineSnapshot snapshot;
    uint32_t count;
} XgRenderFieldPolylineState;

#define XG_RENDER_RESIDENT_LINE_F2_CAPACITY 6u

typedef struct XgRenderResidentLineF2Source {
    uint32_t packet_addresses[XG_RENDER_RESIDENT_LINE_F2_CAPACITY];
    uint32_t xy[XG_RENDER_RESIDENT_LINE_F2_CAPACITY][2];
    CPUState *owner_cpu;
    uint32_t heap_address;
    uint8_t buffer_index;
    uint8_t source_count;
    uint8_t publish_count;
    bool valid;
} XgRenderResidentLineF2Source;

typedef struct XgRenderWorldHorizonShadowState {
    XgWorldHorizonCapture capture;
    XgWorldHorizonRecord records[XG_WORLD_HORIZON_QUAD_COUNT];
    PsxXgRenderWorldHorizonShadowSnapshot snapshot;
    uint32_t packet_addresses[XG_WORLD_HORIZON_QUAD_COUNT];
    uint32_t initial_packet_tags[XG_WORLD_HORIZON_QUAD_COUNT];
    uint32_t initial_texture_window_tags[2];
    uint32_t entry_stack_pointer;
    uint32_t ot_address;
    uint32_t initial_ot_word;
} XgRenderWorldHorizonShadowState;

typedef struct XgRenderWorldEffectsShadowState {
    XgWorldEffectsCapture capture;
    XgWorldEffectsRecord records[XG_WORLD_EFFECTS_SOURCE_CAPACITY];
    PsxXgRenderWorldEffectsShadowSnapshot snapshot;
    uint32_t packet_addresses[XG_WORLD_EFFECTS_SOURCE_CAPACITY];
    uint32_t expected_tags[XG_WORLD_EFFECTS_SOURCE_CAPACITY];
    uint32_t initial_ot_words[0xc0];
    uint32_t final_ot_packets[0xc0];
    bool ot_touched[0xc0];
    uint32_t entry_stack_pointer;
    uint32_t initial_packet_cursor;
    uint32_t ot_base;
    uint32_t count;
} XgRenderWorldEffectsShadowState;

static XgRenderAuthRuntimeState state = {
    .armed = true,
    .requested_timing_mode = GUEST_RENDER_TIMING_ORIGINAL,
    .requested_render_mode = GUEST_RENDER_RENDER_ORIGINAL,
};
static XgNativeView native_view;

static int32_t render_screen_x_cull_margin(void) {
    const int32_t native_margin = xg_host_3d_native_view_margin();
    const int32_t temporal_margin = psx_ws_x_margin();

    return native_margin > temporal_margin ? native_margin : temporal_margin;
}

void psx_xg_render_auth_set_terrain_temporal_coverage(bool enabled) {
    xg_world_terrain_water_set_temporal_coverage(enabled);
}

bool g_psx_xg_render_auth_cold_enabled;
static bool ui_ot_pending;
static uint32_t ui_ot_pending_frame;
static uint64_t ui_ot_visual_sequence = 1u;
static PsxXgRenderUiOtSnapshot ui_ot_snapshot;
static XgRenderSourceState source_state = {
    .aggregate = { .next_sequence = 1u },
    .next_auth_sequence = 1u,
};
static XgRenderFt4GeometryState ft4_geometry = { .next_sequence = 1u };
static XgRenderPreSceneState pre_scene;
static XgRenderStandaloneSubmissionState standalone_submission;
static uint32_t standalone_stage_failure_detail;
static XgRenderModelFt4ShadowState model_ft4_shadow;
static XgRenderModelFt3ShadowState model_ft3_shadow;
static XgRenderSpriteFt4ShadowState sprite_ft4_shadow;
static XgRenderFieldSpriteBuilderState field_sprite_builder;
static XgRenderFieldSpriteTemplateState field_sprite_templates;
static XgRenderResidualTemplateState residual_templates;
static XgRenderOverlayFt4State overlay_ft4_state;
static uint32_t overlay_ft4_upsert_failure_detail;
static XgRenderF4SourceState f4_sources;
static uint64_t next_resource_generation = 1u;
static XgRenderFieldPolylineState field_polyline;
static XgRenderResidentLineF2Source resident_line_f2_source;
static XgRenderWorldHorizonShadowState world_horizon_shadow;
static XgRenderWorldEffectsShadowState world_effects_shadow;
static XgRenderWorldModelTemplate world_model_templates[
    XG_RENDER_WORLD_MODEL_TEMPLATE_CAPACITY];
static XgRenderWorldModelColorWrite world_model_color_writes[
    XG_RENDER_WORLD_MODEL_COLOR_WRITE_CAPACITY];
static XgRenderWorldModelInitializerReceipt world_model_initializer_receipts[
    XG_RENDER_WORLD_MODEL_TEMPLATE_CAPACITY];
static XgRenderWorldModelInitializerContext world_model_initializer;
static XgRenderWorldModelPacketCopyContext world_model_packet_copy;
static XgRenderWorldModelPacketCopyRange world_model_packet_copy_ranges[64];
static uint32_t world_model_packet_copy_range_count;
static bool world_model_templates_populated;
static bool world_model_initializer_populated;
static uint32_t world_model_template_read_failure;
static uint64_t world_model_resource_epoch;
static uint32_t world_model_template_epoch = 1u;
static XgRenderWorldModelsNativeState world_models_native_state;
static XgRenderWorldTerrainWaterNativeState world_terrain_water_native_state;
static XgWorldTerrainWaterTileSource world_terrain_water_temporal_tiles[81];
static uint64_t world_terrain_water_temporal_scene;
static uint32_t world_terrain_water_native_last_caller;
static uint32_t world_terrain_water_native_blocker_detail;
static uint32_t world_terrain_mesh_duplicate_vertices;
static uint32_t world_terrain_mesh_cross_tile_duplicate_vertices;
static uint32_t world_terrain_mesh_canonical_raster_conflicts;
static uint32_t world_terrain_mesh_native_raster_conflicts;
static uint32_t world_terrain_mesh_cross_tile_native_raster_conflicts;
static XgRenderWorldEntityShadowsNativeState world_entity_shadows_native_state;
static XgRenderWorldDecorationsNativeState world_decorations_native_state;
static XgRenderWorldCloudsNativePending world_clouds_native_pending;
static XgRenderWorldActorSpritesNativeState world_actor_sprites_native_state;
static XgRenderWorldActorContext world_actor_context;
static PsxXgRenderWorldNativeSnapshot world_models_native_snapshot;
static PsxXgRenderWorldNativeSnapshot world_actor_sprites_native_snapshot;
static PsxXgRenderWorldNativeSnapshot world_sky_native_snapshot;
static PsxXgRenderWorldExecutionSnapshot world_execution;
static bool world_execution_active;
static bool world_native_cutover_in_progress;
static bool world_native_cutover_failed;
#ifdef PSX_XG_RENDER_AUTH_RUNTIME_TESTING
static XgRenderIrNativePrimitive particle_test_primitive;
static bool particle_test_primitive_valid;
static XgRenderIrNativePrimitive zoom_test_primitives[XG_RENDER_ZOOM_QUAD_COUNT];
static uint32_t zoom_test_primitive_count;
static XgRenderIrNativePrimitive
    projected_test_primitives[XG_RENDER_PROJECTED_MAX_RECORDS];
static uint32_t projected_test_primitive_count;
#endif
static PsxXgRenderProducerFamilySnapshot producer_family;
static PsxXgRenderProjectedLifecycleSnapshot projected_lifecycle;
static PsxXgRenderOverlayFt4Snapshot overlay_ft4_2c;
static bool overlay_projected_2e_descriptor_scope;
static PsxXgRenderAuthCompletedProofReceipt completed_proof;
static uint64_t completed_proof_scene_generation;
static atomic_flag completed_proof_guard = ATOMIC_FLAG_INIT;
static PsxXgRenderAuthInstrumentation instrumentation = { .revision = 1u };
static atomic_flag instrumentation_guard = ATOMIC_FLAG_INIT;
static uint64_t instrumentation_next_sequence = 1u;
static PsxXgRenderExecPhaseExchange exec_phase_exchange;

static void clear_model_ft3_sources(void) {
    model_ft3_source_count = 0u;
    xg_render_lookup_reset(model_ft3_source_lookup,
                            &model_ft3_source_lookup_epoch);
}

static void clear_model_ft4_sources(void) {
    model_ft4_source_count = 0u;
    xg_render_lookup_reset(model_ft4_source_lookup,
                           &model_ft4_source_lookup_epoch);
}

static XgRenderModelFt4SourceRecord *model_ft4_source_upsert(
        uint32_t source_id) {
    const uint32_t indexed = xg_render_lookup_find(
        model_ft4_source_lookup, model_ft4_source_lookup_epoch, source_id,
        model_ft4_source_count);

    if (indexed != UINT32_MAX && model_ft4_sources[indexed].valid &&
        model_ft4_sources[indexed].source_id == source_id) {
        model_ft4_sources[indexed].semantic_ready = false;
        return &model_ft4_sources[indexed];
    }
    for (uint32_t index = 0u; index < model_ft4_source_count; ++index) {
        if (!model_ft4_sources[index].valid) {
            model_ft4_sources[index].semantic_ready = false;
            return &model_ft4_sources[index];
        }
    }
    if (model_ft4_source_count == XG_RENDER_MODEL_FT4_SOURCE_CAPACITY)
        return NULL;
    return &model_ft4_sources[model_ft4_source_count++];
}

static XgRenderModelFt3SourceRecord *model_ft3_source_upsert(
        uint32_t source_id) {
    const uint32_t indexed = xg_render_lookup_find(
        model_ft3_source_lookup, model_ft3_source_lookup_epoch, source_id,
        model_ft3_source_count);

    if (indexed != UINT32_MAX && model_ft3_sources[indexed].valid &&
        model_ft3_sources[indexed].source_id == source_id) {
        model_ft3_sources[indexed].semantic_ready = false;
        return &model_ft3_sources[indexed];
    }
    for (uint32_t index = 0u; index < model_ft3_source_count; ++index) {
        XgRenderModelFt3SourceRecord *candidate = &model_ft3_sources[index];

        if (candidate->valid && candidate->source_id == source_id) {
            candidate->semantic_ready = false;
            return candidate;
        }
        if (!candidate->valid) {
            candidate->semantic_ready = false;
            return candidate;
        }
    }
    if (model_ft3_source_count == XG_RENDER_MODEL_FT3_SOURCE_CAPACITY)
        return NULL;
    return &model_ft3_sources[model_ft3_source_count++];
}

static void clear_f4_sources(void) {
    f4_sources.count = 0u;
    xg_render_lookup_reset(f4_source_lookup, &f4_source_lookup_epoch);
}

static void clear_residual_templates(void) {
    residual_templates.count = 0u;
    xg_render_lookup_reset(residual_template_lookup,
                           &residual_template_lookup_epoch);
}

static void abort_active(XgRenderAuthReason reason,
                         PsxXgRenderAuthRejectionSource source,
                         bool has_hook, PsxXgRenderAuthHook hook,
                         uint32_t pc);
static void clear_sprite_ft4_shadow_context(void);
static void clear_field_sprite_builder(void);
static void clear_field_sprite_templates(void);
static void clear_zoom_source(void);
static void block_field_sprite_builder(uint32_t blocker);
static void clear_field_polyline_pending(void);
static void block_field_polyline(uint32_t blocker);
static void clear_resident_line_f2_source(void);
static bool overlay_ft4_capture_direct_templates(CPUState *cpu,
                                                  uint32_t producer_pc);
static bool overlay_ft4_capture_rectangle_template(CPUState *cpu,
                                                    uint32_t producer_pc);
static bool overlay_ft4_capture_projected_material(CPUState *cpu,
                                                    uint32_t producer_pc);
static bool overlay_ft4_capture_glyph_material(CPUState *cpu,
                                                uint32_t producer_pc);
static bool overlay_ft4_capture_projected_geometry(CPUState *cpu);
static bool overlay_ft4_capture_projected_2e_material(
    CPUState *cpu, uint32_t template_family, uint32_t producer_pc);
static bool overlay_ft4_capture_projected_2e_geometry(CPUState *cpu);
static void residual_capture_fullscreen_tile(CPUState *cpu);
static void residual_capture_fade_tiles(CPUState *cpu);
static void residual_capture_projected_gouraud(CPUState *cpu);
static bool native_model_ft3_stream_resolve(
    const GuestRenderNativeStreamMissContext *context,
    GpuRenderSemantic *out_semantic);
static bool native_model_ft4_stream_resolve(
    const GuestRenderNativeStreamMissContext *context,
    GpuRenderSemantic *out_semantic);
static bool native_field_sprite_stream_resolve(
    const GuestRenderNativeStreamMissContext *context,
    GpuRenderSemantic *out_semantic);
static bool native_residual_stream_resolve(
    const GuestRenderNativeStreamMissContext *context,
    GpuRenderSemantic *out_semantic);
static bool native_shared_packet_resolve(
    const GuestRenderNativeStreamMissContext *context,
    GpuRenderTransactionId *out_visual_id,
    GpuRenderSemantic *out_semantic);
static XgRenderOverlayFt4Template *overlay_ft4_find_template(
    uint32_t packet_address);
static XgRenderOverlayFt4Template *overlay_ft4_upsert_template(
    uint32_t packet_address, uint32_t producer_pc);
static void overlay_ft4_observe_add_prim(CPUState *cpu);
static bool native_zoom_stream_resolve(
    const GuestRenderNativeStreamMissContext *context,
    GpuRenderSemantic *out_semantic);
static bool translate_native_primitive_cached(
    const XgRenderIrNativePrimitive *primitive,
    GpuRenderSemantic *cached_semantic, bool *semantic_ready,
    GpuRenderSemantic *out_semantic);
static bool native_f4_stream_resolve(
    const GuestRenderNativeStreamMissContext *context,
    GpuRenderSemantic *out_semantic);
static bool native_stream_resolve(
    const GuestRenderNativeStreamMissContext *context,
    GpuRenderTransactionId *out_visual_id,
    GpuRenderSemantic *out_semantic);
static void overlay_ft4_observe_field_material(CPUState *cpu, uint32_t family,
                                                bool semi_transparent,
                                                uint32_t packet_offset);
static void clear_world_horizon_shadow_pending(void);
static void block_world_horizon_shadow(uint32_t blocker);
static void clear_world_effects_shadow_pending(void);
static void block_world_effects_shadow(uint32_t blocker);
static void clear_world_actor_context(void);
static void clear_world_actor_native_pending(void);
static void clear_world_models_native_pending(void);
static void clear_world_clouds_native_pending(void);
static void invalidate_world_model_templates(void);
static void invalidate_world_model_initializer(void);
static bool world_authentication_generation(uint64_t *out_generation);

typedef enum XgNativeResolveFamily {
    XG_NATIVE_RESOLVE_NONE = 0,
    XG_NATIVE_RESOLVE_MODEL_FT4,
    XG_NATIVE_RESOLVE_MODEL_FT3,
    XG_NATIVE_RESOLVE_ZOOM,
    XG_NATIVE_RESOLVE_FIELD_SPRITE,
    XG_NATIVE_RESOLVE_RESIDUAL,
    XG_NATIVE_RESOLVE_F4,
    XG_NATIVE_RESOLVE_SHARED,
    XG_NATIVE_RESOLVE_NONSHARED_MISS,
} XgNativeResolveFamily;

typedef struct XgNativeResolveHint {
    uint32_t command_id;
    uint32_t word_count;
    uint64_t container_id;
    uint64_t resource_generation;
    uint8_t family;
    uint8_t opcode;
    uint8_t source_kind;
    bool valid;
} XgNativeResolveHint;

#define XG_NATIVE_RESOLVE_HINT_CAPACITY 1024u
static XgNativeResolveHint native_resolve_hints[
    XG_NATIVE_RESOLVE_HINT_CAPACITY];
static void native_resolve_hint_put(uint64_t command_id,
                                    XgNativeResolveFamily family);

static void invalidate_world_semantic_shadows(void) {
    (void)xg_world_terrain_water_shadow_lifecycle_invalidate();
    xg_world_entity_shadows_shadow_lifecycle_invalidate();
    xg_world_decorations_shadow_lifecycle_invalidate();
    xg_world_clouds_shadow_invalidate();
    xg_world_minimap_shadow_invalidate();
    clear_world_clouds_native_pending();
    clear_world_actor_native_pending();
    clear_world_actor_context();
    clear_world_models_native_pending();
    invalidate_world_model_initializer();
}

static void lock_completed_proof(void) {
    while (atomic_flag_test_and_set_explicit(&completed_proof_guard,
                                             memory_order_acquire)) {}
}

static void unlock_completed_proof(void) {
    atomic_flag_clear_explicit(&completed_proof_guard, memory_order_release);
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

static void lock_instrumentation(void) {
    while (atomic_flag_test_and_set_explicit(&instrumentation_guard,
                                             memory_order_acquire)) {}
}

static void unlock_instrumentation(void) {
    atomic_flag_clear_explicit(&instrumentation_guard, memory_order_release);
}

static uint64_t next_instrumentation_sequence(void) {
    return instrumentation_next_sequence++;
}

static void record_reset(bool scene_boundary) {
    lock_instrumentation();
    if (scene_boundary)
        ++instrumentation.scene_boundary_count;
    else
        ++instrumentation.disarm_count;
    instrumentation.last_reset_sequence = next_instrumentation_sequence();
    unlock_instrumentation();
}

static void record_variant_progress(XgRenderRuntimeVariantEvent event,
                                    bool exact) {
    lock_instrumentation();
    switch (event) {
    case XG_RENDER_RUNTIME_VARIANT_ACTIVATED:
        ++instrumentation.activation_physical_count;
        if (exact) ++instrumentation.activation_exact_count;
        break;
    case XG_RENDER_RUNTIME_VARIANT_ENTRY:
        ++instrumentation.entry_physical_count;
        if (exact) ++instrumentation.entry_exact_count;
        break;
    case XG_RENDER_RUNTIME_VARIANT_CAPTURE:
        ++instrumentation.capture_physical_count;
        if (exact) ++instrumentation.capture_exact_count;
        break;
    case XG_RENDER_RUNTIME_VARIANT_RETURN:
        ++instrumentation.return_physical_count;
        if (exact) ++instrumentation.return_exact_count;
        break;
    default:
        unlock_instrumentation();
        return;
    }
    instrumentation.last_progress_sequence = next_instrumentation_sequence();
    unlock_instrumentation();
}

static void record_completed_proof_publication(void) {
    lock_instrumentation();
    ++instrumentation.completed_proof_publication_count;
    instrumentation.last_publish_sequence = next_instrumentation_sequence();
    unlock_instrumentation();
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
    return pc == 0u ? 0u : (pc & 0x1fffffffu) | 0x80000000u;
}

uint32_t xg_render_runtime_guest_address(uint32_t address) {
    return guest_address(address);
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

static bool protected_code_write_overlaps(uint32_t write_address,
                                          uint32_t write_size,
                                          uint32_t variant_code_write_mask) {
    const XgRenderManifestValidation *validation =
        &xg_render_manifest_validation;
    const uint32_t data_dependency_mask =
        (UINT32_C(1) << PSX_XG_RENDER_CODE_WRITE_MODEL_DISPATCH_DATA) |
        (UINT32_C(1) << PSX_XG_RENDER_CODE_WRITE_SHARED_TRIG_DATA);

    return normalized_ranges_overlap(validation->producer_entry, 4u,
                                     write_address, write_size) ||
           normalized_ranges_overlap(validation->instruction_window_start,
                                     validation->instruction_window_size,
                                     write_address, write_size) ||
           (variant_code_write_mask & ~data_dependency_mask) != 0u;
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
           candidate->artifact_crc32 == validation->field_base_crc32 &&
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
    memset(&state.pending_candidate, 0, sizeof(state.pending_candidate));
    state.pending_candidate_valid = false;
    state.pending_scene_generation = 0u;
}

static void clear_authenticated_artifact_candidate(void) {
    memset(&state.authenticated_artifact_candidate, 0,
           sizeof(state.authenticated_artifact_candidate));
    state.authenticated_artifact_candidate_valid = false;
    state.authenticated_artifact_scene_generation = 0u;
}

static void clear_candidate_outcome(void) {
    state.candidate_matched = false;
    state.candidate_dispatched = false;
}

static void clear_pending_variant_sequence(void) {
    state.pending_variant_entry = 0u;
    state.pending_variant_instruction = 0u;
    state.pending_variant_delay_slot = 0u;
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

static bool pending_variant_artifact_candidate_matches(uint32_t pc) {
    if (xg_render_runtime_variant_no_gates_enabled()) return true;

    const bool candidate_authorized =
        state.authenticated_artifact_candidate_valid &&
        state.authenticated_artifact_generation != 0u &&
        state.authenticated_artifact_scene_generation == state.scene_generation &&
        (xg_render_runtime_variant_artifact_candidate_authorizes_pc(
             &state.authenticated_artifact_candidate, pc) ||
         xg_render_authoritative_overlay_artifact_candidate_authorizes_pc(
              &state.authenticated_artifact_candidate, pc));

    return candidate_authorized;
}

static bool authenticated_variant_hook_matches(uint32_t pc) {
    return pending_variant_artifact_candidate_matches(pc);
}

static bool current_artifact_is_authorized(void) {
    if (xg_render_runtime_variant_no_gates_enabled()) return true;
    return state.authenticated_artifact_candidate_valid &&
        state.authenticated_artifact_generation != 0u &&
        state.authenticated_artifact_scene_generation == state.scene_generation &&
        (xg_render_runtime_variant_artifact_candidate_matches(
             &state.authenticated_artifact_candidate) ||
         xg_render_authoritative_overlay_artifact_candidate_matches(
             &state.authenticated_artifact_candidate));
}

static bool current_artifact_code_range_overlaps(uint32_t address,
                                                  uint32_t size) {
    return state.authenticated_artifact_candidate_valid &&
        state.authenticated_artifact_generation != 0u &&
        state.authenticated_artifact_scene_generation == state.scene_generation &&
        normalized_ranges_overlap(
            state.authenticated_artifact_candidate.range_start,
            state.authenticated_artifact_candidate.range_size, address, size) &&
        xg_render_authoritative_overlay_artifact_candidate_matches(
            &state.authenticated_artifact_candidate);
}

static bool current_artifact_range_contains_pc(uint32_t pc) {
    return state.authenticated_artifact_candidate_valid &&
        state.authenticated_artifact_generation != 0u &&
        state.authenticated_artifact_scene_generation == state.scene_generation &&
        (xg_render_runtime_variant_artifact_candidate_authorizes_pc(
             &state.authenticated_artifact_candidate, pc) ||
         (xg_render_authoritative_overlay_artifact_candidate_matches(
              &state.authenticated_artifact_candidate) &&
          range_contains(
              state.authenticated_artifact_candidate.range_start,
              state.authenticated_artifact_candidate.range_size, pc, 4u)));
}

static bool current_artifact_memory_contains_pc(uint32_t pc) {
    return state.authenticated_artifact_candidate_valid &&
        state.authenticated_artifact_generation != 0u &&
        state.authenticated_artifact_scene_generation == state.scene_generation &&
        normalized_ranges_overlap(
            state.authenticated_artifact_candidate.artifact_base,
            state.authenticated_artifact_candidate.artifact_size, pc, 4u);
}

static bool artifact_binary_identity_matches(
        const PsxXgRenderAuthCandidate *left,
        const PsxXgRenderAuthCandidate *right) {
    return left != NULL && right != NULL &&
        left->artifact_size == right->artifact_size &&
        left->artifact_crc32 == right->artifact_crc32 &&
        physical_address_equals(left->artifact_base, right->artifact_base) &&
        memcmp(&left->identity, &right->identity, sizeof(left->identity)) == 0;
}

static uint8_t producer_resource_byte_mask_for_word(
        uint32_t begin, uint32_t end, uint32_t word) {
    const uint32_t word_begin = word << 2u;
    const uint32_t word_end = word_begin + 4u;
    const uint32_t first = begin > word_begin ? begin : word_begin;
    const uint32_t last = end < word_end ? end : word_end;

    if (first >= last) return 0u;
    const uint32_t low = first - word_begin;
    const uint32_t width = last - first;
    return (uint8_t)(((UINT32_C(1) << width) - 1u) << low);
}

static void watch_producer_resource(uint32_t address, uint32_t size) {
    const uint32_t physical = address & UINT32_C(0x1fffffff);
    uint32_t end;
    uint32_t first_word;
    uint32_t last_word;

    if (size == 0u || physical >= UINT32_C(0x200000)) return;
    end = physical + size - 1u;
    if (end < physical || end >= UINT32_C(0x200000))
        end = UINT32_C(0x1fffff);
    first_word = physical >> 2u;
    last_word = end >> 2u;
    for (uint32_t word = first_word; word <= last_word; ++word) {
        const uint32_t bit = UINT32_C(1) << (word & 31u);
        const uint8_t byte_mask = producer_resource_byte_mask_for_word(
            physical, end + 1u, word);
        producer_resource_watch_bitmap[word >> 5u] |= bit;
        producer_resource_invalidated_byte_masks[word] &=
            (uint8_t)~byte_mask;
    }
    if (producer_resource_watch != NULL)
        producer_resource_watch(physical, size);
}

static bool producer_resource_write_may_overlap(uint32_t address,
                                                 uint32_t size) {
    const uint32_t physical = address & UINT32_C(0x1fffffff);
    uint32_t end;
    uint32_t first_word;
    uint32_t last_word;

    if (size == 0u || physical >= UINT32_C(0x200000)) return false;
    end = physical + size - 1u;
    if (end < physical || end >= UINT32_C(0x200000))
        end = UINT32_C(0x1fffff);
    first_word = physical >> 2u;
    last_word = end >> 2u;
    for (uint32_t word = first_word; word <= last_word; ++word) {
        if (producer_resource_watch_bitmap[word >> 5u] &
            (UINT32_C(1) << (word & 31u)))
            return true;
    }
    return false;
}

static bool producer_resource_write_needs_invalidation(uint32_t address,
                                                       uint32_t size) {
    const uint32_t physical = address & UINT32_C(0x1fffffff);
    uint32_t end;
    uint32_t first_word;
    uint32_t last_word;

    if (size == 0u || physical >= UINT32_C(0x200000)) return false;
    end = physical + size - 1u;
    if (end < physical || end >= UINT32_C(0x200000))
        end = UINT32_C(0x1fffff);
    first_word = physical >> 2u;
    last_word = end >> 2u;
    for (uint32_t word = first_word; word <= last_word; ++word) {
        const uint32_t bit = UINT32_C(1) << (word & 31u);
        const uint8_t byte_mask = producer_resource_byte_mask_for_word(
            physical, end + 1u, word);
        if ((producer_resource_watch_bitmap[word >> 5u] & bit) != 0u &&
            (producer_resource_invalidated_byte_masks[word] & byte_mask) !=
                byte_mask)
            return true;
    }
    return false;
}

static void producer_resource_mark_invalidated(uint32_t address,
                                               uint32_t size) {
    const uint32_t physical = address & UINT32_C(0x1fffffff);
    uint32_t end;
    uint32_t first_word;
    uint32_t last_word;

    if (size == 0u || physical >= UINT32_C(0x200000)) return;
    end = physical + size - 1u;
    if (end < physical || end >= UINT32_C(0x200000))
        end = UINT32_C(0x1fffff);
    first_word = physical >> 2u;
    last_word = end >> 2u;
    for (uint32_t word = first_word; word <= last_word; ++word) {
        const uint32_t bit = UINT32_C(1) << (word & 31u);
        const uint8_t byte_mask = producer_resource_byte_mask_for_word(
            physical, end + 1u, word);
        if (producer_resource_watch_bitmap[word >> 5u] & bit)
            producer_resource_invalidated_byte_masks[word] |= byte_mask;
    }
}

static bool resident_render_site_is_authorized(uint32_t pc,
                                                uint32_t instruction_word);

static bool overlay_pc_is_authorized(uint32_t pc, uint32_t instruction_word) {
    const uint32_t physical = pc & UINT32_C(0x1fffffff);
    const uint32_t code_mask =
        xg_render_runtime_variant_code_write_overlap_mask(pc, 4u);
    const uint32_t resident_world_mask =
        (UINT32_C(1) << PSX_XG_RENDER_CODE_WRITE_WORLD_SKY) |
        (UINT32_C(1) << PSX_XG_RENDER_CODE_WRITE_WORLD_HORIZON) |
        (UINT32_C(1) << PSX_XG_RENDER_CODE_WRITE_WORLD_EFFECTS);

    return physical < UINT32_C(0x0005a000) ||
        (code_mask & resident_world_mask) != 0u ||
        resident_render_site_is_authorized(pc, instruction_word) ||
        pending_variant_artifact_candidate_matches(pc);
}

static uint64_t artifact_generation_for_pc(uint32_t pc) {
    return pending_variant_artifact_candidate_matches(pc)
        ? state.authenticated_artifact_generation : 0u;
}

static bool resident_producer_lifecycle_pc(uint32_t pc) {
    return physical_address_equals(pc, UINT32_C(0x8001e874)) ||
           physical_address_equals(pc, UINT32_C(0x8002675c)) ||
           physical_address_equals(pc, UINT32_C(0x8002c700)) ||
           physical_address_equals(pc, UINT32_C(0x8002d100)) ||
           physical_address_equals(pc, UINT32_C(0x8002da00)) ||
           physical_address_equals(pc, UINT32_C(0x80045ed0));
}

enum {
    XG_MODEL_DISPATCH_CALLER_NONE = 0u,
    XG_MODEL_DISPATCH_CALLER_RESIDENT,
    XG_MODEL_DISPATCH_CALLER_OVERLAY,
};

static const uint32_t model_dispatch_resident_caller_instructions[] = {
    UINT32_C(0x3c068006), UINT32_C(0x8cc6956c),
    UINT32_C(0x8e020020), UINT32_C(0x8f830188),
    UINT32_C(0x96070042), UINT32_C(0x00031880),
    UINT32_C(0x00621821), UINT32_C(0x8c440034),
    UINT32_C(0x8c65002c), UINT32_C(0x0c00b1c0),
    UINT32_C(0x30e70004),
};

static bool model_dispatch_instruction_window_matches(
        CPUState *cpu, uint32_t start, const uint32_t *instructions,
        uint32_t instruction_count) {
    uint32_t index;

    if (cpu == NULL || cpu->read_word == NULL || instructions == NULL ||
        instruction_count == 0u)
        return false;
    for (index = 0u; index < instruction_count; ++index) {
        if (cpu->read_word(start + index * 4u) != instructions[index])
            return false;
    }
    return true;
}

static bool model_dispatch_overlay_window_matches(
        CPUState *cpu, uint32_t return_address,
        uint32_t *out_window_start, uint32_t *out_matrix_stack_offset) {
    const uint32_t contract_count =
        xg_render_runtime_variant_model_dispatch_contract_count();

    for (uint32_t index = 0u; index < contract_count; ++index) {
        XgRenderRuntimeVariantModelDispatchContract contract = { 0 };
        uint32_t window_start;

        if (!xg_render_runtime_variant_model_dispatch_contract_at(
                index, &contract) ||
            return_address < contract.instruction_count * 4u)
            continue;
        window_start = return_address - contract.instruction_count * 4u;
        if (!model_dispatch_instruction_window_matches(
                cpu, window_start, contract.instructions,
                contract.instruction_count))
            continue;
        if (out_window_start != NULL) *out_window_start = window_start;
        if (out_matrix_stack_offset != NULL)
            *out_matrix_stack_offset = contract.matrix_stack_offset;
        return true;
    }
    return false;
}

static bool model_dispatch_resident_context_is_authenticated(void) {
    return state.requested_render_mode != GUEST_RENDER_RENDER_NATIVE ||
        xg_render_runtime_variant_no_gates_enabled() ||
        (state.authenticated_artifact_candidate_valid &&
         state.authenticated_artifact_scene_generation ==
             state.scene_generation) ||
        completed_proof_matches_tier(XG_RENDER_AUTH_TIER_WARM_NATIVE);
}

static bool model_dispatch_caller_contract_matches(
        CPUState *cpu, uint32_t return_address, uint8_t *out_contract,
        uint32_t *out_window_start, uint32_t *out_matrix_stack_offset) {
    const uint32_t resident_count = (uint32_t)(
        sizeof(model_dispatch_resident_caller_instructions) /
        sizeof(model_dispatch_resident_caller_instructions[0]));
    uint32_t window_start;

    if (return_address >= resident_count * 4u) {
        window_start = return_address - resident_count * 4u;
        if (model_dispatch_instruction_window_matches(
                cpu, window_start,
                model_dispatch_resident_caller_instructions,
                resident_count) && physical_address_equals(
                    window_start, UINT32_C(0x800257b0)) &&
            model_dispatch_resident_context_is_authenticated()) {
            if (out_contract != NULL)
                *out_contract = XG_MODEL_DISPATCH_CALLER_RESIDENT;
            if (out_window_start != NULL) *out_window_start = window_start;
            if (out_matrix_stack_offset != NULL)
                *out_matrix_stack_offset = 0x10u;
            return true;
        }
    }
    if (!model_dispatch_overlay_window_matches(
            cpu, return_address, &window_start, out_matrix_stack_offset))
        return false;
    if (out_contract != NULL) *out_contract = XG_MODEL_DISPATCH_CALLER_OVERLAY;
    if (out_window_start != NULL) *out_window_start = window_start;
    return true;
}

static bool model_dispatch_context_contract_matches(
        CPUState *cpu, const XgRenderModelFt4ShadowContext *context) {
    const uint32_t *instructions;
    uint32_t instruction_count;
    XgRenderRuntimeVariantModelDispatchContract overlay_contract = { 0 };

    if (context == NULL || !context->valid) return false;
    if (context->caller_contract == XG_MODEL_DISPATCH_CALLER_RESIDENT) {
        instructions = model_dispatch_resident_caller_instructions;
        instruction_count = (uint32_t)(
            sizeof(model_dispatch_resident_caller_instructions) /
            sizeof(model_dispatch_resident_caller_instructions[0]));
    } else if (context->caller_contract == XG_MODEL_DISPATCH_CALLER_OVERLAY) {
        const uint32_t contract_count =
            xg_render_runtime_variant_model_dispatch_contract_count();

        for (uint32_t index = 0u; index < contract_count; ++index) {
            if (xg_render_runtime_variant_model_dispatch_contract_at(
                    index, &overlay_contract) &&
                model_dispatch_instruction_window_matches(
                    cpu, context->caller_window_start,
                    overlay_contract.instructions,
                    overlay_contract.instruction_count))
                return true;
        }
        return false;
    } else {
        return false;
    }
    if (!model_dispatch_instruction_window_matches(
            cpu, context->caller_window_start, instructions,
            instruction_count))
        return false;
    return true;
}

static bool producer_lifecycle_begin(uint32_t producer_pc,
                                     XgRenderProducerLifecycle *out_lifecycle) {
    const bool resident = resident_producer_lifecycle_pc(producer_pc);
    const bool no_gates = xg_render_runtime_variant_no_gates_enabled();
    const uint64_t artifact_generation = resident || no_gates
        ? 0u : artifact_generation_for_pc(producer_pc);

    if (out_lifecycle == NULL || next_resource_generation == 0u ||
        (!resident && !no_gates && artifact_generation == 0u))
        return false;
    *out_lifecycle = (XgRenderProducerLifecycle){
        .artifact_generation = artifact_generation,
        .resource_generation = next_resource_generation++,
        .scene_generation = state.scene_generation,
        .producer_pc = guest_address(producer_pc),
        .scene_resource = resident ? 0u : (no_gates ? 2u : 1u),
    };
    return true;
}

static bool producer_lifecycle_matches(
        const XgRenderProducerLifecycle *lifecycle) {
    if (lifecycle == NULL || lifecycle->resource_generation == 0u)
        return false;
    if (lifecycle->scene_resource == 0u)
        return lifecycle->artifact_generation == 0u &&
            resident_producer_lifecycle_pc(lifecycle->producer_pc);
    if (lifecycle->scene_resource == 2u)
        return lifecycle->artifact_generation == 0u &&
            lifecycle->scene_generation == state.scene_generation &&
            xg_render_runtime_variant_no_gates_enabled();
    return lifecycle->scene_resource == 1u &&
        lifecycle->artifact_generation != 0u &&
        lifecycle->artifact_generation ==
            state.authenticated_artifact_generation &&
        lifecycle->scene_generation == state.scene_generation &&
        current_artifact_is_authorized();
}

static bool producer_lifecycle_matches_replay(
        const XgRenderProducerLifecycle *lifecycle,
        const GuestRenderNativeStreamMissContext *context) {
    return context != NULL && context->command_id != 0u &&
        (context->source_kind == GUEST_RENDER_NATIVE_STREAM_SOURCE_MMIO ||
         context->source_kind == GUEST_RENDER_NATIVE_STREAM_SOURCE_DMA_BLOCK ||
         context->source_kind ==
               GUEST_RENDER_NATIVE_STREAM_SOURCE_DMA_LINKED_LIST ||
         context->source_kind == GUEST_RENDER_NATIVE_STREAM_SOURCE_DMA_BURST) &&
        producer_lifecycle_matches(lifecycle);
}

static bool replay_container_matches_command(
        const GuestRenderNativeStreamMissContext *context) {
    return context != NULL &&
        (context->source_kind !=
             GUEST_RENDER_NATIVE_STREAM_SOURCE_DMA_LINKED_LIST ||
         (context->command_id >= 4u &&
          physical_address_equals((uint32_t)context->container_id,
                                  (uint32_t)context->command_id - 4u)));
}

static void retain_resident_residual_templates(void) {
    uint32_t resident_residual_count = 0u;

    for (uint32_t index = 0u; index < residual_templates.count; ++index) {
        const XgRenderResidualTemplate *record =
            &residual_templates.records[index];

        if (!record->valid || record->lifecycle.scene_resource != 0u)
            continue;
        if (resident_residual_count != index)
            residual_templates.records[resident_residual_count] = *record;
        ++resident_residual_count;
    }
    residual_templates.count = resident_residual_count;
    xg_render_lookup_reset(residual_template_lookup,
                           &residual_template_lookup_epoch);
    for (uint32_t index = 0u; index < residual_templates.count; ++index)
        xg_render_lookup_put(
            residual_template_lookup, residual_template_lookup_epoch,
            residual_templates.records[index].command_address, index);
}

static void invalidate_nonresident_producer_resources(void) {
    clear_zoom_source();
    clear_field_sprite_templates();
    retain_resident_residual_templates();
    overlay_ft4_state.count = 0u;
    clear_f4_sources();
}

static void invalidate_residual_templates_overlapping(uint32_t address,
                                                       uint32_t size) {
    const uint32_t physical = address & UINT32_C(0x1fffffff);
    uint32_t first_candidate;
    uint32_t end;

    if (size == 0u || physical >= UINT32_C(0x200000)) return;
    end = size > UINT32_C(0x200000) - physical
        ? UINT32_C(0x200000) : physical + size;
    first_candidate = physical >= XG_RENDER_RESIDUAL_MAX_RESOURCE_SIZE - 1u
        ? physical - (XG_RENDER_RESIDUAL_MAX_RESOURCE_SIZE - 1u) : 0u;
    first_candidate &= UINT32_C(0x1ffffc);

    for (uint32_t candidate = first_candidate;
         candidate <= ((end - 1u) & UINT32_C(0x1ffffc));
         candidate += 4u) {
        const uint32_t indexed = xg_render_lookup_find(
            residual_template_lookup, residual_template_lookup_epoch,
            candidate, residual_templates.count);
        XgRenderResidualTemplate *record;

        if (indexed == UINT32_MAX) continue;
        record = &residual_templates.records[indexed];
        if (!record->valid || !physical_address_equals(
                record->command_address, candidate) ||
            !normalized_ranges_overlap(record->command_address,
                                       record->resource_size, address, size))
            continue;
        record->valid = false;
        xg_render_lookup_remove(
            residual_template_lookup, residual_template_lookup_epoch,
            record->command_address, indexed);
    }
}

static void invalidate_producer_resources_overlapping(uint32_t address,
                                                       uint32_t size) {
    const uint64_t write_begin = address & UINT32_C(0x1fffffff);
    uint64_t write_end = write_begin + size;
    uint64_t first;
    uint64_t last;

    if (size == 0u) return;
    producer_resource_mark_invalidated(address, size);
    if (write_begin >= UINT32_C(0x200000)) return;
    if (write_end > UINT32_C(0x200000)) write_end = UINT32_C(0x200000);
    for (uint32_t index = 0u; index < field_sprite_templates.count; ++index) {
        XgRenderFieldSpriteBuilderRecord *record =
            &field_sprite_templates.records[index];
        if (normalized_ranges_overlap(record->packet_address, 0x28u,
                                      address, size) ||
            normalized_ranges_overlap(record->descriptor_address, 0x1cu,
                                      address, size))
            record->lifecycle = (XgRenderProducerLifecycle){0};
    }
    invalidate_residual_templates_overlapping(address, size);
    for (uint32_t index = 0u; index < overlay_ft4_state.count; ++index) {
        XgRenderOverlayFt4Template *record =
            &overlay_ft4_state.templates[index];
        if (normalized_ranges_overlap(record->packet_address, 0x28u,
                                      address, size))
            record->valid = false;
    }
    for (uint32_t index = 0u; index < f4_sources.count; ++index) {
        XgRenderF4SourceRecord *record = &f4_sources.records[index];
        if (record->source_id >= 4u &&
            normalized_ranges_overlap(record->source_id - 4u, 0x18u,
                                      address, size)) {
            record->valid = false;
            xg_render_lookup_remove(
                f4_source_lookup, f4_source_lookup_epoch,
                record->source_id, index);
        }
    }
    first = write_begin >= 0x28u ? write_begin - 0x28u + 1u : 0u;
    first = (first + 3u) & ~UINT64_C(3);
    last = (write_end - 1u) & ~UINT64_C(3);
    for (uint64_t candidate = first; candidate <= last; candidate += 4u) {
        const uint32_t source_id = (uint32_t)candidate + 4u;
        const uint32_t index = xg_render_lookup_find(
            model_ft4_source_lookup, model_ft4_source_lookup_epoch,
            source_id, model_ft4_source_count);
        if (index != UINT32_MAX) {
            XgRenderModelFt4SourceRecord *record = &model_ft4_sources[index];
            if (record->source_id >= 4u &&
                normalized_ranges_overlap(record->source_id - 4u, 0x28u,
                                          address, size)) {
                record->valid = false;
                xg_render_lookup_remove(
                    model_ft4_source_lookup, model_ft4_source_lookup_epoch,
                    record->source_id, index);
            }
        }
    }
    first = write_begin >= 0x20u ? write_begin - 0x20u + 1u : 0u;
    first = (first + 3u) & ~UINT64_C(3);
    for (uint64_t candidate = first; candidate <= last; candidate += 4u) {
        const uint32_t source_id = (uint32_t)candidate + 4u;
        const uint32_t index = xg_render_lookup_find(
            model_ft3_source_lookup, model_ft3_source_lookup_epoch,
            source_id, model_ft3_source_count);
        if (index != UINT32_MAX) {
            XgRenderModelFt3SourceRecord *record = &model_ft3_sources[index];
            if (record->source_id >= 4u &&
                normalized_ranges_overlap(record->source_id - 4u, 0x20u,
                                          address, size)) {
                record->valid = false;
                xg_render_lookup_remove(
                    model_ft3_source_lookup, model_ft3_source_lookup_epoch,
                    record->source_id, index);
            }
        }
    }
    if (model_ft4_template_count != 0u ||
        model_ft4_descriptor_template_count != 0u)
        invalidate_model_ft4_templates_overlapping(address, size);
    for (uint32_t quad = 0u; quad < XG_RENDER_ZOOM_QUAD_COUNT; ++quad) {
        for (uint32_t buffer = 0u; buffer < XG_RENDER_ZOOM_BUFFER_COUNT;
             ++buffer) {
            if (normalized_ranges_overlap(
                    UINT32_C(0x800b1274) + quad * 0x50u + buffer * 0x28u,
                    0x28u, address, size)) {
                clear_zoom_source();
                return;
            }
        }
    }
}

static void clear_source_pending(void) {
    source_state.pending = (XgRenderSourcePending){ 0 };
}

static void clear_ft4_geometry_pending(void) {
    ft4_geometry.pending = (XgRenderFt4GeometryPending){ 0 };
}

static void clear_pre_scene(void) {
    /* count bounds every record access, so stale primitive storage is
     * unreachable after a lifecycle reset. Avoid clearing 4096 large native
     * primitives on every rejected producer cutover. */
    pre_scene.count = 0u;
    pre_scene.blocker = 0u;
    pre_scene.blocked = false;
}

static bool stage_pre_scene_primitive(
    const XgRenderPreScenePrimitive *record) {
    const uint32_t packet_address =
        record != NULL
            ? record->packet_address & UINT32_C(0x001ffffc)
            : 0u;
    uint32_t index;

    if (record == NULL || pre_scene.blocked) return false;
    for (index = 0u; index < pre_scene.count; ++index) {
        if ((record->temporal_only && pre_scene.records[index].temporal_only &&
             pre_scene.records[index].interpolation_producer_id ==
                 record->interpolation_producer_id &&
             pre_scene.records[index].interpolation_primitive_id ==
                 record->interpolation_primitive_id) ||
            (!record->temporal_only &&
             !pre_scene.records[index].temporal_only &&
             (pre_scene.records[index].packet_address &
              UINT32_C(0x001ffffc)) == packet_address)) {
            pre_scene.records[index] = *record;
            return true;
        }
    }
    if (pre_scene.count == XG_RENDER_PRE_SCENE_PRIMITIVE_CAPACITY)
        return false;
    pre_scene.records[pre_scene.count++] = *record;
    return true;
}

static void clear_standalone_submission(void) {
    standalone_submission = (XgRenderStandaloneSubmissionState){ 0 };
}

static void clear_particle_sources(void) {
    xg_field_particles_reset();
}

static void clear_zoom_source(void) {
    xg_field_zoom_reset();
}

static void clear_zoom_pending(void) {
    xg_field_zoom_reset_pending();
}

static void clear_projected_source(void) {
    xg_field_projected_reset();
}

static void clear_model_ft4_shadow_pending(void) {
    if (!model_ft4_shadow.context.valid &&
        !model_ft4_shadow.snapshot.pending && model_ft4_shadow.count == 0u)
        return;
    model_ft4_shadow.context = (XgRenderModelFt4ShadowContext){ 0 };
    model_ft4_shadow.initial_packet_cursor = 0u;
    model_ft4_shadow.initial_counter = 0u;
    model_ft4_shadow.expected_counter_delta = 0u;
    model_ft4_shadow.descriptor_base = 0u;
    model_ft4_shadow.count = 0u;
    model_ft4_shadow.snapshot.pending = false;
}

static void block_model_ft4_shadow(uint32_t blocker) {
    clear_model_ft4_shadow_pending();
    model_ft4_shadow.snapshot.blocked = true;
    if (model_ft4_shadow.snapshot.blocker == 0u)
        model_ft4_shadow.snapshot.blocker = blocker;
}

static void clear_model_ft3_shadow_pending(void) {
    if (!model_ft3_shadow.snapshot.pending && model_ft3_shadow.count == 0u)
        return;
    model_ft3_shadow.initial_packet_cursor = 0u;
    model_ft3_shadow.initial_counter = 0u;
    model_ft3_shadow.expected_counter_delta = 0u;
    model_ft3_shadow.descriptor_base = 0u;
    model_ft3_shadow.count = 0u;
    model_ft3_shadow.snapshot.pending = false;
}

static void block_model_ft3_shadow(uint32_t blocker) {
    clear_model_ft3_shadow_pending();
    model_ft3_shadow.snapshot.blocked = true;
    if (model_ft3_shadow.snapshot.blocker == 0u)
        model_ft3_shadow.snapshot.blocker = blocker;
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

static bool source_context_matches(XgRenderAuthTier tier) {
    bool identity_bound = false;
    bool identity_gate_passed = false;
    const bool candidate_context =
        tier == XG_RENDER_AUTH_TIER_COLD_INTERPRETER ||
        (tier == XG_RENDER_AUTH_TIER_WARM_NATIVE &&
         ((state.active && state.candidate_matched &&
           state.candidate_dispatched) ||
          pending_variant_candidate_matches()));
    const bool proof_context = completed_proof_matches_tier(tier);
    const bool pending_context =
        state.active && state.pending_variant_sequence &&
        state.pending_variant_tier == tier && candidate_context;
    const bool completed_context = proof_context;
    const bool static_valid = xg_render_static_auth_metadata_is_valid();
    const bool identity_valid = xg_render_static_auth_bind_identity(
        &identity_bound, &identity_gate_passed);
    const bool no_gates = xg_render_runtime_variant_no_gates_enabled();

    source_state.aggregate.context_bits =
        (state.armed ? 1u : 0u) |
        (state.pending_variant_sequence ? 2u : 0u) |
        (state.pending_variant_capture_ready ? 4u : 0u) |
        (state.pending_variant_tier == tier ? 8u : 0u) |
        (candidate_context ? 16u : 0u) |
        (state.completed ? 32u : 0u) |
        (proof_context ? 64u : 0u) |
        (static_valid ? 128u : 0u) |
        (identity_valid ? 256u : 0u);

    return state.armed && (no_gates || pending_context || completed_context) &&
           static_valid && identity_valid;
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

static bool source_inner_call_capture_matches(XgRenderAuthTier tier,
                                              uint32_t hook, uint32_t pc,
                                              uint32_t instruction_word) {
    return hook == PSX_XG_RENDER_AUTH_HOOK_INTERNAL_OBSERVATION &&
           source_state.pending.valid && source_state.pending.tier == tier &&
           source_state.pending.metadata.operation ==
               PSX_XG_RENDER_SOURCE_OPERATION_CALL &&
           physical_address_equals(pc, source_state.pending.pc) &&
           instruction_word == source_state.pending.instruction &&
           source_context_matches(tier);
}

static void append_source_event(
    PsxXgRenderSourceStage stage, uint32_t pc,
    const PsxXgRenderSourceSiteMetadata *metadata, uint32_t auxiliary) {
    PsxXgRenderSourceEvent *event =
        &source_state.aggregate.events[source_state.aggregate.count++];

    *event = (PsxXgRenderSourceEvent){
        source_state.aggregate.next_sequence++, guest_address(pc), auxiliary,
        metadata->operation, stage, metadata->width,
    };
}

static void append_source_pair(uint32_t pc,
                               const XgRenderSourcePending *pending,
                               const PsxXgRenderSourceSiteMetadata *metadata,
                               uint32_t auxiliary) {
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
    const uint32_t guest_pc = guest_address(pending->pc);
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

static int16_t low_s16(uint32_t value) {
    return (int16_t)(uint16_t)value;
}

static void apply_projected_quad_positions(
    XgRenderIrNativePrimitive *primitive,
    const XgHost3dProjectedVertex projected[4]) {
    static const uint8_t split[2][3] = {{0u, 1u, 2u}, {2u, 1u, 3u}};

    /* Field characters, particles, and zoom quads are screen-space sprites.
     * Their temporal path must not reproject the billboard as 3D geometry. */
    if (primitive == NULL || projected == NULL) return;
    for (uint32_t triangle = 0u; triangle < 2u; ++triangle) {
        for (uint32_t vertex = 0u; vertex < 3u; ++vertex) {
            const uint32_t source_index = split[triangle][vertex];
            XgRenderQuadSourceVertex source_vertex = {0};
            XgRenderIrVertex *target =
                &primitive->triangles[triangle].vertices[vertex];

            xg_render_quad_set_projected_position(
                &source_vertex, &projected[source_index]);
            target->x = (int32_t)source_vertex.x * INT32_C(65536);
            target->y = (int32_t)source_vertex.y * INT32_C(65536);
            target->native_view_x = source_vertex.native_view_x_16_16;
            target->native_view_y = source_vertex.native_view_y_16_16;
            target->native_view_position = source_vertex.native_view_position;
            target->projective_view_z = source_vertex.projective_view_z;
            target->temporal_depth = source_vertex.projective_view_z;
            target->temporal_depth_valid = true;
        }
    }
}

int16_t xg_render_runtime_low_s16(uint32_t value) {
    return low_s16(value);
}

static bool ft4_destinations_are_valid(const uint32_t destinations[4]);

static void capture_shadow_projection(const CPUState *cpu,
                                      XgHost3dProjection *projection) {
    const uint32_t *control = cpu->gte_ctrl;

    projection->rotation[0][0] = low_s16(control[0]);
    projection->rotation[0][1] = low_s16(control[0] >> 16u);
    projection->rotation[0][2] = low_s16(control[1]);
    projection->rotation[1][0] = low_s16(control[1] >> 16u);
    projection->rotation[1][1] = low_s16(control[2]);
    projection->rotation[1][2] = low_s16(control[2] >> 16u);
    projection->rotation[2][0] = low_s16(control[3]);
    projection->rotation[2][1] = low_s16(control[3] >> 16u);
    projection->rotation[2][2] = low_s16(control[4]);
    projection->translation[0] = (int32_t)control[5];
    projection->translation[1] = (int32_t)control[6];
    projection->translation[2] = (int32_t)control[7];
    projection->screen_offset_x = (int32_t)control[24];
    projection->screen_offset_y = (int32_t)control[25];
    projection->projection_distance = (uint16_t)control[26];
    projection->depth_cue_a = low_s16(control[27]);
    projection->depth_cue_b = (int32_t)control[28];
    projection->average_z_scale4 = low_s16(control[30]);
}

void xg_render_runtime_capture_shadow_projection(
    const CPUState *cpu, XgHost3dProjection *projection) {
    capture_shadow_projection(cpu, projection);
}

static bool vector_address_is_valid(uint32_t address) {
    const uint32_t segment = address & UINT32_C(0xe0000000);
    const uint32_t physical = address & UINT32_C(0x1fffffff);

    return (segment == 0u || segment == UINT32_C(0x80000000) ||
            segment == UINT32_C(0xa0000000)) &&
           physical <= UINT32_C(0x001ffff8) && (physical & 1u) == 0u;
}

static bool stack_address_is_valid(uint32_t address) {
    const uint32_t segment = address & UINT32_C(0xe0000000);
    const uint32_t physical = address & UINT32_C(0x1fffffff);
    const bool valid_segment =
        segment == 0u || segment == UINT32_C(0x80000000) ||
        segment == UINT32_C(0xa0000000);

    return valid_segment && (physical & 3u) == 0u &&
           (physical <= UINT32_C(0x001fffd8) ||
            (physical >= UINT32_C(0x1f800000) &&
             physical <= UINT32_C(0x1f8003d8)));
}

static bool word_address_is_valid(uint32_t address) {
    const uint32_t segment = address & UINT32_C(0xe0000000);
    const uint32_t physical = address & UINT32_C(0x1fffffff);
    const bool valid_segment =
        segment == 0u || segment == UINT32_C(0x80000000) ||
        segment == UINT32_C(0xa0000000);

    return valid_segment && (physical & 3u) == 0u &&
           (physical <= UINT32_C(0x001ffffc) ||
           (physical >= UINT32_C(0x1f800000) &&
            physical <= UINT32_C(0x1f8003fc)));
}

static bool guest_data_range_is_valid(uint32_t address, uint32_t size,
                                      uint32_t alignment,
                                      bool allow_scratchpad) {
    const uint32_t segment = address & UINT32_C(0xe0000000);
    const uint64_t physical = address & UINT32_C(0x1fffffff);
    const bool valid_segment =
        segment == 0u || segment == UINT32_C(0x80000000) ||
        segment == UINT32_C(0xa0000000);
    const bool in_ram = physical + size <= UINT64_C(0x00200000);
    const bool in_scratchpad = allow_scratchpad &&
        physical >= UINT32_C(0x1f800000) &&
        physical + size <= UINT64_C(0x1f800400);

    return size != 0u && alignment != 0u && valid_segment &&
        (address & (alignment - 1u)) == 0u &&
        (uint64_t)address + size - 1u <= UINT32_MAX &&
        (in_ram || in_scratchpad);
}

static bool read_native_u8(void *context, uint32_t address,
                           uint8_t *out_value) {
    CPUState *cpu = (CPUState *)context;

    if (cpu == NULL || cpu->read_byte == NULL || out_value == NULL ||
        !guest_data_range_is_valid(address, 1u, 1u, true))
        return false;
    *out_value = cpu->read_byte(address);
    return true;
}

static bool read_native_u16(void *context, uint32_t address,
                            uint16_t *out_value) {
    CPUState *cpu = (CPUState *)context;

    if (cpu == NULL || cpu->read_half == NULL || out_value == NULL ||
        !guest_data_range_is_valid(address, 2u, 2u, true))
        return false;
    *out_value = cpu->read_half(address);
    return true;
}

static bool read_native_u32(void *context, uint32_t address,
                            uint32_t *out_value) {
    CPUState *cpu = (CPUState *)context;

    if (cpu == NULL || cpu->read_word == NULL || out_value == NULL ||
        !guest_data_range_is_valid(address, 4u, 4u, true))
        return false;
    *out_value = cpu->read_word(address);
    return true;
}

static bool authorize_entity_shadow_source_range(
    void *context, XgWorldEntityShadowsSourceRangeKind kind,
    uint32_t address, uint32_t size) {
    (void)context;
    if (kind != XG_WORLD_ENTITY_SHADOWS_SOURCE_PENDING_LIST &&
        kind != XG_WORLD_ENTITY_SHADOWS_SOURCE_TERRAIN_CHUNK)
        return false;
    return guest_data_range_is_valid(address, size, 4u, false);
}

static bool authorize_actor_source_range(
    void *context, XgWorldActorSpritesSourceRangeKind kind,
    uint32_t address, uint32_t size) {
    uint32_t alignment;

    (void)context;
    switch (kind) {
    case XG_WORLD_ACTOR_SPRITES_SOURCE_ACTOR:
    case XG_WORLD_ACTOR_SPRITES_SOURCE_DATA:
    case XG_WORLD_ACTOR_SPRITES_SOURCE_DESCRIPTORS:
    case XG_WORLD_ACTOR_SPRITES_SOURCE_CONTEXT:
    case XG_WORLD_ACTOR_SPRITES_SOURCE_TRIG_TABLE:
    case XG_WORLD_ACTOR_SPRITES_SOURCE_CAMERA:
    case XG_WORLD_ACTOR_SPRITES_SOURCE_SCRATCH_TEMPLATE:
    case XG_WORLD_ACTOR_SPRITES_SOURCE_GLOBAL:
        alignment = 4u;
        break;
    case XG_WORLD_ACTOR_SPRITES_SOURCE_PARTS:
        alignment = 2u;
        break;
    default:
        return false;
    }
    return guest_data_range_is_valid(address, size, alignment, false);
}

static bool authorize_world_model_range(
    void *context, XgWorldModelsNativeRangeKind kind,
    uint32_t address, uint32_t size) {
    uint32_t alignment;

    (void)context;
    switch (kind) {
    case XG_WORLD_MODELS_NATIVE_RANGE_RECORDS:
    case XG_WORLD_MODELS_NATIVE_RANGE_TRANSFORM_NODE:
    case XG_WORLD_MODELS_NATIVE_RANGE_MODEL_HEADER:
    case XG_WORLD_MODELS_NATIVE_RANGE_PACKET_OUTPUT:
    case XG_WORLD_MODELS_NATIVE_RANGE_ORDERING_TABLE_OUTPUT:
    case XG_WORLD_MODELS_NATIVE_RANGE_ATTRIBUTE:
    case XG_WORLD_MODELS_NATIVE_RANGE_VERTEX:
    case XG_WORLD_MODELS_NATIVE_RANGE_AUXILIARY_VERTEX:
        alignment = 4u;
        break;
    case XG_WORLD_MODELS_NATIVE_RANGE_TOPOLOGY:
        alignment = 2u;
        break;
    default:
        return false;
    }
    return guest_data_range_is_valid(address, size, alignment, false);
}

bool xg_render_runtime_vector_address_is_valid(uint32_t address) {
    return vector_address_is_valid(address);
}

bool xg_render_runtime_stack_address_is_valid(uint32_t address) {
    return stack_address_is_valid(address);
}

bool xg_render_runtime_word_address_is_valid(uint32_t address) {
    return word_address_is_valid(address);
}

static XgRenderParticleSource *find_particle_source(uint32_t particle_address) {
    return xg_field_particles_find(particle_address);
}

static void invalidate_particle_source(uint32_t particle_address) {
    xg_field_particles_invalidate(particle_address);
}

static bool capture_ft4_destinations(
    CPUState *cpu, XgRenderFt4GeometryPending *geometry) {
    uint32_t stack_pointer;
    size_t index;

    if (cpu == NULL || geometry == NULL || cpu->read_word == NULL) return false;
    stack_pointer = cpu->gpr[29];
    if (!stack_address_is_valid(stack_pointer)) return false;
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
    if (!stack_address_is_valid(stack_pointer)) return false;
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

        if (!vector_address_is_valid(vector_addresses[index])) return false;
        xy = cpu->read_word(vector_addresses[index]);
        z_pad = cpu->read_word(vector_addresses[index] + 4u);
        geometry->pre_transform.vertices[index] = (XgHost3dVector){
            low_s16(xy), low_s16(xy >> 16u), low_s16(z_pad),
            (uint16_t)(z_pad >> 16u),
        };
        geometry->destinations[index] =
            cpu->read_word(stack_pointer + 0x10u + (uint32_t)index * 4u);
    }
    if (!ft4_destinations_are_valid(geometry->destinations)) return false;
    capture_shadow_projection(cpu, &geometry->pre_transform.projection);
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

static bool capture_source_values(CPUState *cpu,
                                   XgRenderFt4GeometryPending *geometry,
                                   uint32_t model_address,
                                   uint32_t ft4_index) {
    XgRenderAuthSnapshot auth_snapshot = { 0 };
    XgFieldCharacterSourceCaptureRequest request = { 0 };
    XgFieldCharacterAuthenticatedReader reader;
    GpuDrawState draw = { 0 };

    if (cpu == NULL || geometry == NULL || state.auth == NULL ||
        cpu->read_half == NULL || cpu->read_word == NULL ||
        gpu_render_vram_mutation_overflowed() ||
        xg_render_auth_snapshot(state.auth, &auth_snapshot) !=
            XG_RENDER_AUTH_OK)
        return false;
    memcpy(request.game_sha256, xg_render_game_identity,
           sizeof(request.game_sha256));
    memcpy(request.manifest_sha256, xg_render_manifest_identity,
           sizeof(request.manifest_sha256));
    request.source_generation = ft4_geometry.next_sequence;
    request.scene_generation = state.scene_generation;
    request.visual_state.scene_epoch =
        auth_snapshot.logical_identity.state_id.scene_epoch;
    request.visual_state.state_sequence =
        auth_snapshot.logical_identity.state_id.state_sequence;
    request.vram_mutation_serial = gpu_render_vram_mutation_serial();
    request.producer_record_id =
        auth_snapshot.logical_identity.producer_record_id;
    request.producer_entry = state.pending_variant_entry;
    request.actor_index = cpu->gpr[21];
    request.actor_record_address = cpu->gpr[18];
    request.model_address = model_address;
    request.producer_stack_pointer = cpu->gpr[29];
    request.producer_ft4_index = ft4_index;
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
    geometry->source_capture_result = (uint32_t)xg_field_character_source_capture(
        &request, &reader, &geometry->source_snapshot);
    geometry->source_captured =
        geometry->source_capture_result == XG_FIELD_CHARACTER_SOURCE_CAPTURE_OK;
    return geometry->source_captured;
}

static void publish_ft4_geometry(void);

static void arm_ft4_geometry(CPUState *cpu,
                             const XgRenderSourcePending *pending) {
    GuestRenderBridgeSnapshot bridge_snapshot = { 0 };

    if (!ft4_geometry.enabled || ft4_geometry.blocked ||
        pending->metadata.operation != PSX_XG_RENDER_SOURCE_OPERATION_CALL)
        return;
    if (ft4_geometry.pending.valid) {
        block_ft4_geometry(false);
        return;
    }
    ft4_geometry.pending = (XgRenderFt4GeometryPending){
        .scene_generation = state.scene_generation,
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
                                cpu->gpr[8]);
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

static void reject_producer_family(uint32_t blocker) {
    producer_family.blocked = true;
    if (producer_family.blocker == 0u) producer_family.blocker = blocker;
    guest_render_transaction_invalidate_deferred();
    guest_render_transaction_clear_pending();
    abort_active(XG_RENDER_AUTH_REJECT_TRANSACTION_FAILURE,
                 PSX_XG_RENDER_AUTH_REJECTION_SOURCE_RUNTIME_HOOK,
                 false, PSX_XG_RENDER_AUTH_HOOK_CAPTURE, 0u);
}

static void reset_render_transaction(void) {
    GuestRenderTransactionSnapshot snapshot = { 0 };

    if (guest_render_native_stream_enabled()) {
        XgRenderAuthSnapshot auth_snapshot = { 0 };

        if (state.active && !state.completed && state.auth != NULL &&
            xg_render_auth_snapshot(state.auth, &auth_snapshot) ==
                XG_RENDER_AUTH_OK) {
            const GuestRenderVisualStateId state_id =
                auth_snapshot.logical_identity.state_id;

            guest_render_native_stream_abandon_visual(
                (GpuRenderTransactionId){
                    state_id.scene_epoch, state_id.state_sequence});
        }
        return;
    }
    if (guest_render_transaction_snapshot(&snapshot) ==
            GUEST_RENDER_TRANSACTION_OK &&
        snapshot.phase == GUEST_RENDER_TRANSACTION_ACTIVE)
        (void)guest_render_transaction_abort_before_observation(
            GUEST_RENDER_TRANSACTION_OBSERVATION_CALLER_ABORT);
    guest_render_transaction_invalidate_deferred();
    guest_render_transaction_clear_pending();
}

static void set_semantic_interpolation_identity(
        GpuRenderSemantic *semantic, uint64_t scene_id,
        uint32_t producer_id, uint32_t primitive_id) {
    if (semantic == NULL || producer_id == 0u) return;
    semantic->interpolation_identity = (GpuRenderInterpolationIdentity){
        scene_id, producer_id, primitive_id, 1u,
    };
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

static GuestRenderTransactionStatus stage_render_semantic_exact(
        GpuRenderTransactionId visual_id, uint64_t exact_command_id,
        const GpuRenderSemantic *semantic) {
    GuestRenderNativeStreamStatus status;

    if (semantic == NULL)
        return GUEST_RENDER_TRANSACTION_INVALID_ARGUMENT;

    if (!guest_render_native_stream_enabled())
        return guest_render_transaction_stage_exact(
            visual_id, exact_command_id, semantic);
    status = guest_render_native_stream_stage_exact(
        visual_id, exact_command_id, semantic);
    switch (status) {
    case GUEST_RENDER_NATIVE_STREAM_OK:
        return GUEST_RENDER_TRANSACTION_OK;
    case GUEST_RENDER_NATIVE_STREAM_INVALID_ARGUMENT:
        return GUEST_RENDER_TRANSACTION_INVALID_ARGUMENT;
    case GUEST_RENDER_NATIVE_STREAM_CAPACITY_EXCEEDED:
        return GUEST_RENDER_TRANSACTION_CAPACITY_EXCEEDED;
    case GUEST_RENDER_NATIVE_STREAM_DUPLICATE_COMMAND:
        return GUEST_RENDER_TRANSACTION_DUPLICATE_TARGET;
    case GUEST_RENDER_NATIVE_STREAM_STALE_VISUAL_ID:
        return GUEST_RENDER_TRANSACTION_STALE_VISUAL_ID;
    case GUEST_RENDER_NATIVE_STREAM_DISABLED:
    case GUEST_RENDER_NATIVE_STREAM_NOT_FOUND:
    default:
        return GUEST_RENDER_TRANSACTION_INVALID_TRANSITION;
    }
}

static void abort_standalone_submission(void) {
    if (!standalone_submission.open) return;
    guest_render_native_stream_abandon_visual(
        (GpuRenderTransactionId){
            standalone_submission.visual_id.scene_epoch,
            standalone_submission.visual_id.state_sequence,
        });
    guest_render_bridge_abort_scene(GUEST_RENDER_FALLBACK_FORCED_ORIGINAL);
    guest_render_transaction_clear_pending();
    clear_standalone_submission();
}

static bool native_ir_flush_failure(uint32_t reason, uint64_t index,
                                    uint32_t packet_address,
                                    uint32_t status) {
    if (instrumentation.native_ir_flush_failure_count == 0u) {
        instrumentation.first_native_ir_flush_failure_index = index;
        instrumentation.first_native_ir_flush_failure_reason = reason;
        instrumentation.first_native_ir_flush_failure_packet = packet_address;
        instrumentation.first_native_ir_flush_failure_status = status;
    }
    ++instrumentation.native_ir_flush_failure_count;
    return false;
}

static bool flush_native_auth_ir(void) {
    XgRenderAuthSnapshot auth_snapshot = { 0 };
    size_t index;

    ++instrumentation.native_ir_flush_attempt_count;
    if (state.auth == NULL)
        return native_ir_flush_failure(1u, 0u, 0u, 0u);
    if (xg_render_auth_snapshot(state.auth, &auth_snapshot) != XG_RENDER_AUTH_OK)
        return native_ir_flush_failure(2u, 0u, 0u, 0u);
    if (auth_snapshot.effective_render_mode != GUEST_RENDER_RENDER_NATIVE)
        return native_ir_flush_failure(
            3u, 0u, 0u, (uint32_t)auth_snapshot.effective_render_mode);
    for (index = 0u; index < auth_snapshot.native_item_count; ++index) {
        XgRenderIrNativeItem item = { 0 };
        GpuRenderSemantic semantic;
        uint32_t packet_address;

        if (xg_render_auth_native_item_get(state.auth, index, &item) !=
            XG_RENDER_AUTH_OK)
            return native_ir_flush_failure(4u, index, 0u, 0u);
        packet_address = item.base.ordering.packet_guest_address &
            UINT32_C(0x001ffffc);
        if (xg_render_backend_translate_primitive(&item.native, &semantic) !=
            XG_RENDER_BACKEND_OK)
            return native_ir_flush_failure(5u, index, packet_address, 0u);
    }
    return true;
}

static bool flush_pre_scene_primitives(void) {
    XgRenderAuthSnapshot auth_snapshot = { 0 };
    GpuRenderTransactionId visual_id;
    uint32_t index;

    if (pre_scene.blocked || state.auth == NULL ||
        xg_render_auth_snapshot(state.auth, &auth_snapshot) !=
            XG_RENDER_AUTH_OK)
        return false;
    visual_id = (GpuRenderTransactionId){
        auth_snapshot.logical_identity.state_id.scene_epoch,
        auth_snapshot.logical_identity.state_id.state_sequence,
    };
    for (index = 0u; index < pre_scene.count; ++index) {
        const XgRenderPreScenePrimitive *record = &pre_scene.records[index];
        GpuRenderSemantic semantic;

        if (xg_render_backend_translate_primitive(&record->primitive,
                                                  &semantic) !=
                XG_RENDER_BACKEND_OK) {
            pre_scene.blocker = 7u;
            return false;
        }
        if (record->interpolation_identity_valid)
            set_semantic_interpolation_identity(
                &semantic, interpolation_scene_generation(),
                record->interpolation_producer_id,
                record->interpolation_primitive_id);
        if (record->interpolation_identity_valid) {
            GpuRenderInterpolationVertexAnchor anchors[
                GPU_RENDER_SEMANTIC_TRIANGLE_CAPACITY * 3u];
            size_t anchor_count = 0u;

            for (uint32_t triangle = 0u;
                 triangle < semantic.triangle_count; ++triangle) {
                for (uint32_t vertex = 0u; vertex < 3u; ++vertex) {
                    const GpuRenderSemanticVertex *candidate =
                        &semantic.triangles[triangle].vertices[vertex];
                    bool duplicate = false;

                    if (!candidate->interpolation_vertex_identity_valid)
                        continue;
                    for (size_t prior = 0u; prior < anchor_count; ++prior) {
                        duplicate |= anchors[prior].vertex.interpolation_group_id ==
                                candidate->interpolation_group_id &&
                            anchors[prior].vertex.interpolation_vertex_id ==
                                candidate->interpolation_vertex_id;
                    }
                    if (duplicate) continue;
                    anchors[anchor_count++] =
                        (GpuRenderInterpolationVertexAnchor){
                            .scene_id = semantic.interpolation_identity.scene_id,
                            .producer_id =
                                semantic.interpolation_identity.producer_id,
                            .material = semantic.material,
                            .vertex = *candidate,
                        };
                }
            }
            if (anchor_count != 0u &&
                gr_record_interpolation_anchors(anchors, anchor_count) !=
                    GPU_RENDER_TRANSACTION_OK) {
                pre_scene.blocker = 8u;
                return false;
            }
        }
        if (record->temporal_only) {
            if (!record->interpolation_identity_valid ||
                gr_draw_semantic_temporal_candidate(
                    &semantic, &record->temporal_cull) !=
                    GPU_RENDER_TRANSACTION_OK) {
                pre_scene.blocker = 10u;
                return false;
            }
            continue;
        }
        XgRenderAuthResult append_result =
            xg_render_auth_append_native_insertion(
                state.auth, record->packet_address & UINT32_C(0x001ffffc),
                record->source_primitive_index, record->ot_bucket,
                record->payload_word_count, &record->primitive, true);
        if (append_result != XG_RENDER_AUTH_OK) {
            pre_scene.blocker = 11u + (uint32_t)append_result;
            return false;
        }
        if (stage_render_semantic_exact(
                visual_id,
                (record->packet_address & UINT32_C(0x001ffffc)) + 4u,
                &semantic) != GUEST_RENDER_TRANSACTION_OK) {
            pre_scene.blocker = 12u;
            return false;
        }
    }
    clear_pre_scene();
    return true;
}

static void process_producer_family_candidate(CPUState *cpu,
                                              uint32_t ot_bucket) {
    PsxXgRenderFt4Geometry geometry;
    XgFieldCharacterRuntimeCandidate candidate;
    XgRenderIrNativePrimitive primitive;
    GpuRenderSemantic semantic;
    GpuRenderTransactionId visual_id;
    XgFieldCharacterRuntimeResult result;
    uint32_t compare_result = 0u;
    uint32_t mismatch_word = UINT32_MAX;
    uint32_t mismatch_byte = UINT32_MAX;
    uint32_t packet_address;
    uint32_t source_primitive_index;
    GuestRenderBridgeSnapshot bridge_snapshot = { 0 };

    if (!producer_family.enabled || producer_family.blocked) return;
    if (guest_render_bridge_snapshot(&bridge_snapshot) != GUEST_RENDER_OK) {
        reject_producer_family(9u);
        return;
    }
    if (bridge_snapshot.modes.effective_render_mode ==
        GUEST_RENDER_RENDER_ORIGINAL)
        return;
    if (cpu == NULL || !psx_xg_render_auth_ft4_geometry_pop(&geometry)) {
        reject_producer_family(1u);
        return;
    }
    ++producer_family.geometry_count;
    if (bridge_snapshot.modes.effective_render_mode ==
        GUEST_RENDER_RENDER_NATIVE) {
        if (!geometry.source_captured) {
            producer_family.last_runtime_result =
                XG_FIELD_CHARACTER_RUNTIME_INCOMPLETE_SOURCE;
            ++producer_family.source_incomplete_count;
            reject_producer_family(10u);
            return;
        }
        ++producer_family.source_capture_count;
        result = xg_field_character_runtime_build_candidate(
            &geometry.source_snapshot, &candidate);
        producer_family.last_runtime_result = (uint32_t)result;
        if (result != XG_FIELD_CHARACTER_RUNTIME_OK) {
            ++producer_family.source_incomplete_count;
            reject_producer_family(11u);
            return;
        }
        candidate.packet_guest_address = geometry.packet_guest_address;
        packet_address = geometry.packet_guest_address & UINT32_C(0x001ffffc);
        if (geometry.source_snapshot.identity.actor_index >
            (UINT32_MAX - geometry.source_snapshot.ordering.ft4_index) /
                XG_FIELD_CHARACTER_SOURCE_DYNAMIC_MODEL_FT4_COUNT) {
            reject_producer_family(12u);
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
            reject_producer_family(12u);
            return;
        }
        apply_projected_quad_positions(
            &primitive, candidate.source_derived.projection.vertices);
        if (xg_render_backend_translate_primitive(&primitive, &semantic) !=
            XG_RENDER_BACKEND_OK) {
            reject_producer_family(12u);
            return;
        }
        set_semantic_interpolation_identity(
            &semantic,
            interpolation_scene_generation(),
            geometry.source_snapshot.identity.producer_record_id,
            source_primitive_index);
        if (state.auth == NULL ||
            xg_render_auth_append_native_insertion(
                state.auth, packet_address,
                source_primitive_index,
                candidate.source_derived.ordering_bucket,
                XG_FIELD_CHARACTER_PACKET_WORD_COUNT, &primitive,
                state.pending_variant_sequence &&
                    !state.pending_variant_capture_ready) !=
                XG_RENDER_AUTH_OK) {
            reject_producer_family(13u);
            return;
        }
        visual_id = (GpuRenderTransactionId){
            geometry.source_snapshot.generation.visual_state.scene_epoch,
            geometry.source_snapshot.generation.visual_state.state_sequence,
        };
        if (stage_render_semantic_exact(
                visual_id, packet_address + 4u,
                &semantic) != GUEST_RENDER_TRANSACTION_OK) {
            reject_producer_family(14u);
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
        reject_producer_family(2u);
        return;
    }
    ++producer_family.source_capture_count;
    result = xg_field_character_runtime_build_candidate(
        &geometry.source_snapshot, &candidate);
    producer_family.last_runtime_result = (uint32_t)result;
    if (result != XG_FIELD_CHARACTER_RUNTIME_OK) {
        reject_producer_family(2u);
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
        reject_producer_family(3u);
        return;
    }
    ++producer_family.match_count;
    /* Shadow alone consumes post-transform values and the private packet copy.
     * It never binds or stages the candidate. */
}

static bool observe_source(CPUState *cpu, XgRenderAuthTier tier,
                           PsxXgRenderSourceStage stage, uint32_t pc,
                           uint32_t instruction_word, uint32_t auxiliary) {
    PsxXgRenderSourceSiteMetadata metadata = { 0 };

    if (source_state.aggregate.blocked) return false;
    if (stage != PSX_XG_RENDER_SOURCE_STAGE_PRE &&
        stage != PSX_XG_RENDER_SOURCE_STAGE_COMMIT) {
        block_source_proof(false, XG_RENDER_SOURCE_BLOCK_INVALID_STAGE);
        return false;
    }
    if (!source_context_matches(tier)) {
        block_source_proof(false, XG_RENDER_SOURCE_BLOCK_CONTEXT);
        return false;
    }
    if (!xg_render_runtime_variant_source_site_lookup(
            pc, instruction_word, &metadata)) {
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
            .pc = guest_address(pc),
            .instruction = instruction_word,
            .auxiliary = auxiliary,
            .tier = tier,
            .valid = true,
        };
        arm_ft4_geometry(cpu, &source_state.pending);
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
    append_source_pair(guest_address(pc), &source_state.pending, &metadata,
                       auxiliary);
    if (source_state.pending.metadata.operation ==
            PSX_XG_RENDER_SOURCE_OPERATION_BUCKET)
        process_producer_family_candidate(cpu, auxiliary);
    clear_source_pending();
    return true;
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
    if (xg_render_runtime_variant_no_gates_enabled()) return;
    record_reset(false);
    block_source_proof(false, XG_RENDER_SOURCE_BLOCK_LIFECYCLE);
    if (ft4_geometry.enabled) block_ft4_geometry(false);
    if (producer_family.enabled) {
        producer_family.blocked = true;
        if (!producer_family.blocker) producer_family.blocker = 4u;
    }
    reset_render_transaction();
    clear_pre_scene();
    abort_standalone_submission();
    clear_world_clouds_native_pending();
    clear_world_actor_native_pending();
    clear_world_actor_context();
    clear_world_models_native_pending();
    end_gte_attribution_producer();
    state.active = false;
    state.armed = false;
    state.completed = false;
    clear_pending_candidate();
    clear_pending_variant_sequence();
    xg_render_runtime_variant_reset();
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
    else
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
    record_completed_proof_publication();
}

static void begin_scene(XgRenderAuthTier tier, uint32_t producer_entry) {
    const GuestRenderSceneConfig config = {
        state.requested_timing_mode,
        state.requested_render_mode,
    };
    GuestRenderVisualStateId state_id = { 0 };
    XgRenderAuthProfile profile;

    if (producer_entry != xg_render_manifest_validation.producer_entry)
        return;
    if (standalone_submission.open)
        abort_standalone_submission();
    /* A rejected pre-scene cutover cannot be flushed into this authenticated
     * producer.  Drop only that poisoned batch; valid staged records remain. */
    if (pre_scene.blocked)
        clear_pre_scene();
    reset_render_transaction();
    /* A new authenticated producer scene gets a new visual-id namespace. Do
     * not retain packet generations from the previous scene: command sources
     * are guest-RAM addresses and may be regenerated indefinitely. Keep the
     * stream's allocated capacity, but reset its occupied entries. */
    guest_render_native_stream_clear();
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
               !flush_native_auth_ir()) {
        abort_active(XG_RENDER_AUTH_REJECT_TRANSACTION_FAILURE,
                     PSX_XG_RENDER_AUTH_REJECTION_SOURCE_RUNTIME_HOOK, true,
                     PSX_XG_RENDER_AUTH_HOOK_RETURN, return_site);
    } else {
        publish_completed_proof(tier);
        state.completed = true;
        end_gte_attribution_producer();
    }
}

static bool ui_draw_ot_observation(uint32_t hook, uint32_t pc,
                                   uint32_t instruction_word) {
    uint32_t target;

    if (hook != PSX_XG_RENDER_AUTH_HOOK_INTERNAL_OBSERVATION ||
        (pc != UINT32_C(0x800758C8) && pc != UINT32_C(0x800759CC)) ||
        (instruction_word >> 26u) != 3u)
        return false;
    target = UINT32_C(0x80000000) |
        ((instruction_word & UINT32_C(0x03ffffff)) << 2u);
    return target == UINT32_C(0x80044BD0);
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
        } else if (!flush_pre_scene_primitives()) {
            if (!producer_family.blocker)
                producer_family.blocker = 30u + pre_scene.blocker;
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
    if (state.requested_render_mode == GUEST_RENDER_RENDER_NATIVE) {
        if (hook == PSX_XG_RENDER_AUTH_HOOK_ENTRY &&
            physical_address_equals(pc, UINT32_C(0x80079784)))
            residual_capture_fullscreen_tile(cpu);
        else if (hook == PSX_XG_RENDER_AUTH_HOOK_ENTRY &&
                 physical_address_equals(pc, UINT32_C(0x8007da44)))
            residual_capture_fade_tiles(cpu);
        else if (hook == PSX_XG_RENDER_AUTH_HOOK_INTERNAL_OBSERVATION &&
                  physical_address_equals(pc, UINT32_C(0x801d09b0)) &&
                  instruction_word == UINT32_C(0x0c0129cf) &&
                  pending_variant_artifact_candidate_matches(pc))
            residual_capture_projected_gouraud(cpu);
    }
#ifndef PSX_XG_RENDER_AUTH_RUNTIME_TESTING
    if (ui_draw_ot_observation(hook, pc, instruction_word)) {
        ui_ot_pending = true;
        ui_ot_pending_frame = (uint32_t)s_frame_count;
    }
#endif
    if (hook == PSX_XG_RENDER_AUTH_HOOK_SOURCE_PRE) {
        (void)observe_source(cpu, tier, PSX_XG_RENDER_SOURCE_STAGE_PRE, pc,
                            instruction_word, delay_slot_word);
        return;
    }
    if (hook == PSX_XG_RENDER_AUTH_HOOK_SOURCE_COMMIT) {
        (void)observe_source(cpu, tier, PSX_XG_RENDER_SOURCE_STAGE_COMMIT, pc,
                            instruction_word, delay_slot_word);
        return;
    }
    if (source_inner_call_capture_matches(tier, hook, pc, instruction_word))
        return;
    const XgRenderRuntimeVariantEvent variant_event =
        xg_render_runtime_variant_observe(hook, pc, instruction_word,
                                           delay_slot_word, return_address,
                                           state.scene_generation);

    record_variant_progress(
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
        state.pending_variant_instruction = instruction_word;
        state.pending_variant_delay_slot = delay_slot_word;
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
    GuestRenderTimingMode requested_timing_mode,
    GuestRenderRenderMode requested_render_mode,
    PsxXgRenderPresentationGate presentation_gate,
    void *presentation_user_data) {
    const bool timing_valid =
        requested_timing_mode == GUEST_RENDER_TIMING_ORIGINAL ||
        requested_timing_mode == GUEST_RENDER_TIMING_NATIVE_59_94;
    const bool render_valid =
        requested_render_mode == GUEST_RENDER_RENDER_ORIGINAL ||
        requested_render_mode == GUEST_RENDER_RENDER_SHADOW ||
        requested_render_mode == GUEST_RENDER_RENDER_NATIVE;
    const bool preserve_producer_family = producer_family.enabled;

    if (state.configured) return false;
    state.requested_timing_mode = timing_valid
        ? requested_timing_mode : GUEST_RENDER_TIMING_ORIGINAL;
    state.requested_render_mode = render_valid
        ? requested_render_mode : GUEST_RENDER_RENDER_ORIGINAL;
    state.presentation_gate = presentation_gate;
    state.presentation_user_data = presentation_user_data;
    state.configured = true;
    if (state.interpolation_scene_generation == 0u)
        state.interpolation_scene_generation = 1u;
    gpu_native_interpolation_scene_boundary(interpolation_scene_generation());
    g_psx_xg_render_auth_cold_enabled =
        state.requested_render_mode != GUEST_RENDER_RENDER_ORIGINAL;
    psx_xg_render_auth_producer_family_enable(
        preserve_producer_family ||
        state.requested_render_mode != GUEST_RENDER_RENDER_ORIGINAL);
    guest_render_native_stream_set_miss_resolver(
        state.requested_render_mode == GUEST_RENDER_RENDER_NATIVE
            ? native_stream_resolve : NULL);
    guest_render_native_stream_set_shared_packet_bindings(
        xg_render_runtime_variant_no_gates_enabled());
    return true;
}

bool psx_xg_render_auth_configure_native_view(
    bool enabled, uint16_t aspect_num, uint16_t aspect_den,
    uint16_t canonical_width, uint16_t canonical_height) {
    return xg_native_view_configure(
        &native_view, enabled, aspect_num, aspect_den,
        canonical_width, canonical_height);
}

void psx_xg_render_auth_register_code_watches(
    void (*set_range)(uint32_t physical_address, uint32_t size)) {
    producer_resource_watch = set_range;
    xg_render_runtime_variant_register_code_watches(set_range);
}

void psx_xg_render_auth_set_exec_phase_exchange(
    PsxXgRenderExecPhaseExchange exchange) {
    exec_phase_exchange = exchange;
}

void psx_xg_render_auth_before_gpu_submission(void) {
    GuestRenderProducerSlot slot = { 0 };
    GuestRenderCompletedState completed = { 0 };
    GuestRenderBridgeSnapshot completed_snapshot = { 0 };
    bool failed = false;

    if (world_native_cutover_failed) {
        abort_standalone_submission();
        return;
    }
    if (!standalone_submission.open) {
        if (guest_render_bridge_last_completed(
                &completed_snapshot, &completed) == GUEST_RENDER_OK &&
            completed.binding_count != 0u &&
            guest_render_native_stream_has_staged_predecessor(
                (GpuRenderTransactionId){
                    completed.id.scene_epoch, completed.id.state_sequence,
                }))
            (void)guest_render_native_stream_activate_visual(
                (GpuRenderTransactionId){
                    completed.id.scene_epoch, completed.id.state_sequence,
                });
        return;
    }
    if (guest_render_bridge_producer_end(
            standalone_submission.producer, &slot) != GUEST_RENDER_OK ||
        !guest_render_bridge_id_equal(slot.handle.state_id,
                                      standalone_submission.visual_id)) {
        failed = true;
    } else if (guest_render_bridge_finalize_state(
                   standalone_submission.visual_id, &completed) !=
                   GUEST_RENDER_OK ||
               !guest_render_bridge_id_equal(
                   completed.id, standalone_submission.visual_id)) {
        failed = true;
    }
    if (failed) {
        world_native_cutover_failed = true;
        abort_standalone_submission();
        return;
    }
    if (completed.binding_count == 0u) {
        abort_standalone_submission();
        return;
    }
    if (guest_render_native_stream_enabled() &&
        guest_render_native_stream_activate_visual(
            (GpuRenderTransactionId){
                completed.id.scene_epoch, completed.id.state_sequence,
            }) != GUEST_RENDER_NATIVE_STREAM_OK) {
        world_native_cutover_failed = true;
        abort_standalone_submission();
        return;
    }
    clear_standalone_submission();
}

typedef struct XgUiOtCandidate {
    uint32_t command_address;
    GpuRenderSemantic semantic;
} XgUiOtCandidate;

static uint64_t ui_ot_hash_u32(uint64_t hash, uint32_t value) {
    for (uint32_t byte = 0u; byte < 4u; ++byte) {
        hash ^= (uint8_t)(value >> (byte * 8u));
        hash *= UINT64_C(1099511628211);
    }
    return hash;
}

static uint64_t ui_ot_hash_material(uint64_t hash,
                                    const GpuRenderMaterial *material) {
#define UI_OT_HASH_MATERIAL(field) \
    hash = ui_ot_hash_u32(hash, (uint32_t)material->field)
    UI_OT_HASH_MATERIAL(tpage);
    UI_OT_HASH_MATERIAL(texture_page_x);
    UI_OT_HASH_MATERIAL(texture_page_y);
    UI_OT_HASH_MATERIAL(clut_x);
    UI_OT_HASH_MATERIAL(clut_y);
    UI_OT_HASH_MATERIAL(draw_area_left);
    UI_OT_HASH_MATERIAL(draw_area_top);
    UI_OT_HASH_MATERIAL(draw_area_right);
    UI_OT_HASH_MATERIAL(draw_area_bottom);
    UI_OT_HASH_MATERIAL(draw_offset_x);
    UI_OT_HASH_MATERIAL(draw_offset_y);
    UI_OT_HASH_MATERIAL(texture_depth);
    UI_OT_HASH_MATERIAL(texture_window_mask_x);
    UI_OT_HASH_MATERIAL(texture_window_mask_y);
    UI_OT_HASH_MATERIAL(texture_window_offset_x);
    UI_OT_HASH_MATERIAL(texture_window_offset_y);
    UI_OT_HASH_MATERIAL(shading);
    UI_OT_HASH_MATERIAL(textured);
    UI_OT_HASH_MATERIAL(raw_texture);
    UI_OT_HASH_MATERIAL(semi_transparent);
    UI_OT_HASH_MATERIAL(blend_mode);
    UI_OT_HASH_MATERIAL(dither);
    UI_OT_HASH_MATERIAL(mask_set);
    UI_OT_HASH_MATERIAL(mask_check);
#undef UI_OT_HASH_MATERIAL
    return hash;
}

static uint64_t ui_ot_hash_semantic(uint64_t hash,
                                    const GpuRenderSemantic *semantic) {
    hash = ui_ot_hash_material(hash, &semantic->material);
    hash = ui_ot_hash_u32(hash, semantic->triangle_count);
    for (uint32_t triangle_index = 0u;
         triangle_index < semantic->triangle_count; ++triangle_index) {
        const GpuRenderSemanticTriangle *triangle =
            &semantic->triangles[triangle_index];
        hash = ui_ot_hash_u32(hash, triangle->split_index);
        hash = ui_ot_hash_u32(hash, triangle->split_count);
        for (uint32_t vertex_index = 0u; vertex_index < 3u; ++vertex_index) {
            const GpuRenderSemanticVertex *vertex =
                &triangle->vertices[vertex_index];
            hash = ui_ot_hash_u32(hash, (uint32_t)vertex->x);
            hash = ui_ot_hash_u32(hash, (uint32_t)vertex->y);
            hash = ui_ot_hash_u32(hash, (uint32_t)vertex->u);
            hash = ui_ot_hash_u32(hash, (uint32_t)vertex->v);
            hash = ui_ot_hash_u32(hash, vertex->r);
            hash = ui_ot_hash_u32(hash, vertex->g);
            hash = ui_ot_hash_u32(hash, vertex->b);
        }
    }
    return hash;
}

static uint64_t ui_ot_hash_environment(
        uint64_t hash, const GpuNativeDrawEnvironment *environment) {
    const GpuDrawState *draw = &environment->draw;

#define UI_OT_HASH_DRAW(field) \
    hash = ui_ot_hash_u32(hash, (uint32_t)draw->field)
    UI_OT_HASH_DRAW(left);
    UI_OT_HASH_DRAW(top);
    UI_OT_HASH_DRAW(right);
    UI_OT_HASH_DRAW(bottom);
    UI_OT_HASH_DRAW(offset_x);
    UI_OT_HASH_DRAW(offset_y);
    UI_OT_HASH_DRAW(texture_window_mask_x);
    UI_OT_HASH_DRAW(texture_window_mask_y);
    UI_OT_HASH_DRAW(texture_window_offset_x);
    UI_OT_HASH_DRAW(texture_window_offset_y);
    UI_OT_HASH_DRAW(dither);
    UI_OT_HASH_DRAW(mask_set);
    UI_OT_HASH_DRAW(mask_check);
#undef UI_OT_HASH_DRAW
    return ui_ot_hash_u32(hash, environment->tpage);
}

bool psx_xg_render_auth_prepare_ui_ot(uint32_t start_addr) {
#ifdef PSX_XG_RENDER_AUTH_RUNTIME_TESTING
    (void)start_addr;
    return true;
#else
    enum { UI_OT_MAX_NODES = 131072u, UI_OT_MAX_CANDIDATES = 4096u };
    XgUiOtCandidate *candidates;
    GpuNativeDrawEnvironment environment;
    uint32_t address = start_addr & UINT32_C(0x001ffffc);
    uint32_t nodes = 0u;
    size_t candidate_count = 0u;
    uint32_t prebound_count = 0u;
    uint32_t staged_count = 0u;
    uint64_t ot_digest = UINT64_C(1469598103934665603);
    uint64_t packet_digest = UINT64_C(1469598103934665603);
    uint64_t semantic_digest = UINT64_C(1469598103934665603);
    uint64_t environment_digest = UINT64_C(1469598103934665603);
    bool success = false;

    if (!ui_ot_pending || state.requested_render_mode != GUEST_RENDER_RENDER_NATIVE)
        return true;
    if ((uint32_t)s_frame_count != ui_ot_pending_frame) {
        ui_ot_pending = false;
        return true;
    }
    ++ui_ot_snapshot.prepare_count;
    ui_ot_snapshot.pending = true;
    ui_ot_snapshot.last_start_address = address;
    candidates = (XgUiOtCandidate *)calloc(UI_OT_MAX_CANDIDATES,
                                            sizeof(*candidates));
    if (!candidates) {
        ui_ot_snapshot.pending = false;
        ++ui_ot_snapshot.blocked_count;
        ui_ot_snapshot.blocked = true;
        return false;
    }
    gpu_native_environment_get(&environment);

    for (;;) {
        uint32_t header;
        uint32_t packet_words;
        uint32_t next;
        uint32_t word_address;
        uint32_t word_offset = 0u;

        if (nodes++ >= UI_OT_MAX_NODES) goto done;
        header = psx_read_word(address);
        packet_words = header >> 24u;
        next = header & UINT32_C(0x00ffffff);
        ot_digest = ui_ot_hash_u32(ot_digest, address);
        ot_digest = ui_ot_hash_u32(ot_digest, header);
        ot_digest = ui_ot_hash_u32(ot_digest, next);
        word_address = (address + 4u) & UINT32_C(0x001ffffc);
        while (word_offset < packet_words) {
            uint32_t words[GPU_GP0_RING_MAX_WORDS] = { 0 };
            uint32_t available = packet_words - word_offset;
            int command_words;
            uint8_t opcode;

            if (available > GPU_GP0_RING_MAX_WORDS)
                available = GPU_GP0_RING_MAX_WORDS;
            for (uint32_t index = 0u; index < available; ++index)
                words[index] = psx_read_word(
                    (word_address + index * 4u) & UINT32_C(0x001ffffc));
            opcode = (uint8_t)(words[0] >> 24u);
            command_words = gpu_gp0_command_word_count(opcode);
            if (command_words <= 0 ||
                (uint32_t)command_words > packet_words - word_offset)
                goto done;
            if (command_words <= GPU_GP0_RING_MAX_WORDS &&
                (uint32_t)command_words > available) {
                for (uint32_t index = available;
                     index < (uint32_t)command_words; ++index)
                    words[index] = psx_read_word(
                        (word_address + index * 4u) & UINT32_C(0x001ffffc));
            }
            environment_digest = ui_ot_hash_environment(environment_digest,
                                                         &environment);
            for (uint32_t index = 0u; index < (uint32_t)command_words; ++index) {
                ot_digest = ui_ot_hash_u32(ot_digest, words[index]);
            }
            if (opcode >= 0x20u && opcode <= 0x7fu) {
                packet_digest = ui_ot_hash_u32(packet_digest, word_address);
                packet_digest = ui_ot_hash_u32(packet_digest, opcode);
                for (uint32_t index = 0u; index < (uint32_t)command_words; ++index)
                    packet_digest = ui_ot_hash_u32(packet_digest, words[index]);
            }
            if (opcode >= 0x20u && opcode <= 0x7fu) {
                GpuRenderSemantic semantic;
                const int build = gpu_native_semantic_from_gp0(
                    words, command_words, &environment, &semantic);
                if (build != 1 || candidate_count == UI_OT_MAX_CANDIDATES)
                    goto done;
                candidates[candidate_count].command_address = word_address;
                candidates[candidate_count].semantic = semantic;
                semantic_digest = ui_ot_hash_semantic(semantic_digest, &semantic);
                ++candidate_count;
            }
            gpu_native_environment_apply(words, command_words, &environment);
            word_offset += (uint32_t)command_words;
            word_address = (word_address + (uint32_t)command_words * 4u) &
                           UINT32_C(0x001ffffc);
        }
        if (next == UINT32_C(0x00ffffff)) break;
        if ((next & 3u) != 0u || next > UINT32_C(0x001ffffc)) goto done;
        address = next;
    }

    {
        const GpuRenderTransactionId visual_id = {
            1u, ui_ot_visual_sequence++,
        };
        for (size_t index = 0u; index < candidate_count; ++index) {
            if (guest_render_native_stream_stage_exact(
                    visual_id, candidates[index].command_address,
                    &candidates[index].semantic) !=
                GUEST_RENDER_NATIVE_STREAM_OK)
                goto done;
            ++staged_count;
        }
        if (candidate_count != 0u &&
            guest_render_native_stream_activate_visual(visual_id) !=
                GUEST_RENDER_NATIVE_STREAM_OK)
            goto done;
    }
    success = true;
done:
    ui_ot_snapshot.node_count += nodes;
    ui_ot_snapshot.candidate_count += candidate_count;
    ui_ot_snapshot.prebound_count += prebound_count;
    ui_ot_snapshot.staged_count += staged_count;
    ui_ot_snapshot.last_node_count = nodes;
    ui_ot_snapshot.last_candidate_count = (uint32_t)candidate_count;
    ui_ot_snapshot.last_prebound_count = prebound_count;
    ui_ot_snapshot.last_staged_count = staged_count;
    ui_ot_snapshot.last_ot_digest = ot_digest;
    ui_ot_snapshot.last_packet_digest = packet_digest;
    ui_ot_snapshot.last_semantic_digest = semantic_digest;
    ui_ot_snapshot.last_environment_digest =
        ui_ot_hash_environment(environment_digest, &environment);
    ui_ot_snapshot.last_vram_serial = gpu_render_vram_mutation_serial();
    ui_ot_snapshot.pending = false;
    if (success) {
        ++ui_ot_snapshot.completed_count;
    } else {
        ++ui_ot_snapshot.blocked_count;
        ui_ot_snapshot.blocked = true;
    }
    free(candidates);
    return success;
#endif
}

void psx_xg_render_auth_ui_ot_snapshot(
        PsxXgRenderUiOtSnapshot *out_snapshot) {
    if (out_snapshot != NULL) *out_snapshot = ui_ot_snapshot;
}

void psx_xg_render_auth_cold_enable(bool enabled) {
    g_psx_xg_render_auth_cold_enabled = enabled;
    if (!enabled) ui_ot_pending = false;
    if (!enabled) {
        abort_standalone_submission();
        world_native_cutover_in_progress = false;
        world_native_cutover_failed = false;
        clear_particle_sources();
        clear_zoom_source();
        ++projected_lifecycle.disable_reset_count;
        clear_projected_source();
        clear_model_ft4_shadow_pending();
        clear_model_ft3_shadow_pending();
        clear_sprite_ft4_shadow_context();
        clear_field_sprite_builder();
        clear_field_sprite_templates();
        clear_f4_sources();
        clear_model_ft4_sources();
        clear_model_ft3_sources();
        clear_residual_templates();
        overlay_ft4_state.count = 0u;
        clear_field_polyline_pending();
        clear_resident_line_f2_source();
        clear_world_horizon_shadow_pending();
        clear_world_effects_shadow_pending();
        invalidate_world_semantic_shadows();
        invalidate_world_model_templates();
        xg_render_runtime_variant_reset();
    }
}

void psx_xg_render_auth_scene_boundary(void) {
    guest_render_native_stream_clear();
    ui_ot_pending = false;
    record_reset(true);
    if (source_state.pending.valid)
        block_source_proof(false, XG_RENDER_SOURCE_BLOCK_LIFECYCLE);
    reset_ft4_geometry(true);
    reset_render_transaction();
    if (xg_render_auth_process_owner(&state.auth) == XG_RENDER_AUTH_OK)
        (void)xg_render_auth_scene_reset(state.auth);
    clear_pre_scene();
    abort_standalone_submission();
    clear_particle_sources();
    clear_zoom_source();
    xg_field_projected_reset_pending();
    ++projected_lifecycle.pending_reset_count;
    clear_model_ft4_shadow_pending();
    clear_model_ft3_shadow_pending();
    clear_sprite_ft4_shadow_context();
    clear_field_sprite_builder();
    invalidate_nonresident_producer_resources();
    overlay_projected_2e_descriptor_scope = false;
    if (field_polyline.snapshot.pending)
        block_field_polyline(9u);
    else
        clear_field_polyline_pending();
    clear_resident_line_f2_source();
    if (world_horizon_shadow.snapshot.pending)
        block_world_horizon_shadow(98u);
    else
        clear_world_horizon_shadow_pending();
    if (world_effects_shadow.snapshot.pending)
        block_world_effects_shadow(104u);
    else
        clear_world_effects_shadow_pending();
    invalidate_world_semantic_shadows();
    end_gte_attribution_producer();
    state.active = false;
    state.armed = true;
    state.completed = false;
    state.authenticated_producer_entry = 0u;
    state.authenticated_producer_scene_generation = 0u;
    state.rejection = (PsxXgRenderAuthRejectionReceipt){ 0 };
    world_native_cutover_in_progress = false;
    if (state.scene_generation != UINT64_MAX) {
        ++state.scene_generation;
        world_native_cutover_failed = false;
    } else {
        world_native_cutover_failed = true;
    }
    clear_pending_candidate();
    clear_pending_variant_sequence();
    clear_candidate_outcome();
    xg_render_runtime_variant_reset();
}

bool psx_xg_render_auth_cold_hook_relevant(uint32_t hook, uint32_t pc,
                                           uint32_t instruction_word) {
    bool canonical = false;

    if (!g_psx_xg_render_auth_cold_enabled) return false;
    if (hook == PSX_XG_RENDER_AUTH_HOOK_ENTRY)
        canonical = physical_address_equals(
            pc, xg_render_manifest_validation.producer_entry) &&
            instruction_word == XG_RENDER_CANONICAL_ENTRY_INSTRUCTION &&
            authenticated_variant_hook_matches(pc);
    else if (hook == PSX_XG_RENDER_AUTH_HOOK_CAPTURE) {
        canonical = physical_address_equals(
            pc, xg_render_manifest_validation.caller_site) &&
            instruction_word == XG_RENDER_CANONICAL_CAPTURE_INSTRUCTION &&
            authenticated_variant_hook_matches(pc);
        /* CAPTURE aliases INTERNAL_OBSERVATION. The cold overlay path
         * executes these calls through dirty-RAM interpretation, so retain
         * only the exact DrawOTag observation sites here. */
        canonical = canonical || ui_draw_ot_observation(
            hook, pc, instruction_word);
    } else if (hook == PSX_XG_RENDER_AUTH_HOOK_RETURN)
        canonical = physical_address_equals(
            pc, xg_render_manifest_validation.return_site) &&
            state.active &&
            authenticated_variant_hook_matches(pc);
    return canonical || xg_render_runtime_variant_hook_relevant(hook, pc);
}

void psx_xg_render_auth_cold_hook(CPUState *cpu, uint32_t hook, uint32_t pc,
                                  uint32_t instruction_word,
                                  uint32_t delay_slot_word) {
    if (!g_psx_xg_render_auth_cold_enabled) return;
    lock_instrumentation();
    ++instrumentation.cold_hook_ingress_count;
    unlock_instrumentation();
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
    return xg_render_runtime_variant_source_site_lookup(
        pc, instruction_word, out_metadata);
}

bool psx_xg_render_auth_cold_source_pc_relevant(uint32_t pc) {
    return g_psx_xg_render_auth_cold_enabled &&
           !source_state.aggregate.blocked &&
           authenticated_variant_hook_matches(pc) &&
           xg_render_runtime_variant_source_pc_relevant(pc);
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
    return observe_source(NULL, XG_RENDER_AUTH_TIER_COLD_INTERPRETER, stage, pc,
                          instruction_word, auxiliary);
}

bool psx_xg_render_auth_cold_source_observe_cpu(
    CPUState *cpu, PsxXgRenderSourceStage stage, uint32_t pc,
    uint32_t instruction_word, uint32_t auxiliary) {
    if (!g_psx_xg_render_auth_cold_enabled) return false;
    return observe_source(cpu, XG_RENDER_AUTH_TIER_COLD_INTERPRETER, stage, pc,
                          instruction_word, auxiliary);
}

void psx_xg_render_auth_source_snapshot(
    PsxXgRenderSourceSnapshot *out_snapshot) {
    if (out_snapshot != NULL) *out_snapshot = source_state.aggregate;
}

void psx_xg_render_auth_source_collector_snapshot(
    FieldCharacterShadowSummary *out_summary) {
    if (out_summary != NULL)
        (void)field_character_shadow_snapshot(&source_state.collector,
                                              out_summary);
}

void psx_xg_render_auth_source_reset(void) {
    source_state = (XgRenderSourceState){
        .aggregate = { .next_sequence = 1u },
        .next_auth_sequence = 1u,
    };
    field_character_shadow_init(&source_state.collector);
    reset_ft4_geometry(false);
    clear_pre_scene();
    producer_family = (PsxXgRenderProducerFamilySnapshot){ 0 };
    xg_field_zoom_counters_reset();
}

void psx_xg_render_auth_ft4_geometry_enable(bool enabled) {
    reset_ft4_geometry(false);
    ft4_geometry.enabled = enabled;
}

static bool ft4_context_matches(const CPUState *cpu) {
    return cpu != NULL && ft4_geometry.pending.valid &&
           !ft4_geometry.blocked &&
           ft4_geometry.pending.scene_generation == state.scene_generation &&
           source_context_matches(ft4_geometry.pending.tier) &&
           physical_address_equals(
               cpu->gpr[31], ft4_geometry.pending.source_return_address);
}

static bool ft4_destinations_are_valid(const uint32_t destinations[4]) {
    return (destinations[0] & 3u) == 0u &&
           destinations[1] == destinations[0] + 8u &&
           destinations[2] == destinations[0] + 16u &&
           destinations[3] == destinations[0] + 24u &&
           destinations[0] >= 8u;
}

static bool native_compass_cutover(CPUState *cpu, uint32_t pc,
                                   bool screen_aligned) {
    XgHost3dRotAverage4Input input = { 0 };
    XgHost3dRotAverage4Output output;
    XgHost3dProjectedVertex render_vertices[4];
    XgFieldCharacterCapture capture = { 0 };
    XgFieldCharacterCandidate candidate;
    XgRenderIrNativePrimitive primitive;
    GpuDrawState draw = { 0 };
    uint32_t source_address;
    uint32_t matrix_address;
    uint32_t packet_address;
    uint32_t ot_address;
    uint32_t packet_tag;
    uint32_t previous_head;
    uint32_t word;
    uint32_t index;

    if (state.requested_render_mode != GUEST_RENDER_RENDER_NATIVE)
        return false;
    if (!xg_render_runtime_variant_no_gates_enabled() &&
        !state.authenticated_artifact_candidate_valid)
        return false;
    if (!pending_variant_artifact_candidate_matches(pc)) {
        pre_scene.blocker = 8u;
        pre_scene.blocked = true;
        return false;
    }
    if (cpu == NULL || cpu->read_word == NULL || cpu->write_word == NULL ||
        cpu->read_half == NULL || cpu->read_byte == NULL) {
        pre_scene.blocker = 9u;
        pre_scene.blocked = true;
        return false;
    }
    if (pre_scene.blocked) return false;
    if (pre_scene.count == XG_RENDER_PRE_SCENE_PRIMITIVE_CAPACITY) {
        pre_scene.blocker = 10u;
        pre_scene.blocked = true;
        return false;
    }
    source_address = cpu->gpr[5];
    matrix_address = cpu->gpr[6];
    if (cpu->gpr[7] > 1u || source_address > UINT32_MAX - 0x68u ||
        matrix_address > UINT32_MAX - 0x20u || cpu->gpr[4] > UINT32_MAX - 4u) {
        pre_scene.blocker = 2u;
        goto fail;
    }
    packet_address = source_address + cpu->gpr[7] * 0x28u + 0x20u;
    ot_address = cpu->gpr[4] + 4u;
    if (!word_address_is_valid(packet_address) ||
        !word_address_is_valid(ot_address)) {
        pre_scene.blocker = 3u;
        goto fail;
    }

    for (index = 0u; index < 4u; ++index) {
        word = cpu->read_word(source_address + index * 8u);
        input.vertices[index].x = low_s16(word);
        input.vertices[index].y = low_s16(word >> 16u);
        word = cpu->read_word(source_address + index * 8u + 4u);
        input.vertices[index].z = low_s16(word);
        input.vertices[index].pad = (uint16_t)(word >> 16u);
    }
    for (index = 0u; index < 4u; ++index) {
        int16_t *rotation = &input.projection.rotation[0][0];
        word = cpu->read_word(matrix_address + index * 4u);
        rotation[index * 2u] = low_s16(word);
        rotation[index * 2u + 1u] = low_s16(word >> 16u);
    }
    word = cpu->read_word(matrix_address + 16u);
    input.projection.rotation[2][2] = low_s16(word);
    for (index = 0u; index < 3u; ++index)
        input.projection.translation[index] =
            (int32_t)cpu->read_word(matrix_address + 20u + index * 4u);
    input.projection.screen_offset_x = INT32_C(0x010a0000);
    input.projection.screen_offset_y = INT32_C(0x00a60000);
    input.projection.projection_distance = 0x80u;
    input.projection.average_z_scale4 = 0x100;
    if (!xg_host_3d_rot_average4(&input, &output) ||
        !xg_field_compass_capture_material(cpu, source_address, &capture)) {
        pre_scene.blocker = 4u;
        goto fail;
    }
    memcpy(render_vertices, output.vertices, sizeof(render_vertices));
    for (index = 0u; index < 4u; ++index) {
        capture.vertices[index].x = output.vertices[index].x;
        capture.vertices[index].y = output.vertices[index].y;
    }
    if (screen_aligned) {
        const int32_t sum = (int32_t)output.vertices[2].x +
                            (int32_t)output.vertices[3].x;
        const int16_t center = (int16_t)(sum / 2);
        const int16_t top = output.vertices[3].y;

        capture.vertices[0].x = center - 8;
        capture.vertices[1].x = center + 8;
        capture.vertices[2].x = center - 8;
        capture.vertices[3].x = center + 8;
        capture.vertices[0].y = top - 10;
        capture.vertices[1].y = top - 10;
        capture.vertices[2].y = top;
        capture.vertices[3].y = top;
        for (index = 0u; index < 4u; ++index) {
            render_vertices[index].x = capture.vertices[index].x;
            render_vertices[index].y = capture.vertices[index].y;
        }
        if (output.vertices[0].native_view_position &&
            output.vertices[1].native_view_position &&
            output.vertices[2].native_view_position &&
            output.vertices[3].native_view_position) {
            const int64_t native_center_x =
                ((int64_t)output.vertices[2].native_view_x_16_16 +
                 output.vertices[3].native_view_x_16_16) / 2;
            const int64_t native_left_x =
                native_center_x - 8 * INT32_C(65536);
            const int64_t native_right_x =
                native_center_x + 8 * INT32_C(65536);
            const int64_t native_top_y =
                (int64_t)output.vertices[3].native_view_y_16_16 -
                10 * INT32_C(65536);

            if (native_left_x >= INT32_MIN && native_left_x <= INT32_MAX &&
                native_right_x >= INT32_MIN && native_right_x <= INT32_MAX &&
                native_top_y >= INT32_MIN && native_top_y <= INT32_MAX) {
                render_vertices[0].native_view_x_16_16 =
                    render_vertices[2].native_view_x_16_16 =
                        (int32_t)native_left_x;
                render_vertices[1].native_view_x_16_16 =
                    render_vertices[3].native_view_x_16_16 =
                        (int32_t)native_right_x;
                render_vertices[0].native_view_y_16_16 =
                    render_vertices[1].native_view_y_16_16 =
                        (int32_t)native_top_y;
                render_vertices[2].native_view_y_16_16 =
                    render_vertices[3].native_view_y_16_16 =
                        output.vertices[3].native_view_y_16_16;
            } else {
                for (index = 0u; index < 4u; ++index)
                    render_vertices[index].native_view_position = 0u;
            }
        } else {
            for (index = 0u; index < 4u; ++index)
                render_vertices[index].native_view_position = 0u;
        }
    }
    gpu_get_draw_state(&draw);
    capture.draw_area_left = draw.left;
    capture.draw_area_top = draw.top;
    capture.draw_area_right = draw.right;
    capture.draw_area_bottom = draw.bottom;
    capture.draw_offset_x = draw.offset_x;
    capture.draw_offset_y = draw.offset_y;
    capture.texture_window_mask_x = draw.texture_window_mask_x;
    capture.texture_window_mask_y = draw.texture_window_mask_y;
    capture.texture_window_offset_x = draw.texture_window_offset_x;
    capture.texture_window_offset_y = draw.texture_window_offset_y;
    capture.dither = draw.dither;
    capture.mask_set = draw.mask_set;
    capture.mask_check = draw.mask_check;
    if (xg_field_character_adapter_build(&capture, &candidate) !=
            XG_FIELD_CHARACTER_ADAPTER_OK ||
        xg_field_character_adapter_build_primitive(&candidate, &primitive) !=
            XG_FIELD_CHARACTER_ADAPTER_OK) {
        pre_scene.blocker = 5u;
        goto fail;
    }
    apply_projected_quad_positions(&primitive, render_vertices);

    packet_tag = cpu->read_word(packet_address);
    previous_head = cpu->read_word(ot_address);
    for (index = 0u; index < 4u; ++index) {
        const uint32_t xy = (uint16_t)capture.vertices[index].x |
            ((uint32_t)(uint16_t)capture.vertices[index].y << 16u);
        psx_store_cycle_barrier();
        cpu->write_word(packet_address + 8u + index * 8u, xy);
    }
    psx_store_cycle_barrier();
    cpu->write_word(packet_address,
                    (packet_tag & UINT32_C(0xff000000)) |
                    (previous_head & UINT32_C(0x00ffffff)));
    psx_store_cycle_barrier();
    cpu->write_word(ot_address,
                    (previous_head & UINT32_C(0xff000000)) |
                    (packet_address & UINT32_C(0x00ffffff)));
    if (!stage_pre_scene_primitive(&(XgRenderPreScenePrimitive){
        .primitive = primitive,
        .packet_address = packet_address,
        .source_primitive_index =
            UINT32_C(0x10000000) | (packet_address & UINT32_C(0x001ffffc)),
        .ot_bucket = 1u,
        .interpolation_producer_id = pc,
        .interpolation_primitive_id =
            source_address & UINT32_C(0x1fffffff),
        .payload_word_count = 9u,
        .interpolation_identity_valid = true,
    })) {
        pre_scene.blocker = 10u;
        goto fail;
    }
    cpu->pc = cpu->gpr[31];
    return true;

fail:
    pre_scene.blocked = true;
    return false;
}

static int32_t particle_position_component(uint32_t value) {
    const int32_t signed_value = (int32_t)value;

    if (signed_value >= 0) return signed_value / 4096;
    return -(int32_t)(((uint64_t)(-(int64_t)signed_value) + 4095u) / 4096u);
}

static bool capture_particle_matrix(CPUState *cpu, uint32_t address,
                                    XgHost3dMatrix *matrix) {
    uint32_t word;

    if (cpu == NULL || matrix == NULL || cpu->read_word == NULL ||
        address > UINT32_MAX - 28u || !word_address_is_valid(address) ||
        !word_address_is_valid(address + 28u))
        return false;
    word = cpu->read_word(address);
    matrix->rotation[0][0] = low_s16(word);
    matrix->rotation[0][1] = low_s16(word >> 16u);
    word = cpu->read_word(address + 4u);
    matrix->rotation[0][2] = low_s16(word);
    matrix->rotation[1][0] = low_s16(word >> 16u);
    word = cpu->read_word(address + 8u);
    matrix->rotation[1][1] = low_s16(word);
    matrix->rotation[1][2] = low_s16(word >> 16u);
    word = cpu->read_word(address + 12u);
    matrix->rotation[2][0] = low_s16(word);
    matrix->rotation[2][1] = low_s16(word >> 16u);
    word = cpu->read_word(address + 16u);
    matrix->rotation[2][2] = low_s16(word);
    matrix->pad = (uint16_t)(word >> 16u);
    matrix->translation[0] = (int32_t)cpu->read_word(address + 20u);
    matrix->translation[1] = (int32_t)cpu->read_word(address + 24u);
    matrix->translation[2] = (int32_t)cpu->read_word(address + 28u);
    return true;
}

bool xg_render_runtime_capture_matrix(
    CPUState *cpu, uint32_t address, XgHost3dMatrix *matrix) {
    return capture_particle_matrix(cpu, address, matrix);
}

static void store_matrix_rotation(CPUState *cpu, uint32_t address,
                                  const XgHost3dMatrix *matrix) {
    const uint32_t words[5] = {
        (uint16_t)matrix->rotation[0][0] |
            ((uint32_t)(uint16_t)matrix->rotation[0][1] << 16u),
        (uint16_t)matrix->rotation[0][2] |
            ((uint32_t)(uint16_t)matrix->rotation[1][0] << 16u),
        (uint16_t)matrix->rotation[1][1] |
            ((uint32_t)(uint16_t)matrix->rotation[1][2] << 16u),
        (uint16_t)matrix->rotation[2][0] |
            ((uint32_t)(uint16_t)matrix->rotation[2][1] << 16u),
        (uint16_t)matrix->rotation[2][2] |
            ((uint32_t)matrix->pad << 16u),
    };
    uint32_t index;

    for (index = 0u; index < 5u; ++index) {
        psx_store_cycle_barrier();
        cpu->write_word(address + index * 4u, words[index]);
    }
}

void xg_render_runtime_store_matrix_rotation(
    CPUState *cpu, uint32_t address, const XgHost3dMatrix *matrix) {
    store_matrix_rotation(cpu, address, matrix);
}

static bool stage_active_native_primitive(
    const XgRenderIrNativePrimitive *primitive, uint32_t packet_address,
    uint32_t source_primitive_index, uint32_t ot_bucket,
    uint8_t payload_word_count, uint32_t interpolation_producer_id,
    uint32_t interpolation_primitive_id, uint32_t *failure_blocker) {
    XgRenderAuthSnapshot auth_snapshot = { 0 };
    GuestRenderBridgeSnapshot bridge = { 0 };
    GpuRenderSemantic semantic;
    GpuRenderTransactionId visual_id;
    if (failure_blocker != NULL) *failure_blocker = 0u;
    if (primitive == NULL || state.auth == NULL || !state.active ||
        state.completed) {
        if (failure_blocker != NULL) *failure_blocker = 58u;
        return false;
    }
    if (guest_render_bridge_snapshot(&bridge) != GUEST_RENDER_OK) {
        if (failure_blocker != NULL) *failure_blocker = 59u;
        return false;
    }
    if (bridge.modes.effective_render_mode != GUEST_RENDER_RENDER_NATIVE) {
        if (failure_blocker != NULL) *failure_blocker = 60u;
        return false;
    }
    if (xg_render_auth_snapshot(state.auth, &auth_snapshot) !=
            XG_RENDER_AUTH_OK) {
        if (failure_blocker != NULL) *failure_blocker = 61u;
        return false;
    }
    if (xg_render_backend_translate_primitive(primitive, &semantic) !=
            XG_RENDER_BACKEND_OK) {
        if (failure_blocker != NULL) *failure_blocker = 62u;
        return false;
    }
    set_semantic_interpolation_identity(
        &semantic, interpolation_scene_generation(), interpolation_producer_id,
        interpolation_primitive_id);
    visual_id = (GpuRenderTransactionId){
        auth_snapshot.logical_identity.state_id.scene_epoch,
        auth_snapshot.logical_identity.state_id.state_sequence,
    };
    if (xg_render_auth_append_native_insertion(
            state.auth, packet_address & UINT32_C(0x001ffffc),
            source_primitive_index, ot_bucket,
             payload_word_count, primitive,
             state.pending_variant_sequence &&
                !state.pending_variant_capture_ready) != XG_RENDER_AUTH_OK) {
        if (failure_blocker != NULL) *failure_blocker = 63u;
        return false;
    }
    if (stage_render_semantic_exact(
            visual_id,
            (packet_address & UINT32_C(0x001ffffc)) + 4u,
            &semantic) != GUEST_RENDER_TRANSACTION_OK) {
        if (failure_blocker != NULL) *failure_blocker = 64u;
        return false;
    }
    return true;
}

static bool begin_standalone_submission(void) {
    const GuestRenderSceneConfig config = {
        state.requested_timing_mode,
        state.requested_render_mode,
    };
    const GuestRenderProducerProvenance provenance = {
        GUEST_RENDER_PRODUCER_NATIVE,
        { 0 },
    };
    GuestRenderBridgeSnapshot bridge = { 0 };
    GuestRenderTransactionPendingSnapshot pending = { 0 };
    GuestRenderTransactionSnapshot transaction = { 0 };

    if (standalone_submission.open) return true;
    if (state.requested_render_mode != GUEST_RENDER_RENDER_NATIVE ||
        guest_render_transaction_pending_snapshot(&pending) !=
            GUEST_RENDER_TRANSACTION_OK ||
        pending.binding_count != 0u ||
        guest_render_transaction_snapshot(&transaction) !=
            GUEST_RENDER_TRANSACTION_OK ||
        transaction.phase == GUEST_RENDER_TRANSACTION_ACTIVE ||
        guest_render_bridge_begin_scene(&config) != GUEST_RENDER_OK)
        return false;
    if (state.presentation_gate != NULL) {
        if (!state.presentation_gate(state.requested_render_mode,
                                     &state.presentation,
                                     state.presentation_user_data))
            guest_render_bridge_force_original(
                GUEST_RENDER_FALLBACK_PRESENTATION_GATE);
    } else {
        guest_render_bridge_force_original(
            GUEST_RENDER_FALLBACK_PRESENTATION_GATE);
    }
    if (guest_render_bridge_snapshot(&bridge) != GUEST_RENDER_OK ||
        bridge.modes.effective_render_mode != GUEST_RENDER_RENDER_NATIVE ||
        guest_render_bridge_begin_state(&standalone_submission.visual_id) !=
            GUEST_RENDER_OK ||
        guest_render_bridge_producer_begin(
            standalone_submission.visual_id, &provenance,
            &standalone_submission.producer) != GUEST_RENDER_OK) {
        guest_render_bridge_abort_scene(GUEST_RENDER_FALLBACK_FORCED_ORIGINAL);
        clear_standalone_submission();
        return false;
    }
    standalone_submission.open = true;
    return true;
}

static bool stage_standalone_native_semantic(
    const GpuRenderSemantic *semantic, uint32_t packet_address,
    uint32_t source_primitive_index) {
    GuestRenderStatus bind_status;
    GuestRenderTransactionStatus stage_status;

    standalone_stage_failure_detail = 0u;
    if (semantic == NULL) {
        standalone_stage_failure_detail = 1u;
        return false;
    }
    if (!begin_standalone_submission()) {
        standalone_stage_failure_detail = 2u;
        return false;
    }
    bind_status = guest_render_bridge_bind_packet(
        standalone_submission.producer,
        packet_address & UINT32_C(0x001ffffc), source_primitive_index);
    if (bind_status != GUEST_RENDER_OK) {
        standalone_stage_failure_detail = 100u + (uint32_t)bind_status;
        abort_standalone_submission();
        return false;
    }
    stage_status = stage_render_semantic_exact(
        (GpuRenderTransactionId){
            standalone_submission.visual_id.scene_epoch,
            standalone_submission.visual_id.state_sequence,
        },
        (packet_address & UINT32_C(0x001ffffc)) + 4u,
        semantic);
    if (stage_status != GUEST_RENDER_TRANSACTION_OK) {
        standalone_stage_failure_detail = 200u + (uint32_t)stage_status;
        abort_standalone_submission();
        return false;
    }
    return true;
}

static bool stage_standalone_native_semantic_identified(
    const GpuRenderSemantic *semantic, uint32_t packet_address,
    uint32_t source_primitive_index, uint32_t interpolation_producer_id,
    uint32_t interpolation_primitive_id) {
    GpuRenderSemantic identified;

    if (semantic == NULL) return false;
    identified = *semantic;
    set_semantic_interpolation_identity(
        &identified, interpolation_scene_generation(), interpolation_producer_id,
        interpolation_primitive_id);
    return stage_standalone_native_semantic(
        &identified, packet_address, source_primitive_index);
}

static void set_semantic_corner_identities(
        GpuRenderSemantic *semantic, uint32_t producer_id,
        uint32_t primitive_id) {
    static const uint8_t quad_corners[2][3] = {{0u, 1u, 2u}, {2u, 1u, 3u}};

    if (semantic == NULL || semantic->topology != GPU_RENDER_SEMANTIC_TRIANGLES ||
        semantic->triangle_count == 0u || primitive_id > UINT32_MAX / 4u)
        return;
    for (uint32_t triangle = 0u; triangle < semantic->triangle_count;
         ++triangle) {
        for (uint32_t vertex = 0u; vertex < 3u; ++vertex) {
            GpuRenderSemanticVertex *target =
                &semantic->triangles[triangle].vertices[vertex];
            const uint32_t corner = semantic->triangle_count == 2u &&
                    triangle < 2u
                ? quad_corners[triangle][vertex] : vertex;

            if (target->interpolation_vertex_identity_valid) continue;
            target->interpolation_group_id = producer_id;
            target->interpolation_vertex_id = primitive_id * 4u + corner;
            target->interpolation_vertex_identity_valid = 1u;
        }
    }
}

static bool stage_standalone_native_primitive_identified(
    const XgRenderIrNativePrimitive *primitive, uint32_t packet_address,
    uint32_t source_primitive_index, uint32_t interpolation_producer_id,
    uint32_t interpolation_primitive_id) {
    GpuRenderSemantic semantic;
    XgRenderBackendStatus translate_status;

    if (primitive == NULL) {
        standalone_stage_failure_detail = 1u;
        return false;
    }
    translate_status = xg_render_backend_translate_primitive(
        primitive, &semantic);
    if (translate_status != XG_RENDER_BACKEND_OK) {
        standalone_stage_failure_detail = 300u +
            (uint32_t)translate_status;
        return false;
    }
    set_semantic_corner_identities(
        &semantic, interpolation_producer_id, interpolation_primitive_id);
    return stage_standalone_native_semantic_identified(
        &semantic, packet_address, source_primitive_index,
        interpolation_producer_id, interpolation_primitive_id);
}

static bool stage_standalone_native_primitive(
    const XgRenderIrNativePrimitive *primitive, uint32_t packet_address,
    uint32_t source_primitive_index) {
    return stage_standalone_native_primitive_identified(
        primitive, packet_address, source_primitive_index, 0u, 0u);
}

static bool stage_temporal_native_primitive_identified(
        const XgRenderIrNativePrimitive *primitive,
        uint32_t interpolation_producer_id,
        uint32_t interpolation_primitive_id,
        const GpuRenderTemporalCullPolicy *policy) {
    GpuRenderSemantic semantic;

    if (primitive == NULL || policy == NULL ||
        xg_render_backend_translate_primitive(primitive, &semantic) !=
            XG_RENDER_BACKEND_OK)
        return false;
    set_semantic_interpolation_identity(
        &semantic, interpolation_scene_generation(), interpolation_producer_id,
        interpolation_primitive_id);
    set_semantic_corner_identities(
        &semantic, interpolation_producer_id, interpolation_primitive_id);
    return gr_draw_semantic_temporal_candidate(&semantic, policy) ==
        GPU_RENDER_TRANSACTION_OK;
}

static bool native_primitive_all_projective(
        const XgRenderIrNativePrimitive *primitive) {
    if (primitive == NULL || primitive->triangle_count == 0u) return false;
    for (uint32_t triangle = 0u; triangle < primitive->triangle_count;
         ++triangle)
        for (uint32_t vertex = 0u; vertex < 3u; ++vertex)
            if (!primitive->triangles[triangle].vertices[vertex]
                    .projective_position)
                return false;
    return true;
}

static bool stage_active_particle(
    const XgRenderIrNativePrimitive *primitive, uint32_t packet_address,
    uint32_t ot_bucket, uint32_t source_generation, uint32_t producer_pc) {
#ifdef PSX_XG_RENDER_AUTH_RUNTIME_TESTING
    if (primitive != NULL) {
        particle_test_primitive = *primitive;
        particle_test_primitive_valid = true;
    }
#endif
    return stage_active_native_primitive(
        primitive, packet_address,
        UINT32_C(0x20000000) |
            (packet_address & UINT32_C(0x001ffffc)),
        ot_bucket, XG_FIELD_CHARACTER_PACKET_WORD_COUNT,
        producer_pc, source_generation, NULL);
}

static bool native_particle_cutover(CPUState *cpu, uint32_t pc) {
    GuestRenderBridgeSnapshot bridge = { 0 };
    XgRenderParticleSource *source;
    XgHost3dMatrix matrix;
    XgHost3dRotAverage4Input input = { 0 };
    XgHost3dRotAverage4Output output;
    XgFieldCharacterCapture capture = { 0 };
    XgFieldCharacterCandidate candidate;
    XgRenderIrNativePrimitive primitive;
    GpuDrawState draw = { 0 };
    uint32_t particle_address;
    uint32_t stack_pointer;
    uint32_t extra_scale_address;
    uint32_t composition_selector;
    uint32_t buffer_index;
    uint32_t packet_address;
    uint32_t packet_tag;
    uint32_t shifted_depth;
    uint32_t ordering_shift;
    uint32_t bucket;
    uint32_t ot_base;
    uint32_t ot_address;
    uint32_t previous_head;
    uint32_t new_ot_word;
    uint32_t index;
    int64_t temporal_depth;

    if (state.requested_render_mode != GUEST_RENDER_RENDER_NATIVE) return false;
    if (guest_render_bridge_snapshot(&bridge) != GUEST_RENDER_OK)
        goto fail_context;
    if (bridge.modes.effective_render_mode != GUEST_RENDER_RENDER_NATIVE)
        return false;
    if (!pending_variant_artifact_candidate_matches(pc) || cpu == NULL ||
        cpu->read_word == NULL || cpu->write_word == NULL ||
        cpu->read_half == NULL || cpu->read_byte == NULL ||
        cpu->write_byte == NULL || !word_address_is_valid(cpu->gpr[4]) ||
        !stack_address_is_valid(cpu->gpr[29]))
        goto fail_context;
    particle_address = guest_address(cpu->gpr[4]);
    source = find_particle_source(particle_address);
    stack_pointer = cpu->gpr[29];
    extra_scale_address = cpu->read_word(stack_pointer + 0x10u);
    composition_selector = cpu->read_word(stack_pointer + 0x14u);
    if (xg_field_particles_state()->blocked || source == NULL ||
        cpu->gpr[7] >= 4u ||
        source->generation == 0u || source->generation > UINT32_MAX ||
        source->command != 0x2eu || source->payload_word_count != 9u ||
        !source->semi_transparent ||
        !xg_field_particles_source_matches_memory(cpu, source) ||
        !xg_field_particles_build_matrix(cpu, particle_address, cpu->gpr[5],
                                         extra_scale_address,
                                         composition_selector, &matrix))
        goto fail_source;
    for (index = 0u; index < 4u; ++index) {
        input.vertices[index] = (XgHost3dVector){
            source->x[index], source->y[index], 0, 0u,
        };
    }
    capture_shadow_projection(cpu, &input.projection);
    memcpy(input.projection.rotation, matrix.rotation,
           sizeof(input.projection.rotation));
    memcpy(input.projection.translation, matrix.translation,
           sizeof(input.projection.translation));
    if (!xg_host_3d_rot_average4(&input, &output)) goto fail_math;
    buffer_index = cpu->read_word(UINT32_C(0x800adb08));
    if (buffer_index > 1u || particle_address > UINT32_MAX - 0xa0u)
        goto fail_packet;
    packet_address = particle_address + 0x50u + buffer_index * 0x28u;
    if (!word_address_is_valid(packet_address) ||
        !word_address_is_valid(packet_address + 0x24u))
        goto fail_packet;
    packet_tag = cpu->read_word(packet_address);
    if ((packet_tag >> 24u) != source->payload_word_count)
        goto fail_packet;
    for (index = 0u; index < 4u; ++index) {
        capture.vertices[index] = (XgFieldCharacterCaptureVertex){
            output.vertices[index].x, output.vertices[index].y,
            source->u[index], source->v[index],
        };
    }
    capture.red = cpu->read_byte(particle_address + 0x48u);
    capture.green = cpu->read_byte(particle_address + 0x49u);
    capture.blue = cpu->read_byte(particle_address + 0x4au);
    capture.tpage = source->tpage;
    capture.clut_x = source->clut_x;
    capture.clut_y = source->clut_y;
    capture.semi_transparent = source->semi_transparent;
    gpu_get_draw_state(&draw);
    capture.draw_area_left = draw.left;
    capture.draw_area_top = draw.top;
    capture.draw_area_right = draw.right;
    capture.draw_area_bottom = draw.bottom;
    capture.draw_offset_x = draw.offset_x;
    capture.draw_offset_y = draw.offset_y;
    capture.texture_window_mask_x = draw.texture_window_mask_x;
    capture.texture_window_mask_y = draw.texture_window_mask_y;
    capture.texture_window_offset_x = draw.texture_window_offset_x;
    capture.texture_window_offset_y = draw.texture_window_offset_y;
    capture.dither = draw.dither;
    capture.mask_set = draw.mask_set;
    capture.mask_check = draw.mask_check;
    if (xg_field_character_adapter_build(&capture, &candidate) !=
            XG_FIELD_CHARACTER_ADAPTER_OK ||
        xg_field_character_adapter_build_primitive(&candidate, &primitive) !=
            XG_FIELD_CHARACTER_ADAPTER_OK)
        goto fail_material;
    apply_projected_quad_positions(&primitive, output.vertices);

    ordering_shift = cpu->read_word(UINT32_C(0x80050100)) & 31u;
    shifted_depth = (uint32_t)output.ordering_depth >> ordering_shift;
    switch (cpu->gpr[7]) {
    case 0u: bucket = 1u; break;
    case 1u: bucket = shifted_depth - 16u; break;
    case 2u: bucket = shifted_depth; break;
    case 3u: bucket = shifted_depth + 16u; break;
    default: goto fail_source;
    }
    temporal_depth = (int64_t)output.ordering_depth +
        (cpu->gpr[7] == 1u ? -((int64_t)16 << ordering_shift) :
         (cpu->gpr[7] == 3u ? ((int64_t)16 << ordering_shift) : 0));
    for (uint32_t triangle = 0u; triangle < primitive.triangle_count;
         ++triangle) {
        for (uint32_t vertex = 0u; vertex < 3u; ++vertex) {
            XgRenderIrVertex *target =
                &primitive.triangles[triangle].vertices[vertex];

            target->temporal_depth = temporal_depth < INT32_MIN
                ? INT32_MIN
                : (temporal_depth > INT32_MAX ? INT32_MAX
                                              : (int32_t)temporal_depth);
            target->temporal_depth_valid = true;
        }
    }
    if (bucket >= 1u && bucket <= 0xfffu) {
        ot_base = cpu->read_word(UINT32_C(0x800c426c));
        if (ot_base > UINT32_MAX - 0xccu - bucket * 4u)
            goto fail_packet;
        ot_address = ot_base + 0xccu + bucket * 4u;
        if (!word_address_is_valid(ot_address) ||
            !stage_active_particle(
                &primitive, packet_address, bucket,
                (uint32_t)source->generation, source->producer_pc))
            goto fail_stage;
        previous_head = cpu->read_word(ot_address);
    } else {
        const int64_t minimum = INT64_C(1) << ordering_shift;
        const int64_t maximum = INT64_C(4096) << ordering_shift;
        const GpuRenderTemporalCullPolicy policy = {
            .flags = GPU_RENDER_TEMPORAL_CULL_DEPTH,
            .depth_min_inclusive = minimum > INT32_MAX
                ? INT32_MAX : (int32_t)minimum,
            .depth_max_exclusive = maximum > INT32_MAX
                ? INT32_MAX : (int32_t)maximum,
            .depth_mode = GPU_RENDER_TEMPORAL_DEPTH_MAXIMUM,
            .ordering_depth_shift = (uint8_t)ordering_shift,
        };

        if (!stage_temporal_native_primitive_identified(
                &primitive, particle_address, 0u, &policy))
            goto fail_stage;
        ot_address = 0u;
        previous_head = 0u;
    }

    psx_store_cycle_barrier();
    cpu->write_byte(packet_address + 4u, capture.red);
    psx_store_cycle_barrier();
    cpu->write_byte(packet_address + 5u, capture.green);
    psx_store_cycle_barrier();
    cpu->write_byte(packet_address + 6u, capture.blue);
    for (index = 0u; index < 4u; ++index) {
        const uint32_t xy = (uint16_t)capture.vertices[index].x |
            ((uint32_t)(uint16_t)capture.vertices[index].y << 16u);

        psx_store_cycle_barrier();
        cpu->write_word(packet_address + 8u + index * 8u, xy);
    }
    if (ot_address != 0u) {
        psx_store_cycle_barrier();
        cpu->write_word(packet_address, (packet_tag & UINT32_C(0xff000000)) |
            (previous_head & UINT32_C(0x00ffffff)));
        new_ot_word = (previous_head & UINT32_C(0xff000000)) |
            (packet_address & UINT32_C(0x00ffffff));
        psx_store_cycle_barrier();
        cpu->write_word(ot_address, new_ot_word);
        cpu->gpr[2] = new_ot_word;
    } else {
        cpu->gpr[2] = 0u;
    }
    cpu->pc = cpu->gpr[31];
    return true;

fail_stage:
    reject_producer_family(46u);
    return false;
fail_material:
    reject_producer_family(45u);
    return false;
fail_math:
    reject_producer_family(44u);
    return false;
fail_packet:
    reject_producer_family(43u);
    return false;
fail_source:
    reject_producer_family(42u);
    return false;
fail_context:
    reject_producer_family(41u);
    return false;
}

typedef struct XgRenderProjectedConfig {
    int32_t strip_width;
    int32_t strip_height;
    int32_t phase_multiplier;
    int16_t texture_x;
    int16_t texture_y;
    int16_t texture_depth;
    int16_t texture_v;
    int16_t point_y;
    int16_t point_radius;
    int16_t fixed_groups;
    int16_t fixed_scale;
    int16_t fade_divisor;
    int16_t fade_offset;
} XgRenderProjectedConfig;

static int32_t projected_wrap_add(int32_t left, int32_t right) {
    return (int32_t)((uint32_t)left + (uint32_t)right);
}

static int32_t projected_wrap_subtract(int32_t left, int32_t right) {
    return (int32_t)((uint32_t)left - (uint32_t)right);
}

static int32_t projected_wrap_multiply(int32_t left, int32_t right) {
    return (int32_t)((uint32_t)left * (uint32_t)right);
}

static int32_t projected_shift_right_floor(int32_t value, unsigned bits) {
    if (bits == 0u) return value;
    if (value >= 0) return value >> bits;
    return -(int32_t)(((uint64_t)(-(int64_t)value) +
                       ((UINT64_C(1) << bits) - 1u)) >> bits);
}

static int32_t projected_rounded_shift(int32_t value, unsigned bits) {
    if (value < 0)
        value = projected_wrap_add(value, (int32_t)((1u << bits) - 1u));
    return projected_shift_right_floor(value, bits);
}

static bool projected_divide(int32_t numerator, int32_t denominator,
                             int32_t *quotient) {
    if (quotient == NULL || denominator == 0 ||
        (numerator == INT32_MIN && denominator == -1))
        return false;
    *quotient = numerator / denominator;
    return true;
}

static unsigned projected_leading_sign_bits(uint32_t value) {
    uint32_t scan = (value & UINT32_C(0x80000000)) != 0u ? ~value : value;
    unsigned count = 0u;

    if (scan == 0u) return 32u;
    while ((scan & UINT32_C(0x80000000)) == 0u) {
        ++count;
        scan <<= 1u;
    }
    return count;
}

static bool projected_square_root0(CPUState *cpu, uint32_t value,
                                   int32_t *result) {
    const unsigned leading = projected_leading_sign_bits(value);
    unsigned normalized_shift;
    unsigned result_shift;
    uint32_t normalized;
    int32_t table_value;

    if (cpu == NULL || cpu->read_half == NULL || result == NULL) return false;
    if (leading == 32u) {
        *result = 0;
        return true;
    }
    normalized_shift = leading & ~1u;
    result_shift = (31u - normalized_shift) >> 1u;
    if (normalized_shift < 24u)
        normalized = value >> (24u - normalized_shift);
    else
        normalized = value << (normalized_shift - 24u);
    if (normalized < 0x40u || normalized > 0xffu) return false;
    table_value = (int16_t)cpu->read_half(
        UINT32_C(0x80056a00) + (normalized - 0x40u) * 2u);
    *result = (int32_t)(((uint32_t)table_value << result_shift) >> 12u);
    return true;
}

static bool projected_ratan2(CPUState *cpu, int32_t y, int32_t x,
                             int32_t *result) {
    bool x_negative;
    bool y_negative;
    int32_t ratio;
    int32_t divisor;
    int32_t angle;

    if (cpu == NULL || cpu->read_half == NULL || result == NULL) return false;
    x_negative = x < 0;
    y_negative = y < 0;
    if (x_negative) x = projected_wrap_subtract(0, x);
    if (y_negative) y = projected_wrap_subtract(0, y);
    if (x == 0 && y == 0) {
        *result = 0;
        return true;
    }
    if (y < x) {
        if (((uint32_t)y & UINT32_C(0x7fe00000)) != 0u) {
            divisor = projected_shift_right_floor(x, 10u);
            if (!projected_divide(y, divisor, &ratio)) return false;
        } else if (!projected_divide((int32_t)((uint32_t)y << 10u), x,
                                     &ratio)) {
            return false;
        }
        if (ratio < 0 || ratio > 0x400) return false;
        angle = (int16_t)cpu->read_half(
            UINT32_C(0x80057030) + (uint32_t)ratio * 2u);
    } else {
        if (((uint32_t)x & UINT32_C(0x7fe00000)) != 0u) {
            divisor = projected_shift_right_floor(y, 10u);
            if (!projected_divide(x, divisor, &ratio)) return false;
        } else if (!projected_divide((int32_t)((uint32_t)x << 10u), y,
                                     &ratio)) {
            return false;
        }
        if (ratio < 0 || ratio > 0x400) return false;
        angle = 0x400 - (int16_t)cpu->read_half(
            UINT32_C(0x80057030) + (uint32_t)ratio * 2u);
    }
    if (x_negative) angle = 0x800 - angle;
    if (y_negative) angle = -angle;
    *result = angle;
    return true;
}

static uint16_t projected_get_tpage(int16_t depth, int32_t page_x,
                                    int32_t page_y) {
    return (uint16_t)(((uint32_t)page_x >> 6u) & 0x0fu) |
        (uint16_t)((((uint32_t)page_y >> 8u) & 1u) << 4u) |
        (uint16_t)(((uint16_t)depth & 3u) << 7u) |
        (uint16_t)(((uint32_t)page_y & 0x200u) << 2u);
}

static bool capture_projected_config(CPUState *cpu, uint32_t object_address,
                                     XgRenderProjectedConfig *config) {
    if (cpu == NULL || config == NULL || cpu->read_word == NULL ||
        cpu->read_half == NULL || object_address > UINT32_MAX - 0x34au ||
        !word_address_is_valid(object_address) ||
        !word_address_is_valid(object_address + 0x348u))
        return false;
    *config = (XgRenderProjectedConfig){
        .strip_width = (int32_t)cpu->read_word(object_address + 0x328u),
        .strip_height = (int32_t)cpu->read_word(object_address + 0x32cu),
        .phase_multiplier =
            (int32_t)cpu->read_word(object_address + 0x330u),
        .texture_x = (int16_t)cpu->read_half(object_address + 0x334u),
        .texture_y = (int16_t)cpu->read_half(object_address + 0x336u),
        .texture_depth = (int16_t)cpu->read_half(object_address + 0x338u),
        .texture_v = (int16_t)cpu->read_half(object_address + 0x33au),
        .point_y = (int16_t)cpu->read_half(object_address + 0x33eu),
        .point_radius = (int16_t)cpu->read_half(object_address + 0x340u),
        .fixed_groups = (int16_t)cpu->read_half(object_address + 0x344u),
        .fixed_scale = (int16_t)cpu->read_half(object_address + 0x346u),
        .fade_divisor = (int16_t)cpu->read_half(object_address + 0x348u),
        .fade_offset = (int16_t)cpu->read_half(object_address + 0x34au),
    };
    return config->strip_width > 0 && config->strip_width <= INT16_MAX &&
        config->strip_height >= 0 && config->strip_height <= INT16_MAX &&
        config->texture_depth >= 0 && config->texture_depth <= 2 &&
        config->fixed_groups >= 0 && config->fixed_groups <= 1;
}

static void apply_draw_state(XgRenderIrMaterialState *material,
                             const GpuDrawState *draw) {
    material->draw_area_left = draw->left;
    material->draw_area_top = draw->top;
    material->draw_area_right = draw->right;
    material->draw_area_bottom = draw->bottom;
    material->draw_offset_x = draw->offset_x;
    material->draw_offset_y = draw->offset_y;
    material->texture_window_mask_x = draw->texture_window_mask_x;
    material->texture_window_mask_y = draw->texture_window_mask_y;
    material->texture_window_offset_x = draw->texture_window_offset_x;
    material->texture_window_offset_y = draw->texture_window_offset_y;
    material->dither = draw->dither != 0u;
    material->mask_set = draw->mask_set != 0u;
    material->mask_check = draw->mask_check != 0u;
}

static bool model_ft4_shadow_consume_controls(
    CPUState *cpu, uint32_t *cursor, uint16_t *tpage, uint16_t *clut) {
    uint32_t control_count;

    if (cpu == NULL || cursor == NULL || tpage == NULL || clut == NULL ||
        cpu->read_byte == NULL || cpu->read_half == NULL)
        return false;
    for (control_count = 0u; control_count < 32u; ++control_count) {
        const uint8_t command = cpu->read_byte(*cursor + 3u);
        const uint16_t value = cpu->read_half(*cursor);

        if (command == 0xc4u) {
            const uint32_t mode = cpu->read_word(UINT32_C(0x80050108));

            if (mode == 1u)
                *tpage = (uint16_t)((value & 0xffe0u) |
                    cpu->read_half(UINT32_C(0x80059310)));
            else if (mode == 2u)
                *tpage = cpu->read_half(UINT32_C(0x80059310));
            else
                *tpage = value;
        } else if (command == 0xc8u) {
            *clut = value;
            if (cpu->read_word(UINT32_C(0x8005010c)) == 0u)
                *clut = (uint16_t)((value & 0x0fu) |
                    cpu->read_half(UINT32_C(0x80059314)));
        } else {
            return true;
        }
        if (*cursor > UINT32_MAX - 4u) return false;
        *cursor += 4u;
    }
    return false;
}

static uint32_t model_ft4_template_key(uint32_t packet_address) {
    return packet_address & UINT32_C(0x001ffffc);
}

static uint32_t model_ft4_template_slot(uint32_t address) {
    return ((address >> 2u) ^ (address >> 13u)) &
        (XG_RENDER_MODEL_FT4_TEMPLATE_CAPACITY - 1u);
}

static XgRenderModelFt4Template *model_ft4_template_direct_find_in(
    XgRenderModelFt4Template templates[XG_RENDER_MODEL_FT4_TEMPLATE_CAPACITY],
    const XgRenderAddressLookupSlot lookup[XG_RENDER_LOOKUP_WORD_CAPACITY],
    uint16_t lookup_epoch, uint32_t address, bool descriptor_key) {
    const uint32_t key = model_ft4_template_key(address);
    const uint32_t index = xg_render_lookup_find(
        lookup, lookup_epoch, key, XG_RENDER_MODEL_FT4_TEMPLATE_CAPACITY);
    XgRenderModelFt4Template *entry;

    if (index == UINT32_MAX) return NULL;
    entry = &templates[index];
    if (!model_ft4_template_is_current(entry) ||
        (descriptor_key ? entry->descriptor_address :
                          entry->packet_address) != key)
        return NULL;
    return entry;
}

static void model_ft4_template_direct_remove(
    XgRenderAddressLookupSlot lookup[XG_RENDER_LOOKUP_WORD_CAPACITY],
    uint32_t address, uint32_t index) {
    const uint16_t lookup_epoch = lookup == model_ft4_packet_lookup
        ? model_ft4_packet_lookup_epoch
        : model_ft4_descriptor_lookup_epoch;

    xg_render_lookup_remove(lookup, lookup_epoch, address, index);
}

static void model_ft4_template_direct_put(
    XgRenderAddressLookupSlot lookup[XG_RENDER_LOOKUP_WORD_CAPACITY],
    uint32_t address, uint32_t index) {
    const uint16_t lookup_epoch = lookup == model_ft4_packet_lookup
        ? model_ft4_packet_lookup_epoch
        : model_ft4_descriptor_lookup_epoch;

    xg_render_lookup_put(lookup, lookup_epoch, address, index);
}

static XgRenderModelFt4Template *model_ft4_template_find_in(
    XgRenderModelFt4Template templates[XG_RENDER_MODEL_FT4_TEMPLATE_CAPACITY],
    XgRenderAddressLookupSlot lookup[XG_RENDER_LOOKUP_WORD_CAPACITY],
    uint16_t lookup_epoch, uint32_t address, bool descriptor_key, bool insert,
    bool require_lifecycle) {
    const uint32_t key = model_ft4_template_key(address);
    XgRenderModelFt4Template *direct = model_ft4_template_direct_find_in(
        templates, lookup, lookup_epoch, key, descriptor_key);
    uint32_t slot = model_ft4_template_slot(key);
    XgRenderModelFt4Template *first_invalid = NULL;

    if (direct != NULL) {
        if (require_lifecycle && !insert && state.requested_render_mode ==
                GUEST_RENDER_RENDER_NATIVE &&
            !producer_lifecycle_matches(&direct->lifecycle))
            return NULL;
        return direct;
    }
    if (!insert) return NULL;
    for (uint32_t probe = 0u;
         probe < XG_RENDER_MODEL_FT4_TEMPLATE_CAPACITY; ++probe) {
        XgRenderModelFt4Template *entry = &templates[slot];

        if (entry->table_epoch != model_ft4_table_epoch) {
            if (!insert) return NULL;
            if (first_invalid != NULL) return first_invalid;
            entry->valid = false;
            entry->table_epoch = model_ft4_table_epoch;
            return entry;
        }
        if (!entry->valid) {
            if (insert && first_invalid == NULL) first_invalid = entry;
            slot = (slot + 1u) &
                (XG_RENDER_MODEL_FT4_TEMPLATE_CAPACITY - 1u);
            continue;
        }
        if ((descriptor_key ? entry->descriptor_address :
                                entry->packet_address) == key) {
            if (require_lifecycle && !insert && state.requested_render_mode ==
                    GUEST_RENDER_RENDER_NATIVE &&
                !producer_lifecycle_matches(&entry->lifecycle))
                return NULL;
            return entry;
        }
        slot = (slot + 1u) & (XG_RENDER_MODEL_FT4_TEMPLATE_CAPACITY - 1u);
    }
    return insert ? first_invalid : NULL;
}

static XgRenderModelFt4Template *model_ft4_template_find_packet(
    uint32_t packet_address, bool insert) {
    return model_ft4_template_find_in(
        model_ft4_templates, model_ft4_packet_lookup,
        model_ft4_packet_lookup_epoch, packet_address, false, insert, true);
}

static XgRenderModelFt4Template *model_ft4_template_find_descriptor(
    uint32_t descriptor_address, bool insert) {
    return model_ft4_template_find_in(
        model_ft4_descriptor_templates, model_ft4_descriptor_lookup,
        model_ft4_descriptor_lookup_epoch, descriptor_address, true, insert,
        true);
}

static XgRenderModelFt4Template *model_ft4_template_find_current_in(
    XgRenderModelFt4Template templates[XG_RENDER_MODEL_FT4_TEMPLATE_CAPACITY],
    uint32_t address, bool descriptor_key) {
    return model_ft4_template_direct_find_in(
        templates,
        descriptor_key ? model_ft4_descriptor_lookup :
                         model_ft4_packet_lookup,
        descriptor_key ? model_ft4_descriptor_lookup_epoch :
                         model_ft4_packet_lookup_epoch,
        address, descriptor_key);
}

static void model_ft4_packet_template_unlink(
        XgRenderModelFt4Template *entry) {
    uint16_t *link;

    if (!model_ft4_template_is_current(entry)) return;
    link = &model_ft4_descriptor_packet_heads[
        model_ft4_template_slot(entry->descriptor_address)];
    while (*link != 0u) {
        XgRenderModelFt4Template *candidate =
            &model_ft4_templates[*link - 1u];

        if (candidate == entry) {
            *link = candidate->descriptor_next;
            candidate->descriptor_next = 0u;
            return;
        }
        link = &candidate->descriptor_next;
    }
}

static void model_ft4_packet_template_store(
        XgRenderModelFt4Template *entry,
        const XgRenderModelFt4Template *captured) {
    const uint32_t index = (uint32_t)(entry - model_ft4_templates);
    const uint32_t bucket =
        model_ft4_template_slot(captured->descriptor_address);

    if (model_ft4_template_is_current(entry)) {
        model_ft4_template_direct_remove(
            model_ft4_packet_lookup, entry->packet_address, index);
        model_ft4_packet_template_unlink(entry);
    }
    *entry = *captured;
    entry->descriptor_next = model_ft4_descriptor_packet_heads[bucket];
    model_ft4_descriptor_packet_heads[bucket] = (uint16_t)(index + 1u);
    model_ft4_template_direct_put(
        model_ft4_packet_lookup, entry->packet_address, index);
}

static void model_ft4_descriptor_template_store(
        XgRenderModelFt4Template *entry,
        const XgRenderModelFt4Template *captured) {
    const uint32_t index =
        (uint32_t)(entry - model_ft4_descriptor_templates);

    if (model_ft4_template_is_current(entry))
        model_ft4_template_direct_remove(
            model_ft4_descriptor_lookup, entry->descriptor_address, index);
    *entry = *captured;
    model_ft4_template_direct_put(
        model_ft4_descriptor_lookup, entry->descriptor_address, index);
}

static void invalidate_model_ft4_packet_template(
        XgRenderModelFt4Template *entry) {
    const uint32_t index = (uint32_t)(entry - model_ft4_templates);

    if (!model_ft4_template_is_current(entry)) return;
    model_ft4_template_direct_remove(
        model_ft4_packet_lookup, entry->packet_address, index);
    model_ft4_packet_template_unlink(entry);
    entry->valid = false;
    --model_ft4_template_count;
}

static void invalidate_model_ft4_descriptor_packets(
        uint32_t descriptor_address) {
    const uint32_t key = model_ft4_template_key(descriptor_address);
    uint16_t *link = &model_ft4_descriptor_packet_heads[
        model_ft4_template_slot(key)];

    while (*link != 0u) {
        XgRenderModelFt4Template *entry =
            &model_ft4_templates[*link - 1u];

        if (entry->descriptor_address == key) {
            *link = entry->descriptor_next;
            entry->descriptor_next = 0u;
            if (model_ft4_template_is_current(entry)) {
                model_ft4_template_direct_remove(
                    model_ft4_packet_lookup, entry->packet_address,
                    (uint32_t)(entry - model_ft4_templates));
                entry->valid = false;
                --model_ft4_template_count;
            }
        } else {
            link = &entry->descriptor_next;
        }
    }
}

static void invalidate_model_ft4_packet_candidates(uint32_t address,
                                                    uint32_t size) {
    const uint64_t write_begin = address & UINT32_C(0x1fffffff);
    uint64_t write_end = write_begin + size;
    uint64_t first;
    uint64_t last;

    if (size == 0u || write_begin >= UINT32_C(0x200000)) return;
    if (write_end > UINT32_C(0x200000)) write_end = UINT32_C(0x200000);
    first = write_begin >= 0x28u ? write_begin - 0x28u + 1u : 0u;
    first = (first + 3u) & ~UINT64_C(3);
    last = (write_end - 1u) & ~UINT64_C(3);
    for (uint64_t candidate = first; candidate <= last; candidate += 4u) {
        XgRenderModelFt4Template *packet = model_ft4_template_find_current_in(
            model_ft4_templates, (uint32_t)candidate, false);

        if (packet != NULL) {
            const uint32_t packet_address = packet->packet_address;
            const uint32_t descriptor_address = packet->descriptor_address;
            XgRenderModelFt4Template *descriptor =
                model_ft4_template_find_current_in(
                    model_ft4_descriptor_templates,
                    descriptor_address, true);

            invalidate_model_ft4_packet_template(packet);
            if (descriptor != NULL &&
                descriptor->packet_address == packet_address) {
                model_ft4_template_direct_remove(
                    model_ft4_descriptor_lookup,
                    descriptor->descriptor_address,
                    (uint32_t)(descriptor -
                        model_ft4_descriptor_templates));
                descriptor->valid = false;
                --model_ft4_descriptor_template_count;
            }
        }
    }
}

static void invalidate_model_ft4_descriptor_candidates(uint32_t address,
                                                        uint32_t size) {
    const uint64_t write_begin = address & UINT32_C(0x1fffffff);
    uint64_t write_end = write_begin + size;
    uint64_t first;
    uint64_t last;

    if (size == 0u || write_begin >= UINT32_C(0x200000)) return;
    if (write_end > UINT32_C(0x200000)) write_end = UINT32_C(0x200000);
    first = write_begin >= 12u ? write_begin - 11u : 0u;
    first = (first + 3u) & ~UINT64_C(3);
    last = (write_end - 1u) & ~UINT64_C(3);
    for (uint64_t candidate = first; candidate <= last; candidate += 4u) {
        XgRenderModelFt4Template *descriptor =
            model_ft4_template_find_current_in(
                model_ft4_descriptor_templates, (uint32_t)candidate, true);

        invalidate_model_ft4_descriptor_packets((uint32_t)candidate);
        if (descriptor != NULL) {
            model_ft4_template_direct_remove(
                model_ft4_descriptor_lookup,
                descriptor->descriptor_address,
                (uint32_t)(descriptor - model_ft4_descriptor_templates));
            descriptor->valid = false;
            --model_ft4_descriptor_template_count;
        }
    }
}

static void invalidate_model_ft4_templates_overlapping(uint32_t address,
                                                        uint32_t size) {
    invalidate_model_ft4_packet_candidates(address, size);
    invalidate_model_ft4_descriptor_candidates(address, size);
}

static uint32_t world_model_template_slot(uint32_t address) {
    return ((address >> 2u) ^ (address >> 13u)) &
        (XG_RENDER_WORLD_MODEL_TEMPLATE_CAPACITY - 1u);
}

static XgRenderWorldModelTemplate *world_model_template_find(
    uint32_t packet_address, bool insert) {
    const uint32_t key = model_ft4_template_key(packet_address);
    uint32_t slot = world_model_template_slot(key);
    XgRenderWorldModelTemplate *first_inactive = NULL;
    uint32_t probe;

    for (probe = 0u; probe < XG_RENDER_WORLD_MODEL_TEMPLATE_CAPACITY;
         ++probe) {
        XgRenderWorldModelTemplate *entry = &world_model_templates[slot];

        if (entry->table_epoch != world_model_template_epoch)
            return insert ? entry : NULL;
        if (!entry->valid)
            return insert && first_inactive != NULL ? first_inactive :
                (insert ? entry : NULL);
        if (entry->packet_address == key) return entry;
        if (!entry->active && first_inactive == NULL)
            first_inactive = entry;
        slot = (slot + 1u) &
            (XG_RENDER_WORLD_MODEL_TEMPLATE_CAPACITY - 1u);
    }
    return insert ? first_inactive : NULL;
}

static uint32_t world_model_color_write_slot(uint32_t address) {
    return ((address >> 2u) ^ (address >> 11u)) &
        (XG_RENDER_WORLD_MODEL_COLOR_WRITE_CAPACITY - 1u);
}

static XgRenderWorldModelColorWrite *world_model_color_write_find(
    uint32_t address, bool insert) {
    const uint32_t key = model_ft4_template_key(address);
    uint32_t slot = world_model_color_write_slot(key);
    uint32_t probe;

    for (probe = 0u; probe < XG_RENDER_WORLD_MODEL_COLOR_WRITE_CAPACITY;
         ++probe) {
        XgRenderWorldModelColorWrite *entry =
            &world_model_color_writes[slot];

        if (!entry->valid || entry->resource_epoch !=
                world_model_initializer.resource_epoch)
            return insert ? entry : NULL;
        if (entry->address == key) return entry;
        slot = (slot + 1u) &
            (XG_RENDER_WORLD_MODEL_COLOR_WRITE_CAPACITY - 1u);
    }
    return NULL;
}

static void clear_world_actor_context(void) {
    if (!world_actor_context.active && !world_actor_context.poisoned) return;
    world_actor_context = (XgRenderWorldActorContext){0};
}

static void clear_world_actor_native_pending(void) {
    if (!world_actor_sprites_native_state.valid) return;
    world_actor_sprites_native_state.valid = false;
}

static void clear_world_models_native_pending(void) {
    if (!world_models_native_state.valid) return;
    world_models_native_state.valid = false;
}

static void invalidate_world_model_templates(void) {
    world_model_packet_copy = (XgRenderWorldModelPacketCopyContext){0};
    world_model_packet_copy_range_count = 0u;
    world_models_native_snapshot.packet_copy_range_count = 0u;
    if (world_model_templates_populated) {
        world_model_templates_populated = false;
    }
    if (world_model_initializer_populated) {
        world_model_initializer =
            (XgRenderWorldModelInitializerContext){0};
        world_model_initializer_populated = false;
    }
    if (world_model_template_epoch == UINT32_MAX) {
        memset(world_model_templates, 0, sizeof(world_model_templates));
        world_model_template_epoch = 1u;
    } else {
        ++world_model_template_epoch;
    }
    invalidate_model_ft4_templates();
}

static void invalidate_world_model_initializer(void) {
    if (world_model_initializer.active) {
        invalidate_world_model_templates();
        return;
    }
    if (world_model_initializer_populated) {
        world_model_initializer =
            (XgRenderWorldModelInitializerContext){0};
        world_model_initializer_populated = false;
    }
}

static bool world_model_initializer_caller_is_valid(uint32_t address) {
    return physical_address_equals(address, UINT32_C(0x800211bc)) ||
        physical_address_equals(address, UINT32_C(0x80021258)) ||
        physical_address_equals(address, UINT32_C(0x800212e8)) ||
        physical_address_equals(address, UINT32_C(0x800714c4)) ||
        physical_address_equals(address, UINT32_C(0x80084750));
}

static bool guest_ranges_overlap(uint32_t left_address, uint32_t left_size,
                                 uint32_t right_address, uint32_t right_size) {
    const uint64_t left = left_address & UINT32_C(0x1fffffff);
    const uint64_t right = right_address & UINT32_C(0x1fffffff);

    return left_size != 0u && right_size != 0u &&
        left < right + right_size && right < left + left_size;
}

static void world_model_initializer_begin(CPUState *cpu) {
    uint64_t authentication_generation;
    uint32_t packet_capacity;
    uint32_t group_count;
    uint32_t index;

    if (state.requested_render_mode == GUEST_RENDER_RENDER_ORIGINAL) return;
    if (world_native_cutover_in_progress || world_model_initializer.active ||
        world_model_resource_epoch == UINT64_MAX) {
        world_native_cutover_failed = true;
        abort_standalone_submission();
        invalidate_world_model_templates();
        return;
    }
    if (cpu == NULL || cpu->read_word == NULL || cpu->read_half == NULL ||
        cpu->read_byte == NULL || !stack_address_is_valid(cpu->gpr[29]) ||
        cpu->gpr[29] < 0x30u ||
        !guest_data_range_is_valid(cpu->gpr[29] - 0x30u, 0x30u, 4u, false) ||
        !guest_data_range_is_valid(cpu->gpr[4], 0x38u, 4u, false) ||
        !guest_data_range_is_valid(cpu->gpr[5], 4u, 4u, false) ||
        cpu->gpr[6] > 3u ||
        !world_model_initializer_caller_is_valid(cpu->gpr[31]) ||
        !world_authentication_generation(&authentication_generation)) {
        invalidate_world_model_templates();
        return;
    }
    group_count = cpu->read_half(cpu->gpr[4] + 6u);
    packet_capacity = cpu->read_word(cpu->gpr[4] + 0x34u);
    if ((packet_capacity & 3u) != 0u ||
        (group_count != 0u && packet_capacity < 4u) ||
        (packet_capacity != 0u && !guest_data_range_is_valid(
            cpu->gpr[5], packet_capacity, 4u, false))) {
        invalidate_world_model_templates();
        return;
    }
    ++world_model_resource_epoch;
    for (index = 0u; index < XG_RENDER_WORLD_MODEL_TEMPLATE_CAPACITY;
         ++index) {
        XgRenderWorldModelTemplate *entry = &world_model_templates[index];

        if (entry->table_epoch == world_model_template_epoch &&
            entry->valid && guest_ranges_overlap(
                entry->packet_address, (uint32_t)entry->word_count * 4u,
                cpu->gpr[5], packet_capacity))
            world_model_templates[index].active = false;
    }
    world_model_initializer = (XgRenderWorldModelInitializerContext){
        .resource_epoch = world_model_resource_epoch,
        .authentication_generation = authentication_generation,
        .owner_cpu = cpu,
        .model_address = cpu->gpr[4],
        .packet_base = cpu->gpr[5],
        .packet_capacity = packet_capacity,
        .caller_return = cpu->gpr[31],
        .entry_stack_pointer = cpu->gpr[29],
        .dispatch_mode = cpu->gpr[6],
        .active = true,
    };
    world_model_initializer_populated = true;
}

static bool world_model_record_color_write(CPUState *cpu, uint32_t address,
                                            uint32_t value) {
    XgRenderWorldModelColorWrite *entry;
    uint32_t packet;

    if (!world_model_initializer.active ||
        world_model_initializer.owner_cpu != cpu ||
        world_model_initializer.packet_capacity < 4u ||
        !guest_data_range_is_valid(address, 4u, 4u, false) ||
        address < world_model_initializer.packet_base ||
        address - world_model_initializer.packet_base >
            world_model_initializer.packet_capacity - 4u)
        return false;
    packet = cpu->read_word(XG_WORLD_MODELS_RESIDENT_PACKET_CURSOR_GLOBAL);
    if (packet < world_model_initializer.packet_base ||
        packet - world_model_initializer.packet_base >=
            world_model_initializer.packet_capacity || address < packet)
        return false;
    entry = world_model_color_write_find(address, true);
    if (entry == NULL) return false;
    *entry = (XgRenderWorldModelColorWrite){
        .resource_epoch = world_model_initializer.resource_epoch,
        .owner_cpu = cpu,
        .model_address = model_ft4_template_key(
            world_model_initializer.model_address),
        .packet_address = model_ft4_template_key(packet),
        .address = model_ft4_template_key(address),
        .value = value,
        .valid = true,
    };
    return true;
}

static void world_model_observe_color_writes(CPUState *cpu, uint32_t pc) {
    bool ok = true;

    if (!world_model_initializer.active) return;
    if (cpu == NULL || cpu->read_word == NULL ||
        world_model_initializer.owner_cpu != cpu ||
        world_native_cutover_in_progress) {
        world_native_cutover_failed = true;
        abort_standalone_submission();
        invalidate_world_model_templates();
        return;
    }
    if (physical_address_equals(pc, UINT32_C(0x8004a1ac))) {
        ok = world_model_record_color_write(
            cpu, cpu->gpr[5], cpu->read_word(cpu->gpr[5]));
    } else if (physical_address_equals(pc, UINT32_C(0x8004a1e8))) {
        ok = world_model_record_color_write(
                 cpu, cpu->gpr[7], cpu->read_word(cpu->gpr[7])) &&
            world_model_record_color_write(
                 cpu, cpu->gpr[8], cpu->read_word(cpu->gpr[8])) &&
            world_model_record_color_write(
                 cpu, cpu->gpr[9], cpu->read_word(cpu->gpr[9]));
    } else if (physical_address_equals(pc, UINT32_C(0x8004a274))) {
        ok = world_model_record_color_write(
            cpu, cpu->gpr[6], cpu->read_word(cpu->gpr[6]));
    } else if (physical_address_equals(pc, UINT32_C(0x8004a2b8))) {
        ok = world_model_record_color_write(
                 cpu, cpu->gpr[8], cpu->read_word(cpu->gpr[8])) &&
            world_model_record_color_write(
                 cpu, cpu->gpr[9], cpu->read_word(cpu->gpr[9])) &&
            world_model_record_color_write(
                 cpu, cpu->gpr[10], cpu->read_word(cpu->gpr[10]));
    }
    if (!ok) invalidate_world_model_templates();
}

static const uint32_t world_model_initializer_functions[
    XG_WORLD_MODELS_PRIMITIVE_FAMILY_COUNT] = {
    UINT32_C(0x8002cdcc), UINT32_C(0x8002d814),
    UINT32_C(0x8002d6ac), UINT32_C(0x8002da14),
    UINT32_C(0x8002cf34), UINT32_C(0x8002d984),
    UINT32_C(0x8002d77c), UINT32_C(0x8002da14),
    UINT32_C(0x8002cf58), UINT32_C(0x8002d530),
    UINT32_C(0x8002d180), UINT32_C(0x8002d244),
    UINT32_C(0x8002d0c0), UINT32_C(0x8002d0e4),
    UINT32_C(0x8002d180), UINT32_C(0x8002d244),
    UINT32_C(0x8002dafc),
};

static void world_model_observe_initializer_success(CPUState *cpu) {
    enum {
        INITIALIZER_TABLE_BASE = 0x8004fe50u,
        INITIALIZER_TABLE_STRIDE = 0x28u,
    };
    XgRenderWorldModelInitializerReceipt *receipt;
    uint32_t family;
    uint32_t packet;
    uint32_t table;

    if (!world_model_initializer.active) return;
    if (cpu == NULL || cpu->read_word == NULL ||
        world_model_initializer.owner_cpu != cpu || cpu->gpr[2] == 0u ||
        cpu->gpr[17] < INITIALIZER_TABLE_BASE ||
        (cpu->gpr[17] - INITIALIZER_TABLE_BASE) %
                INITIALIZER_TABLE_STRIDE != 0u) {
        invalidate_world_model_templates();
        return;
    }
    table = cpu->gpr[17];
    family = (table - INITIALIZER_TABLE_BASE) / INITIALIZER_TABLE_STRIDE;
    packet = cpu->read_word(XG_WORLD_MODELS_RESIDENT_PACKET_CURSOR_GLOBAL);
    if (family >= XG_WORLD_MODELS_PRIMITIVE_FAMILY_COUNT ||
        !physical_address_equals(
            cpu->gpr[19], world_model_initializer_functions[family]) ||
        world_model_initializer.receipt_count >=
            XG_RENDER_WORLD_MODEL_TEMPLATE_CAPACITY ||
        packet < world_model_initializer.packet_base ||
        packet - world_model_initializer.packet_base >=
            world_model_initializer.packet_capacity) {
        invalidate_world_model_templates();
        return;
    }
    receipt = &world_model_initializer_receipts[
        world_model_initializer.receipt_count++];
    *receipt = (XgRenderWorldModelInitializerReceipt){
        .resource_epoch = world_model_initializer.resource_epoch,
        .owner_cpu = cpu,
        .model_address = model_ft4_template_key(
            world_model_initializer.model_address),
        .packet_address = model_ft4_template_key(packet),
        .initializer_function = cpu->gpr[19],
        .primitive_family = (uint8_t)family,
        .valid = true,
    };
}

static uint32_t world_model_expected_color_word_mask(uint32_t family,
                                                     uint32_t dispatch_mode) {
    switch (family) {
    case 0u:
    case 1u:
    case 8u:
    case 9u:
        return dispatch_mode == 0u ? 0u : UINT32_C(1) << 1u;
    case 2u:
    case 6u:
        return (UINT32_C(1) << 1u) | (UINT32_C(1) << 3u) |
            (UINT32_C(1) << 5u);
    case 3u:
    case 7u:
        return (UINT32_C(1) << 1u) | (UINT32_C(1) << 4u) |
            (UINT32_C(1) << 7u);
    case 10u:
    case 14u:
        return (UINT32_C(1) << 1u) | (UINT32_C(1) << 3u) |
            (UINT32_C(1) << 5u) | (UINT32_C(1) << 7u);
    case 11u:
    case 15u:
        return (UINT32_C(1) << 1u) | (UINT32_C(1) << 4u) |
            (UINT32_C(1) << 7u) | (UINT32_C(1) << 10u);
    default:
        return 0u;
    }
}

static bool world_model_seed_templates(CPUState *cpu) {
    static const uint8_t attribute_sizes[
        XG_WORLD_MODELS_PRIMITIVE_FAMILY_COUNT] = {
        4u, 8u, 4u, 8u, 4u, 8u, 4u, 8u, 4u,
        12u, 4u, 12u, 4u, 12u, 4u, 12u, 4u,
    };
    static const uint8_t packet_strides[
        XG_WORLD_MODELS_PRIMITIVE_FAMILY_COUNT] = {
        0x14u, 0x20u, 0x1cu, 0x28u, 0x14u, 0x20u, 0x1cu, 0x28u,
        0x18u, 0x28u, 0x24u, 0x34u, 0x18u, 0x28u, 0x24u, 0x34u,
        0x20u,
    };
    const XgRenderWorldModelInitializerContext context =
        world_model_initializer;
    uint32_t topology;
    uint32_t attribute = 0u;
    uint32_t packet = context.packet_base;
    uint32_t captured_total = 0u;
    uint32_t group_count;
    uint32_t group = 0u;
    uint64_t authentication_generation;
    if (cpu == NULL || cpu->read_word == NULL || cpu->read_half == NULL ||
        cpu->read_byte == NULL || !context.active ||
        context.resource_epoch == 0u || context.owner_cpu != cpu ||
        context.authentication_generation == 0u ||
        !world_authentication_generation(&authentication_generation) ||
        authentication_generation != context.authentication_generation ||
        cpu->gpr[29] != context.entry_stack_pointer ||
        !physical_address_equals(cpu->gpr[31], context.caller_return))
        return false;
    group_count = cpu->read_half(context.model_address + 6u);
    topology = cpu->read_word(context.model_address + 0x10u);
    attribute = cpu->read_word(context.model_address + 0x14u);
    if (group_count != 0u &&
        (!guest_data_range_is_valid(topology, 4u, 2u, false) ||
         !guest_data_range_is_valid(attribute, 4u, 4u, false)))
        return false;

    for (group = 0u; group < group_count; ++group) {
        const uint32_t family = cpu->read_byte(topology);
        const uint32_t count = cpu->read_half(topology + 2u);
        const uint32_t table = UINT32_C(0x8004fe50) + family * 0x28u;
        uint32_t primitive;

        if (family >= XG_WORLD_MODELS_PRIMITIVE_FAMILY_COUNT || count == 0u ||
            count > (UINT32_MAX - 4u) / 8u ||
            !guest_data_range_is_valid(topology, 4u + count * 8u, 2u,
                                       false) ||
            !physical_address_equals(cpu->read_word(table + 0x18u),
                                      world_model_initializer_functions[family]) ||
            cpu->read_word(table + 0x1cu) != 8u ||
            cpu->read_word(table + 0x20u) != attribute_sizes[family] ||
            cpu->read_word(table + 0x24u) != packet_strides[family])
            return false;
        for (primitive = 0u; primitive < count; ++primitive) {
            XgRenderWorldModelTemplate captured = {0};
            XgRenderWorldModelTemplate *entry;
            const XgRenderWorldModelInitializerReceipt *receipt;
            uint32_t expected_color_mask;
            uint32_t expected_tag_payload_word_count;
            uint32_t controls;
            uint32_t word;

            for (controls = 0u;
                 controls < XG_WORLD_MODELS_MAX_CONTROL_COMMANDS_PER_PRIMITIVE;
                 ++controls) {
                const uint8_t command = cpu->read_byte(attribute + 3u);

                if (command != 0xc4u && command != 0xc8u) break;
                if (attribute > UINT32_MAX - 4u) return false;
                attribute += 4u;
            }
            if (controls ==
                    XG_WORLD_MODELS_MAX_CONTROL_COMMANDS_PER_PRIMITIVE ||
                packet > UINT32_MAX - packet_strides[family] ||
                attribute > UINT32_MAX - attribute_sizes[family] ||
                packet < context.packet_base ||
                packet - context.packet_base > context.packet_capacity ||
                packet_strides[family] > context.packet_capacity -
                    (packet - context.packet_base) ||
                !guest_data_range_is_valid(packet, packet_strides[family], 4u,
                                           false) ||
                !guest_data_range_is_valid(attribute, attribute_sizes[family],
                                            4u, false))
                return false;
            if (captured_total >= context.receipt_count) return false;
            receipt = &world_model_initializer_receipts[captured_total];
            if (!receipt->valid ||
                receipt->resource_epoch != context.resource_epoch ||
                receipt->owner_cpu != cpu ||
                receipt->model_address !=
                    model_ft4_template_key(context.model_address) ||
                receipt->packet_address != model_ft4_template_key(packet) ||
                receipt->primitive_family != family ||
                !physical_address_equals(
                    receipt->initializer_function,
                    world_model_initializer_functions[family]))
                return false;
            captured.packet_address = model_ft4_template_key(packet);
            captured.resource_epoch = context.resource_epoch;
            captured.table_epoch = world_model_template_epoch;
            captured.owner_cpu = cpu;
            captured.model_address =
                model_ft4_template_key(context.model_address);
            captured.packet_base = model_ft4_template_key(context.packet_base);
            captured.primitive_ordinal = captured_total;
            captured.attribute_address = model_ft4_template_key(attribute);
            captured.primitive_family = (uint8_t)family;
            captured.word_count = packet_strides[family] / 4u;
            expected_tag_payload_word_count = captured.word_count -
                (family == 8u ? 2u : 1u);
            expected_color_mask = world_model_expected_color_word_mask(
                family, context.dispatch_mode);
            for (word = 0u; word < captured.word_count; ++word) {
                XgRenderWorldModelColorWrite *color =
                    world_model_color_write_find(packet + word * 4u, false);
                const bool color_expected =
                    (expected_color_mask & (UINT32_C(1) << word)) != 0u;

                captured.words[word] = cpu->read_word(packet + word * 4u);
                if (color_expected != (color != NULL)) return false;
                if (color_expected) {
                    if (color->resource_epoch != context.resource_epoch ||
                        color->owner_cpu != cpu ||
                        color->model_address != captured.model_address ||
                        color->packet_address != captured.packet_address ||
                        (captured.words[word] & UINT32_C(0x00ffffff)) !=
                            (color->value & UINT32_C(0x00ffffff))) {
                        return false;
                    }
                    color->used = true;
                }
            }
            if ((captured.words[0] >> 24u) !=
                expected_tag_payload_word_count)
                return false;
            entry = world_model_template_find(packet, true);
            if (entry == NULL) return false;
            captured.active = true;
            captured.valid = true;
            *entry = captured;
            world_model_templates_populated = true;
            ++captured_total;
            packet += packet_strides[family];
            attribute += attribute_sizes[family];
        }
        topology += 4u + count * 8u;
    }
    if (captured_total != context.receipt_count ||
        cpu->read_word(XG_WORLD_MODELS_RESIDENT_PACKET_CURSOR_GLOBAL) !=
            packet)
        return false;
    for (group = 0u; group < XG_RENDER_WORLD_MODEL_COLOR_WRITE_CAPACITY;
         ++group) {
        if (world_model_color_writes[group].valid &&
            world_model_color_writes[group].resource_epoch ==
                context.resource_epoch &&
            !world_model_color_writes[group].used)
            return false;
    }
    return true;
}

static void world_model_initializer_finish(CPUState *cpu) {
    if (!world_model_initializer.active) return;
    if (!world_model_seed_templates(cpu)) {
        invalidate_world_model_templates();
        return;
    }
    world_model_initializer =
        (XgRenderWorldModelInitializerContext){0};
    world_model_initializer_populated = false;
}

static void world_model_begin_packet_buffer_copy(CPUState *cpu) {
    uint32_t destination;
    uint32_t source;
    uint32_t size;

    if (state.requested_render_mode == GUEST_RENDER_RENDER_ORIGINAL) return;
    if (cpu != NULL && !physical_address_equals(
            cpu->gpr[31], UINT32_C(0x80084778)))
        return;
    if (world_models_native_snapshot.packet_copy_begin_count != UINT64_MAX)
        ++world_models_native_snapshot.packet_copy_begin_count;
    world_models_native_snapshot.packet_copy_failure_detail = 1u;
    if (cpu == NULL || cpu->read_word == NULL ||
        world_model_initializer.active || world_model_packet_copy.active)
        goto fail;
    destination = cpu->gpr[4];
    source = cpu->gpr[5];
    size = cpu->gpr[6];
    world_models_native_snapshot.packet_copy_last_destination = destination;
    world_models_native_snapshot.packet_copy_last_source = source;
    world_models_native_snapshot.packet_copy_last_size = size;
    world_models_native_snapshot.packet_copy_failure_detail = 2u;
    if (size == 0u || destination > UINT32_MAX - size ||
        source > UINT32_MAX - size ||
        !guest_data_range_is_valid(source, size, 4u, false) ||
        !guest_data_range_is_valid(destination, size, 4u, false))
        goto fail;
    world_model_packet_copy = (XgRenderWorldModelPacketCopyContext){
        .owner_cpu = cpu,
        .destination = destination,
        .source = source,
        .size = size,
        .active = true,
    };
    world_models_native_snapshot.packet_copy_failure_detail = 0u;
    return;

fail:
    invalidate_world_model_templates();
}

static void world_model_finish_packet_buffer_copy(CPUState *cpu) {
    XgRenderWorldModelPacketCopyContext copy;
    uint32_t destination;
    uint32_t destination_end;
    uint32_t source;
    uint32_t source_end;
    uint32_t size;
    uint32_t cursor;
    uint32_t primitive_ordinal = 0u;

    if (!world_model_packet_copy.active) return;
    copy = world_model_packet_copy;
    world_model_packet_copy = (XgRenderWorldModelPacketCopyContext){0};
    if (state.requested_render_mode == GUEST_RENDER_RENDER_ORIGINAL) return;
    if (world_models_native_snapshot.packet_copy_finish_count != UINT64_MAX)
        ++world_models_native_snapshot.packet_copy_finish_count;
    world_models_native_snapshot.packet_copy_failure_detail = 3u;
    if (cpu == NULL || cpu != copy.owner_cpu || cpu->read_word == NULL ||
        world_model_initializer.active || cpu->gpr[6] != 0u)
        goto fail;

    destination = copy.destination;
    source = copy.source;
    size = copy.size;
    destination_end = destination + size;
    source_end = source + size;
    world_models_native_snapshot.packet_copy_failure_detail = 4u;
    if (cpu->gpr[2] != destination || cpu->gpr[4] != destination_end ||
        cpu->gpr[5] != source_end)
        goto fail;
    world_models_native_snapshot.packet_copy_failure_detail = 5u;
    if (destination != source_end ||
        !guest_data_range_is_valid(source, size, 4u, false) ||
        !guest_data_range_is_valid(destination, size, 4u, false))
        goto fail;

    cursor = source;
    while (cursor < source_end) {
        const XgRenderWorldModelTemplate *entry =
            world_model_template_find(cursor, false);
        const uint32_t packet_size = entry != NULL
            ? (uint32_t)entry->word_count * 4u : 0u;
        uint32_t word;

        world_models_native_snapshot.packet_copy_failure_detail =
            entry == NULL ? 6u : 7u;
        if (entry == NULL || !entry->active || entry->resource_epoch == 0u ||
            entry->owner_cpu != cpu ||
            entry->packet_base != model_ft4_template_key(source) ||
            entry->packet_address != model_ft4_template_key(cursor) ||
            entry->primitive_ordinal != primitive_ordinal ||
            packet_size == 0u || packet_size > source_end - cursor)
            goto fail;
        world_models_native_snapshot.packet_copy_failure_detail = 8u;
        for (word = 0u; word < entry->word_count; ++word) {
            const uint32_t source_word = cpu->read_word(cursor + word * 4u);

            if (source_word != entry->words[word] ||
                cpu->read_word(destination + (cursor - source) + word * 4u) !=
                    source_word)
                goto fail;
        }
        cursor += packet_size;
        ++primitive_ordinal;
    }

    cursor = source;
    world_models_native_snapshot.packet_copy_failure_detail = 9u;
    while (cursor < source_end) {
        const XgRenderWorldModelTemplate *source_entry =
            world_model_template_find(cursor, false);
        XgRenderWorldModelTemplate *destination_entry =
            world_model_template_find(
                destination + (cursor - source), true);
        XgRenderWorldModelTemplate cloned;

        if (source_entry == NULL || destination_entry == NULL) goto fail;
        cloned = *source_entry;
        cloned.packet_base = model_ft4_template_key(destination);
        cloned.packet_address = model_ft4_template_key(
            destination + (cursor - source));
        *destination_entry = cloned;
        if (world_models_native_snapshot.packet_copy_template_count !=
                UINT64_MAX)
            ++world_models_native_snapshot.packet_copy_template_count;
        cursor += (uint32_t)cloned.word_count * 4u;
    }
    if (world_model_packet_copy_range_count < 64u) {
        world_model_packet_copy_ranges[world_model_packet_copy_range_count++] =
            (XgRenderWorldModelPacketCopyRange){
                .destination = destination,
                .source = source,
                .size = size,
            };
        world_models_native_snapshot.packet_copy_range_count =
            world_model_packet_copy_range_count;
    }
    world_models_native_snapshot.packet_copy_failure_detail = 0u;
    return;

fail:
    invalidate_world_model_templates();
}

static bool read_world_model_packet_template(
    void *context, uint32_t model_header_address,
    uint32_t packet_base_address, uint32_t packet_address,
    uint32_t attribute_address, uint8_t primitive_family,
    uint32_t *out_words, uint8_t word_count,
    uint64_t *out_resource_epoch) {
    XgRenderWorldModelTemplate *entry;
    if (out_words == NULL || out_resource_epoch == NULL ||
        primitive_family >= XG_WORLD_MODELS_PRIMITIVE_FAMILY_COUNT ||
        word_count == 0u ||
        word_count > XG_WORLD_MODELS_MAX_PACKET_WORD_COUNT) {
        world_model_template_read_failure =
            XG_RENDER_WORLD_MODEL_TEMPLATE_READ_INVALID_ARGUMENT;
        return false;
    }
    entry = world_model_template_find(packet_address, false);
    if (entry == NULL) {
        if (world_models_native_snapshot.first_missing_packet_address == 0u) {
            uint32_t range_index;

            world_models_native_snapshot.first_missing_model_address =
                model_header_address;
            world_models_native_snapshot.first_missing_packet_base =
                packet_base_address;
            world_models_native_snapshot.first_missing_packet_address =
                packet_address;
            for (range_index = 0u;
                 range_index < world_model_packet_copy_range_count;
                 ++range_index) {
                const XgRenderWorldModelPacketCopyRange *range =
                    &world_model_packet_copy_ranges[range_index];

                if (packet_address >= range->source &&
                    packet_address - range->source < range->size) {
                    world_models_native_snapshot.first_missing_copy_range_kind =
                        1u;
                    world_models_native_snapshot.first_missing_copy_range_index =
                        range_index;
                    break;
                }
                if (packet_address >= range->destination &&
                    packet_address - range->destination < range->size) {
                    world_models_native_snapshot.first_missing_copy_range_kind =
                        2u;
                    world_models_native_snapshot.first_missing_copy_range_index =
                        range_index;
                    break;
                }
            }
        }
        world_model_template_read_failure =
            XG_RENDER_WORLD_MODEL_TEMPLATE_READ_MISSING;
        return false;
    }
    if (!entry->active) {
        world_model_template_read_failure =
            XG_RENDER_WORLD_MODEL_TEMPLATE_READ_INACTIVE;
        return false;
    }
    if (entry->resource_epoch == 0u) {
        world_model_template_read_failure =
            XG_RENDER_WORLD_MODEL_TEMPLATE_READ_EPOCH;
        return false;
    }
    if (entry->owner_cpu != context) {
        world_model_template_read_failure =
            XG_RENDER_WORLD_MODEL_TEMPLATE_READ_OWNER;
        return false;
    }
    if (entry->model_address != model_ft4_template_key(model_header_address)) {
        world_model_template_read_failure =
            XG_RENDER_WORLD_MODEL_TEMPLATE_READ_MODEL;
        return false;
    }
    if (entry->packet_base != model_ft4_template_key(packet_base_address)) {
        world_model_template_read_failure =
            XG_RENDER_WORLD_MODEL_TEMPLATE_READ_PACKET_BASE;
        return false;
    }
    if (entry->primitive_family != primitive_family) {
        world_model_template_read_failure =
            XG_RENDER_WORLD_MODEL_TEMPLATE_READ_FAMILY;
        return false;
    }
    if (entry->word_count != word_count) {
        world_model_template_read_failure =
            XG_RENDER_WORLD_MODEL_TEMPLATE_READ_WORD_COUNT;
        return false;
    }
    if (entry->attribute_address != model_ft4_template_key(attribute_address)) {
        world_model_template_read_failure =
            XG_RENDER_WORLD_MODEL_TEMPLATE_READ_ATTRIBUTE;
        return false;
    }
    memcpy(out_words, entry->words, word_count * sizeof(out_words[0]));
    *out_resource_epoch = entry->resource_epoch;
    return true;
}

static void model_ft4_template_observe(CPUState *cpu) {
    XgRenderModelFt4Template captured = {0};
    XgRenderModelFt4Template *descriptor_entry;
    XgRenderModelFt4Template *packet_entry;
    const uint32_t descriptor_address = cpu != NULL ? cpu->gpr[16] : 0u;
    const uint32_t packet_address = cpu != NULL && cpu->read_word != NULL
        ? cpu->read_word(UINT32_C(0x80059424)) : 0u;

    if (cpu == NULL || cpu->read_word == NULL || cpu->read_half == NULL ||
        (state.requested_render_mode == GUEST_RENDER_RENDER_NATIVE &&
         !producer_lifecycle_begin(UINT32_C(0x8002d100),
                                   &captured.lifecycle)) ||
        !word_address_is_valid(descriptor_address) ||
        !word_address_is_valid(descriptor_address + 8u) ||
        !word_address_is_valid(packet_address))
        return;
    packet_entry = model_ft4_template_find_packet(packet_address, true);
    descriptor_entry = model_ft4_template_find_descriptor(
        descriptor_address, true);
    if (packet_entry == NULL || descriptor_entry == NULL) return;
    const bool new_packet = !model_ft4_template_is_current(packet_entry);
    const bool new_descriptor = !model_ft4_template_is_current(descriptor_entry);
    captured = (XgRenderModelFt4Template){
        .packet_address = model_ft4_template_key(packet_address),
        .descriptor_address = model_ft4_template_key(descriptor_address),
        .material_word = cpu->read_word(descriptor_address),
         .uv = {
             cpu->read_half(descriptor_address + 4u),
             cpu->read_half(descriptor_address + 6u),
             cpu->read_half(descriptor_address + 8u),
             cpu->read_half(descriptor_address + 10u),
         },
         .tpage = cpu->read_half(UINT32_C(0x80059308)),
         .clut = cpu->read_half(UINT32_C(0x8005930c)),
         .table_epoch = model_ft4_table_epoch,
         .valid = true,
     };
    model_ft4_packet_template_store(packet_entry, &captured);
    model_ft4_descriptor_template_store(descriptor_entry, &captured);
    if (new_packet) ++model_ft4_template_count;
    if (new_descriptor) ++model_ft4_descriptor_template_count;
    watch_producer_resource(packet_address, 0x28u);
    watch_producer_resource(descriptor_address, 12u);
    ++model_ft4_shadow.snapshot.template_capture_count;
}

static void model_ft3_template_observe(CPUState *cpu) {
    XgRenderModelFt4Template captured = { 0 };
    XgRenderModelFt4Template *descriptor_entry;
    XgRenderModelFt4Template *packet_entry;
    const uint32_t descriptor_address = cpu != NULL ? cpu->gpr[16] : 0u;
    const uint32_t packet_address = cpu != NULL && cpu->read_word != NULL
        ? cpu->read_word(UINT32_C(0x80059424)) : 0u;

    if (cpu == NULL || cpu->read_word == NULL || cpu->read_half == NULL ||
        cpu->read_byte == NULL ||
        (state.requested_render_mode == GUEST_RENDER_RENDER_NATIVE &&
         !producer_lifecycle_begin(UINT32_C(0x8002da00),
                                   &captured.lifecycle)) ||
        !word_address_is_valid(descriptor_address) ||
        !word_address_is_valid(descriptor_address + 8u) ||
        !word_address_is_valid(packet_address))
        return;
    packet_entry = model_ft4_template_find_packet(packet_address, true);
    descriptor_entry = model_ft4_template_find_descriptor(
        descriptor_address, true);
    if (packet_entry == NULL || descriptor_entry == NULL) return;
    const bool new_packet = !model_ft4_template_is_current(packet_entry);
    const bool new_descriptor = !model_ft4_template_is_current(descriptor_entry);
    captured.packet_address = model_ft4_template_key(packet_address);
    captured.descriptor_address = model_ft4_template_key(descriptor_address);
    captured.material_word = UINT32_C(0x00808080) |
        ((uint32_t)cpu->read_byte(descriptor_address + 3u) << 24u);
    captured.uv[0] = cpu->read_half(descriptor_address + 4u);
    captured.uv[1] = cpu->read_half(descriptor_address + 6u);
    captured.uv[2] = cpu->read_half(descriptor_address);
    captured.tpage = cpu->read_half(UINT32_C(0x80059308));
    captured.clut = cpu->read_half(UINT32_C(0x8005930c));
    captured.table_epoch = model_ft4_table_epoch;
    captured.valid = true;
    model_ft4_packet_template_store(packet_entry, &captured);
    model_ft4_descriptor_template_store(descriptor_entry, &captured);
    if (new_packet) ++model_ft4_template_count;
    if (new_descriptor) ++model_ft4_descriptor_template_count;
    watch_producer_resource(packet_address, 0x20u);
    watch_producer_resource(descriptor_address, 10u);
    ++model_ft3_shadow.snapshot.template_capture_count;
}

static void model_ft4_shadow_begin(CPUState *cpu) {
    XgRenderModelFt4ShadowContext context = { 0 };
    XgHost3dMatrix matrix;
    uint32_t matrix_stack_offset;
    uint32_t matrix_mismatch_mask = 0u;
    uint8_t caller_contract = XG_MODEL_DISPATCH_CALLER_NONE;
    uint32_t caller_window_start = 0u;
    const bool caller_authenticated = model_dispatch_caller_contract_matches(
        cpu, cpu != NULL ? cpu->gpr[31] : 0u, &caller_contract,
        &caller_window_start, &matrix_stack_offset);

    if (state.requested_render_mode != GUEST_RENDER_RENDER_SHADOW &&
        state.requested_render_mode != GUEST_RENDER_RENDER_NATIVE) {
        clear_model_ft4_shadow_pending();
        clear_model_ft3_shadow_pending();
        return;
    }
    if (state.requested_render_mode == GUEST_RENDER_RENDER_NATIVE &&
        !caller_authenticated) {
        clear_model_ft4_shadow_pending();
        clear_model_ft3_shadow_pending();
        return;
    }
    if (model_ft4_shadow.snapshot.blocked) return;
    if (model_ft4_shadow.snapshot.pending) {
        block_model_ft4_shadow(70u);
        return;
    }
    if (model_ft3_shadow.snapshot.pending) {
        block_model_ft3_shadow(70u);
        return;
    }
    if (cpu == NULL || cpu->read_word == NULL || cpu->read_half == NULL ||
        cpu->read_byte == NULL) {
        block_model_ft4_shadow(71u);
        return;
    }
    ++model_ft4_shadow.snapshot.dispatch_begin_count;
    model_ft4_shadow.snapshot.last_dispatch_caller = cpu->gpr[31];
    model_ft4_shadow.snapshot.last_dispatch_mode = cpu->gpr[7];
    if (!caller_authenticated) {
        ++model_ft4_shadow.snapshot.dispatch_caller_reject_count;
        return;
    }
    if (cpu->gpr[7] != XG_MODEL_FT4_RAW_DISPATCH_AVERAGE &&
        cpu->gpr[7] != XG_MODEL_FT4_RAW_DISPATCH_FARTHEST) {
        ++model_ft4_shadow.snapshot.dispatch_mode_reject_count;
        return;
    }
    if (!stack_address_is_valid(cpu->gpr[29]) ||
        !capture_particle_matrix(
            cpu, cpu->gpr[29] + matrix_stack_offset, &matrix)) {
        block_model_ft4_shadow(71u);
        return;
    }
    context.model_address = cpu->gpr[4];
    /* Both resident and overlay callers retain the
     * logical model-instance pointer in s0 across the dispatch. */
    context.instance_address = cpu->gpr[16];
    context.packet_base = cpu->gpr[5];
    context.ot_base = cpu->gpr[6];
    if (!word_address_is_valid(context.model_address) ||
        !word_address_is_valid(context.model_address + 0x14u) ||
        !word_address_is_valid(context.packet_base) ||
        !word_address_is_valid(context.ot_base)) {
        block_model_ft4_shadow(72u);
        return;
    }
    context.vertex_base = cpu->read_word(context.model_address + 8u);
    context.topology_base = cpu->read_word(context.model_address + 0x10u);
    context.material_base = cpu->read_word(context.model_address + 0x14u);
    model_ft4_shadow.snapshot.last_model_address = context.model_address;
    model_ft4_shadow.snapshot.last_topology_base = context.topology_base;
    model_ft4_shadow.snapshot.last_material_base = context.material_base;
    if (!word_address_is_valid(context.vertex_base) ||
        !word_address_is_valid(context.topology_base) ||
        !word_address_is_valid(context.material_base)) {
        block_model_ft4_shadow(72u);
        return;
    }
    capture_shadow_projection(cpu, &context.projection);
    if (memcmp(context.projection.rotation, matrix.rotation,
               sizeof(context.projection.rotation)) != 0)
        matrix_mismatch_mask |= 1u;
    if (memcmp(context.projection.translation, matrix.translation,
               sizeof(context.projection.translation)) != 0)
        matrix_mismatch_mask |= 2u;
    model_ft4_shadow.snapshot.last_projection_matrix_mismatch_mask =
        matrix_mismatch_mask;
    if (matrix_mismatch_mask != 0u)
        ++model_ft4_shadow.snapshot.projection_matrix_mismatch_count;
    memcpy(context.projection.rotation, matrix.rotation,
           sizeof(context.projection.rotation));
    memcpy(context.projection.translation, matrix.translation,
           sizeof(context.projection.translation));
    context.tpage = cpu->read_half(UINT32_C(0x80059308));
    context.clut = cpu->read_half(UINT32_C(0x8005930c));
    context.dispatch_mode = (uint8_t)cpu->gpr[7];
    context.caller_window_start = caller_window_start;
    context.caller_contract = caller_contract;
    context.resident_dispatch =
        caller_contract == XG_MODEL_DISPATCH_CALLER_RESIDENT;
    context.valid = true;
    model_ft4_shadow.context = context;
}

static bool model_ft4_shadow_apply_material(
    CPUState *cpu, XgModelFt4RawSource *source,
    XgRenderModelFt4ShadowRecord *record) {
    GpuDrawState draw = { 0 };
    uint32_t vertex;

    if ((record->material_word >> 24u) != 0x2du &&
        (record->material_word >> 24u) != 0x2fu)
        return false;
    gpu_get_draw_state(&draw);
    apply_draw_state(&source->material, &draw);
    source->material.tpage = record->tpage;
    source->material.texture_page_x = record->tpage & 0x0fu;
    source->material.texture_page_y = (record->tpage >> 4u) & 1u;
    source->material.texture_depth =
        (XgRenderIrTextureDepth)((record->tpage >> 7u) & 3u);
    source->material.blend_mode =
        (XgRenderIrBlendMode)((record->tpage >> 5u) & 3u);
    source->material.clut_x = (record->clut & 0x3fu) << 4u;
    source->material.clut_y = record->clut >> 6u;
    source->material.shading = XG_RENDER_IR_SHADING_FLAT;
    source->material.textured = true;
    source->material.raw_texture = true;
    source->material.semi_transparent =
        ((record->material_word >> 24u) & 2u) != 0u;
    for (vertex = 0u; vertex < 4u; ++vertex) {
        source->uv[vertex][0] = (uint8_t)record->uv[vertex];
        source->uv[vertex][1] = (uint8_t)(record->uv[vertex] >> 8u);
    }
    (void)cpu;
    return true;
}

static bool model_ft4_shadow_decode_material(
    CPUState *cpu, uint32_t attribute_address, uint16_t tpage, uint16_t clut,
    XgModelFt4RawSource *source, XgRenderModelFt4ShadowRecord *record) {
    if (cpu == NULL || source == NULL || record == NULL ||
        cpu->read_word == NULL || cpu->read_half == NULL ||
        cpu->read_byte == NULL || !word_address_is_valid(attribute_address) ||
        !word_address_is_valid(attribute_address + 8u))
        return false;
    record->material_word = cpu->read_word(attribute_address);
    record->uv[0] = cpu->read_half(attribute_address + 4u);
    record->uv[1] = cpu->read_half(attribute_address + 6u);
    record->uv[2] = cpu->read_half(attribute_address + 8u);
    record->uv[3] = cpu->read_half(attribute_address + 10u);
    record->tpage = tpage;
    record->clut = clut;
    return model_ft4_shadow_apply_material(cpu, source, record);
}

static bool model_ft4_shadow_capture_packet_material(
    CPUState *cpu, uint32_t packet_address, XgModelFt4RawSource *source,
    XgRenderModelFt4ShadowRecord *record) {
    uint32_t material_word;

    if (cpu == NULL || source == NULL || record == NULL ||
        cpu->read_word == NULL || cpu->read_half == NULL ||
        !word_address_is_valid(packet_address) ||
        !word_address_is_valid(packet_address + 36u))
        return false;
    material_word = cpu->read_word(packet_address + 4u);
    if ((material_word >> 24u) != 0x2du &&
        (material_word >> 24u) != 0x2fu)
        return false;
    record->material_word = material_word;
    record->uv[0] = cpu->read_half(packet_address + 12u);
    record->uv[1] = cpu->read_half(packet_address + 20u);
    record->uv[2] = cpu->read_half(packet_address + 28u);
    record->uv[3] = cpu->read_half(packet_address + 36u);
    record->tpage = cpu->read_half(packet_address + 22u);
    record->clut = cpu->read_half(packet_address + 14u);
    return model_ft4_shadow_apply_material(cpu, source, record);
}

static uint32_t model_shadow_prepare_precondition_mask(
        CPUState *cpu, const XgRenderModelFt4ShadowContext *context) {
    uint32_t mask = 0u;

    if (context == NULL || !context->valid)
        mask |= XG_RENDER_MODEL_PRECONDITION_CONTEXT;
    if (cpu == NULL)
        return mask | XG_RENDER_MODEL_PRECONDITION_CPU;
    if (cpu->read_word == NULL || cpu->read_half == NULL ||
        cpu->read_byte == NULL)
        mask |= XG_RENDER_MODEL_PRECONDITION_CALLBACKS;
    if (!physical_address_equals(cpu->gpr[31], UINT32_C(0x8002c86c)))
        mask |= XG_RENDER_MODEL_PRECONDITION_RETURN;
    if (cpu->gpr[5] == 0u)
        mask |= XG_RENDER_MODEL_PRECONDITION_TARGET_ZERO;
    if (cpu->gpr[5] > XG_RENDER_MODEL_FT4_SHADOW_CAPACITY)
        mask |= XG_RENDER_MODEL_PRECONDITION_TARGET_CAPACITY;
    if (context != NULL && context->valid && context->resident_dispatch &&
        cpu->read_word != NULL) {
        if (cpu->read_word(UINT32_C(0x8005953c)) != context->vertex_base)
            mask |= XG_RENDER_MODEL_PRECONDITION_VERTEX_BASE;
        if (cpu->read_word(UINT32_C(0x80059568)) != context->ot_base)
            mask |= XG_RENDER_MODEL_PRECONDITION_OT_BASE;
    }
    return mask;
}

static bool model_ft4_shadow_prepare(CPUState *cpu) {
    static const uint8_t attribute_sizes[17] = {
        4u, 8u, 4u, 8u, 4u, 8u, 4u, 8u, 4u,
        12u, 4u, 12u, 4u, 12u, 4u, 12u, 4u,
    };
    static const uint8_t split[2][3] = {{0u, 1u, 2u}, {2u, 1u, 3u}};
    XgRenderModelFt4ShadowContext *context = &model_ft4_shadow.context;
    uint32_t group_address;
    uint32_t attribute_address;
    uint32_t group_count;
    uint32_t group;
    uint32_t target_count;
    uint16_t tpage;
    uint16_t clut;
    bool found = false;
    uint32_t precondition_mask;

    model_ft4_shadow.snapshot.prepare_failure_detail = 0u;
    model_ft4_shadow.snapshot.last_target_count =
        cpu != NULL ? cpu->gpr[5] : 0u;
    precondition_mask = model_shadow_prepare_precondition_mask(cpu, context);
    model_ft4_shadow.snapshot.prepare_precondition_failure_mask =
        precondition_mask;
    if (precondition_mask != 0u) {
        model_ft4_shadow.snapshot.prepare_failure_detail = 1u;
        return false;
    }
    target_count = cpu->gpr[5];
    group_address = context->topology_base;
    attribute_address = context->material_base;
    group_count = cpu->read_half(context->model_address + 6u);
    model_ft4_shadow.snapshot.last_group_count = group_count;
    model_ft4_shadow.snapshot.last_target_count = target_count;
    tpage = context->tpage;
    clut = context->clut;
    if (group_count == 0u || group_count > 256u) {
        model_ft4_shadow.snapshot.prepare_failure_detail = 2u;
        return false;
    }

    for (group = 0u; group < group_count; ++group) {
        const uint8_t row = cpu->read_byte(group_address);
        const uint32_t primitive_count = cpu->read_half(group_address + 2u);
        const uint32_t descriptors = group_address + 4u;
        uint32_t primitive;

        if (row >= 17u || primitive_count > 4096u ||
            descriptors > UINT32_MAX - primitive_count * 8u) {
            model_ft4_shadow.snapshot.prepare_failure_detail = 3u;
            return false;
        }
        for (primitive = 0u; primitive < primitive_count; ++primitive) {
            if (!model_ft4_shadow_consume_controls(
                    cpu, &attribute_address, &tpage, &clut)) {
                model_ft4_shadow.snapshot.prepare_failure_detail = 4u;
                return false;
            }
            if (descriptors == cpu->gpr[4]) {
                XgRenderModelFt4ShadowRecord *record;
                XgModelFt4RawSource source = { 0 };
                uint32_t vertex;

                if (row != 13u || primitive_count != target_count ||
                    primitive >= XG_RENDER_MODEL_FT4_SHADOW_CAPACITY) {
                    model_ft4_shadow.snapshot.prepare_failure_detail = 5u;
                    return false;
                }
                found = true;
                record = &model_ft4_shadow.records[primitive];
                *record = (XgRenderModelFt4ShadowRecord){ 0 };
                record->attribute_address = attribute_address;
                model_ft4_shadow.snapshot.last_attribute_address =
                    attribute_address;
                model_ft4_shadow.snapshot.last_material_word =
                    cpu->read_word(attribute_address);
                source.projection = context->projection;
                source.screen_right = (int16_t)cpu->read_word(
                    UINT32_C(0x800500f8));
                source.screen_x_cull_margin =
                    render_screen_x_cull_margin();
                source.packed_screen_bottom = cpu->read_word(
                    UINT32_C(0x800500fc));
                source.packet_address = cpu->read_word(
                    UINT32_C(0x80059424)) + primitive * 0x28u;
                source.ordering_shift = cpu->read_word(
                    UINT32_C(0x80050100));
                source.dispatch_mode = context->dispatch_mode;
                record->packet_address = source.packet_address;
                XgRenderModelFt4Template *material_template =
                    model_ft4_template_find_packet(
                        source.packet_address, false);
                if (material_template == NULL)
                    material_template = model_ft4_template_find_descriptor(
                        attribute_address, false);
                if (material_template != NULL) {
                    record->attribute_address =
                        material_template->descriptor_address;
                    record->material_word = material_template->material_word;
                    memcpy(record->uv, material_template->uv,
                           sizeof(record->uv));
                    record->tpage = material_template->tpage;
                    record->clut = material_template->clut;
                    ++model_ft4_shadow.snapshot.template_hit_count;
                    if (!model_ft4_shadow_apply_material(
                            cpu, &source, record)) {
                        model_ft4_shadow.snapshot.prepare_failure_detail = 6u;
                        return false;
                    }
                } else {
                    ++model_ft4_shadow.snapshot.template_miss_count;
                    if (!model_ft4_shadow_capture_packet_material(
                            cpu, source.packet_address, &source, record) &&
                        !model_ft4_shadow_decode_material(
                            cpu, attribute_address, tpage, clut, &source,
                            record)) {
                        model_ft4_shadow.snapshot.prepare_failure_detail = 6u;
                        return false;
                    }
                }
                for (vertex = 0u; vertex < 4u; ++vertex) {
                    const uint32_t index = cpu->read_half(
                        descriptors + primitive * 8u + vertex * 2u);
                    const uint32_t address = context->vertex_base + index * 8u;
                    const uint32_t xy = cpu->read_word(address);
                    const uint32_t zp = cpu->read_word(address + 4u);

                    record->source_vertex_indices[vertex] = index;
                    source.vertices[vertex] = (XgHost3dVector){
                        low_s16(xy), low_s16(xy >> 16u), low_s16(zp),
                        (uint16_t)(zp >> 16u),
                    };
                }
                if (xg_model_ft4_raw_build(&source, &record->native) !=
                    XG_MODEL_FT4_RAW_OK) {
                    model_ft4_shadow.snapshot.prepare_failure_detail = 7u;
                    return false;
                }
                for (uint32_t triangle = 0u; triangle < 2u; ++triangle) {
                    for (vertex = 0u; vertex < 3u; ++vertex) {
                        XgRenderIrVertex *destination = &record->native.primitive
                            .triangles[triangle].vertices[vertex];
                        const uint32_t group_id =
                            context->instance_address & UINT32_C(0x1fffffff);

                        destination->interpolation_group_id = group_id;
                        destination->interpolation_vertex_id =
                            record->source_vertex_indices[split[triangle][vertex]];
                        destination->interpolation_vertex_identity_valid =
                            group_id != 0u;
                    }
                }
            }
            attribute_address += attribute_sizes[row];
        }
        group_address = descriptors + primitive_count * 8u;
        if (found) break;
    }
    if (!found) {
        model_ft4_shadow.snapshot.prepare_failure_detail = 8u;
        return false;
    }

    model_ft4_shadow.initial_packet_cursor = cpu->read_word(
        UINT32_C(0x80059424));
    model_ft4_shadow.initial_counter = cpu->read_word(
        UINT32_C(0x80059578));
    model_ft4_shadow.descriptor_base = cpu->gpr[4];
    model_ft4_shadow.count = target_count;
    for (uint32_t index = 0u; index < target_count; ++index) {
        XgRenderModelFt4ShadowRecord *record =
            &model_ft4_shadow.records[index];

        if (!record->native.accepted) continue;
        if (record->native.ordering_bucket >= 0x1000u ||
            context->ot_base > UINT32_MAX -
                record->native.ordering_bucket * 4u ||
            !word_address_is_valid(context->ot_base +
                record->native.ordering_bucket * 4u)) {
            model_ft4_shadow.snapshot.prepare_failure_detail = 9u;
            return false;
        }
    }
    model_ft4_shadow.snapshot.pending = true;
    return true;
}

static bool native_model_ft4_raw_stage(CPUState *cpu) {
    uint32_t accepted_count = 0u;
    uint32_t staged_count = 0u;
    uint32_t index;
    XgRenderProducerLifecycle lifecycle;

    if (state.requested_render_mode != GUEST_RENDER_RENDER_NATIVE ||
        !model_dispatch_context_contract_matches(
            cpu, &model_ft4_shadow.context) ||
        cpu == NULL || !model_ft4_shadow.context.valid ||
        !model_ft4_shadow_prepare(cpu) ||
        !producer_lifecycle_begin(UINT32_C(0x8002c700), &lifecycle))
        return false;
    for (index = 0u; index < model_ft4_shadow.count; ++index) {
        accepted_count += model_ft4_shadow.records[index].native.accepted;
        staged_count += model_ft4_shadow.records[index].native.accepted ||
            native_primitive_all_projective(
                &model_ft4_shadow.records[index].native.primitive);
    }
    if (pre_scene.blocked ||
        staged_count > XG_RENDER_PRE_SCENE_PRIMITIVE_CAPACITY ||
        pre_scene.count >
            XG_RENDER_PRE_SCENE_PRIMITIVE_CAPACITY - staged_count ||
        model_ft4_shadow.snapshot.native_cutover_count == UINT64_MAX ||
        model_ft4_shadow.snapshot.native_primitive_count >
            UINT64_MAX - accepted_count) {
        block_model_ft4_shadow(77u);
        return false;
    }

    for (index = 0u; index < model_ft4_shadow.count; ++index) {
        XgRenderModelFt4ShadowRecord *record =
            &model_ft4_shadow.records[index];
        const uint32_t interpolation_producer_id =
            model_ft4_shadow.context.instance_address & UINT32_C(0x1fffffff);
        const uint32_t interpolation_primitive_id =
            (record->attribute_address & UINT32_C(0x1fffffff)) |
            ((uint32_t)(model_ft4_shadow.context.dispatch_mode & 7u) << 29u);
        const bool interpolation_identity_valid =
            interpolation_producer_id != 0u;
        const int32_t native_margin = render_screen_x_cull_margin();
        const uint32_t ordering_shift =
            cpu->read_word(UINT32_C(0x80050100));
        const bool average_depth =
            model_ft4_shadow.context.dispatch_mode ==
                XG_MODEL_FT4_RAW_DISPATCH_AVERAGE ||
            model_ft4_shadow.context.dispatch_mode ==
                XG_MODEL_FT4_RAW_DISPATCH_AVERAGE_DEPTH_CUE;
        const GpuRenderTemporalCullPolicy temporal_cull = {
            .flags = GPU_RENDER_TEMPORAL_CULL_PROJECTIVE |
                GPU_RENDER_TEMPORAL_CULL_SCREEN |
                GPU_RENDER_TEMPORAL_CULL_FRONT_FACE |
                GPU_RENDER_TEMPORAL_CULL_DEPTH,
            .screen_left = native_margin > 0
                ? -native_margin * INT32_C(65536) : INT32_MIN,
            .screen_top = INT32_MIN,
            .screen_right_exclusive =
                ((int32_t)(uint16_t)cpu->read_word(UINT32_C(0x800500f8)) +
                 native_margin) * INT32_C(65536),
            .screen_bottom_exclusive =
                (int32_t)(uint16_t)(cpu->read_word(UINT32_C(0x800500fc)) >> 16u) *
                    INT32_C(65536),
            .depth_min_inclusive = 1,
            .depth_max_exclusive = 0x10000,
            .depth_mode = average_depth
                ? GPU_RENDER_TEMPORAL_DEPTH_AVERAGE
                : (model_ft4_shadow.context.dispatch_mode ==
                       XG_MODEL_FT4_RAW_DISPATCH_NEAREST
                    ? GPU_RENDER_TEMPORAL_DEPTH_MINIMUM
                    : GPU_RENDER_TEMPORAL_DEPTH_MAXIMUM),
            .front_face = GPU_RENDER_TEMPORAL_FRONT_POSITIVE,
            .ordering_depth_shift = (uint8_t)(average_depth
                ? ordering_shift : ((ordering_shift + 2u) & 31u)),
        };

        record->lifecycle = lifecycle;
        record->interpolation_producer_id = interpolation_producer_id;
        record->interpolation_primitive_id = interpolation_primitive_id;
        record->interpolation_identity_valid = interpolation_identity_valid;
        if (!record->native.accepted) {
            if (!interpolation_identity_valid ||
                !native_primitive_all_projective(&record->native.primitive))
                continue;
            if (!stage_pre_scene_primitive(&(XgRenderPreScenePrimitive){
                    .primitive = record->native.primitive,
                    .interpolation_producer_id = interpolation_producer_id,
                    .interpolation_primitive_id = interpolation_primitive_id,
                    .interpolation_identity_valid = true,
                    .temporal_only = true,
                    .temporal_cull = temporal_cull,
                })) {
                block_model_ft4_shadow(78u);
                return false;
            }
            continue;
        }
        if (!stage_pre_scene_primitive(&(XgRenderPreScenePrimitive){
                .primitive = record->native.primitive,
                .packet_address = record->packet_address,
                .source_primitive_index = UINT32_C(0x50000000) |
                    (record->packet_address & UINT32_C(0x001ffffc)),
                .ot_bucket = record->native.ordering_bucket,
                .interpolation_producer_id = interpolation_producer_id,
                .interpolation_primitive_id = interpolation_primitive_id,
                .payload_word_count = 9u,
                .interpolation_identity_valid = interpolation_identity_valid,
            })) {
            block_model_ft4_shadow(78u);
            return false;
        }
    }
    ++model_ft4_shadow.snapshot.native_cutover_count;
    model_ft4_shadow.snapshot.native_primitive_count += accepted_count;
    return true;
}

static void model_ft4_observe_guest_pass(CPUState *cpu, bool average_mode) {
    static const uint8_t split[2][3] = {{0u, 1u, 2u}, {2u, 1u, 3u}};
    uint32_t descriptor_base;
    uint32_t next_descriptor;
    uint32_t descriptor_offset;
    uint32_t primitive_index;
    uint32_t ordering_depth;
    uint32_t ordering_bucket;
    XgRenderModelFt4ShadowRecord *record;

    if (!model_ft4_shadow.snapshot.pending || cpu == NULL ||
        cpu->read_word == NULL)
        return;
    if ((average_mode && model_ft4_shadow.context.dispatch_mode !=
             XG_MODEL_FT4_RAW_DISPATCH_AVERAGE) ||
        (!average_mode && model_ft4_shadow.context.dispatch_mode !=
             XG_MODEL_FT4_RAW_DISPATCH_FARTHEST)) {
        block_model_ft4_shadow(79u);
        return;
    }
    descriptor_base = model_ft4_shadow.descriptor_base & UINT32_C(0x1fffffff);
    next_descriptor = cpu->gpr[4] & UINT32_C(0x1fffffff);
    if (next_descriptor < descriptor_base + 8u) {
        block_model_ft4_shadow(79u);
        return;
    }
    descriptor_offset = next_descriptor - descriptor_base - 8u;
    if ((descriptor_offset & 7u) != 0u) {
        block_model_ft4_shadow(79u);
        return;
    }
    primitive_index = descriptor_offset / 8u;
    if (primitive_index >= model_ft4_shadow.count) {
        block_model_ft4_shadow(79u);
        return;
    }
    record = &model_ft4_shadow.records[primitive_index];
    if (record->guest_observed_passed_screen_cull) {
        block_model_ft4_shadow(80u);
        return;
    }
    record->guest_observed_passed_screen_cull = true;
    ++model_ft4_shadow.expected_counter_delta;
    ++model_ft4_shadow.snapshot.guest_pass_observation_count;
    if (!record->native.passed_screen_cull)
        ++model_ft4_shadow.snapshot.guest_pass_projection_disagreement_count;

    if (average_mode) {
        ordering_depth = (uint16_t)cpu->gpr[8];
    } else {
        ordering_depth = (uint16_t)cpu->gte_data[16];
        for (uint32_t depth = 17u; depth <= 19u; ++depth) {
            if ((uint16_t)cpu->gte_data[depth] > ordering_depth)
                ordering_depth = (uint16_t)cpu->gte_data[depth];
        }
    }
    ordering_bucket = ordering_depth >>
        ((cpu->read_word(UINT32_C(0x80050100)) +
          (average_mode ? 0u : 2u)) & 31u);
    if (ordering_depth != 0u) {
        const uint32_t ot_base = model_ft4_shadow.context.ot_base;

        if (ordering_bucket >= 0x1000u ||
            ot_base > UINT32_MAX - ordering_bucket * 4u ||
            !word_address_is_valid(ot_base + ordering_bucket * 4u)) {
            block_model_ft4_shadow(81u);
            return;
        }
        record->native.ordering_bucket = (uint16_t)ordering_bucket;
        record->expected_tag = cpu->read_word(
            ot_base + ordering_bucket * 4u) | UINT32_C(0x09000000);
        record->guest_observed_accepted = true;
    }
    for (uint32_t vertex = 0u; vertex < 4u; ++vertex) {
        record->observed_xy[vertex] = average_mode
            ? cpu->gpr[9u + vertex]
            : cpu->read_word(record->packet_address + 8u + vertex * 8u);
        record->native.vertices[vertex].x =
            low_s16(record->observed_xy[vertex]);
        record->native.vertices[vertex].y =
            low_s16(record->observed_xy[vertex] >> 16u);
    }
    for (uint32_t triangle = 0u; triangle < 2u; ++triangle) {
        for (uint32_t vertex = 0u; vertex < 3u; ++vertex) {
            const uint32_t source = split[triangle][vertex];
            XgRenderIrVertex *destination =
                &record->native.primitive.triangles[triangle].vertices[vertex];

            destination->x =
                (int32_t)low_s16(record->observed_xy[source]) * INT32_C(65536);
            destination->y =
                (int32_t)low_s16(record->observed_xy[source] >> 16u) *
                    INT32_C(65536);
        }
    }
}

enum {
    XG_RENDER_FT4_PAYLOAD_MATERIAL = 1u << 0,
    XG_RENDER_FT4_PAYLOAD_UV0 = 1u << 1,
    XG_RENDER_FT4_PAYLOAD_UV1 = 1u << 2,
    XG_RENDER_FT4_PAYLOAD_UV2 = 1u << 3,
    XG_RENDER_FT4_PAYLOAD_UV3 = 1u << 4,
    XG_RENDER_FT4_PAYLOAD_TPAGE = 1u << 5,
    XG_RENDER_FT4_PAYLOAD_CLUT = 1u << 6,
};

static bool compare_ft4_payload(
    CPUState *cpu, uint32_t packet_address, uint32_t descriptor_address,
    uint32_t expected_material_word, const uint16_t expected_uv[4],
    uint16_t expected_tpage, uint16_t expected_clut,
    PsxXgRenderFt4PayloadMismatch *first_mismatch) {
    PsxXgRenderFt4PayloadMismatch mismatch = { 0 };

    mismatch.packet_address = packet_address;
    mismatch.descriptor_address = descriptor_address;
    mismatch.expected_material_word = expected_material_word;
    mismatch.actual_material_word = cpu->read_word(packet_address + 4u);
    mismatch.expected_tpage = expected_tpage;
    mismatch.actual_tpage = cpu->read_half(packet_address + 22u);
    mismatch.expected_clut = expected_clut;
    mismatch.actual_clut = cpu->read_half(packet_address + 14u);
    if (mismatch.actual_material_word != expected_material_word)
        mismatch.field_bits |= XG_RENDER_FT4_PAYLOAD_MATERIAL;
    if (mismatch.actual_tpage != expected_tpage)
        mismatch.field_bits |= XG_RENDER_FT4_PAYLOAD_TPAGE;
    if (mismatch.actual_clut != expected_clut)
        mismatch.field_bits |= XG_RENDER_FT4_PAYLOAD_CLUT;
    for (uint32_t vertex = 0u; vertex < 4u; ++vertex) {
        mismatch.expected_uv[vertex] = expected_uv[vertex];
        mismatch.actual_uv[vertex] = cpu->read_half(
            packet_address + 12u + vertex * 8u);
        if (mismatch.actual_uv[vertex] != expected_uv[vertex])
            mismatch.field_bits |= XG_RENDER_FT4_PAYLOAD_UV0 << vertex;
    }
    if (mismatch.field_bits != 0u && first_mismatch != NULL &&
        first_mismatch->field_bits == 0u)
        *first_mismatch = mismatch;
    return mismatch.field_bits == 0u;
}

static bool publish_model_ft4_sources(void) {
    for (uint32_t index = 0u; index < model_ft4_shadow.count; ++index) {
        const XgRenderModelFt4ShadowRecord *record =
            &model_ft4_shadow.records[index];
        const uint32_t source_id =
            (record->packet_address & UINT32_C(0x1fffffff)) + 4u;
        XgRenderModelFt4SourceRecord *source;

        if (!record->guest_observed_accepted || !record->output_validated)
            continue;
        source = model_ft4_source_upsert(source_id);
        if (source == NULL) return false;
        *source = (XgRenderModelFt4SourceRecord){
            .primitive = record->native.primitive,
            .lifecycle = record->lifecycle,
            .source_id = source_id,
            .interpolation_producer_id = record->interpolation_producer_id,
            .interpolation_primitive_id = record->interpolation_primitive_id,
            .opcode = (uint8_t)(record->material_word >> 24u),
            .interpolation_identity_valid =
                record->interpolation_identity_valid,
            .valid = true,
        };
        xg_render_lookup_put(
            model_ft4_source_lookup, model_ft4_source_lookup_epoch,
            source_id, (uint32_t)(source - model_ft4_sources));
        native_resolve_hint_put(source_id, XG_NATIVE_RESOLVE_MODEL_FT4);
        watch_producer_resource(record->packet_address, 0x28u);
        ++model_ft4_shadow.snapshot.publish_source_count;
    }
    return true;
}

static void model_ft4_shadow_finish(CPUState *cpu) {
    XgRenderModelFt4ShadowContext context;
    uint32_t index;
    bool framing_matches = true;
    uint32_t actual_counter_delta;

    if (!model_ft4_shadow.snapshot.pending) return;
    if (cpu == NULL || cpu->read_word == NULL || cpu->read_half == NULL) {
        block_model_ft4_shadow(73u);
        return;
    }
    ++model_ft4_shadow.snapshot.invocation_count;
    model_ft4_shadow.snapshot.last_primitive_count = model_ft4_shadow.count;
    model_ft4_shadow.snapshot.primitive_count += model_ft4_shadow.count;
    for (index = 0u; index < model_ft4_shadow.count; ++index) {
        XgRenderModelFt4ShadowRecord *record =
            &model_ft4_shadow.records[index];
        bool payload_matches = compare_ft4_payload(
            cpu, record->packet_address, record->attribute_address,
            record->material_word, record->uv, record->tpage, record->clut,
            &model_ft4_shadow.snapshot.first_payload_mismatch);
        bool geometry_matches = true;
        bool tag_matches = true;
        bool ot_matches = true;

        if (record->guest_observed_accepted) {
            uint32_t vertex;
            bool last_in_bucket = true;

            tag_matches = cpu->read_word(record->packet_address) ==
                record->expected_tag;
            for (vertex = 0u; vertex < 4u; ++vertex) {
                geometry_matches &= cpu->read_word(
                    record->packet_address + 8u + vertex * 8u) ==
                        record->observed_xy[vertex];
            }
            for (uint32_t later = index + 1u;
                 later < model_ft4_shadow.count; ++later) {
                if (model_ft4_shadow.records[later].guest_observed_accepted &&
                    model_ft4_shadow.records[later].native.ordering_bucket ==
                        record->native.ordering_bucket)
                    last_in_bucket = false;
            }
            if (last_in_bucket)
                ot_matches = cpu->read_word(model_ft4_shadow.context.ot_base +
                    record->native.ordering_bucket * 4u) ==
                    (record->packet_address & 0x00ffffffu);
        }
        const bool matches = payload_matches && geometry_matches &&
            tag_matches && ot_matches;
        record->output_validated =
            record->guest_observed_accepted && matches;
        if (record->guest_observed_accepted && !matches)
            ++model_ft4_shadow.snapshot.validation_rejected_source_count;
        if (!payload_matches)
            ++model_ft4_shadow.snapshot.payload_mismatch_count;
        if (!geometry_matches)
            ++model_ft4_shadow.snapshot.geometry_mismatch_count;
        if (!tag_matches)
            ++model_ft4_shadow.snapshot.tag_mismatch_count;
        if (!ot_matches)
            ++model_ft4_shadow.snapshot.ot_mismatch_count;
        if (matches)
            ++model_ft4_shadow.snapshot.match_count;
        else {
            if (model_ft4_shadow.snapshot.mismatch_count == 0u) {
                model_ft4_shadow.snapshot.first_mismatch_primitive = index;
                model_ft4_shadow.snapshot.first_mismatch_packet =
                    record->packet_address;
            }
            ++model_ft4_shadow.snapshot.mismatch_count;
        }
    }
    if ((cpu->read_word(UINT32_C(0x80059424)) & UINT32_C(0x00ffffff)) !=
            ((model_ft4_shadow.initial_packet_cursor +
                model_ft4_shadow.count * 0x28u) & UINT32_C(0x00ffffff))) {
        framing_matches = false;
        ++model_ft4_shadow.snapshot.cursor_mismatch_count;
        ++model_ft4_shadow.snapshot.mismatch_count;
    }
    actual_counter_delta =
        cpu->read_word(UINT32_C(0x80059578)) -
            model_ft4_shadow.initial_counter;
    model_ft4_shadow.snapshot.last_expected_counter_delta =
        model_ft4_shadow.expected_counter_delta;
    model_ft4_shadow.snapshot.last_actual_counter_delta = actual_counter_delta;
    if (actual_counter_delta != model_ft4_shadow.expected_counter_delta) {
        framing_matches = false;
        ++model_ft4_shadow.snapshot.counter_mismatch_count;
        ++model_ft4_shadow.snapshot.mismatch_count;
    }
    if (state.requested_render_mode == GUEST_RENDER_RENDER_NATIVE) {
        if (framing_matches) {
            ++model_ft4_shadow.snapshot.publish_invocation_count;
            if (!publish_model_ft4_sources()) {
                block_model_ft4_shadow(78u);
                return;
            }
        } else {
            ++model_ft4_shadow.snapshot.framing_rejected_invocation_count;
        }
    }
    context = model_ft4_shadow.context;
    clear_model_ft4_shadow_pending();
    model_ft4_shadow.context = context;
}

static void model_ft4_shadow_end(void) {
    if (model_ft4_shadow.snapshot.pending)
        block_model_ft4_shadow(74u);
    if (model_ft3_shadow.snapshot.pending)
        block_model_ft3_shadow(74u);
    if (!model_ft4_shadow.snapshot.pending &&
        !model_ft3_shadow.snapshot.pending)
        model_ft4_shadow.context = (XgRenderModelFt4ShadowContext){ 0 };
}

static int32_t model_ft3_nclip(
    const XgHost3dProjectedVertex vertices[3]) {
    return (int32_t)vertices[0].x * vertices[1].y +
        (int32_t)vertices[1].x * vertices[2].y +
        (int32_t)vertices[2].x * vertices[0].y -
        (int32_t)vertices[0].x * vertices[2].y -
        (int32_t)vertices[1].x * vertices[0].y -
        (int32_t)vertices[2].x * vertices[1].y;
}

static bool model_ft3_decode_material(
        CPUState *cpu, uint32_t attribute_address, uint16_t tpage,
        uint16_t clut, XgRenderModelFt4Template *material) {
    uint8_t opcode;

    if (cpu == NULL || material == NULL || cpu->read_word == NULL ||
        cpu->read_half == NULL || cpu->read_byte == NULL ||
        !word_address_is_valid(attribute_address) ||
        !word_address_is_valid(attribute_address + 4u))
        return false;
    opcode = cpu->read_byte(attribute_address + 3u);
    if (opcode < 0x24u || opcode > 0x27u) return false;
    *material = (XgRenderModelFt4Template){
        .descriptor_address = model_ft4_template_key(attribute_address),
        .material_word = UINT32_C(0x00808080) | ((uint32_t)opcode << 24u),
        .uv = {
            cpu->read_half(attribute_address + 4u),
            cpu->read_half(attribute_address + 6u),
            cpu->read_half(attribute_address),
        },
        .tpage = tpage,
        .clut = clut,
        .table_epoch = model_ft4_table_epoch,
        .valid = true,
    };
    return true;
}

static bool model_ft3_build_record(
    CPUState *cpu, const XgHost3dVector vertices[3],
    const XgRenderModelFt4Template *material_template,
    uint32_t packet_address, XgRenderModelFt3ShadowRecord *record) {
    XgHost3dProject4Input input = { 0 };
    XgHost3dRotTransPers4Output output;
    GpuDrawState draw = { 0 };
    const int32_t native_margin = render_screen_x_cull_margin();
    uint16_t max_depth = 0u;
    bool all_below = true;
    bool guest_all_horizontal_outside = true;
    bool all_left = true;
    bool all_right = true;
    uint32_t vertex;

    if (cpu == NULL || vertices == NULL || material_template == NULL ||
        record == NULL ||
        (material_template->material_word >> 24u) < 0x24u ||
        (material_template->material_word >> 24u) > 0x27u)
        return false;
    memcpy(input.vertices, vertices, 3u * sizeof(vertices[0]));
    input.vertices[3] = vertices[2];
    input.projection = model_ft4_shadow.context.projection;
    if (!xg_host_3d_rot_trans_pers4(&input, &output)) return false;
    memcpy(record->vertices, output.vertices, sizeof(record->vertices));
    for (vertex = 0u; vertex < 3u; ++vertex) {
        const uint32_t packed = (uint16_t)output.vertices[vertex].x |
            ((uint32_t)(uint16_t)output.vertices[vertex].y << 16u);

        all_below &= packed >= cpu->read_word(UINT32_C(0x800500fc));
        guest_all_horizontal_outside &=
            (uint16_t)output.vertices[vertex].x >=
                (uint16_t)cpu->read_word(UINT32_C(0x800500f8));
        if (native_margin > 0) {
            all_left &= (int32_t)output.vertices[vertex].x < -native_margin;
            all_right &= (int32_t)output.vertices[vertex].x >=
                (int32_t)(uint16_t)cpu->read_word(UINT32_C(0x800500f8)) +
                    native_margin;
        } else {
            all_left = false;
            all_right &= (uint16_t)output.vertices[vertex].x >=
                (uint16_t)cpu->read_word(UINT32_C(0x800500f8));
        }
        if (output.vertices[vertex].z > max_depth)
            max_depth = output.vertices[vertex].z;
    }
    record->ordering_bucket = max_depth >>
        ((cpu->read_word(UINT32_C(0x80050100)) + 2u) & 31u);
    record->nclip_positive = model_ft3_nclip(output.vertices) > 0;
    record->guest_vertical_accepted = !all_below;
    record->guest_horizontal_accepted = !guest_all_horizontal_outside;
    record->guest_screen_accepted = record->guest_vertical_accepted &&
        record->guest_horizontal_accepted;
    record->projection_flag_negative = (int32_t)output.rtpt_flags < 0;
    /* The resident handler tests COP2 data register 31 (LZCR), not FLAG.
     * LZCR is never negative, so projection flags do not gate guest output. */
    record->guest_passed_screen_cull =
        record->nclip_positive && record->guest_screen_accepted;
    record->guest_accepted =
        record->guest_passed_screen_cull && max_depth != 0u;
    record->passed_screen_cull = (int32_t)output.rtpt_flags >= 0 &&
        model_ft3_nclip(output.vertices) > 0 && !all_below &&
        !all_left && !all_right;
    record->accepted = record->passed_screen_cull && max_depth != 0u;
    record->packet_address = packet_address;
    record->material_word = material_template->material_word;
    memcpy(record->uv, material_template->uv, sizeof(record->uv));
    record->tpage = material_template->tpage;
    record->clut = material_template->clut;

    gpu_get_draw_state(&draw);
    apply_draw_state(&record->primitive.material, &draw);
    record->primitive.material.tpage = record->tpage;
    record->primitive.material.texture_page_x = record->tpage & 0x0fu;
    record->primitive.material.texture_page_y = (record->tpage >> 4u) & 1u;
    record->primitive.material.texture_depth =
        (XgRenderIrTextureDepth)((record->tpage >> 7u) & 3u);
    record->primitive.material.blend_mode =
        (XgRenderIrBlendMode)((record->tpage >> 5u) & 3u);
    record->primitive.material.clut_x = (record->clut & 0x3fu) << 4u;
    record->primitive.material.clut_y = record->clut >> 6u;
    record->primitive.material.shading = XG_RENDER_IR_SHADING_FLAT;
    record->primitive.material.textured = true;
    record->primitive.material.raw_texture =
        ((record->material_word >> 24u) & 1u) != 0u;
    record->primitive.material.semi_transparent =
        ((record->material_word >> 24u) & 2u) != 0u;
    record->primitive.triangle_count = 1u;
    record->primitive.triangles[0].split_count = 1u;
    for (vertex = 0u; vertex < 3u; ++vertex) {
        XgRenderIrVertex *destination =
            &record->primitive.triangles[0].vertices[vertex];

        destination->x = (int32_t)record->vertices[vertex].x * INT32_C(65536);
        destination->y = (int32_t)record->vertices[vertex].y * INT32_C(65536);
        destination->u = (int32_t)(uint8_t)record->uv[vertex] * INT32_C(65536);
        destination->v = (int32_t)(uint8_t)(record->uv[vertex] >> 8u) *
            INT32_C(65536);
        destination->r = (uint8_t)record->material_word;
        destination->g = (uint8_t)(record->material_word >> 8u);
        destination->b = (uint8_t)(record->material_word >> 16u);
        destination->native_view_x =
            record->vertices[vertex].native_view_x_16_16;
        destination->native_view_y =
            record->vertices[vertex].native_view_y_16_16;
        destination->native_view_position =
            record->vertices[vertex].native_view_position != 0u;
        destination->projective_view_x =
            record->vertices[vertex].projective_view_x;
        destination->projective_view_y =
            record->vertices[vertex].projective_view_y;
        destination->projective_view_z =
            record->vertices[vertex].projective_view_z;
        destination->projective_offset_x =
            record->vertices[vertex].projective_offset_x_16_16;
        destination->projective_offset_y =
            record->vertices[vertex].projective_offset_y_16_16;
        destination->projective_native_offset_x = record->vertices[vertex]
            .projective_native_offset_x_16_16;
        destination->projective_native_offset_y = record->vertices[vertex]
            .projective_native_offset_y_16_16;
        destination->projective_distance =
            record->vertices[vertex].projective_distance;
        destination->projective_position =
            record->vertices[vertex].projective_position != 0u;
    }
    return true;
}

static bool model_ft3_shadow_prepare(CPUState *cpu) {
    static const uint8_t attribute_sizes[17] = {
        4u, 8u, 4u, 8u, 4u, 8u, 4u, 8u, 4u,
        12u, 4u, 12u, 4u, 12u, 4u, 12u, 4u,
    };
    XgRenderModelFt4ShadowContext *context = &model_ft4_shadow.context;
    uint32_t group_address;
    uint32_t attribute_address;
    uint32_t group_count;
    uint32_t target_count;
    uint16_t tpage;
    uint16_t clut;
    bool found = false;
    uint32_t precondition_mask;
    XgHost3dProjection handler_projection = {0};
    uint32_t projection_mismatch_mask = 0u;

    model_ft3_shadow.snapshot.prepare_failure_detail = 0u;
    model_ft3_shadow.snapshot.last_target_count =
        cpu != NULL ? cpu->gpr[5] : 0u;
    model_ft3_shadow.snapshot.last_nclip_positive_count = 0u;
    model_ft3_shadow.snapshot.last_guest_screen_accepted_count = 0u;
    model_ft3_shadow.snapshot.last_guest_vertical_accepted_count = 0u;
    model_ft3_shadow.snapshot.last_guest_horizontal_accepted_count = 0u;
    model_ft3_shadow.snapshot.last_projection_flag_negative_count = 0u;
    model_ft3_shadow.snapshot.last_handler_projection_mismatch_mask = 0u;
    precondition_mask = model_shadow_prepare_precondition_mask(cpu, context);
    model_ft3_shadow.snapshot.prepare_precondition_failure_mask =
        precondition_mask;
    if (precondition_mask != 0u) {
        model_ft3_shadow.snapshot.prepare_failure_detail = 1u;
        return false;
    }
    capture_shadow_projection(cpu, &handler_projection);
    if (memcmp(handler_projection.rotation, context->projection.rotation,
               sizeof(handler_projection.rotation)) != 0)
        projection_mismatch_mask |= 1u;
    if (memcmp(handler_projection.translation, context->projection.translation,
               sizeof(handler_projection.translation)) != 0)
        projection_mismatch_mask |= 2u;
    if (handler_projection.screen_offset_x != context->projection.screen_offset_x ||
        handler_projection.screen_offset_y != context->projection.screen_offset_y)
        projection_mismatch_mask |= 4u;
    if (handler_projection.projection_distance !=
            context->projection.projection_distance ||
        handler_projection.depth_cue_a != context->projection.depth_cue_a ||
        handler_projection.depth_cue_b != context->projection.depth_cue_b)
        projection_mismatch_mask |= 8u;
    model_ft3_shadow.snapshot.last_handler_projection_mismatch_mask =
        projection_mismatch_mask;
    if (projection_mismatch_mask != 0u)
        ++model_ft3_shadow.snapshot.handler_projection_mismatch_count;
    target_count = cpu->gpr[5];
    group_address = context->topology_base;
    attribute_address = context->material_base;
    group_count = cpu->read_half(context->model_address + 6u);
    model_ft3_shadow.snapshot.last_group_count = group_count;
    model_ft3_shadow.snapshot.last_target_count = target_count;
    tpage = context->tpage;
    clut = context->clut;
    if (group_count == 0u || group_count > 256u) {
        model_ft3_shadow.snapshot.prepare_failure_detail = 2u;
        return false;
    }
    for (uint32_t group = 0u; group < group_count; ++group) {
        const uint8_t row = cpu->read_byte(group_address);
        const uint32_t primitive_count = cpu->read_half(group_address + 2u);
        const uint32_t descriptors = group_address + 4u;

        if (row >= 17u || primitive_count > 4096u ||
            descriptors > UINT32_MAX - primitive_count * 8u) {
            model_ft3_shadow.snapshot.prepare_failure_detail = 3u;
            return false;
        }
        for (uint32_t primitive = 0u; primitive < primitive_count; ++primitive) {
            if (!model_ft4_shadow_consume_controls(
                    cpu, &attribute_address, &tpage, &clut)) {
                model_ft3_shadow.snapshot.prepare_failure_detail = 4u;
                return false;
            }
            if (descriptors == cpu->gpr[4]) {
                XgRenderModelFt3ShadowRecord *record;
                XgRenderModelFt4Template decoded_material = { 0 };
                const XgRenderModelFt4Template *material_template;
                XgHost3dVector vertices[3];
                const uint32_t packet_address = cpu->read_word(
                    UINT32_C(0x80059424)) + primitive * 0x20u;

                if (row != 5u || primitive_count != target_count ||
                    primitive >= XG_RENDER_MODEL_FT4_SHADOW_CAPACITY) {
                    model_ft3_shadow.snapshot.prepare_failure_detail = 5u;
                    return false;
                }
                found = true;
                record = &model_ft3_shadow.records[primitive];
                *record = (XgRenderModelFt3ShadowRecord){ 0 };
                record->attribute_address = attribute_address;
                material_template = model_ft4_template_find_packet(
                    packet_address, false);
                if (material_template == NULL)
                    material_template = model_ft4_template_find_descriptor(
                        attribute_address, false);
                if (material_template == NULL) {
                    ++model_ft3_shadow.snapshot.template_miss_count;
                    if (!model_ft3_decode_material(
                            cpu, attribute_address, tpage, clut,
                            &decoded_material)) {
                        model_ft3_shadow.snapshot.prepare_failure_detail = 6u;
                        return false;
                    }
                    material_template = &decoded_material;
                } else {
                    ++model_ft3_shadow.snapshot.template_hit_count;
                }
                for (uint32_t vertex = 0u; vertex < 3u; ++vertex) {
                    const uint32_t index = cpu->read_half(
                        descriptors + primitive * 8u + vertex * 2u);
                    const uint32_t address = context->vertex_base + index * 8u;
                    const uint32_t xy = cpu->read_word(address);
                    const uint32_t zp = cpu->read_word(address + 4u);

                    record->source_vertex_indices[vertex] = index;
                    vertices[vertex] = (XgHost3dVector){
                        low_s16(xy), low_s16(xy >> 16u), low_s16(zp),
                        (uint16_t)(zp >> 16u),
                    };
                }
                if (!model_ft3_build_record(
                        cpu, vertices, material_template, packet_address,
                        record)) {
                    model_ft3_shadow.snapshot.prepare_failure_detail = 7u;
                    return false;
                }
                for (uint32_t vertex = 0u; vertex < 3u; ++vertex) {
                    XgRenderIrVertex *destination = &record->primitive
                        .triangles[0].vertices[vertex];
                    const uint32_t group_id =
                        context->instance_address & UINT32_C(0x1fffffff);

                    destination->interpolation_group_id = group_id;
                    destination->interpolation_vertex_id =
                        record->source_vertex_indices[vertex];
                    destination->interpolation_vertex_identity_valid =
                        group_id != 0u;
                }
                model_ft3_shadow.snapshot.last_nclip_positive_count +=
                    record->nclip_positive;
                model_ft3_shadow.snapshot.last_guest_screen_accepted_count +=
                    record->guest_screen_accepted;
                model_ft3_shadow.snapshot.last_guest_vertical_accepted_count +=
                    record->guest_vertical_accepted;
                model_ft3_shadow.snapshot.last_guest_horizontal_accepted_count +=
                    record->guest_horizontal_accepted;
                model_ft3_shadow.snapshot.last_projection_flag_negative_count +=
                    record->projection_flag_negative;
            }
            attribute_address += attribute_sizes[row];
        }
        group_address = descriptors + primitive_count * 8u;
        if (found) break;
    }
    if (!found) {
        model_ft3_shadow.snapshot.prepare_failure_detail = 8u;
        return false;
    }
    model_ft3_shadow.initial_packet_cursor = cpu->read_word(
        UINT32_C(0x80059424));
    model_ft3_shadow.initial_counter = cpu->read_word(
        UINT32_C(0x80059578));
    model_ft3_shadow.descriptor_base = cpu->gpr[4];
    model_ft3_shadow.count = target_count;
    for (uint32_t index = 0u; index < target_count; ++index) {
        XgRenderModelFt3ShadowRecord *record = &model_ft3_shadow.records[index];

        if (!record->accepted) continue;
        if (record->ordering_bucket >= 0x1000u ||
            context->ot_base > UINT32_MAX - record->ordering_bucket * 4u ||
            !word_address_is_valid(
                context->ot_base + record->ordering_bucket * 4u)) {
            model_ft3_shadow.snapshot.prepare_failure_detail = 9u;
            return false;
        }
    }
    model_ft3_shadow.snapshot.pending = true;
    return true;
}

static bool native_model_ft3_raw_stage(CPUState *cpu) {
    uint32_t accepted_count = 0u;
    uint32_t staged_count = 0u;
    XgRenderProducerLifecycle lifecycle;

    if (state.requested_render_mode != GUEST_RENDER_RENDER_NATIVE ||
        !model_dispatch_context_contract_matches(
            cpu, &model_ft4_shadow.context) ||
          !model_ft3_shadow_prepare(cpu) ||
        !producer_lifecycle_begin(UINT32_C(0x8002c700), &lifecycle))
        return false;
    for (uint32_t index = 0u; index < model_ft3_shadow.count; ++index) {
        accepted_count += model_ft3_shadow.records[index].accepted;
        staged_count += model_ft3_shadow.records[index].accepted ||
            native_primitive_all_projective(
                &model_ft3_shadow.records[index].primitive);
    }
    if (pre_scene.blocked ||
        staged_count > XG_RENDER_PRE_SCENE_PRIMITIVE_CAPACITY ||
        pre_scene.count > XG_RENDER_PRE_SCENE_PRIMITIVE_CAPACITY - staged_count)
        return false;
    for (uint32_t index = 0u; index < model_ft3_shadow.count; ++index) {
        XgRenderModelFt3ShadowRecord *record =
            &model_ft3_shadow.records[index];
        const uint32_t interpolation_producer_id =
            model_ft4_shadow.context.instance_address & UINT32_C(0x1fffffff);
        const uint32_t interpolation_primitive_id =
            (record->attribute_address & UINT32_C(0x1fffffff)) |
            ((uint32_t)(model_ft4_shadow.context.dispatch_mode & 7u) << 29u) |
            UINT32_C(1);
        const bool interpolation_identity_valid =
            (model_ft4_shadow.context.instance_address &
             UINT32_C(0x1fffffff)) != 0u;
        const int32_t native_margin = render_screen_x_cull_margin();
        const GpuRenderTemporalCullPolicy temporal_cull = {
            .flags = GPU_RENDER_TEMPORAL_CULL_PROJECTIVE |
                GPU_RENDER_TEMPORAL_CULL_SCREEN |
                GPU_RENDER_TEMPORAL_CULL_FRONT_FACE |
                GPU_RENDER_TEMPORAL_CULL_DEPTH,
            .screen_left = native_margin > 0
                ? -native_margin * INT32_C(65536) : INT32_MIN,
            .screen_top = INT32_MIN,
            .screen_right_exclusive =
                ((int32_t)(uint16_t)cpu->read_word(UINT32_C(0x800500f8)) +
                 native_margin) * INT32_C(65536),
            .screen_bottom_exclusive =
                (int32_t)(uint16_t)(cpu->read_word(UINT32_C(0x800500fc)) >> 16u) *
                    INT32_C(65536),
            .depth_min_inclusive = 1,
            .depth_max_exclusive = 0x10000,
            .depth_mode = GPU_RENDER_TEMPORAL_DEPTH_MAXIMUM,
            .front_face = GPU_RENDER_TEMPORAL_FRONT_POSITIVE,
            .ordering_depth_shift = (uint8_t)((
                cpu->read_word(UINT32_C(0x80050100)) + 2u) & 31u),
        };

        record->lifecycle = lifecycle;
        record->interpolation_producer_id = interpolation_producer_id;
        record->interpolation_primitive_id = interpolation_primitive_id;
        record->interpolation_identity_valid = interpolation_identity_valid;
        if (!record->accepted) {
            if (!interpolation_identity_valid ||
                !native_primitive_all_projective(&record->primitive))
                continue;
            if (!stage_pre_scene_primitive(&(XgRenderPreScenePrimitive){
                    .primitive = record->primitive,
                    .interpolation_producer_id = interpolation_producer_id,
                    .interpolation_primitive_id = interpolation_primitive_id,
                    .interpolation_identity_valid = true,
                    .temporal_only = true,
                    .temporal_cull = temporal_cull,
                }))
                return false;
            continue;
        }
        if (!stage_pre_scene_primitive(&(XgRenderPreScenePrimitive){
                .primitive = record->primitive,
                .packet_address = record->packet_address,
                .source_primitive_index = UINT32_C(0x51000000) |
                    (record->packet_address & UINT32_C(0x001ffffc)),
                .ot_bucket = record->ordering_bucket,
                .interpolation_producer_id = interpolation_producer_id,
                .interpolation_primitive_id = interpolation_primitive_id,
                .payload_word_count = 7u,
                .interpolation_identity_valid = interpolation_identity_valid,
            }))
            return false;
    }
    ++model_ft3_shadow.snapshot.native_cutover_count;
    model_ft3_shadow.snapshot.native_primitive_count += accepted_count;
    return true;
}

static void model_ft3_observe_guest_pass(CPUState *cpu) {
    uint32_t descriptor_base;
    uint32_t next_descriptor;
    uint32_t descriptor_offset;
    uint32_t primitive_index;
    uint32_t max_depth;
    uint32_t ordering_bucket;
    XgRenderModelFt3ShadowRecord *record;

    if (!model_ft3_shadow.snapshot.pending || cpu == NULL ||
        cpu->read_word == NULL)
        return;
    descriptor_base = model_ft3_shadow.descriptor_base & UINT32_C(0x1fffffff);
    next_descriptor = cpu->gpr[4] & UINT32_C(0x1fffffff);
    if (next_descriptor < descriptor_base + 8u) {
        block_model_ft3_shadow(75u);
        return;
    }
    descriptor_offset = next_descriptor - descriptor_base - 8u;
    if ((descriptor_offset & 7u) != 0u) {
        block_model_ft3_shadow(75u);
        return;
    }
    primitive_index = descriptor_offset / 8u;
    if (primitive_index >= model_ft3_shadow.count) {
        block_model_ft3_shadow(75u);
        return;
    }
    record = &model_ft3_shadow.records[primitive_index];
    if (record->guest_observed_passed_screen_cull) {
        block_model_ft3_shadow(76u);
        return;
    }
    record->guest_observed_passed_screen_cull = true;
    ++model_ft3_shadow.expected_counter_delta;
    ++model_ft3_shadow.snapshot.guest_pass_observation_count;
    if (!record->guest_passed_screen_cull)
        ++model_ft3_shadow.snapshot.guest_pass_projection_disagreement_count;
    max_depth = (uint16_t)cpu->gte_data[17];
    for (uint32_t depth = 18u; depth <= 19u; ++depth) {
        if ((uint16_t)cpu->gte_data[depth] > max_depth)
            max_depth = (uint16_t)cpu->gte_data[depth];
    }
    ordering_bucket = max_depth >>
        ((cpu->read_word(UINT32_C(0x80050100)) + 2u) & 31u);
    if (max_depth != 0u) {
        const uint32_t ot_base = model_ft4_shadow.context.ot_base;

        if (ordering_bucket >= 0x1000u ||
            ot_base > UINT32_MAX - ordering_bucket * 4u ||
            !word_address_is_valid(ot_base + ordering_bucket * 4u)) {
            block_model_ft3_shadow(77u);
            return;
        }
        record->ordering_bucket = (uint16_t)ordering_bucket;
        record->expected_tag = cpu->read_word(
            ot_base + ordering_bucket * 4u) | UINT32_C(0x07000000);
        record->guest_observed_accepted = true;
    }
    for (uint32_t vertex = 0u; vertex < 3u; ++vertex) {
        XgRenderIrVertex *destination =
            &record->primitive.triangles[0].vertices[vertex];

        record->observed_xy[vertex] = cpu->gte_data[12u + vertex];
        destination->x = (int32_t)low_s16(record->observed_xy[vertex]) *
            INT32_C(65536);
        destination->y =
            (int32_t)low_s16(record->observed_xy[vertex] >> 16u) *
                INT32_C(65536);
    }
}

static bool capture_model_ft3_linked_source(CPUState *cpu) {
    const uint32_t packet_address = cpu != NULL ? cpu->gpr[19] : 0u;
    const uint32_t material_word = cpu != NULL && cpu->read_word != NULL
        ? cpu->read_word(packet_address + 4u) : 0u;
    const uint8_t opcode = (uint8_t)(material_word >> 24u);
    XgRenderModelFt4Template decoded_material = { 0 };
    const XgRenderModelFt4Template *material_template;
    XgRenderIrNativePrimitive primitive = {0};
    XgRenderModelFt3SourceRecord *source;
    GpuDrawState draw = {0};
    XgRenderProducerLifecycle lifecycle;

    if (cpu == NULL || state.requested_render_mode != GUEST_RENDER_RENDER_NATIVE ||
        cpu->read_word == NULL || cpu->read_half == NULL ||
        cpu->read_byte == NULL || !word_address_is_valid(packet_address) ||
        opcode < 0x24u || opcode > 0x27u)
        return false;
    if (!producer_lifecycle_begin(UINT32_C(0x8002da00), &lifecycle))
        return false;
    material_template = model_ft4_template_find_packet(packet_address, false);
    if (material_template == NULL && !model_ft3_decode_material(
            cpu, cpu->gpr[16], cpu->read_half(UINT32_C(0x80059308)),
            cpu->read_half(UINT32_C(0x8005930c)), &decoded_material)) {
        ++model_ft3_shadow.snapshot.template_miss_count;
        return false;
    }
    if (material_template == NULL) {
        ++model_ft3_shadow.snapshot.template_miss_count;
        material_template = &decoded_material;
    }
    if (!material_template->valid) return false;
    for (uint32_t index = 0u; index < model_ft3_shadow.count; ++index) {
        if (physical_address_equals(
                model_ft3_shadow.records[index].packet_address,
                packet_address)) {
            primitive = model_ft3_shadow.records[index].primitive;
            break;
        }
    }
    gpu_get_draw_state(&draw);
    apply_draw_state(&primitive.material, &draw);
    primitive.material.tpage = material_template->tpage;
    primitive.material.texture_page_x = material_template->tpage & 0x0fu;
    primitive.material.texture_page_y =
        (material_template->tpage >> 4u) & 1u;
    primitive.material.texture_depth = (XgRenderIrTextureDepth)(
        (material_template->tpage >> 7u) & 3u);
    primitive.material.blend_mode = (XgRenderIrBlendMode)(
        (material_template->tpage >> 5u) & 3u);
    primitive.material.clut_x = (material_template->clut & 0x3fu) << 4u;
    primitive.material.clut_y = material_template->clut >> 6u;
    primitive.material.shading = XG_RENDER_IR_SHADING_FLAT;
    primitive.material.textured = true;
    primitive.material.raw_texture = (opcode & 1u) != 0u;
    primitive.material.semi_transparent =
        (opcode & 2u) != 0u;
    primitive.triangle_count = 1u;
    primitive.triangles[0].split_count = 1u;
    for (uint32_t vertex = 0u; vertex < 3u; ++vertex) {
        const uint32_t xy = cpu->gpr[9u + vertex];
        XgRenderIrVertex *destination =
            &primitive.triangles[0].vertices[vertex];

        destination->x = (int32_t)low_s16(xy) * INT32_C(65536);
        destination->y = (int32_t)low_s16(xy >> 16u) * INT32_C(65536);
        destination->u = (int32_t)(uint8_t)material_template->uv[vertex] *
            INT32_C(65536);
        destination->v =
            (int32_t)(uint8_t)(material_template->uv[vertex] >> 8u) *
            INT32_C(65536);
        destination->r = (uint8_t)material_word;
        destination->g = (uint8_t)(material_word >> 8u);
        destination->b = (uint8_t)(material_word >> 16u);
    }
    source = model_ft3_source_upsert(
        model_ft4_template_key(packet_address) + 4u);
    if (source == NULL) return false;
    *source = (XgRenderModelFt3SourceRecord){
        .primitive = primitive,
        .lifecycle = lifecycle,
        .source_id = model_ft4_template_key(packet_address) + 4u,
        .interpolation_producer_id =
            model_ft4_shadow.context.instance_address & UINT32_C(0x1fffffff),
        .interpolation_primitive_id =
            ((material_template->descriptor_address != 0u
                  ? material_template->descriptor_address : cpu->gpr[16]) &
             UINT32_C(0x1fffffff)) |
            ((uint32_t)(model_ft4_shadow.context.dispatch_mode & 7u) << 29u) |
            UINT32_C(1),
        .interpolation_identity_valid =
            (model_ft4_shadow.context.instance_address &
             UINT32_C(0x1fffffff)) != 0u,
        .valid = true,
    };
    xg_render_lookup_put(
        model_ft3_source_lookup, model_ft3_source_lookup_epoch,
        source->source_id, (uint32_t)(source - model_ft3_sources));
    native_resolve_hint_put(source->source_id, XG_NATIVE_RESOLVE_MODEL_FT3);
    watch_producer_resource(packet_address, 0x20u);
    return true;
}

void psx_xg_render_auth_capture_model_ft3_link(CPUState *cpu) {
    (void)capture_model_ft3_linked_source(cpu);
}

static bool publish_model_ft3_sources(void) {
    for (uint32_t index = 0u; index < model_ft3_shadow.count; ++index) {
        const XgRenderModelFt3ShadowRecord *record =
            &model_ft3_shadow.records[index];
        const uint32_t source_id =
            (record->packet_address & UINT32_C(0x1fffffff)) + 4u;
        XgRenderModelFt3SourceRecord *source;

        if (!record->guest_observed_accepted || !record->output_validated)
            continue;
        source = model_ft3_source_upsert(source_id);
        if (source == NULL) return false;
        *source = (XgRenderModelFt3SourceRecord){
            .primitive = record->primitive,
            .lifecycle = record->lifecycle,
            .source_id = source_id,
            .interpolation_producer_id = record->interpolation_producer_id,
            .interpolation_primitive_id = record->interpolation_primitive_id,
            .interpolation_identity_valid =
                record->interpolation_identity_valid,
            .valid = true,
        };
        xg_render_lookup_put(
            model_ft3_source_lookup, model_ft3_source_lookup_epoch,
            source_id, (uint32_t)(source - model_ft3_sources));
        native_resolve_hint_put(source_id, XG_NATIVE_RESOLVE_MODEL_FT3);
        watch_producer_resource(record->packet_address, 0x20u);
        ++model_ft3_shadow.snapshot.publish_source_count;
    }
    return true;
}

static void model_ft3_shadow_finish(CPUState *cpu) {
    bool framing_matches = true;
    uint32_t actual_counter_delta;

    if (!model_ft3_shadow.snapshot.pending) return;
    if (cpu == NULL || cpu->read_word == NULL || cpu->read_half == NULL) {
        block_model_ft3_shadow(73u);
        return;
    }
    ++model_ft3_shadow.snapshot.invocation_count;
    model_ft3_shadow.snapshot.primitive_count += model_ft3_shadow.count;
    for (uint32_t index = 0u; index < model_ft3_shadow.count; ++index) {
        XgRenderModelFt3ShadowRecord *record =
            &model_ft3_shadow.records[index];
        const uint32_t actual_material_word = cpu->read_word(
            record->packet_address + 4u);
        const uint8_t actual_opcode = (uint8_t)(actual_material_word >> 24u);
        bool payload_matches =
            (actual_material_word & UINT32_C(0xff000000)) ==
                (record->material_word & UINT32_C(0xff000000)) &&
            cpu->read_half(record->packet_address + 12u) == record->uv[0] &&
            cpu->read_half(record->packet_address + 20u) == record->uv[1] &&
            cpu->read_half(record->packet_address + 28u) == record->uv[2] &&
            cpu->read_half(record->packet_address + 22u) == record->tpage &&
            cpu->read_half(record->packet_address + 14u) == record->clut;
        bool geometry_matches = true;
        bool tag_matches = true;
        bool ot_matches = true;

        if ((actual_material_word & UINT32_C(0x00ffffff)) !=
            (record->material_word & UINT32_C(0x00ffffff)))
            ++model_ft3_shadow.snapshot.raw_color_difference_count;
        record->primitive.material.raw_texture = (actual_opcode & 1u) != 0u;
        record->primitive.material.semi_transparent =
            (actual_opcode & 2u) != 0u;
        for (uint32_t vertex = 0u; vertex < 3u; ++vertex) {
            XgRenderIrVertex *destination =
                &record->primitive.triangles[0].vertices[vertex];

            destination->r = (uint8_t)actual_material_word;
            destination->g = (uint8_t)(actual_material_word >> 8u);
            destination->b = (uint8_t)(actual_material_word >> 16u);
        }

        if (record->guest_observed_accepted) {
            bool last_in_bucket = true;

            tag_matches = cpu->read_word(record->packet_address) ==
                record->expected_tag;
            for (uint32_t vertex = 0u; vertex < 3u; ++vertex) {
                geometry_matches &= cpu->read_word(
                    record->packet_address + 8u + vertex * 8u) ==
                        record->observed_xy[vertex];
            }
            for (uint32_t later = index + 1u;
                 later < model_ft3_shadow.count; ++later) {
                if (model_ft3_shadow.records[later].guest_observed_accepted &&
                    model_ft3_shadow.records[later].ordering_bucket ==
                        record->ordering_bucket)
                    last_in_bucket = false;
            }
            if (last_in_bucket)
                ot_matches = cpu->read_word(
                    model_ft4_shadow.context.ot_base +
                        record->ordering_bucket * 4u) ==
                    (record->packet_address & 0x00ffffffu);
        }
        if (!payload_matches)
            ++model_ft3_shadow.snapshot.payload_mismatch_count;
        if (!payload_matches &&
            model_ft3_shadow.snapshot.first_payload_mismatch.field_bits == 0u) {
            PsxXgRenderFt4PayloadMismatch *mismatch =
                &model_ft3_shadow.snapshot.first_payload_mismatch;

            mismatch->packet_address = record->packet_address;
            mismatch->descriptor_address = record->attribute_address;
            mismatch->expected_material_word = record->material_word;
            mismatch->actual_material_word = cpu->read_word(
                record->packet_address + 4u);
            mismatch->expected_tpage = record->tpage;
            mismatch->actual_tpage = cpu->read_half(record->packet_address + 22u);
            mismatch->expected_clut = record->clut;
            mismatch->actual_clut = cpu->read_half(record->packet_address + 14u);
            if (mismatch->actual_material_word != record->material_word)
                mismatch->field_bits |= XG_RENDER_FT4_PAYLOAD_MATERIAL;
            if (mismatch->actual_tpage != record->tpage)
                mismatch->field_bits |= XG_RENDER_FT4_PAYLOAD_TPAGE;
            if (mismatch->actual_clut != record->clut)
                mismatch->field_bits |= XG_RENDER_FT4_PAYLOAD_CLUT;
            for (uint32_t vertex = 0u; vertex < 3u; ++vertex) {
                mismatch->expected_uv[vertex] = record->uv[vertex];
                mismatch->actual_uv[vertex] = cpu->read_half(
                    record->packet_address + 12u + vertex * 8u);
                if (mismatch->actual_uv[vertex] != record->uv[vertex])
                    mismatch->field_bits |= XG_RENDER_FT4_PAYLOAD_UV0 << vertex;
            }
        }
        if (!geometry_matches)
            ++model_ft3_shadow.snapshot.geometry_mismatch_count;
        if (!tag_matches)
            ++model_ft3_shadow.snapshot.tag_mismatch_count;
        if (!ot_matches)
            ++model_ft3_shadow.snapshot.ot_mismatch_count;
        const bool matches = payload_matches && geometry_matches &&
            tag_matches && ot_matches;
        record->output_validated =
            record->guest_observed_accepted && matches;
        if (record->guest_observed_accepted && !matches)
            ++model_ft3_shadow.snapshot.validation_rejected_source_count;
        if (matches) {
            ++model_ft3_shadow.snapshot.match_count;
        } else {
            if (model_ft3_shadow.snapshot.mismatch_count == 0u)
                model_ft3_shadow.snapshot.first_mismatch_packet =
                    record->packet_address;
            ++model_ft3_shadow.snapshot.mismatch_count;
        }
    }
    if ((cpu->read_word(UINT32_C(0x80059424)) & UINT32_C(0x00ffffff)) !=
            ((model_ft3_shadow.initial_packet_cursor +
              model_ft3_shadow.count * 0x20u) & UINT32_C(0x00ffffff))) {
        framing_matches = false;
        ++model_ft3_shadow.snapshot.cursor_mismatch_count;
        ++model_ft3_shadow.snapshot.mismatch_count;
    }
    actual_counter_delta =
        cpu->read_word(UINT32_C(0x80059578)) -
            model_ft3_shadow.initial_counter;
    model_ft3_shadow.snapshot.last_expected_counter_delta =
        model_ft3_shadow.expected_counter_delta;
    model_ft3_shadow.snapshot.last_actual_counter_delta =
        actual_counter_delta;
    if (actual_counter_delta != model_ft3_shadow.expected_counter_delta) {
        framing_matches = false;
        model_ft3_shadow.snapshot.last_mismatch_expected_counter_delta =
            model_ft3_shadow.expected_counter_delta;
        model_ft3_shadow.snapshot.last_mismatch_actual_counter_delta =
            actual_counter_delta;
        model_ft3_shadow.snapshot.last_mismatch_target_count =
            model_ft3_shadow.count;
        model_ft3_shadow.snapshot.last_mismatch_nclip_positive_count =
            model_ft3_shadow.snapshot.last_nclip_positive_count;
        model_ft3_shadow.snapshot.last_mismatch_guest_screen_accepted_count =
            model_ft3_shadow.snapshot.last_guest_screen_accepted_count;
        model_ft3_shadow.snapshot.last_mismatch_guest_vertical_accepted_count =
            model_ft3_shadow.snapshot.last_guest_vertical_accepted_count;
        model_ft3_shadow.snapshot.last_mismatch_guest_horizontal_accepted_count =
            model_ft3_shadow.snapshot.last_guest_horizontal_accepted_count;
        model_ft3_shadow.snapshot.last_mismatch_projection_flag_negative_count =
            model_ft3_shadow.snapshot.last_projection_flag_negative_count;
        model_ft3_shadow.snapshot.last_mismatch_screen_right =
            cpu->read_word(UINT32_C(0x800500f8));
        model_ft3_shadow.snapshot.last_mismatch_screen_bottom =
            cpu->read_word(UINT32_C(0x800500fc));
        if (actual_counter_delta > model_ft3_shadow.expected_counter_delta) {
            ++model_ft3_shadow.snapshot.counter_actual_greater_count;
        } else {
            ++model_ft3_shadow.snapshot.counter_actual_less_count;
        }
        ++model_ft3_shadow.snapshot.counter_mismatch_count;
        ++model_ft3_shadow.snapshot.mismatch_count;
    }
    if (state.requested_render_mode == GUEST_RENDER_RENDER_NATIVE) {
        if (framing_matches) {
            if (!publish_model_ft3_sources()) {
                block_model_ft3_shadow(78u);
                return;
            }
            ++model_ft3_shadow.snapshot.publish_invocation_count;
        } else {
            ++model_ft3_shadow.snapshot.framing_rejected_invocation_count;
        }
    }
    clear_model_ft3_shadow_pending();
}

static void clear_sprite_ft4_shadow_context(void) {
    if (!sprite_ft4_shadow.snapshot.context_active &&
        !sprite_ft4_shadow.snapshot.pending)
        return;
    sprite_ft4_shadow.snapshot.context_active = false;
    sprite_ft4_shadow.snapshot.pending = false;
    sprite_ft4_shadow.sprite_address = 0u;
    sprite_ft4_shadow.packet_address = 0u;
    sprite_ft4_shadow.descriptor_address = 0u;
    sprite_ft4_shadow.material_word = 0u;
    sprite_ft4_shadow.tpage = 0u;
    sprite_ft4_shadow.clut = 0u;
    sprite_ft4_shadow.phase = XG_RENDER_SPRITE_FT4_SHADOW_IDLE;
    sprite_ft4_shadow.native_record_count = 0u;
    sprite_ft4_shadow.geometry_matches = false;
    sprite_ft4_shadow.payload_matches = false;
    sprite_ft4_shadow.invocation_matches = false;
    sprite_ft4_shadow.wrapper_scope = false;
}

static void block_sprite_ft4_shadow(uint32_t blocker) {
    clear_sprite_ft4_shadow_context();
    sprite_ft4_shadow.snapshot.blocked = true;
    if (sprite_ft4_shadow.snapshot.blocker == 0u)
        sprite_ft4_shadow.snapshot.blocker = blocker;
}

static void clear_field_sprite_builder(void) {
    if (field_sprite_builder.count == 0u &&
        field_sprite_builder.overlay_family == 0u &&
        !sprite_ft4_shadow.snapshot.field_builder_pending)
        return;
    field_sprite_builder.count = 0u;
    field_sprite_builder.overlay_family = 0u;
    sprite_ft4_shadow.snapshot.field_builder_pending = false;
}

static void clear_field_sprite_templates(void) {
    field_sprite_templates.count = 0u;
    xg_render_lookup_reset(field_sprite_template_lookup,
                           &field_sprite_template_lookup_epoch);
    sprite_ft4_shadow.snapshot.field_builder_template_count = 0u;
}

static XgRenderFieldSpriteBuilderRecord *field_sprite_template_find(
        uint32_t packet_address) {
    const uint32_t indexed = xg_render_lookup_find(
        field_sprite_template_lookup, field_sprite_template_lookup_epoch,
        packet_address, field_sprite_templates.count);

    if (indexed != UINT32_MAX && physical_address_equals(
            field_sprite_templates.records[indexed].packet_address,
            packet_address))
        return &field_sprite_templates.records[indexed];
    for (uint32_t index = 0u; index < field_sprite_templates.count; ++index) {
        if (physical_address_equals(
                field_sprite_templates.records[index].packet_address,
                packet_address)) {
            xg_render_lookup_put(
                field_sprite_template_lookup,
                field_sprite_template_lookup_epoch, packet_address, index);
            return &field_sprite_templates.records[index];
        }
    }
    return NULL;
}

static bool field_sprite_template_capture(
        const XgRenderFieldSpriteBuilderRecord *record) {
    XgRenderFieldSpriteBuilderRecord *target;

    if (record == NULL) return false;
    target = field_sprite_template_find(record->packet_address);
    if (target != NULL) {
        *target = *record;
        target->semantic_ready = false;
        ++sprite_ft4_shadow.snapshot.field_builder_template_update_count;
    } else {
        if (field_sprite_templates.count ==
                XG_RENDER_FIELD_SPRITE_TEMPLATE_CAPACITY)
            return false;
        field_sprite_templates.records[field_sprite_templates.count] = *record;
        field_sprite_templates.records[field_sprite_templates.count].semantic_ready =
            false;
        xg_render_lookup_put(
            field_sprite_template_lookup,
            field_sprite_template_lookup_epoch, record->packet_address,
            field_sprite_templates.count);
        ++field_sprite_templates.count;
        sprite_ft4_shadow.snapshot.field_builder_template_count =
            field_sprite_templates.count;
    }
    ++sprite_ft4_shadow.snapshot.field_builder_template_capture_count;
    native_resolve_hint_put(
        (record->packet_address & UINT32_C(0x1fffffff)) + 4u,
        XG_NATIVE_RESOLVE_FIELD_SPRITE);
    watch_producer_resource(record->packet_address, 0x28u);
    watch_producer_resource(record->descriptor_address, 0x1cu);
    return true;
}

static void field_sprite_template_invalidate(uint32_t packet_address) {
    XgRenderFieldSpriteBuilderRecord *record =
        field_sprite_template_find(packet_address);
    uint32_t index;

    if (record == NULL) return;
    index = (uint32_t)(record - field_sprite_templates.records);
    --field_sprite_templates.count;
    xg_render_lookup_remove(
        field_sprite_template_lookup, field_sprite_template_lookup_epoch,
        packet_address, index);
    if (index != field_sprite_templates.count)
        field_sprite_templates.records[index] =
            field_sprite_templates.records[field_sprite_templates.count];
    if (index != field_sprite_templates.count)
        xg_render_lookup_put(
            field_sprite_template_lookup,
            field_sprite_template_lookup_epoch,
            field_sprite_templates.records[index].packet_address, index);
    sprite_ft4_shadow.snapshot.field_builder_template_count =
        field_sprite_templates.count;
    ++sprite_ft4_shadow.snapshot.field_builder_template_invalidation_count;
}

static void block_field_sprite_builder(uint32_t blocker) {
    clear_field_sprite_builder();
    sprite_ft4_shadow.snapshot.field_builder_blocked = true;
    if (sprite_ft4_shadow.snapshot.field_builder_blocker == 0u)
        sprite_ft4_shadow.snapshot.field_builder_blocker = blocker;
}

static XgRenderOverlayFt4Template *overlay_ft4_upsert_field_template(
        const XgRenderFieldSpriteBuilderRecord *source) {
    XgRenderOverlayFt4Template *target;

    overlay_ft4_upsert_failure_detail = 0u;
    if (source == NULL || !producer_lifecycle_matches(&source->lifecycle)) {
        overlay_ft4_upsert_failure_detail = 3u;
        return NULL;
    }
    target = overlay_ft4_find_template(source->packet_address);
    if (target == NULL) {
        if (overlay_ft4_state.count ==
                XG_RENDER_OVERLAY_FT4_TEMPLATE_CAPACITY) {
            overlay_ft4_upsert_failure_detail = 2u;
            return NULL;
        }
        target = &overlay_ft4_state.templates[overlay_ft4_state.count++];
    }
    *target = (XgRenderOverlayFt4Template){
        .lifecycle = source->lifecycle,
        .packet_address = source->packet_address,
        .interpolation_producer_id = source->interpolation_producer_id,
        .interpolation_primitive_id = source->interpolation_primitive_id,
        .interpolation_identity_valid = source->interpolation_identity_valid,
    };
    watch_producer_resource(source->packet_address, 0x28u);
    return target;
}

static uint16_t field_sprite_tpage(
    uint16_t texture_depth, int16_t x, int16_t y) {
    return (uint16_t)(((texture_depth & 3u) << 7u) |
        (((uint16_t)y & 0x100u) >> 4u) |
        (((uint16_t)x & 0x3ffu) >> 6u));
}

static uint16_t field_sprite_clut(int16_t x, int16_t y) {
    return (uint16_t)((((uint16_t)y & 0x1ffu) << 6u) |
        (((uint16_t)x >> 4u) & 0x3fu));
}

static void observe_field_sprite_builder_caller(uint32_t caller) {
    uint32_t empty = 4u;

    for (uint32_t index = 0u; index < 4u; ++index) {
        if (sprite_ft4_shadow.snapshot.field_builder_caller_counts[index] !=
                0u &&
            physical_address_equals(
                sprite_ft4_shadow.snapshot.field_builder_caller_candidates[index],
                caller)) {
            ++sprite_ft4_shadow.snapshot.field_builder_caller_counts[index];
            return;
        }
        if (empty == 4u &&
            sprite_ft4_shadow.snapshot.field_builder_caller_counts[index] == 0u)
            empty = index;
    }
    if (empty != 4u) {
        sprite_ft4_shadow.snapshot.field_builder_caller_candidates[empty] = caller;
        sprite_ft4_shadow.snapshot.field_builder_caller_counts[empty] = 1u;
        return;
    }
    for (uint32_t index = 0u; index < 4u; ++index)
        --sprite_ft4_shadow.snapshot.field_builder_caller_counts[index];
}

static uint8_t field_sprite_overlay_family(uint32_t caller) {
    if (overlay_projected_2e_descriptor_scope &&
        (physical_address_equals(caller, UINT32_C(0x801d3e30)) ||
         physical_address_equals(caller, UINT32_C(0x801d3e70)) ||
         physical_address_equals(caller, UINT32_C(0x801d3eb0)) ||
         physical_address_equals(caller, UINT32_C(0x801d3ef0))))
        return 10u;
    if (physical_address_equals(caller, UINT32_C(0x801e855c)) ||
        physical_address_equals(caller, UINT32_C(0x801e8a38)))
        return 4u;
    if (physical_address_equals(caller, UINT32_C(0x801e8628)) ||
        physical_address_equals(caller, UINT32_C(0x801e8a98)))
        return 5u;
    return 0u;
}

static bool field_sprite_builder_begin(CPUState *cpu) {
    GpuDrawState draw = { 0 };
    uint32_t source_base;
    uint32_t table_entry;
    uint32_t descriptor_base;
    uint32_t packet_base;
    uint32_t packet_buffer;
    int16_t origin_x;
    int16_t origin_y;
    uint16_t scale;
    uint32_t count;
    XgRenderProducerLifecycle lifecycle;

    ++sprite_ft4_shadow.snapshot.field_builder_begin_count;
    sprite_ft4_shadow.snapshot.last_field_builder_caller =
        cpu != NULL ? cpu->gpr[31] : 0u;
    observe_field_sprite_builder_caller(
        sprite_ft4_shadow.snapshot.last_field_builder_caller);
    field_sprite_builder.overlay_family = field_sprite_overlay_family(
        sprite_ft4_shadow.snapshot.last_field_builder_caller);
    if (state.active)
        ++sprite_ft4_shadow.snapshot.field_builder_active_scene_count;
    if (state.requested_render_mode != GUEST_RENDER_RENDER_SHADOW &&
        state.requested_render_mode != GUEST_RENDER_RENDER_NATIVE)
        return false;
    if (state.requested_render_mode == GUEST_RENDER_RENDER_NATIVE &&
        !producer_lifecycle_begin(UINT32_C(0x8002675c), &lifecycle))
        return false;
    if (sprite_ft4_shadow.snapshot.field_builder_blocked) return false;
    if (sprite_ft4_shadow.snapshot.field_builder_pending) {
        block_field_sprite_builder(1u);
        return false;
    }
    if (cpu == NULL || cpu->read_word == NULL || cpu->read_half == NULL ||
        cpu->read_byte == NULL)
        return false;
    source_base = cpu->gpr[4];
    if (cpu->gpr[5] > (UINT32_MAX - source_base - 4u) / 2u) {
        block_field_sprite_builder(2u);
        return false;
    }
    table_entry = source_base + cpu->gpr[5] * 2u + 4u;
    descriptor_base = source_base + cpu->read_half(table_entry);
    count = cpu->read_half(descriptor_base);
    packet_base = cpu->gpr[6];
    packet_buffer = cpu->gpr[7];
    origin_x = (int16_t)cpu->read_half(cpu->gpr[29] + 0x10u);
    origin_y = (int16_t)cpu->read_half(cpu->gpr[29] + 0x14u);
    scale = cpu->read_half(cpu->gpr[29] + 0x18u);
    if (count > XG_RENDER_FIELD_SPRITE_BUILDER_CAPACITY) {
        block_field_sprite_builder(3u);
        return false;
    }
    gpu_get_draw_state(&draw);
    for (uint32_t index = 0u; index < count; ++index) {
        const uint32_t descriptor = descriptor_base + 4u + index * 0x1cu;
        const uint32_t packet = packet_base + index * 0x50u +
            packet_buffer * 0x28u;
        XgRenderFieldSpriteBuilderRecord *record =
            &field_sprite_builder.records[index];
        XgRenderQuadSource source = { 0 };
        const int32_t x = origin_x +
            ((int32_t)(int16_t)cpu->read_half(descriptor + 8u) * scale) /
                4096;
        const int32_t y = origin_y +
            ((int32_t)(int16_t)cpu->read_half(descriptor + 10u) * scale) /
                4096;
        const int32_t width =
            ((int32_t)(int16_t)cpu->read_half(descriptor + 4u) * scale) /
                4096;
        const int32_t height =
            ((int32_t)(int16_t)cpu->read_half(descriptor + 6u) * scale) /
                4096;
        int32_t left = x;
        int32_t right = x + width;
        int32_t top = y;
        int32_t bottom = y + height;
        int32_t u0 = cpu->read_half(descriptor) - 1u;
        int32_t v0 = cpu->read_half(descriptor + 2u) - 1u;
        int32_t u1;
        int32_t v1;
        int32_t uv_width = cpu->read_byte(descriptor + 4u);
        int32_t uv_height = cpu->read_byte(descriptor + 6u);

        if (cpu->read_byte(descriptor + 0x1au) == 0u) {
            u0 = cpu->read_half(descriptor);
        } else {
            const int32_t swap = left;
            left = right;
            right = swap;
            if ((int16_t)u0 < 0) {
                u0 = 0;
                --uv_width;
            }
        }
        u1 = u0 + uv_width;
        if (cpu->read_byte(descriptor + 0x1bu) == 0u) {
            v0 = cpu->read_half(descriptor + 2u);
        } else {
            const int32_t swap = top;
            top = bottom;
            bottom = swap;
            if ((int16_t)v0 < 0) {
                v0 = 0;
                --uv_height;
            }
        }
        v1 = v0 + uv_height;
        record->lifecycle = lifecycle;
        record->packet_address = packet;
        record->descriptor_address = descriptor;
        /* packet_base names the logical packet slot; packet_buffer only
         * selects its alternating physical copy. Source descriptors are
         * shared assets and therefore cannot identify sprite instances. */
        record->interpolation_producer_id =
            packet_base & UINT32_C(0x1fffffff);
        record->interpolation_primitive_id = index;
        record->interpolation_identity_valid =
            record->interpolation_producer_id != 0u;
        if (sprite_ft4_shadow.snapshot.field_builder_min_packet == 0u ||
            packet < sprite_ft4_shadow.snapshot.field_builder_min_packet)
            sprite_ft4_shadow.snapshot.field_builder_min_packet = packet;
        if (packet > sprite_ft4_shadow.snapshot.field_builder_max_packet)
            sprite_ft4_shadow.snapshot.field_builder_max_packet = packet;
        record->xy[0] = (uint16_t)left | ((uint32_t)(uint16_t)top << 16u);
        record->xy[1] = (uint16_t)right | ((uint32_t)(uint16_t)top << 16u);
        record->xy[2] = (uint16_t)left | ((uint32_t)(uint16_t)bottom << 16u);
        record->xy[3] = (uint16_t)right | ((uint32_t)(uint16_t)bottom << 16u);
        record->uv[0] = (uint8_t)u0 | ((uint16_t)(uint8_t)v0 << 8u);
        record->uv[1] = (uint8_t)u1 | ((uint16_t)(uint8_t)v0 << 8u);
        record->uv[2] = (uint8_t)u0 | ((uint16_t)(uint8_t)v1 << 8u);
        record->uv[3] = (uint8_t)u1 | ((uint16_t)(uint8_t)v1 << 8u);
        record->tpage = field_sprite_tpage(
            cpu->read_half(descriptor + 16u),
            (int16_t)cpu->read_half(descriptor + 22u),
            (int16_t)cpu->read_half(descriptor + 24u));
        record->clut = field_sprite_clut(
            (int16_t)cpu->read_half(descriptor + 18u),
            (int16_t)cpu->read_half(descriptor + 20u));
        apply_draw_state(&source.material, &draw);
        source.material.tpage = record->tpage;
        source.material.texture_page_x = record->tpage & 0x0fu;
        source.material.texture_page_y = (record->tpage >> 4u) & 1u;
        source.material.texture_depth = (XgRenderIrTextureDepth)(
            (record->tpage >> 7u) & 3u);
        source.material.blend_mode = (XgRenderIrBlendMode)(
            (record->tpage >> 5u) & 3u);
        source.material.clut_x = (record->clut & 0x3fu) << 4u;
        source.material.clut_y = record->clut >> 6u;
        source.material.shading = XG_RENDER_IR_SHADING_FLAT;
        source.material.textured = true;
        source.material.raw_texture = true;
        for (uint32_t vertex = 0u; vertex < 4u; ++vertex) {
            source.vertices[vertex].x = (int16_t)record->xy[vertex];
            source.vertices[vertex].y = (int16_t)(record->xy[vertex] >> 16u);
            source.vertices[vertex].u = (uint8_t)record->uv[vertex];
            source.vertices[vertex].v = (uint8_t)(record->uv[vertex] >> 8u);
        }
        if (xg_render_quad_build_primitive(
                &source, &record->primitive) != XG_RENDER_QUAD_BUILDER_OK) {
            block_field_sprite_builder(4u);
            return false;
        }
    }
    field_sprite_builder.count = count;
    sprite_ft4_shadow.snapshot.field_builder_pending = true;
    return true;
}

static void field_sprite_builder_finish(CPUState *cpu) {
    bool all_match = true;

    if (!sprite_ft4_shadow.snapshot.field_builder_pending) return;
    if (cpu == NULL || cpu->read_word == NULL || cpu->read_half == NULL) {
        block_field_sprite_builder(6u);
        return;
    }
    if (state.requested_render_mode == GUEST_RENDER_RENDER_NATIVE &&
        (field_sprite_builder.count == 0u ||
         !producer_lifecycle_matches(
             &field_sprite_builder.records[0].lifecycle))) {
        block_field_sprite_builder(10u);
        return;
    }
    sprite_ft4_shadow.snapshot.field_builder_primitive_count +=
        field_sprite_builder.count;
    if (field_sprite_builder.overlay_family == 10u) {
        for (uint32_t index = 0u; index < field_sprite_builder.count; ++index) {
            const XgRenderFieldSpriteBuilderRecord *record =
                &field_sprite_builder.records[index];
            XgRenderOverlayFt4Template *overlay =
                overlay_ft4_upsert_field_template(record);

            if (overlay == NULL) {
                sprite_ft4_shadow.snapshot.field_builder_failure_detail =
                    100u + overlay_ft4_upsert_failure_detail;
                block_field_sprite_builder(5u);
                return;
            }
            overlay->primitive = record->primitive;
            overlay->primitive.material.raw_texture = false;
            overlay->primitive.material.semi_transparent = true;
            for (uint32_t triangle = 0u;
                 triangle < overlay->primitive.triangle_count; ++triangle) {
                for (uint32_t vertex = 0u; vertex < 3u; ++vertex) {
                    XgRenderIrVertex *target =
                        &overlay->primitive.triangles[triangle].vertices[vertex];
                    target->r = 0x80u;
                    target->g = 0x80u;
                    target->b = 0x80u;
                }
            }
            overlay->material = overlay->primitive.material;
            overlay->source_primitive_index = UINT32_C(0x2e3d0000) |
                (record->packet_address & UINT32_C(0xffff));
            overlay->family = 10u;
            overlay->material_ready = true;
            overlay->valid = true;
            ++overlay_ft4_2c.projected_2e_material_count;
        }
        clear_field_sprite_builder();
        return;
    }
    for (uint32_t index = 0u; index < field_sprite_builder.count; ++index) {
        const XgRenderFieldSpriteBuilderRecord *record =
            &field_sprite_builder.records[index];
        const uint32_t command = cpu->read_word(record->packet_address + 4u);
        const uint16_t actual_clut =
            cpu->read_half(record->packet_address + 14u);
        const uint16_t actual_tpage =
            cpu->read_half(record->packet_address + 22u);
        uint32_t mismatch_bits =
            ((command & UINT32_C(0xff000000)) != UINT32_C(0x2d000000) ?
                 1u : 0u) |
            (actual_clut != record->clut ? 2u : 0u) |
            (actual_tpage != record->tpage ? 4u : 0u);

        for (uint32_t vertex = 0u; vertex < 4u; ++vertex) {
            if (cpu->read_word(record->packet_address + 8u + vertex * 8u) !=
                record->xy[vertex])
                mismatch_bits |= 8u << vertex;
            if (cpu->read_half(record->packet_address + 12u + vertex * 8u) !=
                record->uv[vertex])
                mismatch_bits |= 0x80u << vertex;
        }
        if (mismatch_bits == 0u) {
            ++sprite_ft4_shadow.snapshot.field_builder_match_count;
        } else {
            all_match = false;
            if (sprite_ft4_shadow.snapshot.field_builder_mismatch_count == 0u) {
                sprite_ft4_shadow.snapshot.field_builder_first_mismatch_packet =
                    record->packet_address;
                sprite_ft4_shadow.snapshot.field_builder_first_mismatch_descriptor =
                    record->descriptor_address;
                sprite_ft4_shadow.snapshot.field_builder_first_mismatch_caller =
                    sprite_ft4_shadow.snapshot.last_field_builder_caller;
                sprite_ft4_shadow.snapshot.field_builder_first_mismatch_bits =
                    mismatch_bits;
                sprite_ft4_shadow.snapshot.field_builder_expected_tpage =
                    record->tpage;
                sprite_ft4_shadow.snapshot.field_builder_actual_tpage =
                    actual_tpage;
                sprite_ft4_shadow.snapshot.field_builder_expected_clut =
                    record->clut;
                sprite_ft4_shadow.snapshot.field_builder_actual_clut = actual_clut;
                sprite_ft4_shadow.snapshot.field_builder_actual_command = command;
                for (uint32_t vertex = 0u; vertex < 4u; ++vertex) {
                    sprite_ft4_shadow.snapshot.field_builder_expected_xy[vertex] =
                        record->xy[vertex];
                    sprite_ft4_shadow.snapshot.field_builder_actual_xy[vertex] =
                        cpu->read_word(record->packet_address + 8u + vertex * 8u);
                    sprite_ft4_shadow.snapshot.field_builder_expected_uv[vertex] =
                        record->uv[vertex];
                    sprite_ft4_shadow.snapshot.field_builder_actual_uv[vertex] =
                        cpu->read_half(
                            record->packet_address + 12u + vertex * 8u);
                }
            }
            ++sprite_ft4_shadow.snapshot.field_builder_mismatch_count;
        }
    }
    if (state.requested_render_mode == GUEST_RENDER_RENDER_NATIVE) {
        uint32_t staged_count = 0u;

        if (!all_match) {
            block_field_sprite_builder(8u);
            return;
        }
        for (uint32_t index = 0u; index < field_sprite_builder.count;
             ++index) {
            if (!field_sprite_template_capture(
                    &field_sprite_builder.records[index])) {
                block_field_sprite_builder(9u);
                return;
            }
        }
        for (uint32_t index = 0u; index < field_sprite_builder.count; ++index) {
            const XgRenderFieldSpriteBuilderRecord *record =
                &field_sprite_builder.records[index];
            const uint32_t packet = record->packet_address;

            if (field_sprite_builder.overlay_family != 0u) {
                XgRenderOverlayFt4Template *overlay =
                    overlay_ft4_upsert_field_template(record);
                if (overlay == NULL) {
                    sprite_ft4_shadow.snapshot.field_builder_failure_detail =
                        200u + overlay_ft4_upsert_failure_detail;
                    block_field_sprite_builder(5u);
                    return;
                }
                overlay->primitive = record->primitive;
                overlay->material = record->primitive.material;
                overlay->source_primitive_index = UINT32_C(0x2e580000) |
                    (packet & UINT32_C(0xffff));
                overlay->family = field_sprite_builder.overlay_family;
                overlay->material_ready = false;
                overlay->valid = true;
                ++overlay_ft4_2c.field_source_template_count;
                if (overlay->family == 4u)
                    ++overlay_ft4_2c.field_base_template_count;
                else
                    ++overlay_ft4_2c.field_offset_template_count;
                continue;
            }

            if (!stage_standalone_native_primitive_identified(
                    &record->primitive, packet,
                    UINT32_C(0x53000000) |
                        (packet & UINT32_C(0x001ffffc)),
                    record->interpolation_producer_id,
                    record->interpolation_primitive_id)) {
                sprite_ft4_shadow.snapshot.field_builder_failure_detail =
                    1000u + standalone_stage_failure_detail;
                block_field_sprite_builder(5u);
                return;
            }
            ++staged_count;
        }
        if (staged_count != 0u)
            ++sprite_ft4_shadow.snapshot.field_builder_native_cutover_count;
        sprite_ft4_shadow.snapshot.field_builder_native_primitive_count +=
            staged_count;
    }
    clear_field_sprite_builder();
}

static void clear_field_polyline_pending(void) {
    PsxXgRenderFieldPolylineSnapshot snapshot = field_polyline.snapshot;

    if (field_polyline.count == 0u && !snapshot.pending) return;
    snapshot.pending = false;
    field_polyline = (XgRenderFieldPolylineState){ .snapshot = snapshot };
}

static void block_field_polyline(uint32_t blocker) {
    clear_field_polyline_pending();
    field_polyline.snapshot.blocked = true;
    if (field_polyline.snapshot.blocker == 0u)
        field_polyline.snapshot.blocker = blocker;
}

static XgHost3dVector field_polyline_read_vector(
    CPUState *cpu, uint32_t address) {
    const uint32_t xy = cpu->read_word(address);
    const uint32_t zp = cpu->read_word(address + 4u);

    return (XgHost3dVector){
        low_s16(xy), low_s16(xy >> 16u), low_s16(zp), (uint16_t)(zp >> 16u),
    };
}

static void field_polyline_apply_draw_state(
    GpuRenderMaterial *material, const GpuDrawState *draw) {
    material->draw_area_left = draw->left;
    material->draw_area_top = draw->top;
    material->draw_area_right = draw->right;
    material->draw_area_bottom = draw->bottom;
    material->draw_offset_x = draw->offset_x;
    material->draw_offset_y = draw->offset_y;
    material->texture_window_mask_x = draw->texture_window_mask_x;
    material->texture_window_mask_y = draw->texture_window_mask_y;
    material->texture_window_offset_x = draw->texture_window_offset_x;
    material->texture_window_offset_y = draw->texture_window_offset_y;
    material->dither = draw->dither;
    material->mask_set = draw->mask_set;
    material->mask_check = draw->mask_check;
}

static void clear_resident_line_f2_source(void) {
    resident_line_f2_source = (XgRenderResidentLineF2Source){0};
}

static bool capture_resident_line_f2_source(CPUState *cpu) {
    XgRenderResidentLineF2Source source = {0};
    uint32_t heap_address;
    uint32_t table_address;
    uint8_t level;
    uint8_t publish_count;
    uint8_t buffer_index;

    clear_resident_line_f2_source();
    if (cpu == NULL || cpu->read_word == NULL || cpu->read_byte == NULL ||
        state.requested_render_mode != GUEST_RENDER_RENDER_NATIVE)
        return false;
    level = (uint8_t)cpu->gpr[4];
    publish_count = (uint8_t)cpu->gpr[5];
    if (level == publish_count) publish_count = (uint8_t)(publish_count - 1u);
    source.source_count = level > 0u ? (uint8_t)(level - 1u) : 0u;
    if (source.source_count > XG_RENDER_RESIDENT_LINE_F2_CAPACITY ||
        publish_count > source.source_count)
        return false;
    heap_address = cpu->read_word(UINT32_C(0x800c3ea4));
    buffer_index = cpu->read_byte(UINT32_C(0x800ccb34));
    if (buffer_index > 1u || !word_address_is_valid(heap_address)) return false;
    table_address = UINT32_C(0x800c3202) + (uint32_t)level * 6u;
    if (source.source_count != 0u && !guest_data_range_is_valid(
            table_address, source.source_count, 1u, false))
        return false;
    for (uint32_t index = 0u; index < source.source_count; ++index) {
        const uint32_t packet = heap_address + 0x908u +
            ((uint32_t)buffer_index + index * 2u) * 0x10u;
        const uint16_t y =
            (uint16_t)(cpu->read_byte(table_address + index) + 0x5eu);

        if (packet < heap_address ||
            !guest_data_range_is_valid(packet, 0x10u, 4u, false))
            return false;
        source.packet_addresses[index] = packet;
        source.xy[index][0] = UINT32_C(0x0000000c) | ((uint32_t)y << 16u);
        source.xy[index][1] = UINT32_C(0x00000012) | ((uint32_t)y << 16u);
    }
    source.owner_cpu = cpu;
    source.heap_address = heap_address;
    source.buffer_index = buffer_index;
    source.publish_count = publish_count;
    source.valid = true;
    resident_line_f2_source = source;
    return true;
}

static bool stage_resident_line_f2(CPUState *cpu) {
    GpuRenderSemantic semantics[XG_RENDER_RESIDENT_LINE_F2_CAPACITY] = {0};
    GpuDrawState draw = {0};
    uint32_t state_address;
    uint32_t heap_address;
    uint8_t count;
    uint8_t buffer_index;

    if (cpu == NULL || cpu->read_word == NULL || cpu->read_byte == NULL ||
        state.requested_render_mode != GUEST_RENDER_RENDER_NATIVE)
        return false;
    state_address = cpu->read_word(UINT32_C(0x800d2d28));
    heap_address = cpu->read_word(UINT32_C(0x800c3ea4));
    if (state_address > UINT32_MAX - 0x99u ||
        !guest_data_range_is_valid(state_address + 0x97u, 2u, 1u, false))
        return false;
    count = cpu->read_byte(state_address + 0x97u);
    buffer_index = cpu->read_byte(state_address + 0x98u);
    if (count == 0u) return true;
    if (!resident_line_f2_source.valid ||
        resident_line_f2_source.owner_cpu != cpu ||
        resident_line_f2_source.heap_address != heap_address ||
        resident_line_f2_source.buffer_index != buffer_index ||
        resident_line_f2_source.publish_count != count ||
        count > resident_line_f2_source.source_count)
        return false;

    gpu_get_draw_state(&draw);
    for (uint32_t index = 0u; index < count; ++index) {
        const uint32_t packet = heap_address + 0x908u +
            ((uint32_t)buffer_index + index * 2u) * 0x10u;
        GpuRenderSemantic *semantic = &semantics[index];

        if (packet != resident_line_f2_source.packet_addresses[index] ||
            !guest_data_range_is_valid(packet, 0x10u, 4u, false) ||
            (cpu->read_word(packet) >> 24u) != 3u ||
            cpu->read_word(packet + 4u) != UINT32_C(0x40ffffff) ||
            cpu->read_word(packet + 8u) !=
                resident_line_f2_source.xy[index][0] ||
            cpu->read_word(packet + 12u) !=
                resident_line_f2_source.xy[index][1])
            return false;
        semantic->topology = GPU_RENDER_SEMANTIC_LINES;
        semantic->line_count = 1u;
        semantic->material.shading = GPU_RENDER_SHADING_FLAT;
        field_polyline_apply_draw_state(&semantic->material, &draw);
        for (uint32_t vertex = 0u; vertex < 2u; ++vertex) {
            const uint32_t xy = resident_line_f2_source.xy[index][vertex];
            GpuRenderSemanticVertex *destination =
                &semantic->lines[0].vertices[vertex];

            destination->x = (int32_t)low_s16(xy) * INT32_C(65536);
            destination->y =
                (int32_t)low_s16(xy >> 16u) * INT32_C(65536);
            destination->r = UINT8_C(0xff);
            destination->g = UINT8_C(0xff);
            destination->b = UINT8_C(0xff);
        }
    }
    for (uint32_t index = 0u; index < count; ++index) {
        const uint32_t packet = resident_line_f2_source.packet_addresses[index];

        if (!stage_standalone_native_semantic_identified(
                &semantics[index], packet,
                UINT32_C(0x40000000) |
                    (packet & UINT32_C(0x001ffffc)),
                UINT32_C(0x80073b64), index)) {
            abort_standalone_submission();
            return false;
        }
    }
    return true;
}

static bool field_polyline_add_record(
    CPUState *cpu, uint32_t object, uint32_t packet,
    const uint32_t vector_offsets[3], uint8_t red, uint8_t green,
    const XgHost3dProjection *projection, const GpuDrawState *draw,
    uint32_t interpolation_primitive_id) {
    XgHost3dProject4Input input = { 0 };
    XgHost3dRotTransPers4Output output;
    XgRenderFieldPolylineRecord *record;

    if (field_polyline.count >= XG_RENDER_FIELD_POLYLINE_CAPACITY)
        return false;
    for (uint32_t vertex = 0u; vertex < 3u; ++vertex) {
        const uint32_t address = object + vector_offsets[vertex];

        if (!guest_data_range_is_valid(address, 8u, 2u, false)) return false;
        input.vertices[vertex] = field_polyline_read_vector(cpu, address);
    }
    input.vertices[3] = input.vertices[2];
    input.projection = *projection;
    if (!xg_host_3d_rot_trans_pers4(&input, &output)) return false;

    record = &field_polyline.records[field_polyline.count++];
    record->packet_address = packet;
    record->interpolation_primitive_id = interpolation_primitive_id;
    record->command_word = UINT32_C(0x48000000) |
        (uint32_t)red | ((uint32_t)green << 8u);
    record->semantic.topology = GPU_RENDER_SEMANTIC_LINES;
    record->semantic.line_count = 2u;
    field_polyline_apply_draw_state(&record->semantic.material, draw);
    record->semantic.material.shading = GPU_RENDER_SHADING_FLAT;
    for (uint32_t vertex = 0u; vertex < 3u; ++vertex) {
        GpuRenderSemanticVertex semantic_vertex = {
            .x = (int32_t)output.vertices[vertex].x * INT32_C(65536),
            .y = (int32_t)output.vertices[vertex].y * INT32_C(65536),
            .r = red,
            .g = green,
            .b = 0u,
            .native_view_x = output.vertices[vertex].native_view_x_16_16,
            .native_view_y = output.vertices[vertex].native_view_y_16_16,
            .native_view_position =
                output.vertices[vertex].native_view_position,
            .projective_view_x = output.vertices[vertex].projective_view_x,
            .projective_view_y = output.vertices[vertex].projective_view_y,
            .projective_view_z = output.vertices[vertex].projective_view_z,
            .projective_offset_x =
                output.vertices[vertex].projective_offset_x_16_16,
            .projective_offset_y =
                output.vertices[vertex].projective_offset_y_16_16,
            .projective_native_offset_x = output.vertices[vertex]
                .projective_native_offset_x_16_16,
            .projective_native_offset_y = output.vertices[vertex]
                .projective_native_offset_y_16_16,
            .projective_distance =
                output.vertices[vertex].projective_distance,
            .projective_position =
                output.vertices[vertex].projective_position,
        };

        record->xy[vertex] = (uint16_t)output.vertices[vertex].x |
            ((uint32_t)(uint16_t)output.vertices[vertex].y << 16u);
        if (vertex < 2u)
            record->semantic.lines[vertex].vertices[0] = semantic_vertex;
        if (vertex > 0u)
            record->semantic.lines[vertex - 1u].vertices[1] = semantic_vertex;
    }
    return true;
}

static bool field_polyline_begin(CPUState *cpu) {
    static const uint32_t first_offsets[3] = { 0x100u, 0x108u, 0x118u };
    static const uint32_t second_offsets[3] = { 0x120u, 0x130u, 0x138u };
    XgHost3dProjection projection;
    GpuDrawState draw = { 0 };
    uint32_t global;
    uint32_t field;
    uint32_t selected_index;
    uint32_t buffer;
    uint8_t selected_group;
    uint8_t mode;

    ++field_polyline.snapshot.begin_count;
    if (state.requested_render_mode != GUEST_RENDER_RENDER_SHADOW &&
        state.requested_render_mode != GUEST_RENDER_RENDER_NATIVE)
        return false;
    if (field_polyline.snapshot.blocked) return false;
    if (field_polyline.snapshot.pending) {
        block_field_polyline(1u);
        return false;
    }
    if (cpu == NULL || cpu->read_word == NULL || cpu->read_half == NULL ||
        cpu->read_byte == NULL)
        return false;
    global = cpu->read_word(UINT32_C(0x800625a0));
    if (!guest_data_range_is_valid(global, 0x4d9u, 4u, false)) {
        block_field_polyline(2u);
        return false;
    }
    mode = cpu->read_byte(global + 0x4d8u);
    if (mode == 0u) return false;
    field = cpu->read_word(global + 0x32cu);
    if (!guest_data_range_is_valid(field, 0x4fe6u, 4u, false)) {
        block_field_polyline(3u);
        return false;
    }
    selected_index = cpu->read_word(
        UINT32_C(0x801e981c) + cpu->read_word(field + 0x4f7cu) * 4u);
    if (selected_index >= 32u) {
        block_field_polyline(4u);
        return false;
    }
    selected_group = cpu->read_byte(field + 0x4faeu + selected_index);
    buffer = cpu->read_word(global + 0x308u);
    if (buffer > 1u) {
        block_field_polyline(5u);
        return false;
    }
    capture_shadow_projection(cpu, &projection);
    gpu_get_draw_state(&draw);
    for (uint32_t index = 0u; index < 32u; ++index) {
        uint32_t object;
        uint8_t red = 0u;
        uint8_t green = UINT8_C(0xff);

        if (index == 15u || index == 31u ||
            cpu->read_byte(field + 0x4fe4u + index / 16u) == 0u)
            continue;
        if (selected_group == cpu->read_byte(field + 0x4faeu + index) &&
            mode == 2u &&
            (selected_group != UINT8_C(0xff) || index == selected_index)) {
            red = UINT8_C(0xff);
            green = 0u;
        }
        object = cpu->read_word(global + 0x3a8u + index * 4u);
        if (!guest_data_range_is_valid(object, 0x140u, 4u, false) ||
            !field_polyline_add_record(
                cpu, object, object + 0x50u + buffer * 0x18u,
                first_offsets, red, green, &projection, &draw,
                index * 2u) ||
            !field_polyline_add_record(
                cpu, object, object + 0x80u + buffer * 0x18u,
                second_offsets, red, green, &projection, &draw,
                index * 2u + 1u)) {
            block_field_polyline(6u);
            return false;
        }
    }
    ++field_polyline.snapshot.invocation_count;
    field_polyline.snapshot.pending = true;
    return true;
}

static void field_polyline_finish(CPUState *cpu) {
    bool all_match = true;

    if (!field_polyline.snapshot.pending) return;
    if (cpu == NULL || cpu->read_word == NULL) {
        block_field_polyline(7u);
        return;
    }
    field_polyline.snapshot.primitive_count += field_polyline.count;
    for (uint32_t index = 0u; index < field_polyline.count; ++index) {
        const XgRenderFieldPolylineRecord *record =
            &field_polyline.records[index];
        bool matches =
            (cpu->read_word(record->packet_address) >> 24u) == 5u &&
            cpu->read_word(record->packet_address + 4u) ==
                record->command_word &&
            (cpu->read_word(record->packet_address + 0x14u) &
             UINT32_C(0xf000f000)) == UINT32_C(0x50005000);

        for (uint32_t vertex = 0u; vertex < 3u; ++vertex)
            matches &= cpu->read_word(
                record->packet_address + 8u + vertex * 4u) ==
                record->xy[vertex];
        if (matches) {
            ++field_polyline.snapshot.match_count;
        } else {
            all_match = false;
            if (field_polyline.snapshot.mismatch_count == 0u)
                field_polyline.snapshot.first_mismatch_packet =
                    record->packet_address;
            ++field_polyline.snapshot.mismatch_count;
        }
    }
    if (state.requested_render_mode == GUEST_RENDER_RENDER_NATIVE) {
        if (!all_match) {
            block_field_polyline(8u);
            return;
        }
        for (uint32_t index = 0u; index < field_polyline.count; ++index) {
            const XgRenderFieldPolylineRecord *record =
                &field_polyline.records[index];

            if (!stage_standalone_native_semantic_identified(
                    &record->semantic, record->packet_address,
                    UINT32_C(0x48000000) |
                        (record->packet_address & UINT32_C(0x001ffffc)),
                    UINT32_C(0x801cfb48),
                    record->interpolation_primitive_id)) {
                block_field_polyline(10u);
                return;
            }
        }
        ++field_polyline.snapshot.native_cutover_count;
        field_polyline.snapshot.native_primitive_count += field_polyline.count;
    }
    clear_field_polyline_pending();
}

static void sprite_ft4_shadow_begin(CPUState *cpu, bool wrapper_scope) {
    uint32_t data_address;
    uint32_t descriptor_address;
    uint32_t primitive_count;

    if (state.requested_render_mode != GUEST_RENDER_RENDER_SHADOW &&
        state.requested_render_mode != GUEST_RENDER_RENDER_NATIVE) {
        clear_sprite_ft4_shadow_context();
        return;
    }
    if (sprite_ft4_shadow.snapshot.blocked) return;
    if (sprite_ft4_shadow.snapshot.context_active ||
        sprite_ft4_shadow.snapshot.pending) {
        block_sprite_ft4_shadow(80u);
        return;
    }
    if (cpu == NULL || cpu->read_word == NULL || cpu->read_byte == NULL) {
        block_sprite_ft4_shadow(81u);
        return;
    }
    ++sprite_ft4_shadow.snapshot.caller_count;
    sprite_ft4_shadow.snapshot.last_caller = cpu->gpr[31];
    sprite_ft4_shadow.snapshot.last_sprite_address = cpu->gpr[4];
    if (!word_address_is_valid(cpu->gpr[4])) {
        sprite_ft4_shadow.snapshot.blocker_detail = 1u;
        block_sprite_ft4_shadow(81u);
        return;
    }
    data_address = cpu->read_word(cpu->gpr[4] + 0x20u);
    primitive_count = (cpu->read_byte(cpu->gpr[4] + 0x40u) >> 2u) & 0x3fu;
    sprite_ft4_shadow.snapshot.last_data_address = data_address;
    sprite_ft4_shadow.snapshot.last_primitive_count = primitive_count;
    if (primitive_count == 0u) {
        ++sprite_ft4_shadow.snapshot.empty_caller_count;
        sprite_ft4_shadow.snapshot.blocker_detail = 0u;
        return;
    }
    if (!word_address_is_valid(data_address)) {
        sprite_ft4_shadow.snapshot.blocker_detail = 2u;
        block_sprite_ft4_shadow(82u);
        return;
    }
    descriptor_address = cpu->read_word(data_address + 0x30u);
    sprite_ft4_shadow.snapshot.last_descriptor_address = descriptor_address;
    if (!word_address_is_valid(descriptor_address) ||
        descriptor_address > UINT32_MAX - primitive_count * 0x18u) {
        sprite_ft4_shadow.snapshot.blocker_detail =
            (!word_address_is_valid(descriptor_address) ? 8u : 0u) |
            (descriptor_address > UINT32_MAX - primitive_count * 0x18u
                 ? 16u : 0u);
        block_sprite_ft4_shadow(82u);
        return;
    }
    sprite_ft4_shadow.sprite_address = cpu->gpr[4];
    sprite_ft4_shadow.invocation_matches = true;
    sprite_ft4_shadow.wrapper_scope = wrapper_scope;
    sprite_ft4_shadow.snapshot.blocker_detail = 0u;
    sprite_ft4_shadow.snapshot.context_active = true;
}

static bool sprite_ft4_shadow_prepare(CPUState *cpu) {
    XgSpriteFt4Source source = { 0 };
    XgSpriteFt4Record projected;
    GpuDrawState draw = { 0 };
    uint32_t data_address;
    uint32_t descriptor_base;
    uint32_t primitive_count;
    uint32_t descriptor_address;
    uint32_t packet_address;
    uint32_t vertex;

    if (!sprite_ft4_shadow.snapshot.context_active ||
        sprite_ft4_shadow.snapshot.pending || cpu == NULL ||
        cpu->read_word == NULL || cpu->read_half == NULL ||
        cpu->read_byte == NULL || cpu->gpr[20] !=
            sprite_ft4_shadow.sprite_address)
        return false;
    data_address = cpu->read_word(sprite_ft4_shadow.sprite_address + 0x20u);
    descriptor_base = cpu->read_word(data_address + 0x30u);
    primitive_count =
        (cpu->read_byte(sprite_ft4_shadow.sprite_address + 0x40u) >> 2u) &
        0x3fu;
    descriptor_address = cpu->gpr[19];
    packet_address = cpu->gpr[16];
    if (cpu->gpr[18] != UINT32_C(0x8004fb98) ||
        descriptor_address < descriptor_base ||
        descriptor_address >= descriptor_base + primitive_count * 0x18u ||
        (descriptor_address - descriptor_base) % 0x18u != 0u ||
        !word_address_is_valid(packet_address) ||
        !word_address_is_valid(packet_address + 36u))
        return false;

    capture_shadow_projection(cpu, &source.projection);
    for (vertex = 0u; vertex < 4u; ++vertex) {
        const uint32_t address = cpu->gpr[18] + vertex * 8u;
        const uint32_t xy = cpu->read_word(address);
        const uint32_t zp = cpu->read_word(address + 4u);

        source.vertices[vertex] = (XgHost3dVector){
            low_s16(xy), low_s16(xy >> 16u), low_s16(zp),
            (uint16_t)(zp >> 16u),
        };
    }
    sprite_ft4_shadow.material_word =
        cpu->read_word(descriptor_address + 0x10u);
    source.color[0] = (uint8_t)sprite_ft4_shadow.material_word;
    source.color[1] = (uint8_t)(sprite_ft4_shadow.material_word >> 8u);
    source.color[2] = (uint8_t)(sprite_ft4_shadow.material_word >> 16u);
    sprite_ft4_shadow.tpage = cpu->read_half(descriptor_address + 0x0au);
    sprite_ft4_shadow.clut = cpu->read_half(descriptor_address + 0x0cu);
    if (((sprite_ft4_shadow.material_word >> 24u) & 0xfcu) != 0x2cu)
        return false;
    gpu_get_draw_state(&draw);
    apply_draw_state(&source.material, &draw);
    source.material.tpage = sprite_ft4_shadow.tpage;
    source.material.texture_page_x = sprite_ft4_shadow.tpage & 0x0fu;
    source.material.texture_page_y =
        (sprite_ft4_shadow.tpage >> 4u) & 1u;
    source.material.texture_depth = (XgRenderIrTextureDepth)(
        (sprite_ft4_shadow.tpage >> 7u) & 3u);
    source.material.blend_mode = (XgRenderIrBlendMode)(
        (sprite_ft4_shadow.tpage >> 5u) & 3u);
    source.material.clut_x = (sprite_ft4_shadow.clut & 0x3fu) << 4u;
    source.material.clut_y = sprite_ft4_shadow.clut >> 6u;
    source.material.shading = XG_RENDER_IR_SHADING_FLAT;
    source.material.textured = true;
    source.material.raw_texture =
        ((sprite_ft4_shadow.material_word >> 24u) & 1u) != 0u;
    source.material.semi_transparent =
        ((sprite_ft4_shadow.material_word >> 24u) & 2u) != 0u;
    source.packet_vertex_for_projection[0] = 0u;
    source.packet_vertex_for_projection[1] = 1u;
    source.packet_vertex_for_projection[2] = 3u;
    source.packet_vertex_for_projection[3] = 2u;
    if (xg_sprite_ft4_build(&source, &projected) != XG_SPRITE_FT4_OK)
        return false;

    if (xg_sprite_ft4_map_uv(
            cpu->read_byte(descriptor_address + 4u),
            cpu->read_byte(descriptor_address + 5u),
            cpu->read_byte(descriptor_address + 6u),
            cpu->read_byte(descriptor_address + 7u),
            projected.vertices[3].x < projected.vertices[0].x,
            source.uv) != XG_SPRITE_FT4_OK)
        return false;
    if (xg_sprite_ft4_build(&source, &sprite_ft4_shadow.native) !=
        XG_SPRITE_FT4_OK)
        return false;
    memcpy(sprite_ft4_shadow.uv, source.uv, sizeof(source.uv));
    sprite_ft4_shadow.packet_address = packet_address;
    sprite_ft4_shadow.descriptor_address = descriptor_address;
    sprite_ft4_shadow.phase = XG_RENDER_SPRITE_FT4_SHADOW_EXPECT_XY;
    sprite_ft4_shadow.geometry_matches = true;
    sprite_ft4_shadow.payload_matches = true;
    sprite_ft4_shadow.snapshot.pending = true;
    return true;
}

static bool native_sprite_ft4_stage(CPUState *cpu) {
    XgRenderPreScenePrimitive *record;
    XgRenderProducerLifecycle lifecycle;
    uint32_t ot_address;
    uint32_t ot_base;
    uint32_t record_index;

    if (state.requested_render_mode != GUEST_RENDER_RENDER_NATIVE ||
        cpu == NULL || !sprite_ft4_shadow.snapshot.pending ||
        sprite_ft4_shadow.native_record_count ==
            XG_RENDER_SPRITE_FT4_NATIVE_CAPACITY)
        return false;
    if (!producer_lifecycle_begin(UINT32_C(0x8001e874), &lifecycle))
        return false;
    record_index = sprite_ft4_shadow.native_record_count++;
    record = &sprite_ft4_shadow.native_records[record_index];
    sprite_ft4_shadow.native_lifecycles[record_index] = lifecycle;
    sprite_ft4_shadow.native_opcodes[record_index] =
        (uint8_t)(sprite_ft4_shadow.material_word >> 24u);
    *record = (XgRenderPreScenePrimitive){
        .primitive = sprite_ft4_shadow.native.primitive,
        .packet_address = sprite_ft4_shadow.packet_address,
        .source_primitive_index = UINT32_C(0x52000000) |
            (sprite_ft4_shadow.packet_address & UINT32_C(0x001ffffc)),
        .interpolation_producer_id =
            sprite_ft4_shadow.sprite_address & UINT32_C(0x1fffffff),
        .interpolation_primitive_id =
            (sprite_ft4_shadow.descriptor_address & UINT32_C(0x1ffffffe)) |
            (sprite_ft4_shadow.wrapper_scope ? 1u : 0u),
        .payload_word_count = 9u,
        .interpolation_identity_valid = true,
    };
    if (sprite_ft4_shadow.wrapper_scope) {
        uint32_t base_ot_address;

        if (cpu->read_word == NULL) return true;
        base_ot_address = cpu->read_word(cpu->gpr[29] + 0x9cu);
        if (xg_sprite_ft4_select_ot_address(
                base_ot_address,
                cpu->read_word(sprite_ft4_shadow.sprite_address + 0x3cu),
                cpu->read_word(sprite_ft4_shadow.descriptor_address + 0x14u),
                &ot_address) != XG_SPRITE_FT4_OK)
            return true;
        ot_base = cpu->read_word(UINT32_C(0x8005956c));
        if (ot_address < ot_base || (ot_address - ot_base) % 4u != 0u ||
            (ot_address - ot_base) / 4u >= 0x1000u)
            return true;
        record->ot_bucket = (ot_address - ot_base) / 4u;
        (void)stage_pre_scene_primitive(record);
    } else if (!stage_standalone_native_primitive_identified(
            &record->primitive, record->packet_address,
            record->source_primitive_index,
            record->interpolation_producer_id,
            record->interpolation_primitive_id)) {
        --sprite_ft4_shadow.native_record_count;
        return false;
    }
    return true;
}

static void sprite_ft4_shadow_observe_xy(CPUState *cpu) {
    static const uint8_t split[2][3] = {{0u, 1u, 2u}, {2u, 1u, 3u}};
    uint32_t vertex;

    if (sprite_ft4_shadow.phase != XG_RENDER_SPRITE_FT4_SHADOW_EXPECT_XY ||
        cpu == NULL || cpu->read_word == NULL) {
        block_sprite_ft4_shadow(83u);
        return;
    }
    for (vertex = 0u; vertex < 4u; ++vertex) {
        const uint32_t expected_xy =
            (uint16_t)sprite_ft4_shadow.native.vertices[vertex].x |
            ((uint32_t)(uint16_t)
                sprite_ft4_shadow.native.vertices[vertex].y << 16u);
        const uint32_t observed_xy = cpu->read_word(
            sprite_ft4_shadow.packet_address + 8u + vertex * 8u);

        sprite_ft4_shadow.geometry_matches &= observed_xy == expected_xy;
        sprite_ft4_shadow.native.vertices[vertex].x = low_s16(observed_xy);
        sprite_ft4_shadow.native.vertices[vertex].y =
            low_s16(observed_xy >> 16u);
    }
    for (uint32_t triangle = 0u; triangle < 2u; ++triangle) {
        for (vertex = 0u; vertex < 3u; ++vertex) {
            const uint32_t source = split[triangle][vertex];
            XgRenderIrVertex *destination =
                &sprite_ft4_shadow.native.primitive
                     .triangles[triangle].vertices[vertex];

            destination->x =
                (int32_t)sprite_ft4_shadow.native.vertices[source].x *
                    INT32_C(65536);
            destination->y =
                (int32_t)sprite_ft4_shadow.native.vertices[source].y *
                    INT32_C(65536);
        }
    }
    sprite_ft4_shadow.phase = XG_RENDER_SPRITE_FT4_SHADOW_EXPECT_MATERIAL;
}

static void sprite_ft4_shadow_observe_material(CPUState *cpu) {
    uint32_t vertex;
    uint16_t expected_uv[4];

    if (sprite_ft4_shadow.phase !=
            XG_RENDER_SPRITE_FT4_SHADOW_EXPECT_MATERIAL ||
        cpu == NULL || cpu->read_word == NULL || cpu->read_half == NULL ||
        cpu->read_byte == NULL) {
        block_sprite_ft4_shadow(84u);
        return;
    }
    for (vertex = 0u; vertex < 4u; ++vertex) {
        expected_uv[vertex] = sprite_ft4_shadow.uv[vertex][0] |
            ((uint16_t)sprite_ft4_shadow.uv[vertex][1] << 8u);
    }
    sprite_ft4_shadow.payload_matches &= compare_ft4_payload(
        cpu, sprite_ft4_shadow.packet_address,
        sprite_ft4_shadow.descriptor_address,
        sprite_ft4_shadow.material_word, expected_uv,
        sprite_ft4_shadow.tpage, sprite_ft4_shadow.clut,
        &sprite_ft4_shadow.snapshot.first_payload_mismatch);
    ++sprite_ft4_shadow.snapshot.projection_count;
    if (!sprite_ft4_shadow.geometry_matches)
        ++sprite_ft4_shadow.snapshot.geometry_mismatch_count;
    if (!sprite_ft4_shadow.payload_matches)
        ++sprite_ft4_shadow.snapshot.payload_mismatch_count;
    if (sprite_ft4_shadow.payload_matches) {
        ++sprite_ft4_shadow.snapshot.match_count;
        if (state.requested_render_mode == GUEST_RENDER_RENDER_NATIVE &&
            !native_sprite_ft4_stage(cpu)) {
            sprite_ft4_shadow.snapshot.blocker_detail =
                880000u + standalone_stage_failure_detail;
            block_sprite_ft4_shadow(88u);
            return;
        }
    } else {
        sprite_ft4_shadow.invocation_matches = false;
        if (sprite_ft4_shadow.snapshot.mismatch_count == 0u) {
            sprite_ft4_shadow.snapshot.first_mismatch_packet =
                sprite_ft4_shadow.packet_address;
            sprite_ft4_shadow.snapshot.first_mismatch_descriptor =
                sprite_ft4_shadow.descriptor_address;
        }
        ++sprite_ft4_shadow.snapshot.mismatch_count;
    }
    sprite_ft4_shadow.snapshot.pending = false;
    sprite_ft4_shadow.phase = XG_RENDER_SPRITE_FT4_SHADOW_IDLE;
}

static void sprite_ft4_shadow_end(void) {
    uint32_t native_record_count = sprite_ft4_shadow.native_record_count;
    XgRenderModelFt4SourceRecord *sources[
        XG_RENDER_SPRITE_FT4_NATIVE_CAPACITY];

    if (sprite_ft4_shadow.snapshot.pending) {
        block_sprite_ft4_shadow(85u);
        return;
    }
    if (state.requested_render_mode == GUEST_RENDER_RENDER_NATIVE &&
        !sprite_ft4_shadow.invocation_matches) {
        block_sprite_ft4_shadow(89u);
        return;
    }
    if (state.requested_render_mode == GUEST_RENDER_RENDER_NATIVE &&
        native_record_count != 0u) {
        for (uint32_t index = 0u; index < native_record_count; ++index) {
            const uint32_t source_id =
                (sprite_ft4_shadow.native_records[index].packet_address &
                 UINT32_C(0x1fffffff)) + 4u;

            sources[index] = model_ft4_source_upsert(source_id);
            if (sources[index] == NULL) {
                for (uint32_t reserved = 0u; reserved < index; ++reserved)
                    sources[reserved]->valid = false;
                block_sprite_ft4_shadow(88u);
                return;
            }
            sources[index]->source_id = source_id;
            sources[index]->valid = true;
        }
        for (uint32_t index = 0u; index < native_record_count; ++index) {
            const XgRenderPreScenePrimitive *record =
                &sprite_ft4_shadow.native_records[index];
            XgRenderModelFt4SourceRecord *source = sources[index];
            const uint32_t source_id = source->source_id;

            *source = (XgRenderModelFt4SourceRecord){
                .primitive = record->primitive,
                .lifecycle = sprite_ft4_shadow.native_lifecycles[index],
                .source_id = source_id,
                .interpolation_producer_id = record->interpolation_producer_id,
                .interpolation_primitive_id = record->interpolation_primitive_id,
                .opcode = sprite_ft4_shadow.native_opcodes[index],
                .interpolation_identity_valid =
                    record->interpolation_identity_valid,
                .valid = true,
            };
            xg_render_lookup_put(
                model_ft4_source_lookup, model_ft4_source_lookup_epoch,
                source_id, (uint32_t)(source - model_ft4_sources));
            native_resolve_hint_put(source_id, XG_NATIVE_RESOLVE_MODEL_FT4);
            watch_producer_resource(record->packet_address, 0x28u);
        }
        sprite_ft4_shadow.snapshot.resident_publish_source_count +=
            native_record_count;
        ++sprite_ft4_shadow.snapshot.native_cutover_count;
        sprite_ft4_shadow.snapshot.native_primitive_count +=
            native_record_count;
    }
    clear_sprite_ft4_shadow_context();
}

static void clear_world_horizon_shadow_pending(void) {
    PsxXgRenderWorldHorizonShadowSnapshot snapshot =
        world_horizon_shadow.snapshot;

    if (!snapshot.pending) return;
    snapshot.pending = false;
    world_horizon_shadow = (XgRenderWorldHorizonShadowState){
        .snapshot = snapshot,
    };
}

static void block_world_horizon_shadow(uint32_t blocker) {
    clear_world_horizon_shadow_pending();
    world_horizon_shadow.snapshot.blocked = true;
    if (world_horizon_shadow.snapshot.blocker == 0u)
        world_horizon_shadow.snapshot.blocker = blocker;
}

static bool world_horizon_read_u16(void *context, uint32_t address,
                                   uint16_t *out_value) {
    CPUState *cpu = context;

    if (cpu == NULL || cpu->read_half == NULL || out_value == NULL)
        return false;
    *out_value = cpu->read_half(address);
    return true;
}

static bool world_horizon_read_u32(void *context, uint32_t address,
                                   uint32_t *out_value) {
    CPUState *cpu = context;

    if (cpu == NULL || cpu->read_word == NULL || out_value == NULL)
        return false;
    *out_value = cpu->read_word(address);
    return true;
}

static uint32_t world_horizon_capture_build(
    CPUState *cpu, XgWorldHorizonCapture *capture,
    XgWorldHorizonRecord records[XG_WORLD_HORIZON_QUAD_COUNT]) {
    XgWorldHorizonCaptureRequest request = { 0 };
    XgWorldHorizonAuthenticatedReader reader = { 0 };
    GpuDrawState draw = { 0 };
    const uint64_t authentication_generation = state.scene_generation + 1u;

    if (cpu == NULL || capture == NULL || records == NULL ||
        cpu->read_word == NULL || cpu->read_half == NULL ||
        authentication_generation == 0u)
        return 1u;
    gpu_get_draw_state(&draw);
    request = (XgWorldHorizonCaptureRequest){
        .authentication_generation = authentication_generation,
        .caller_return = cpu->gpr[31],
        .screen_offset_x = (int32_t)cpu->gte_ctrl[24],
        .screen_offset_y = (int32_t)cpu->gte_ctrl[25],
        .projection_distance = (uint16_t)cpu->gte_ctrl[26],
        .raster = {
            .draw_area_left = draw.left,
            .draw_area_top = draw.top,
            .draw_area_right = draw.right,
            .draw_area_bottom = draw.bottom,
            .draw_offset_x = draw.offset_x,
            .draw_offset_y = draw.offset_y,
            .dither = draw.dither != 0u,
            .mask_set = draw.mask_set != 0u,
            .mask_check = draw.mask_check != 0u,
        },
        .projection_state_authenticated = true,
    };
    reader = (XgWorldHorizonAuthenticatedReader){
        .context = cpu,
        .read_u16 = world_horizon_read_u16,
        .read_u32 = world_horizon_read_u32,
        .authentication_generation = authentication_generation,
        .authenticated = true,
    };
    if (xg_world_horizon_source_capture(&request, &reader, capture) !=
        XG_WORLD_HORIZON_CAPTURE_OK)
        return 1u;
    if (xg_world_horizon_build_for_view(
            &capture->source, native_view.enabled ? &native_view : NULL,
            records) !=
        XG_WORLD_HORIZON_OK)
        return 2u;
    return 0u;
}

static uint32_t world_effects_capture_build(
    CPUState *cpu, XgWorldEffectsCapture *capture,
    XgWorldEffectsRecord records[XG_WORLD_EFFECTS_SOURCE_CAPACITY],
    uint32_t *out_count, XgWorldEffectsRecord *temporal_records,
    uint32_t *out_temporal_count) {
    XgWorldEffectsCaptureRequest request = { 0 };
    XgWorldEffectsAuthenticatedReader reader = { 0 };
    GpuDrawState draw = { 0 };
    const uint64_t authentication_generation = state.scene_generation + 1u;

    if (cpu == NULL || capture == NULL || records == NULL || out_count == NULL ||
        cpu->read_word == NULL || cpu->read_half == NULL ||
        authentication_generation == 0u)
        return 1u;
    gpu_get_draw_state(&draw);
    request = (XgWorldEffectsCaptureRequest){
        .authentication_generation = authentication_generation,
        .caller_return = cpu->gpr[31],
        .screen_offset_x = (int32_t)cpu->gte_ctrl[24],
        .screen_offset_y = (int32_t)cpu->gte_ctrl[25],
        .screen_x_cull_margin = render_screen_x_cull_margin(),
        .projection_distance = (uint16_t)cpu->gte_ctrl[26],
        .raster = {
            .draw_area_left = draw.left,
            .draw_area_top = draw.top,
            .draw_area_right = draw.right,
            .draw_area_bottom = draw.bottom,
            .draw_offset_x = draw.offset_x,
            .draw_offset_y = draw.offset_y,
            .dither = draw.dither != 0u,
            .mask_set = draw.mask_set != 0u,
            .mask_check = draw.mask_check != 0u,
        },
        .projection_state_authenticated = true,
    };
    reader = (XgWorldEffectsAuthenticatedReader){
        .context = cpu,
        .read_u16 = world_horizon_read_u16,
        .read_u32 = world_horizon_read_u32,
        .authentication_generation = authentication_generation,
        .authenticated = true,
    };
    if (xg_world_effects_source_capture(&request, &reader, capture) !=
        XG_WORLD_EFFECTS_CAPTURE_OK)
        return 1u;
    if (xg_world_effects_build_with_temporal(
            &capture->source, records, XG_WORLD_EFFECTS_SOURCE_CAPACITY,
            out_count, temporal_records, XG_WORLD_EFFECTS_SOURCE_CAPACITY,
            out_temporal_count) != XG_WORLD_EFFECTS_OK)
        return 2u;
    return 0u;
}

static uint32_t world_clouds_capture_build(
    CPUState *cpu, XgWorldCloudsCapture *capture,
    XgWorldCloudRecord records[XG_WORLD_CLOUD_PACKET_CAPACITY],
    uint32_t *out_count, XgWorldCloudPosition stepped_positions[XG_WORLD_CLOUD_COUNT],
    XgWorldCloudsBuildStats *out_stats,
    XgWorldCloudRecord *temporal_records, uint32_t *out_temporal_count) {
    XgWorldCloudsCaptureRequest request = { 0 };
    XgWorldCloudsAuthenticatedReader reader = { 0 };
    GpuDrawState draw = { 0 };
    const uint64_t authentication_generation = state.scene_generation + 1u;

    if (cpu == NULL || capture == NULL || records == NULL ||
        out_count == NULL || stepped_positions == NULL || out_stats == NULL ||
        cpu->read_word == NULL || cpu->read_half == NULL ||
        authentication_generation == 0u)
        return 1u;
    gpu_get_draw_state(&draw);
    request = (XgWorldCloudsCaptureRequest){
        .authentication_generation = authentication_generation,
        .caller_return = cpu->gpr[31],
        .screen_offset_x = (int32_t)cpu->gte_ctrl[24],
        .screen_offset_y = (int32_t)cpu->gte_ctrl[25],
        .screen_x_cull_margin = render_screen_x_cull_margin(),
        .projection_distance = (uint16_t)cpu->gte_ctrl[26],
        .raster = {
            .draw_area_left = draw.left,
            .draw_area_top = draw.top,
            .draw_area_right = draw.right,
            .draw_area_bottom = draw.bottom,
            .draw_offset_x = draw.offset_x,
            .draw_offset_y = draw.offset_y,
            .texture_window_mask_x = draw.texture_window_mask_x,
            .texture_window_mask_y = draw.texture_window_mask_y,
            .texture_window_offset_x = draw.texture_window_offset_x,
            .texture_window_offset_y = draw.texture_window_offset_y,
            .dither = draw.dither != 0u,
            .mask_set = draw.mask_set != 0u,
            .mask_check = draw.mask_check != 0u,
        },
        .projection_state_authenticated = true,
    };
    reader = (XgWorldCloudsAuthenticatedReader){
        .context = cpu,
        .read_u16 = world_horizon_read_u16,
        .read_u32 = world_horizon_read_u32,
        .authentication_generation = authentication_generation,
        .authenticated = true,
    };
    if (xg_world_clouds_source_capture(&request, &reader, capture) !=
        XG_WORLD_CLOUDS_CAPTURE_OK)
        return 1u;
    if (xg_world_clouds_build_with_temporal(
            &capture->source, records, XG_WORLD_CLOUD_PACKET_CAPACITY,
            out_count, out_stats, temporal_records,
            XG_WORLD_CLOUD_TEMPORAL_CAPACITY,
            out_temporal_count) != XG_WORLD_CLOUDS_OK)
        return 2u;
    memcpy(stepped_positions, capture->source.positions,
           sizeof(capture->source.positions));
    if (xg_world_clouds_step_positions(
            stepped_positions, capture->source.velocities) !=
        XG_WORLD_CLOUDS_OK)
        return 2u;
    return 0u;
}

static uint32_t world_minimap_capture_build(
    CPUState *cpu, XgWorldMinimapCapture *capture,
    XgWorldMinimapBuildOutput *output) {
    XgWorldMinimapCaptureRequest request = { 0 };
    XgWorldMinimapAuthenticatedReader reader = { 0 };
    GpuDrawState draw = { 0 };
    const uint64_t authentication_generation = state.scene_generation + 1u;

    if (cpu == NULL || capture == NULL || output == NULL ||
        cpu->read_word == NULL || cpu->read_half == NULL ||
        authentication_generation == 0u)
        return 1u;
    gpu_get_draw_state(&draw);
    request = (XgWorldMinimapCaptureRequest){
        .authentication_generation = authentication_generation,
        .caller_return = cpu->gpr[31],
        .screen_offset_x = (int32_t)cpu->gte_ctrl[24],
        .screen_offset_y = (int32_t)cpu->gte_ctrl[25],
        .projection_distance = (uint16_t)cpu->gte_ctrl[26],
        .raster = {
            .draw_area_left = draw.left,
            .draw_area_top = draw.top,
            .draw_area_right = draw.right,
            .draw_area_bottom = draw.bottom,
            .draw_offset_x = draw.offset_x,
            .draw_offset_y = draw.offset_y,
            .dither = draw.dither != 0u,
            .mask_set = draw.mask_set != 0u,
            .mask_check = draw.mask_check != 0u,
        },
        .projection_state_authenticated = true,
    };
    reader = (XgWorldMinimapAuthenticatedReader){
        .context = cpu,
        .read_u16 = world_horizon_read_u16,
        .read_u32 = world_horizon_read_u32,
        .authentication_generation = authentication_generation,
        .authenticated = true,
    };
    if (xg_world_minimap_source_capture(&request, &reader, capture) !=
        XG_WORLD_MINIMAP_CAPTURE_OK)
        return 1u;
    if (xg_world_minimap_build(&capture->source, output) !=
        XG_WORLD_MINIMAP_OK)
        return 2u;
    return 0u;
}

static void clear_world_effects_shadow_pending(void) {
    PsxXgRenderWorldEffectsShadowSnapshot snapshot =
        world_effects_shadow.snapshot;

    if (world_effects_shadow.count == 0u && !snapshot.pending) return;
    snapshot.pending = false;
    world_effects_shadow = (XgRenderWorldEffectsShadowState){
        .snapshot = snapshot,
    };
}

static void block_world_effects_shadow(uint32_t blocker) {
    clear_world_effects_shadow_pending();
    world_effects_shadow.snapshot.blocked = true;
    if (world_effects_shadow.snapshot.blocker == 0u)
        world_effects_shadow.snapshot.blocker = blocker;
}

static void world_effects_shadow_begin(CPUState *cpu) {
    enum {
        EFFECTS_PACKET_BASES = 0x8009be1cu,
        EFFECTS_BUFFER_INDEX = 0x8009d7f0u,
        EFFECTS_CONTEXT = 0x8009be3cu,
        EFFECTS_PACKET_STRIDE = 0x28u,
        EFFECTS_CAPACITY = 0x100u,
        EFFECTS_OT_BUCKET_COUNT = 0xc0u,
    };
    uint32_t buffer_index;
    uint32_t packet_cursor;
    uint32_t context;
    uint32_t last_heads[EFFECTS_OT_BUCKET_COUNT];
    uint32_t capture_result;
    uint32_t index;

    if (state.requested_render_mode != GUEST_RENDER_RENDER_SHADOW) {
        clear_world_effects_shadow_pending();
        return;
    }
    if (world_effects_shadow.snapshot.blocked) return;
    if (world_effects_shadow.snapshot.pending) {
        block_world_effects_shadow(100u);
        return;
    }
    if (cpu == NULL || cpu->read_word == NULL || cpu->read_half == NULL ||
        !physical_address_equals(cpu->gpr[31], UINT32_C(0x80071aa8)))
        return;
    if (!stack_address_is_valid(cpu->gpr[29]) ||
        world_effects_shadow.snapshot.begin_count == UINT64_MAX) {
        block_world_effects_shadow(101u);
        return;
    }
    buffer_index = cpu->read_word(EFFECTS_BUFFER_INDEX);
    if (buffer_index >= 2u) {
        block_world_effects_shadow(101u);
        return;
    }
    packet_cursor = cpu->read_word(
        EFFECTS_PACKET_BASES + buffer_index * 4u);
    if (!word_address_is_valid(packet_cursor) ||
        packet_cursor > UINT32_MAX -
            EFFECTS_CAPACITY * EFFECTS_PACKET_STRIDE ||
        !word_address_is_valid(packet_cursor +
            EFFECTS_CAPACITY * EFFECTS_PACKET_STRIDE - 4u)) {
        block_world_effects_shadow(101u);
        return;
    }
    ++world_effects_shadow.snapshot.begin_count;
    capture_result = world_effects_capture_build(
        cpu, &world_effects_shadow.capture, world_effects_shadow.records,
        &world_effects_shadow.count, NULL, NULL);
    if (capture_result == 1u) {
        ++world_effects_shadow.snapshot.source_capture_failure_count;
        block_world_effects_shadow(106u);
        return;
    }
    if (world_effects_shadow.snapshot.source_read_count > UINT64_MAX -
            world_effects_shadow.capture.authenticated_read_count ||
        world_effects_shadow.snapshot.source_read_bytes > UINT64_MAX -
            world_effects_shadow.capture.authenticated_read_bytes ||
        world_effects_shadow.snapshot.active_source_count > UINT64_MAX -
            world_effects_shadow.capture.active_source_count ||
        capture_result != 0u) {
        block_world_effects_shadow(107u);
        return;
    }
    world_effects_shadow.snapshot.source_read_count +=
        world_effects_shadow.capture.authenticated_read_count;
    world_effects_shadow.snapshot.source_read_bytes +=
        world_effects_shadow.capture.authenticated_read_bytes;
    world_effects_shadow.snapshot.active_source_count +=
        world_effects_shadow.capture.active_source_count;

    context = cpu->read_word(EFFECTS_CONTEXT);
    if (context > UINT32_MAX - 0x70u ||
        !word_address_is_valid(context + 0x70u)) {
        block_world_effects_shadow(108u);
        return;
    }
    world_effects_shadow.ot_base = cpu->read_word(context + 0x70u);
    if (!word_address_is_valid(world_effects_shadow.ot_base) ||
        world_effects_shadow.ot_base > UINT32_MAX -
            (EFFECTS_OT_BUCKET_COUNT - 1u) * 4u) {
        block_world_effects_shadow(108u);
        return;
    }
    memset(last_heads, 0, sizeof(last_heads));
    for (index = 0u; index < world_effects_shadow.count; ++index) {
        const uint32_t packet = packet_cursor + index * EFFECTS_PACKET_STRIDE;
        const uint32_t bucket =
            world_effects_shadow.records[index].ordering_bucket;

        if (bucket >= EFFECTS_OT_BUCKET_COUNT ||
            !word_address_is_valid(packet) ||
            (cpu->read_word(packet) >> 24u) != 9u) {
            block_world_effects_shadow(109u);
            return;
        }
        world_effects_shadow.packet_addresses[index] = packet;
        if (!world_effects_shadow.ot_touched[bucket]) {
            const uint32_t ot_address =
                world_effects_shadow.ot_base + bucket * 4u;
            const uint32_t initial = cpu->read_word(ot_address);

            world_effects_shadow.ot_touched[bucket] = true;
            world_effects_shadow.initial_ot_words[bucket] = initial;
            last_heads[bucket] = initial & UINT32_C(0x00ffffff);
        }
        world_effects_shadow.expected_tags[index] = UINT32_C(0x09000000) |
            last_heads[bucket];
        last_heads[bucket] = packet & UINT32_C(0x00ffffff);
        world_effects_shadow.final_ot_packets[bucket] = packet;
    }
    world_effects_shadow.entry_stack_pointer = cpu->gpr[29];
    world_effects_shadow.initial_packet_cursor = packet_cursor;
    world_effects_shadow.snapshot.pending = true;
}

static void world_effects_shadow_finish(CPUState *cpu) {
    enum {
        EFFECTS_STACK_FRAME_SIZE = 0x50u,
        EFFECTS_PACKET_STRIDE = 0x28u,
        EFFECTS_CAPACITY = 0x100u,
        EFFECTS_PACKET_PAYLOAD_END = 0x24u,
    };
    uint32_t packet_cursor;
    uint32_t delta;
    uint32_t primitive_count;
    uint32_t compare_count;
    uint32_t index;
    bool invocation_matches = true;

    if (!world_effects_shadow.snapshot.pending) return;
    if (cpu == NULL || cpu->read_word == NULL || cpu->read_half == NULL ||
        world_effects_shadow.entry_stack_pointer < EFFECTS_STACK_FRAME_SIZE ||
        cpu->gpr[29] != world_effects_shadow.entry_stack_pointer -
            EFFECTS_STACK_FRAME_SIZE) {
        block_world_effects_shadow(102u);
        return;
    }
    packet_cursor = cpu->gpr[19];
    if (packet_cursor < world_effects_shadow.initial_packet_cursor) {
        block_world_effects_shadow(102u);
        return;
    }
    delta = packet_cursor - world_effects_shadow.initial_packet_cursor;
    primitive_count = delta / EFFECTS_PACKET_STRIDE;
    if (delta % EFFECTS_PACKET_STRIDE != 0u ||
        primitive_count > EFFECTS_CAPACITY || cpu->gpr[21] != EFFECTS_CAPACITY ||
        packet_cursor > UINT32_MAX - EFFECTS_PACKET_PAYLOAD_END ||
        cpu->gpr[18] != packet_cursor + EFFECTS_PACKET_PAYLOAD_END ||
        world_effects_shadow.snapshot.completion_count == UINT64_MAX ||
        world_effects_shadow.snapshot.primitive_count >
            UINT64_MAX - primitive_count ||
        world_effects_shadow.snapshot.candidate_count >
            UINT64_MAX - world_effects_shadow.count) {
        block_world_effects_shadow(103u);
        return;
    }
    ++world_effects_shadow.snapshot.completion_count;
    world_effects_shadow.snapshot.primitive_count += primitive_count;
    world_effects_shadow.snapshot.candidate_count += world_effects_shadow.count;
    world_effects_shadow.snapshot.last_primitive_count = primitive_count;
    world_effects_shadow.snapshot.last_candidate_count =
        world_effects_shadow.count;
    if (primitive_count != world_effects_shadow.count) {
        ++world_effects_shadow.snapshot.count_mismatch_count;
        invocation_matches = false;
    }
    compare_count = primitive_count < world_effects_shadow.count
        ? primitive_count : world_effects_shadow.count;
    for (index = 0u; index < compare_count; ++index) {
        const XgWorldEffectsRecord *record =
            &world_effects_shadow.records[index];
        const uint32_t packet = world_effects_shadow.packet_addresses[index];
        bool payload_matches = compare_ft4_payload(
            cpu, packet, 0u, record->material_word, record->uv,
            record->tpage, record->clut,
            &world_effects_shadow.snapshot.first_payload_mismatch);
        bool geometry_matches = true;
        bool tag_matches = cpu->read_word(packet) ==
            world_effects_shadow.expected_tags[index];
        uint32_t vertex;

        for (vertex = 0u; vertex < XG_HOST_3D_VERTEX_COUNT; ++vertex) {
            const uint32_t expected_xy =
                (uint16_t)record->vertices[vertex].x |
                ((uint32_t)(uint16_t)record->vertices[vertex].y << 16u);

            geometry_matches &= cpu->read_word(
                packet + 8u + vertex * 8u) == expected_xy;
        }
        if (!payload_matches)
            ++world_effects_shadow.snapshot.payload_mismatch_count;
        if (!geometry_matches)
            ++world_effects_shadow.snapshot.geometry_mismatch_count;
        if (!tag_matches)
            ++world_effects_shadow.snapshot.tag_mismatch_count;
        if (payload_matches && geometry_matches && tag_matches) {
            ++world_effects_shadow.snapshot.match_count;
        } else {
            ++world_effects_shadow.snapshot.mismatch_count;
            invocation_matches = false;
            if (world_effects_shadow.snapshot.first_mismatch_packet == 0u) {
                world_effects_shadow.snapshot.first_mismatch_packet = packet;
                world_effects_shadow.snapshot.first_mismatch_source =
                    record->source_index;
            }
        }
    }
    if (primitive_count != world_effects_shadow.count) {
        const uint32_t difference = primitive_count > world_effects_shadow.count
            ? primitive_count - world_effects_shadow.count
            : world_effects_shadow.count - primitive_count;

        world_effects_shadow.snapshot.mismatch_count += difference;
        if (world_effects_shadow.snapshot.first_mismatch_packet == 0u)
            world_effects_shadow.snapshot.first_mismatch_packet =
                world_effects_shadow.initial_packet_cursor +
                compare_count * EFFECTS_PACKET_STRIDE;
    }
    for (index = 0u; index < 0xc0u; ++index) {
        uint32_t expected_ot;

        if (!world_effects_shadow.ot_touched[index]) continue;
        expected_ot =
            (world_effects_shadow.initial_ot_words[index] &
             UINT32_C(0xff000000)) |
            (world_effects_shadow.final_ot_packets[index] &
             UINT32_C(0x00ffffff));
        if (cpu->read_word(world_effects_shadow.ot_base + index * 4u) !=
            expected_ot) {
            ++world_effects_shadow.snapshot.ot_mismatch_count;
            invocation_matches = false;
            if (world_effects_shadow.snapshot.first_mismatch_packet == 0u)
                world_effects_shadow.snapshot.first_mismatch_packet =
                    world_effects_shadow.ot_base + index * 4u;
        }
    }
    if (invocation_matches)
        ++world_effects_shadow.snapshot.invocation_match_count;
    else
        ++world_effects_shadow.snapshot.invocation_mismatch_count;
    clear_world_effects_shadow_pending();
}

static void world_horizon_shadow_begin(CPUState *cpu) {
    enum {
        HORIZON_CONTEXT = 0x8009be3cu,
        HORIZON_SET_WINDOW = 0x8009d3d8u,
        HORIZON_RESET_WINDOW = 0x8009d3e4u,
        HORIZON_OT_BUCKET_COUNT = 0x1000u,
    };
    uint32_t context;
    uint32_t ot_base;
    uint32_t capture_result;
    uint32_t quad;

    if (state.requested_render_mode != GUEST_RENDER_RENDER_SHADOW) {
        clear_world_horizon_shadow_pending();
        return;
    }
    if (world_horizon_shadow.snapshot.blocked) return;
    if (world_horizon_shadow.snapshot.pending) {
        block_world_horizon_shadow(90u);
        return;
    }
    if (cpu == NULL || cpu->read_word == NULL || cpu->read_half == NULL ||
        !physical_address_equals(cpu->gpr[31], UINT32_C(0x80071b58)))
        return;
    if (!stack_address_is_valid(cpu->gpr[29]) ||
        world_horizon_shadow.snapshot.begin_count == UINT64_MAX) {
        block_world_horizon_shadow(91u);
        return;
    }
    ++world_horizon_shadow.snapshot.begin_count;
    capture_result = world_horizon_capture_build(
        cpu, &world_horizon_shadow.capture, world_horizon_shadow.records);
    if (capture_result == 1u) {
        ++world_horizon_shadow.snapshot.source_capture_failure_count;
        block_world_horizon_shadow(92u);
        return;
    }
    if (world_horizon_shadow.snapshot.source_read_count >
            UINT64_MAX -
                world_horizon_shadow.capture.authenticated_read_count ||
        world_horizon_shadow.snapshot.source_read_bytes >
            UINT64_MAX -
                world_horizon_shadow.capture.authenticated_read_bytes ||
        capture_result != 0u) {
        block_world_horizon_shadow(93u);
        return;
    }
    world_horizon_shadow.snapshot.source_read_count +=
        world_horizon_shadow.capture.authenticated_read_count;
    world_horizon_shadow.snapshot.source_read_bytes +=
        world_horizon_shadow.capture.authenticated_read_bytes;
    world_horizon_shadow.snapshot.last_ot_bucket =
        world_horizon_shadow.records[0].ordering_bucket;
    if (world_horizon_shadow.records[0].ordering_bucket >=
            HORIZON_OT_BUCKET_COUNT) {
        block_world_horizon_shadow(94u);
        return;
    }

    world_horizon_shadow.entry_stack_pointer = cpu->gpr[29];
    for (quad = 0u; quad < XG_WORLD_HORIZON_QUAD_COUNT; ++quad) {
        const uint32_t packet = UINT32_C(0x8009c744) + quad * 0x50u +
            world_horizon_shadow.capture.buffer_index * 0x28u;

        if (!word_address_is_valid(packet) ||
            !word_address_is_valid(packet + 0x24u)) {
            block_world_horizon_shadow(94u);
            return;
        }
        world_horizon_shadow.packet_addresses[quad] = packet;
        world_horizon_shadow.initial_packet_tags[quad] =
            cpu->read_word(packet);
        if ((world_horizon_shadow.initial_packet_tags[quad] >> 24u) != 9u) {
            block_world_horizon_shadow(95u);
            return;
        }
    }
    world_horizon_shadow.initial_texture_window_tags[0] =
        cpu->read_word(HORIZON_SET_WINDOW);
    world_horizon_shadow.initial_texture_window_tags[1] =
        cpu->read_word(HORIZON_RESET_WINDOW);
    if ((world_horizon_shadow.initial_texture_window_tags[0] >> 24u) != 2u ||
        (world_horizon_shadow.initial_texture_window_tags[1] >> 24u) != 2u) {
        block_world_horizon_shadow(95u);
        return;
    }
    context = cpu->read_word(HORIZON_CONTEXT);
    if (context > UINT32_MAX - 0x70u ||
        !word_address_is_valid(context + 0x70u)) {
        block_world_horizon_shadow(94u);
        return;
    }
    ot_base = cpu->read_word(context + 0x70u);
    if (ot_base > UINT32_MAX -
            world_horizon_shadow.records[0].ordering_bucket * 4u) {
        block_world_horizon_shadow(94u);
        return;
    }
    world_horizon_shadow.ot_address = ot_base +
        world_horizon_shadow.records[0].ordering_bucket * 4u;
    if (!word_address_is_valid(world_horizon_shadow.ot_address)) {
        block_world_horizon_shadow(94u);
        return;
    }
    world_horizon_shadow.initial_ot_word =
        cpu->read_word(world_horizon_shadow.ot_address);
    world_horizon_shadow.snapshot.pending = true;
}

static void world_horizon_shadow_finish(CPUState *cpu) {
    enum {
        HORIZON_SET_WINDOW = 0x8009d3d8u,
        HORIZON_RESET_WINDOW = 0x8009d3e4u,
    };
    const bool accepted = world_horizon_shadow.records[0].accepted;
    const uint8_t u0 =
        (uint8_t)(world_horizon_shadow.capture.source.angle >> 2u) & 0x7fu;
    const uint8_t u1 = u0 | 0x80u;
    const uint16_t expected_uv[4] = {
        u0, u1, (uint16_t)(u0 | 0x3f00u),
        (uint16_t)(u1 | 0x3f00u),
    };
    bool payload_matches = true;
    bool geometry_matches = true;
    bool tag_matches = true;
    bool ot_matches;
    bool texture_window_matches;
    uint32_t quad;

    if (!world_horizon_shadow.snapshot.pending) return;
    if (cpu == NULL || cpu->read_word == NULL || cpu->read_half == NULL ||
        world_horizon_shadow.entry_stack_pointer < 0x40u ||
        cpu->gpr[29] != world_horizon_shadow.entry_stack_pointer - 0x40u) {
        block_world_horizon_shadow(96u);
        return;
    }
    if (world_horizon_shadow.snapshot.completion_count == UINT64_MAX ||
        world_horizon_shadow.snapshot.match_count == UINT64_MAX ||
        world_horizon_shadow.snapshot.mismatch_count == UINT64_MAX ||
        (accepted &&
         (world_horizon_shadow.snapshot.accepted_invocation_count ==
              UINT64_MAX ||
          world_horizon_shadow.snapshot.primitive_count > UINT64_MAX - 2u))) {
        block_world_horizon_shadow(97u);
        return;
    }

    for (quad = 0u; quad < XG_WORLD_HORIZON_QUAD_COUNT; ++quad) {
        const XgWorldHorizonRecord *record =
            &world_horizon_shadow.records[quad];
        const uint32_t packet = world_horizon_shadow.packet_addresses[quad];
        uint32_t vertex;

        payload_matches &= compare_ft4_payload(
            cpu, packet, 0u, UINT32_C(0x2e303030), expected_uv,
            0x003eu, 0x7f91u,
            &world_horizon_shadow.snapshot.first_payload_mismatch);
        for (vertex = 0u; vertex < XG_HOST_3D_VERTEX_COUNT; ++vertex) {
            const uint32_t expected_xy =
                (uint16_t)record->vertices[vertex].x |
                ((uint32_t)(uint16_t)record->vertices[vertex].y << 16u);
            const uint32_t actual_xy =
                cpu->read_word(packet + 8u + vertex * 8u);
            const bool vertex_matches = actual_xy == expected_xy;

            if (!vertex_matches &&
                world_horizon_shadow.snapshot
                    .first_geometry_mismatch_invocation == 0u) {
                world_horizon_shadow.snapshot
                    .first_geometry_mismatch_invocation =
                        world_horizon_shadow.snapshot.completion_count + 1u;
                world_horizon_shadow.snapshot.first_geometry_mismatch_quad =
                    quad;
                world_horizon_shadow.snapshot.first_geometry_mismatch_vertex =
                    vertex;
                world_horizon_shadow.snapshot.first_geometry_expected_xy =
                    expected_xy;
                world_horizon_shadow.snapshot.first_geometry_actual_xy =
                    actual_xy;
            }
            geometry_matches &= vertex_matches;
        }
    }

    texture_window_matches =
        cpu->read_word(HORIZON_SET_WINDOW + 4u) == UINT32_C(0xe2000010) &&
        cpu->read_word(HORIZON_SET_WINDOW + 8u) == 0u &&
        cpu->read_word(HORIZON_RESET_WINDOW + 4u) ==
            UINT32_C(0xe2000000) &&
        cpu->read_word(HORIZON_RESET_WINDOW + 8u) == 0u;
    if (accepted) {
        const uint32_t expected_packet_tags[2] = {
            UINT32_C(0x09000000) |
                (HORIZON_RESET_WINDOW & UINT32_C(0x00ffffff)),
            UINT32_C(0x09000000) |
                (world_horizon_shadow.packet_addresses[0] &
                 UINT32_C(0x00ffffff)),
        };
        const uint32_t expected_window_tags[2] = {
            UINT32_C(0x02000000) |
                (world_horizon_shadow.packet_addresses[1] &
                 UINT32_C(0x00ffffff)),
            UINT32_C(0x02000000) |
                (world_horizon_shadow.initial_ot_word &
                 UINT32_C(0x00ffffff)),
        };

        for (quad = 0u; quad < XG_WORLD_HORIZON_QUAD_COUNT; ++quad)
            tag_matches &= cpu->read_word(
                world_horizon_shadow.packet_addresses[quad]) ==
                expected_packet_tags[quad];
        tag_matches &= cpu->read_word(HORIZON_SET_WINDOW) ==
            expected_window_tags[0];
        tag_matches &= cpu->read_word(HORIZON_RESET_WINDOW) ==
            expected_window_tags[1];
        ot_matches = cpu->read_word(world_horizon_shadow.ot_address) ==
            ((world_horizon_shadow.initial_ot_word & UINT32_C(0xff000000)) |
             (HORIZON_SET_WINDOW & UINT32_C(0x00ffffff)));
    } else {
        for (quad = 0u; quad < XG_WORLD_HORIZON_QUAD_COUNT; ++quad)
            tag_matches &= cpu->read_word(
                world_horizon_shadow.packet_addresses[quad]) ==
                world_horizon_shadow.initial_packet_tags[quad];
        tag_matches &= cpu->read_word(HORIZON_SET_WINDOW) ==
            world_horizon_shadow.initial_texture_window_tags[0];
        tag_matches &= cpu->read_word(HORIZON_RESET_WINDOW) ==
            world_horizon_shadow.initial_texture_window_tags[1];
        ot_matches = cpu->read_word(world_horizon_shadow.ot_address) ==
            world_horizon_shadow.initial_ot_word;
    }

    ++world_horizon_shadow.snapshot.completion_count;
    if (accepted) {
        ++world_horizon_shadow.snapshot.accepted_invocation_count;
        world_horizon_shadow.snapshot.primitive_count += 2u;
    }
    if (!payload_matches)
        ++world_horizon_shadow.snapshot.payload_mismatch_count;
    if (!geometry_matches)
        ++world_horizon_shadow.snapshot.geometry_mismatch_count;
    if (!tag_matches)
        ++world_horizon_shadow.snapshot.tag_mismatch_count;
    if (!ot_matches)
        ++world_horizon_shadow.snapshot.ot_mismatch_count;
    if (!texture_window_matches)
        ++world_horizon_shadow.snapshot.texture_window_mismatch_count;
    if (payload_matches && geometry_matches && tag_matches && ot_matches &&
        texture_window_matches) {
        ++world_horizon_shadow.snapshot.match_count;
    } else {
        if (world_horizon_shadow.snapshot.mismatch_count == 0u) {
            if (!payload_matches || !geometry_matches || !tag_matches)
                world_horizon_shadow.snapshot.first_mismatch_packet =
                    world_horizon_shadow.packet_addresses[0];
            else if (!texture_window_matches)
                world_horizon_shadow.snapshot.first_mismatch_packet =
                    HORIZON_SET_WINDOW;
            else
                world_horizon_shadow.snapshot.first_mismatch_packet =
                    world_horizon_shadow.ot_address;
        }
        ++world_horizon_shadow.snapshot.mismatch_count;
    }
    clear_world_horizon_shadow_pending();
}

typedef struct XgRenderWorldEntry {
    uint32_t pc;
    uint32_t instruction;
    PsxXgRenderWorldFamily family;
} XgRenderWorldEntry;

static const XgRenderWorldEntry world_entries[] = {
    { UINT32_C(0x800983a0), UINT32_C(0x27bdff90),
      PSX_XG_RENDER_WORLD_TERRAIN_WATER },
    { UINT32_C(0x800848f4), UINT32_C(0x24020800),
      PSX_XG_RENDER_WORLD_MODELS },
    { UINT32_C(0x80085cdc), UINT32_C(0x3c02800a),
      PSX_XG_RENDER_WORLD_ACTOR_SPRITES },
    { UINT32_C(0x800747dc), UINT32_C(0x3c02800a),
      PSX_XG_RENDER_WORLD_ENTITY_SHADOWS },
    { UINT32_C(0x80086798), UINT32_C(0x3c02800a),
      PSX_XG_RENDER_WORLD_CLOUDS },
    { UINT32_C(0x80089c78), UINT32_C(0x27bdffb0),
      PSX_XG_RENDER_WORLD_EFFECTS },
    { UINT32_C(0x8008615c), UINT32_C(0x27bdffd8),
      PSX_XG_RENDER_WORLD_DECORATIONS },
    { UINT32_C(0x80073b04), UINT32_C(0x27bdffc0),
      PSX_XG_RENDER_WORLD_HORIZON },
    { UINT32_C(0x800737ec), UINT32_C(0x27bdffb8),
      PSX_XG_RENDER_WORLD_SKY },
    { UINT32_C(0x800740b8), UINT32_C(0x27bdffc8),
      PSX_XG_RENDER_WORLD_MINIMAP },
};

static bool resident_render_site_is_authorized(uint32_t pc,
                                                uint32_t instruction_word) {
    static const XgRenderWorldEntry resident_sites[] = {
        { UINT32_C(0x8007fbe0), UINT32_C(0x00a04021), 0 },
        { UINT32_C(0x80073b64), UINT32_C(0x3c03800d), 0 },
        { UINT32_C(0x8007412c), UINT32_C(0x266400b8), 0 },
        { XG_WORLD_TERRAIN_WATER_NATIVE_ENTRY_PC,
          UINT32_C(0x27bdffc8), 0 },
        { UINT32_C(0x80084cd0), UINT32_C(0x8fbf0038), 0 },
        { UINT32_C(0x80085f38), UINT32_C(0x8fbf0020), 0 },
    };

    for (uint32_t index = 0u;
         index < sizeof(world_entries) / sizeof(world_entries[0]); ++index) {
        if (physical_address_equals(pc, world_entries[index].pc) &&
            instruction_word == world_entries[index].instruction)
            return true;
    }
    for (uint32_t index = 0u;
         index < sizeof(resident_sites) / sizeof(resident_sites[0]); ++index) {
        if (physical_address_equals(pc, resident_sites[index].pc) &&
            instruction_word == resident_sites[index].instruction)
            return true;
    }
    return false;
}

static void observe_world_execution(uint32_t pc, uint32_t instruction_word) {
    uint32_t index;

    if (state.requested_render_mode == GUEST_RENDER_RENDER_ORIGINAL) return;
    if (physical_address_equals(pc, UINT32_C(0x80071a58))) {
        if (instruction_word != UINT32_C(0x3c02800a)) {
            world_execution_active = false;
        } else {
            world_execution_active = true;
            if (world_execution.full_dispatcher_count == UINT64_MAX)
                world_execution.overflowed = true;
            else
                ++world_execution.full_dispatcher_count;
        }
        return;
    }
    if (!world_execution_active) return;
    for (index = 0u;
         index < sizeof(world_entries) / sizeof(world_entries[0]); ++index) {
        const XgRenderWorldEntry *entry = &world_entries[index];
        const uint32_t bit = UINT32_C(1) << entry->family;

        if (!physical_address_equals(pc, entry->pc)) continue;
        if (instruction_word != entry->instruction) return;
        if (world_execution.family_entry_count[entry->family] == UINT64_MAX) {
            world_execution.overflowed = true;
        } else {
            ++world_execution.family_entry_count[entry->family];
            world_execution.observed_family_mask |= bit;
        }
        return;
    }
}

/* Battle background strips are authored as screen-space 4:3 quads.  They do
 * not pass through the GTE projection path, so the normal Native-view center
 * offset would leave both reveal columns uncovered.  Keep their canonical
 * coordinates for guest packet side effects, but provide a full-surface
 * Native position for the second pass. */
static void projected_set_native_view_position(
        XgRenderQuadSourceVertex *vertex) {
    int32_t surface_width;
    int64_t native_x;
    int64_t native_y;

    if (vertex == NULL || !native_view.enabled ||
        native_view.canonical_width == 0u ||
        native_view.canonical_height == 0u ||
        native_view.aspect_den == 0u)
        return;
    surface_width = (int32_t)(native_view.surface_width_16_16 >> 16u);
    if (surface_width <= (int32_t)native_view.canonical_width)
        return;
    if (vertex->x <= 0) {
        native_x = 0;
    } else if ((uint32_t)(uint16_t)vertex->x >=
               (uint32_t)native_view.canonical_width) {
        /* Use the integer GL surface endpoint, not the truncated 16.16
         * aspect product, so the final rightmost pixel is owned. */
        native_x = (int64_t)surface_width * INT32_C(65536);
    } else {
        native_x = ((int64_t)vertex->x * surface_width * INT32_C(65536) +
                    (int32_t)native_view.canonical_width / 2) /
            (int32_t)native_view.canonical_width;
    }
    native_y = (int64_t)vertex->y * INT32_C(65536);
    if (native_x < INT32_MIN || native_x > INT32_MAX ||
        native_y < INT32_MIN || native_y > INT32_MAX)
        return;
    vertex->native_view_x_16_16 = (int32_t)native_x;
    vertex->native_view_y_16_16 = (int32_t)native_y;
    vertex->native_view_position = true;
}

static bool projected_build_primitive(
        XgRenderProjectedNativeRecord *record,
        const XgRenderProjectedSource *source, const GpuDrawState *draw) {
    XgRenderQuadSource quad = { 0 };
    uint32_t vertex;

    apply_draw_state(&quad.material, draw);
    if (record->kind == XG_RENDER_PROJECTED_RECORD_FT4) {
        quad.material.tpage = record->tpage;
        quad.material.texture_page_x = record->tpage & 0x0fu;
        quad.material.texture_page_y = (record->tpage >> 4u) & 1u;
        quad.material.clut_x = source->clut_x;
        quad.material.clut_y = source->clut_y;
        quad.material.texture_depth =
            (XgRenderIrTextureDepth)((record->tpage >> 7u) & 3u);
        quad.material.blend_mode =
            (XgRenderIrBlendMode)((record->tpage >> 5u) & 3u);
        quad.material.shading = XG_RENDER_IR_SHADING_FLAT;
        quad.material.textured = true;
        quad.material.raw_texture = true;
    } else {
        quad.material.shading =
            record->kind == XG_RENDER_PROJECTED_RECORD_G4
                ? XG_RENDER_IR_SHADING_GOURAUD
                : XG_RENDER_IR_SHADING_FLAT;
    }
    for (vertex = 0u; vertex < 4u; ++vertex) {
        const uint8_t *color;

        if (record->kind == XG_RENDER_PROJECTED_RECORD_FT4) {
            static const uint8_t neutral[3] = { 0x80u, 0x80u, 0x80u };
            color = neutral;
        } else if (record->kind == XG_RENDER_PROJECTED_RECORD_G4) {
            color = vertex < 2u ? source->middle_top_color
                                : source->lower_color;
        } else {
            color = record->kind == XG_RENDER_PROJECTED_RECORD_F4_LOWER
                ? source->lower_color : source->upper_color;
        }
        quad.vertices[vertex] = (XgRenderQuadSourceVertex){
            record->x[vertex], record->y[vertex],
            record->u[vertex], record->v[vertex],
            color[0], color[1], color[2],
        };
        projected_set_native_view_position(&quad.vertices[vertex]);
    }
    return xg_render_quad_build_primitive(&quad, &record->primitive) ==
        XG_RENDER_QUAD_BUILDER_OK;
}

static bool projected_prepare_record(
    CPUState *cpu, XgRenderProjectedNativeRecord *record,
    const XgRenderProjectedSource *source, const GpuDrawState *draw) {
    uint8_t expected_payload;

    if (cpu == NULL || record == NULL || source == NULL || draw == NULL ||
        cpu->read_word == NULL)
        return false;
    expected_payload = record->kind == XG_RENDER_PROJECTED_RECORD_FT4 ? 9u :
        (record->kind == XG_RENDER_PROJECTED_RECORD_G4 ? 8u : 5u);
    if (record->packet_address > UINT32_MAX - expected_payload * 4u ||
        !word_address_is_valid(record->packet_address) ||
        !word_address_is_valid(record->packet_address + expected_payload * 4u))
        return false;
    record->packet_tag = cpu->read_word(record->packet_address);
    if ((record->packet_tag >> 24u) != expected_payload) return false;
    record->payload_word_count = expected_payload;
    return projected_build_primitive(record, source, draw);
}

static bool projected_append_record(
    CPUState *cpu, XgRenderProjectedNativeRecord records[], uint32_t *count,
    const XgRenderProjectedSource *source, const GpuDrawState *draw,
    const XgRenderProjectedNativeRecord *record) {
    if (count == NULL || *count >= XG_RENDER_PROJECTED_MAX_RECORDS) return false;
    records[*count] = *record;
    if (!projected_prepare_record(cpu, &records[*count], source, draw))
        return false;
    ++*count;
    return true;
}

static bool projected_build_strips(
    CPUState *cpu, const XgRenderProjectedSource *source,
    const XgRenderProjectedConfig *config, const GpuDrawState *draw,
    uint32_t buffer_index, int32_t phase, int16_t projected_y, int16_t fade,
    XgRenderProjectedNativeRecord records[], uint32_t *count,
    bool temporal) {
    int32_t denominator = (int32_t)fade + 0x100;
    int32_t centered_width;
    int32_t fade_adjustment;
    int32_t phase_cursor;
    int32_t strip_screen_height = 0;
    int32_t screen_x = 0;
    int32_t quotient;
    uint32_t strip;

    if (!projected_divide(
            (int32_t)((uint32_t)config->strip_width << 8u), denominator,
            &quotient))
        return false;
    centered_width = (0x140 - quotient) / 2;
    fade_adjustment = projected_rounded_shift(
        projected_wrap_multiply(centered_width, fade), 8u);
    phase_cursor = low_s16((uint32_t)projected_wrap_subtract(
        phase, projected_wrap_add(centered_width, fade_adjustment)));
    phase_cursor %= config->strip_width;
    if (phase_cursor < 0)
        phase_cursor = (uint16_t)config->strip_width + phase_cursor;
    if (temporal || (projected_y >= 0 &&
        projected_y <= projected_wrap_add(config->strip_height, 0xf0))) {
        if (!projected_divide(
                (int32_t)((uint32_t)config->strip_height << 8u), denominator,
                &strip_screen_height))
            return false;
    }
    if (low_s16((uint32_t)strip_screen_height) <= 0) return true;

    for (strip = 0u; strip < XG_RENDER_PROJECTED_MAX_STRIPS; ++strip) {
        const unsigned texture_shift =
            (2u - (uint32_t)config->texture_depth) & 0x1fu;
        const int32_t texture_page_width =
            0x100 >> (uint32_t)config->texture_depth;
        int32_t texture_page = config->texture_x;
        int32_t page_x;
        int32_t page_y = config->texture_y;
        int32_t page_remainder;
        int32_t u;
        int32_t source_width;
        int32_t screen_width;
        int32_t next_phase;
        XgRenderProjectedNativeRecord record = {
            .kind = XG_RENDER_PROJECTED_RECORD_FT4,
            .packet_address = source->object_address +
                (buffer_index & 1u) * 0x140u + strip * 0x28u,
        };

        if (texture_page < 0) texture_page = projected_wrap_add(texture_page, 0x3f);
        page_remainder = config->texture_x -
            projected_shift_right_floor(texture_page, 6u) * 0x40;
        u = (phase_cursor + (int32_t)(uint16_t)
             ((uint32_t)(uint16_t)page_remainder << texture_shift)) &
            (texture_page_width - 1);
        source_width = 0x100 - u;
        if (config->strip_width < phase_cursor + source_width)
            source_width = config->strip_width - phase_cursor;
        if (!projected_divide(
                (int32_t)((uint32_t)source_width << 8u), denominator,
                &screen_width))
            return false;
        if (0x140 < low_s16((uint32_t)screen_x) +
                        low_s16((uint32_t)screen_width)) {
            screen_width = 0x140 - screen_x;
            source_width = projected_rounded_shift(
                projected_wrap_multiply(low_s16((uint32_t)screen_width),
                                        denominator), 8u);
        }
        next_phase = low_s16((uint32_t)(phase_cursor + source_width));
        next_phase %= config->strip_width;
        screen_width = low_s16((uint32_t)screen_width);
        record.x[0] = record.x[2] = low_s16((uint32_t)screen_x);
        screen_x = projected_wrap_add(screen_x, screen_width);
        record.x[1] = record.x[3] = low_s16((uint32_t)screen_x);
        record.y[0] = record.y[1] = (int16_t)
            ((uint16_t)projected_y - (uint16_t)strip_screen_height);
        record.y[2] = record.y[3] = projected_y;
        record.u[0] = record.u[2] = (uint8_t)u;
        record.u[1] = record.u[3] = (uint8_t)(u + source_width - 1);
        record.v[0] = record.v[1] = (uint8_t)config->texture_v;
        record.v[2] = record.v[3] =
            (uint8_t)(config->texture_v + (uint8_t)config->strip_height);
        page_x = low_s16((uint32_t)(uint16_t)config->texture_x +
            (uint32_t)projected_shift_right_floor(
                low_s16((uint32_t)phase_cursor), texture_shift));
        if (page_x < 0) page_x = projected_wrap_add(page_x, 0x3f);
        if (page_y < 0) page_y = projected_wrap_add(page_y, 0xff);
        record.tpage = projected_get_tpage(
            config->texture_depth,
            (int32_t)((uint32_t)projected_shift_right_floor(page_x, 6u) << 6u),
            (int32_t)((uint32_t)projected_shift_right_floor(page_y, 8u) << 8u));
        if (!projected_append_record(cpu, records, count, source, draw,
                                     &record))
            return false;
        phase_cursor = next_phase;
        if (low_s16((uint32_t)screen_x) > 0x13f) break;
    }
    return true;
}

static bool projected_resolve_ot_bucket(CPUState *cpu, uint32_t ot_address,
                                        uint32_t *bucket,
                                        XgRenderOrderingDomain *domain) {
    uint32_t context;
    uint32_t base;

    if (cpu == NULL || cpu->read_word == NULL || bucket == NULL ||
        domain == NULL)
        return false;
    *domain = XG_RENDER_ORDERING_DOMAIN_UNKNOWN;
    context = cpu->read_word(UINT32_C(0x800c426c));
    if (context <= UINT32_MAX - 0x80c8u) {
        base = context + 0x40ccu;
        if (ot_address >= base && ot_address <= base + 0x3ffcu &&
            ((ot_address - base) & 3u) == 0u) {
            *bucket = (ot_address - base) / 4u;
            *domain = XG_RENDER_ORDERING_DOMAIN_FIELD;
            return true;
        }
    }
    base = cpu->read_word(UINT32_C(0x800ccb04));
    if (base <= UINT32_MAX - 0x3ffcu &&
        ot_address >= base && ot_address <= base + 0x3ffcu &&
        ((ot_address - base) & 3u) == 0u) {
        *bucket = (ot_address - base) / 4u;
        *domain = XG_RENDER_ORDERING_DOMAIN_BATTLE;
        return true;
    }
    context = cpu->read_word(UINT32_C(0x800ccb00));
    if (context <= UINT32_MAX - 0x406cu) {
        base = context + 0x70u;
        if (ot_address >= base && ot_address <= base + 0x3ffcu &&
            ((ot_address - base) & 3u) == 0u) {
            *bucket = (ot_address - base) / 4u;
            *domain = XG_RENDER_ORDERING_DOMAIN_BATTLE;
            return true;
        }
    }
    return false;
}

static bool projected_stage_records(
    const XgRenderProjectedNativeRecord records[], uint32_t count,
    uint32_t ot_bucket, XgRenderOrderingDomain domain,
    uint32_t interpolation_producer_id) {
    uint32_t index;

    if (state.active && !state.completed) {
        for (index = 0u; index < count; ++index) {
            if (!stage_active_native_primitive(
                    &records[index].primitive, records[index].packet_address,
                    UINT32_C(0x40000000) |
                         (records[index].packet_address & UINT32_C(0x001ffffc)),
                    ot_bucket, records[index].payload_word_count,
                    interpolation_producer_id, index,
                    NULL))
                return false;
        }
        return true;
    }
    if (domain == XG_RENDER_ORDERING_DOMAIN_BATTLE) {
        for (index = 0u; index < count; ++index) {
            if (!stage_standalone_native_primitive_identified(
                    &records[index].primitive, records[index].packet_address,
                    UINT32_C(0x40000000) |
                        (records[index].packet_address &
                         UINT32_C(0x001ffffc)),
                    interpolation_producer_id, index))
                return false;
        }
        return true;
    }
    if (domain != XG_RENDER_ORDERING_DOMAIN_FIELD) return false;
    if (pre_scene.blocked ||
        pre_scene.count > XG_RENDER_PRE_SCENE_PRIMITIVE_CAPACITY - count)
        return false;
    for (index = 0u; index < count; ++index) {
        if (!stage_pre_scene_primitive(&(XgRenderPreScenePrimitive){
            .primitive = records[index].primitive,
            .packet_address = records[index].packet_address,
            .source_primitive_index = UINT32_C(0x40000000) |
                (records[index].packet_address & UINT32_C(0x001ffffc)),
            .ot_bucket = ot_bucket,
            .interpolation_producer_id = interpolation_producer_id,
            .interpolation_primitive_id = index,
            .payload_word_count = records[index].payload_word_count,
            .interpolation_identity_valid =
                interpolation_producer_id != 0u,
        }))
            return false;
    }
    return true;
}

static bool projected_stage_temporal_strips(
    const XgRenderProjectedNativeRecord records[], uint32_t count,
    XgRenderOrderingDomain domain, uint32_t interpolation_producer_id) {
    const GpuRenderTemporalCullPolicy policy = {
        .flags = GPU_RENDER_TEMPORAL_CULL_SCREEN,
        .screen_left = 0,
        .screen_top = 0,
        .screen_right_exclusive = 320 * INT32_C(65536),
        .screen_bottom_exclusive = 240 * INT32_C(65536),
    };
    uint32_t index;

    if (count == 0u) return true;
    if (state.active && !state.completed) {
        for (index = 0u; index < count; ++index) {
            if (!stage_temporal_native_primitive_identified(
                    &records[index].primitive, interpolation_producer_id,
                    index, &policy))
                return false;
        }
        return true;
    }
    if (domain == XG_RENDER_ORDERING_DOMAIN_BATTLE) {
        for (index = 0u; index < count; ++index) {
            if (!stage_temporal_native_primitive_identified(
                    &records[index].primitive, interpolation_producer_id,
                    index, &policy))
                return false;
        }
        return true;
    }
    if (domain != XG_RENDER_ORDERING_DOMAIN_FIELD || pre_scene.blocked ||
        pre_scene.count > XG_RENDER_PRE_SCENE_PRIMITIVE_CAPACITY - count)
        return false;
    for (index = 0u; index < count; ++index) {
        if (!stage_pre_scene_primitive(&(XgRenderPreScenePrimitive){
                .primitive = records[index].primitive,
                .interpolation_producer_id = interpolation_producer_id,
                .interpolation_primitive_id = index,
                .interpolation_identity_valid = true,
                .temporal_only = true,
                .temporal_cull = policy,
            }))
            return false;
    }
    return true;
}

static void projected_write_half(CPUState *cpu, uint32_t address,
                                 int16_t value) {
    psx_store_cycle_barrier();
    cpu->write_half(address, (uint16_t)value);
}

static void projected_write_byte(CPUState *cpu, uint32_t address,
                                 uint8_t value) {
    psx_store_cycle_barrier();
    cpu->write_byte(address, value);
}

static void projected_write_record_payload(
    CPUState *cpu, const XgRenderProjectedNativeRecord *record) {
    uint32_t vertex;

    switch (record->kind) {
    case XG_RENDER_PROJECTED_RECORD_FT4:
        for (vertex = 0u; vertex < 4u; ++vertex) {
            projected_write_half(cpu, record->packet_address + 8u + vertex * 8u,
                                 record->x[vertex]);
            projected_write_half(cpu, record->packet_address + 10u + vertex * 8u,
                                 record->y[vertex]);
        }
        projected_write_byte(cpu, record->packet_address + 12u, record->u[0]);
        projected_write_byte(cpu, record->packet_address + 13u, record->v[0]);
        projected_write_byte(cpu, record->packet_address + 20u, record->u[1]);
        projected_write_byte(cpu, record->packet_address + 21u, record->v[1]);
        projected_write_byte(cpu, record->packet_address + 28u, record->u[2]);
        projected_write_byte(cpu, record->packet_address + 29u, record->v[2]);
        projected_write_byte(cpu, record->packet_address + 36u, record->u[3]);
        projected_write_byte(cpu, record->packet_address + 37u, record->v[3]);
        projected_write_half(cpu, record->packet_address + 22u,
                             (int16_t)record->tpage);
        break;
    case XG_RENDER_PROJECTED_RECORD_F4_UPPER:
        projected_write_half(cpu, record->packet_address + 18u, record->y[2]);
        projected_write_half(cpu, record->packet_address + 22u, record->y[3]);
        break;
    case XG_RENDER_PROJECTED_RECORD_G4:
        for (vertex = 0u; vertex < 4u; ++vertex)
            projected_write_half(cpu,
                record->packet_address + 10u + vertex * 8u,
                record->y[vertex]);
        break;
    case XG_RENDER_PROJECTED_RECORD_F4_LOWER:
        projected_write_half(cpu, record->packet_address + 10u, record->y[0]);
        projected_write_half(cpu, record->packet_address + 14u, record->y[1]);
        break;
    }
}

static bool reject_projected(uint32_t blocker) {
    *xg_field_projected_initializer_pending() =
        (XgRenderProjectedInitializerPending){ 0 };
    if (state.active && !state.completed) {
        reject_producer_family(blocker);
    }
    return false;
}

static bool native_projected_effect_cutover(CPUState *cpu, uint32_t pc) {
    GuestRenderBridgeSnapshot bridge = { 0 };
    XgRenderProjectedConfig config;
    XgRenderProjectedSource *source;
    XgRenderProjectedNativeRecord records[XG_RENDER_PROJECTED_MAX_RECORDS];
    XgRenderProjectedNativeRecord
        temporal_strips[XG_RENDER_PROJECTED_MAX_STRIPS];
    XgHost3dLongVector direction;
    XgHost3dLongVector normalized;
    XgHost3dMatrix matrix;
    XgHost3dProjection projection;
    XgHost3dVector point = { 0 };
    XgHost3dProjectedVertex first_projection;
    XgHost3dProjectedVertex second_projection;
    GpuDrawState draw = { 0 };
    uint32_t projection_flags;
    uint32_t stack_pointer;
    uint32_t object_address;
    uint32_t eye_address;
    uint32_t at_address;
    uint32_t matrix_address;
    uint32_t ot_address;
    uint32_t buffer_index;
    uint32_t previous_head;
    uint32_t ot_bucket;
    XgRenderOrderingDomain ordering_domain;
    uint32_t record_count = 0u;
    uint32_t strip_record_count;
    uint32_t temporal_strip_count = 0u;
    uint32_t index;
    int16_t eye[3];
    int16_t at[3];
    int16_t fade;
    int32_t distance;
    int32_t quotient;
    int32_t angle;
    int32_t phase;
    int32_t product;

    (void)pc;
    ++projected_lifecycle.cutover_attempt_count;

    if (state.requested_render_mode != GUEST_RENDER_RENDER_NATIVE) return false;
    if (guest_render_bridge_snapshot(&bridge) != GUEST_RENDER_OK)
        return reject_projected(68u);
    if ((state.active && bridge.modes.effective_render_mode !=
             GUEST_RENDER_RENDER_NATIVE) ||
        (!state.active && !state.armed))
        return false;
    if (cpu == NULL || cpu->read_word == NULL || cpu->write_word == NULL ||
        cpu->read_half == NULL || cpu->write_half == NULL ||
        cpu->read_byte == NULL || cpu->write_byte == NULL ||
        !stack_address_is_valid(cpu->gpr[29]))
        return reject_projected(61u);
    stack_pointer = cpu->gpr[29];
    object_address = cpu->gpr[4];
    eye_address = cpu->gpr[5];
    at_address = cpu->gpr[6];
    matrix_address = cpu->gpr[7];
    ot_address = cpu->read_word(stack_pointer + 0x10u);
    buffer_index = cpu->read_word(stack_pointer + 0x14u);
    source = xg_field_projected_find(object_address);
    if (xg_field_projected_state()->blocked) {
        ++projected_lifecycle.source_blocked_count;
        return false;
    }
    if (source == NULL) {
        projected_lifecycle.last_source_miss_object = object_address;
        ++projected_lifecycle.source_miss_count;
        return false;
    }
    projected_lifecycle.last_source_success_object = object_address;
    if (source->generation == 0u || buffer_index > 1u ||
        !vector_address_is_valid(eye_address) ||
        !vector_address_is_valid(at_address) ||
        !word_address_is_valid(ot_address) ||
        !projected_resolve_ot_bucket(
            cpu, ot_address, &ot_bucket, &ordering_domain) ||
        !capture_particle_matrix(cpu, matrix_address, &matrix) ||
        !capture_projected_config(cpu, object_address, &config))
        return reject_projected(62u);
    for (index = 0u; index < 3u; ++index) {
        eye[index] = (int16_t)cpu->read_half(eye_address + index * 2u);
        at[index] = (int16_t)cpu->read_half(at_address + index * 2u);
    }
    direction = (XgHost3dLongVector){
        projected_wrap_subtract(at[0], eye[0]),
        0,
        projected_wrap_subtract(at[2], eye[2]),
    };
    if (!xg_host_3d_vector_normal(&direction, &normalized))
        return reject_projected(63u);
    product = projected_wrap_multiply(normalized.x, config.point_radius);
    point.x = (int16_t)(projected_rounded_shift(product, 12u) + at[0]);
    point.y = config.point_y;
    product = projected_wrap_multiply(normalized.z, config.point_radius);
    point.z = (int16_t)(projected_rounded_shift(product, 12u) + at[2]);
    capture_shadow_projection(cpu, &projection);
    memcpy(projection.rotation, matrix.rotation, sizeof(projection.rotation));
    memcpy(projection.translation, matrix.translation,
           sizeof(projection.translation));
    if (!xg_host_3d_rtps(&projection, &point, &first_projection,
                         &projection_flags))
        return reject_projected(63u);

    if (config.fade_divisor == 0) {
        fade = 0;
    } else {
        uint32_t length_squared = 0u;

        for (index = 0u; index < 3u; ++index) {
            const int32_t delta = projected_wrap_subtract(at[index], eye[index]);
            length_squared += (uint32_t)projected_wrap_multiply(delta, delta);
        }
        if (!projected_square_root0(cpu, length_squared, &distance) ||
            !projected_divide(
                projected_wrap_subtract(distance, config.fade_offset),
                config.fade_divisor, &quotient))
            return reject_projected(64u);
        fade = low_s16((uint32_t)quotient);
        if (fade < 0) fade = 0;
        if (fade > 0x100) fade = 0x100;
    }
    if (!projected_ratan2(cpu, direction.x, direction.z, &angle))
        return reject_projected(64u);
    product = projected_wrap_multiply(config.strip_width,
                                      config.phase_multiplier);
    product = projected_wrap_multiply(product, angle & 0xfff);
    phase = projected_rounded_shift(product, 12u);
    gpu_get_draw_state(&draw);
    memset(records, 0, sizeof(records));
    if (!projected_build_strips(
            cpu, source, &config, &draw, buffer_index, phase,
            first_projection.y, fade, records, &record_count, false))
        return reject_projected(65u);
    strip_record_count = record_count;
    if (strip_record_count == 0u &&
        !projected_build_strips(
            cpu, source, &config, &draw, buffer_index, phase,
            first_projection.y, fade, temporal_strips,
            &temporal_strip_count, true))
        return reject_projected(65u);

    if (config.fixed_groups > 0) {
        int16_t upper_y = (int16_t)((uint16_t)first_projection.y -
                                    (uint16_t)config.strip_height);

        if (upper_y > 0xf0) upper_y = 0xf0;
        if (upper_y > 0) {
            XgRenderProjectedNativeRecord record = {
                .kind = XG_RENDER_PROJECTED_RECORD_F4_UPPER,
                .packet_address = object_address + 0x280u + buffer_index * 0x18u,
                .x = { 0, 0x140, 0, 0x140 },
                .y = { 0, 0, upper_y, upper_y },
            };

            if (!projected_append_record(cpu, records, &record_count,
                                         source, &draw, &record))
                return reject_projected(66u);
        }
        product = projected_wrap_multiply(normalized.x, config.point_radius);
        product = projected_wrap_multiply(
            projected_rounded_shift(product, 12u), config.fixed_scale);
        point.x = (int16_t)(projected_rounded_shift(product, 8u) + at[0]);
        product = projected_wrap_multiply(config.point_y, config.fixed_scale);
        point.y = (int16_t)projected_rounded_shift(product, 8u);
        product = projected_wrap_multiply(normalized.z, config.point_radius);
        product = projected_wrap_multiply(
            projected_rounded_shift(product, 12u), config.fixed_scale);
        point.z = (int16_t)(projected_rounded_shift(product, 8u) + at[2]);
        if (!xg_host_3d_rtps(&projection, &point, &second_projection,
                             &projection_flags))
            return reject_projected(63u);
        if ((int32_t)second_projection.y - first_projection.y > 0xf0)
            second_projection.y = (int16_t)(first_projection.y + 0xf0);
        if (second_projection.y >= 0 && first_projection.y < 0xf0) {
            XgRenderProjectedNativeRecord record = {
                .kind = XG_RENDER_PROJECTED_RECORD_G4,
                .packet_address = object_address + 0x2e0u + buffer_index * 0x24u,
                .x = { 0, 0x140, 0, 0x140 },
                .y = { first_projection.y, first_projection.y,
                       second_projection.y, second_projection.y },
            };

            if (!projected_append_record(cpu, records, &record_count,
                                         source, &draw, &record))
                return reject_projected(66u);
        }
        {
            const int16_t lower_y = second_projection.y < 0
                ? 0 : second_projection.y;

            if (lower_y < 0xf0) {
                XgRenderProjectedNativeRecord record = {
                    .kind = XG_RENDER_PROJECTED_RECORD_F4_LOWER,
                    .packet_address = object_address + 0x2b0u +
                        buffer_index * 0x18u,
                    .x = { 0, 0x140, 0, 0x140 },
                    .y = { lower_y, lower_y, 0xf0, 0xf0 },
                };

                if (!projected_append_record(cpu, records, &record_count,
                                             source, &draw, &record))
                    return reject_projected(66u);
            }
        }
    }
    if (!projected_stage_records(
            records, record_count, ot_bucket, ordering_domain,
            object_address & UINT32_C(0x1fffffff)))
        return reject_projected(67u);
    if (!projected_stage_temporal_strips(
            temporal_strips, temporal_strip_count, ordering_domain,
            object_address & UINT32_C(0x1fffffff)))
        return reject_projected(67u);
#ifdef PSX_XG_RENDER_AUTH_RUNTIME_TESTING
    for (index = 0u; index < record_count; ++index)
        projected_test_primitives[index] = records[index].primitive;
    projected_test_primitive_count = record_count;
#endif
    previous_head = cpu->read_word(ot_address);
    for (index = 0u; index < record_count; ++index) {
        uint32_t linked_head;

        projected_write_record_payload(cpu, &records[index]);
        psx_store_cycle_barrier();
        cpu->write_word(records[index].packet_address,
            (records[index].packet_tag & UINT32_C(0xff000000)) |
            (previous_head & UINT32_C(0x00ffffff)));
        linked_head = (previous_head & UINT32_C(0xff000000)) |
            (records[index].packet_address & UINT32_C(0x00ffffff));
        psx_store_cycle_barrier();
        cpu->write_word(ot_address, linked_head);
        previous_head = linked_head;
    }
    cpu->gpr[2] = (uint32_t)(int32_t)first_projection.y;
    cpu->pc = cpu->gpr[31];
    ++projected_lifecycle.cutover_success_count;
    projected_lifecycle.primitive_count += record_count;
    return true;
}

static bool world_sky_compute_fog(uint32_t mode, int32_t projection_distance,
                                  int16_t *depth_cue_a,
                                  int32_t *depth_cue_b) {
    const int32_t near_distance = mode == 2u ? 0xb00 : 0x800;
    const int32_t far_distance = 0xe80;
    const int32_t range = far_distance - near_distance;
    int64_t value;

    if (depth_cue_a == NULL || depth_cue_b == NULL ||
        projection_distance == 0 || range <= 99)
        return false;
    value = ((int64_t)-near_distance * far_distance / range) * 256;
    value /= projection_distance;
    if (value < INT16_MIN) value = INT16_MIN;
    if (value > INT16_MAX) value = INT16_MAX;
    *depth_cue_a = (int16_t)value;
    value = ((int64_t)far_distance * 4096 / range) * 4096;
    *depth_cue_b = (int32_t)(uint32_t)value;
    return true;
}

static void world_sky_store_matrix(CPUState *cpu, uint32_t address,
                                   const XgHost3dMatrix *matrix) {
    const uint32_t words[8] = {
        (uint16_t)matrix->rotation[0][0] |
            ((uint32_t)(uint16_t)matrix->rotation[0][1] << 16u),
        (uint16_t)matrix->rotation[0][2] |
            ((uint32_t)(uint16_t)matrix->rotation[1][0] << 16u),
        (uint16_t)matrix->rotation[1][1] |
            ((uint32_t)(uint16_t)matrix->rotation[1][2] << 16u),
        (uint16_t)matrix->rotation[2][0] |
            ((uint32_t)(uint16_t)matrix->rotation[2][1] << 16u),
        (uint16_t)matrix->rotation[2][2] |
            ((uint32_t)matrix->pad << 16u),
        (uint32_t)matrix->translation[0],
        (uint32_t)matrix->translation[1],
        (uint32_t)matrix->translation[2],
    };
    uint32_t index;

    for (index = 0u; index < 8u; ++index) {
        psx_store_cycle_barrier();
        cpu->write_word(address + index * 4u, words[index]);
    }
}

static uint32_t native_world_cutover_readiness_blocker(void) {
    GuestRenderBridgeSnapshot bridge = {0};

    if (world_native_cutover_failed) return 1u;
    if (state.requested_render_mode != GUEST_RENDER_RENDER_NATIVE) return 2u;
    if (guest_render_bridge_snapshot(&bridge) != GUEST_RENDER_OK) return 3u;
    if (bridge.modes.effective_render_mode != GUEST_RENDER_RENDER_NATIVE)
        return 4u;
    return 0u;
}

static bool native_world_cutover_ready(void) {
    return native_world_cutover_readiness_blocker() == 0u;
}

typedef bool (*XgRenderWorldNativeCutover)(CPUState *cpu);

static bool run_native_world_cutover(CPUState *cpu,
                                     XgRenderWorldNativeCutover cutover) {
    bool accepted;
    int previous_exec_phase = 0;

    if (cutover == NULL || world_native_cutover_in_progress) {
        world_native_cutover_failed = true;
        abort_standalone_submission();
        return false;
    }
    /* Each complete producer cutover is atomic. A rejected sibling may abort
     * its own standalone submission, but must not disable unrelated world
     * families for the remainder of the scene. */
    if (world_native_cutover_failed) {
        world_native_cutover_failed = false;
    }
    world_native_cutover_in_progress = true;
    if (exec_phase_exchange != NULL)
        previous_exec_phase = exec_phase_exchange(2);
    accepted = cutover(cpu);
    if (exec_phase_exchange != NULL)
        (void)exec_phase_exchange(previous_exec_phase);
    world_native_cutover_in_progress = false;
    return accepted;
}

static bool world_authentication_generation(uint64_t *out_generation) {
    if (out_generation == NULL || state.scene_generation == UINT64_MAX)
        return false;
    *out_generation = state.scene_generation + 1u;
    return *out_generation != 0u;
}

static bool native_snapshot_can_record(
    const PsxXgRenderWorldNativeSnapshot *snapshot,
    uint32_t primitive_count) {
    return snapshot != NULL && snapshot->native_cutover_count != UINT64_MAX &&
        snapshot->native_primitive_count <= UINT64_MAX - primitive_count;
}

static void native_snapshot_record(PsxXgRenderWorldNativeSnapshot *snapshot,
                                   uint32_t primitive_count) {
    ++snapshot->native_cutover_count;
    snapshot->native_primitive_count += primitive_count;
}

static void native_snapshot_record_failure(
    PsxXgRenderWorldNativeSnapshot *snapshot,
    PsxXgRenderWorldNativeFailureStage stage, uint32_t detail,
    uint32_t anchor_count) {
    if (snapshot == NULL) return;
    if (snapshot->native_failure_count == 0u) {
        snapshot->first_failure_stage = (uint32_t)stage;
        snapshot->first_failure_detail = detail;
        snapshot->first_anchor_count = anchor_count;
    }
    if (snapshot->native_failure_count != UINT64_MAX)
        ++snapshot->native_failure_count;
    snapshot->last_failure_stage = (uint32_t)stage;
    snapshot->last_failure_detail = detail;
    snapshot->last_anchor_count = anchor_count;
}

static uint32_t fixed_ir_uv(const XgRenderIrVertex *vertex) {
    return (uint32_t)vertex->u >> 16u |
        (((uint32_t)vertex->v >> 16u) << 8u);
}

static uint32_t projected_xy(const XgHost3dProjectedVertex *vertex) {
    return (uint16_t)vertex->x |
        ((uint32_t)(uint16_t)vertex->y << 16u);
}

static void store_full_matrix(CPUState *cpu, uint32_t address,
                              const XgHost3dMatrix *matrix) {
    world_sky_store_matrix(cpu, address, matrix);
}

static int32_t terrain_fixed_floor(int32_t value) {
    const int64_t wide = value;

    return wide >= 0
        ? (int32_t)(wide / INT64_C(65536))
        : -(int32_t)((-wide + INT64_C(65535)) / INT64_C(65536));
}

static void diagnose_world_terrain_mesh(
    XgRenderWorldTerrainWaterNativeState *workspace, uint32_t record_count) {
    uint32_t mesh_generation = ++workspace->mesh_generation;
    uint32_t record_index;

    if (mesh_generation == 0u) {
        memset(workspace->mesh_vertices, 0,
               sizeof(workspace->mesh_vertices));
        mesh_generation = ++workspace->mesh_generation;
    }

    world_terrain_mesh_duplicate_vertices = 0u;
    world_terrain_mesh_cross_tile_duplicate_vertices = 0u;
    world_terrain_mesh_canonical_raster_conflicts = 0u;
    world_terrain_mesh_native_raster_conflicts = 0u;
    world_terrain_mesh_cross_tile_native_raster_conflicts = 0u;
    for (record_index = 0u; record_index < record_count; ++record_index) {
        const XgRenderIrTriangle *triangle =
            &workspace->records[record_index].primitive.triangles[0];
        uint32_t vertex_index;

        for (vertex_index = 0u; vertex_index < 3u; ++vertex_index) {
            const XgRenderIrVertex *vertex = &triangle->vertices[vertex_index];
            const uint32_t mesh_index = vertex->interpolation_vertex_id;

            if (vertex->interpolation_group_id != UINT32_C(0x63000000) ||
                mesh_index >= 145u * 145u)
                continue;
            {
            const int32_t canonical_x = terrain_fixed_floor(vertex->x);
            const int32_t canonical_y = terrain_fixed_floor(vertex->y);
            const int32_t native_x = terrain_fixed_floor(
                vertex->native_view_position ? vertex->native_view_x : vertex->x);
            const int32_t native_y = terrain_fixed_floor(
                vertex->native_view_position ? vertex->native_view_y : vertex->y);
            if (workspace->mesh_vertices[mesh_index].generation !=
                    mesh_generation) {
                workspace->mesh_vertices[mesh_index].group_id =
                    vertex->interpolation_group_id;
                workspace->mesh_vertices[mesh_index].canonical_x = canonical_x;
                workspace->mesh_vertices[mesh_index].canonical_y = canonical_y;
                workspace->mesh_vertices[mesh_index].native_x = native_x;
                workspace->mesh_vertices[mesh_index].native_y = native_y;
                workspace->mesh_vertices[mesh_index].generation =
                    mesh_generation;
                continue;
            }
            ++world_terrain_mesh_duplicate_vertices;
            if (workspace->mesh_vertices[mesh_index].canonical_x != canonical_x ||
                workspace->mesh_vertices[mesh_index].canonical_y != canonical_y)
                ++world_terrain_mesh_canonical_raster_conflicts;
            if (workspace->mesh_vertices[mesh_index].native_x != native_x ||
                workspace->mesh_vertices[mesh_index].native_y != native_y) {
                ++world_terrain_mesh_native_raster_conflicts;
            }
            }
        }
    }
}

static bool native_world_terrain_water_cutover(CPUState *cpu) {
    XgRenderWorldTerrainWaterNativeState *workspace =
        &world_terrain_water_native_state;
    XgWorldTerrainWaterNativePreparation preparation;
    XgWorldTerrainWaterNativeRequest request = {0};
    XgWorldTerrainWaterAuthenticatedReader reader;
    XgWorldTerrainWaterShadowSnapshot shadow_snapshot;
    GpuDrawState draw = {0};
    uint64_t generation;
    uint64_t scene_id;
    uint32_t context;
    uint32_t index;
    uint32_t unculled_record_count = 0u;

    world_terrain_water_native_last_caller = cpu != NULL ? cpu->gpr[31] : 0u;
    world_terrain_water_native_blocker_detail = 1u;
    {
        const uint32_t readiness_blocker =
            native_world_cutover_readiness_blocker();
        if (readiness_blocker != 0u) {
            world_terrain_water_native_blocker_detail =
                10u + readiness_blocker;
            return false;
        }
    }
    if (cpu == NULL) {
        world_terrain_water_native_blocker_detail = 12u;
        return false;
    }
    if (cpu->read_word == NULL || cpu->read_half == NULL ||
        cpu->write_word == NULL) {
        world_terrain_water_native_blocker_detail = 13u;
        return false;
    }
    if (!xg_world_terrain_water_caller_is_valid(cpu->gpr[31])) {
        world_terrain_water_native_blocker_detail = 14u;
        return false;
    }
    if (!world_authentication_generation(&generation)) {
        world_terrain_water_native_blocker_detail = 15u;
        return false;
    }
    world_terrain_water_native_blocker_detail = 2u;
    context = cpu->read_word(XG_WORLD_TERRAIN_WATER_NATIVE_CONTEXT_ADDRESS);
    if (!guest_data_range_is_valid(context, 0x78u, 4u, false)) return false;
    gpu_get_draw_state(&draw);
    request.capture = (XgWorldTerrainWaterCaptureRequest){
        .authentication_generation = generation,
        .caller_return = cpu->gpr[31],
        .screen_offset_x = (int32_t)cpu->gte_ctrl[24],
        .screen_offset_y = (int32_t)cpu->gte_ctrl[25],
        .screen_x_cull_margin = render_screen_x_cull_margin(),
        .projection_distance = (uint16_t)cpu->gte_ctrl[26],
        .raster = {
            .draw_area_left = draw.left,
            .draw_area_top = draw.top,
            .draw_area_right = draw.right,
            .draw_area_bottom = draw.bottom,
            .draw_offset_x = draw.offset_x,
            .draw_offset_y = draw.offset_y,
            .texture_window_mask_x = draw.texture_window_mask_x,
            .texture_window_mask_y = draw.texture_window_mask_y,
            .texture_window_offset_x = draw.texture_window_offset_x,
            .texture_window_offset_y = draw.texture_window_offset_y,
            .dither = draw.dither,
            .mask_set = draw.mask_set,
            .mask_check = draw.mask_check,
        },
        .projection_state_authenticated = true,
    };
    request.entry_pc = XG_WORLD_TERRAIN_WATER_NATIVE_ENTRY_PC;
    request.ot_base = cpu->gpr[4];
    request.packet_base = cpu->gpr[5];
    request.position_address = cpu->gpr[6];
    reader = (XgWorldTerrainWaterAuthenticatedReader){
        .context = cpu,
        .read_u16 = read_native_u16,
        .read_u32 = read_native_u32,
        .authentication_generation = generation,
        .authenticated = true,
    };
    memset(workspace->ot_touched, 0, sizeof(workspace->ot_touched));
    world_terrain_water_native_blocker_detail = 3u;
    scene_id = interpolation_scene_generation();
    if (xg_world_terrain_water_native_prepare(
            &request, &reader, workspace->records,
            XG_WORLD_TERRAIN_WATER_RECORD_CAPACITY, workspace->anchors,
            XG_WORLD_TERRAIN_WATER_ANCHOR_CAPACITY, &workspace->source,
            &preparation) !=
            XG_WORLD_TERRAIN_WATER_NATIVE_OK ||
        !preparation.authenticated || !preparation.sealed ||
        preparation.authentication_generation != generation ||
        preparation.anchor_count > XG_WORLD_TERRAIN_WATER_ANCHOR_CAPACITY ||
        !physical_address_equals(preparation.continuation_pc, cpu->gpr[31]) ||
        preparation.record_count > XG_WORLD_TERRAIN_WATER_RECORD_CAPACITY ||
        !guest_data_range_is_valid(
            XG_WORLD_TERRAIN_WATER_NATIVE_SCRATCH_ADDRESS,
            XG_WORLD_TERRAIN_WATER_NATIVE_SCRATCH_BYTES, 4u, true) ||
        !guest_data_range_is_valid(
            XG_WORLD_TERRAIN_WATER_NATIVE_FINAL_COUNT_ADDRESS, 4u, 4u,
            false))
        return false;

    if (world_terrain_water_temporal_scene != scene_id) {
        memset(world_terrain_water_temporal_tiles, 0,
               sizeof(world_terrain_water_temporal_tiles));
        world_terrain_water_temporal_scene = scene_id;
    }
    if (xg_world_terrain_water_append_temporal_tile_anchors(
            world_terrain_water_temporal_tiles,
            (uint32_t)(sizeof(world_terrain_water_temporal_tiles) /
                       sizeof(world_terrain_water_temporal_tiles[0])),
            &workspace->source,
            workspace->anchors, XG_WORLD_TERRAIN_WATER_ANCHOR_CAPACITY,
            &preparation.anchor_count) != XG_WORLD_TERRAIN_WATER_OK)
        return false;

    if (xg_world_terrain_water_build_unculled(
            &workspace->source, workspace->unculled_records,
            XG_WORLD_TERRAIN_WATER_UNCULLED_CAPACITY,
            &unculled_record_count) != XG_WORLD_TERRAIN_WATER_OK)
        return false;

    diagnose_world_terrain_mesh(workspace, preparation.record_count);

    for (index = 0u; index < preparation.anchor_count;) {
        GpuRenderInterpolationVertexAnchor anchors[256];
        const uint32_t count = preparation.anchor_count - index < 256u
            ? preparation.anchor_count - index : 256u;
        uint32_t anchor_index;

        for (anchor_index = 0u; anchor_index < count; ++anchor_index) {
            const XgWorldTerrainWaterAnchor *source =
                &workspace->anchors[index + anchor_index];

            if (xg_render_backend_translate_anchor(
                    &source->material, &source->vertex,
                    interpolation_scene_generation(),
                    XG_WORLD_TERRAIN_WATER_NATIVE_ENTRY_PC,
                    &anchors[anchor_index]) != XG_RENDER_BACKEND_OK) {
                abort_standalone_submission();
                return false;
            }
        }
        if (gr_record_interpolation_anchors(anchors, count) !=
                GPU_RENDER_TRANSACTION_OK) {
            abort_standalone_submission();
            return false;
        }
        index += count;
    }

    world_terrain_water_native_blocker_detail = 4u;
    for (index = 0u;
         index < XG_WORLD_TERRAIN_WATER_NATIVE_SCRATCH_WORD_COUNT; ++index) {
        const uint32_t address =
            XG_WORLD_TERRAIN_WATER_NATIVE_SCRATCH_ADDRESS + index * 4u;

        workspace->scratch_words[index] = cpu->read_word(address);
    }
    xg_world_terrain_water_shadow_snapshot(&shadow_snapshot);
    if (shadow_snapshot.native_cutover_count == UINT64_MAX ||
        shadow_snapshot.native_primitive_count >
            UINT64_MAX - preparation.record_count)
        return false;
    for (index = 0u; index < preparation.record_count; ++index) {
        const XgWorldTerrainWaterRecord *record = &workspace->records[index];
        const XgRenderIrTriangle *triangle = &record->primitive.triangles[0];
        const uint32_t packet = preparation.packet_base +
            index * XG_WORLD_TERRAIN_WATER_NATIVE_PACKET_STRIDE;
        const uint32_t ot_address = preparation.ot_base +
            record->ordering_bucket * 4u;
        uint32_t vertex;

        if (record->allocation_ordinal != index ||
            record->ordering_bucket >=
                XG_WORLD_TERRAIN_WATER_NATIVE_OT_BUCKET_COUNT ||
            !guest_data_range_is_valid(
                packet, XG_WORLD_TERRAIN_WATER_NATIVE_PACKET_STRIDE, 4u,
                false) ||
            !guest_data_range_is_valid(ot_address, 4u, 4u, false) ||
            cpu->read_word(packet + 4u) !=
                XG_WORLD_TERRAIN_WATER_NATIVE_PACKET_MATERIAL_WORD)
            return false;
        workspace->packet_uv2_high[index] =
            cpu->read_word(packet + 28u) & UINT32_C(0xffff0000);
        for (vertex = 0u; vertex < 3u; ++vertex) {
            if ((fixed_ir_uv(&triangle->vertices[vertex]) &
                 UINT32_C(0xffff0000)) != 0u)
                return false;
        }
        if (!workspace->ot_touched[record->ordering_bucket]) {
            workspace->ot_heads[record->ordering_bucket] =
                cpu->read_word(ot_address);
            workspace->simulated_ot_heads[record->ordering_bucket] =
                workspace->ot_heads[record->ordering_bucket];
            workspace->ot_touched[record->ordering_bucket] = true;
        }
        if (((workspace->simulated_ot_heads[record->ordering_bucket] |
              UINT32_C(0x07000000)) >> 24u) !=
                XG_WORLD_TERRAIN_WATER_NATIVE_PACKET_TAG_WORD_COUNT)
            return false;
        workspace->simulated_ot_heads[record->ordering_bucket] =
            packet & UINT32_C(0x00ffffff);
    }
    for (index = 0u; index < preparation.record_count; ++index) {
        const uint32_t packet = preparation.packet_base +
            index * XG_WORLD_TERRAIN_WATER_NATIVE_PACKET_STRIDE;

        if (!stage_standalone_native_primitive_identified(
                &workspace->records[index].primitive, packet,
                UINT32_C(0x63000000) |
                    workspace->records[index].source_primitive_index,
                XG_WORLD_TERRAIN_WATER_NATIVE_ENTRY_PC,
                workspace->records[index].interpolation_primitive_id)) {
            abort_standalone_submission();
            return false;
        }
    }
    for (index = 0u; index < unculled_record_count; ++index) {
        const XgWorldTerrainWaterRecord *record =
            &workspace->unculled_records[index];
        const GpuRenderTemporalCullPolicy policy = {
            .flags = GPU_RENDER_TEMPORAL_CULL_PROJECTIVE |
                GPU_RENDER_TEMPORAL_CULL_DEPTH |
                GPU_RENDER_TEMPORAL_FORCE_PHASES,
            .depth_min_inclusive = 0,
            .depth_max_exclusive = 0x0f00,
            .depth_mode = GPU_RENDER_TEMPORAL_DEPTH_MAXIMUM,
            .ordering_depth_shift = 4u,
        };

        if (!record->temporal_only ||
            record->temporal_cull_reasons !=
                XG_WORLD_TERRAIN_WATER_CULL_DEPTH ||
            record->max_depth >= UINT16_C(0x0f80))
            continue;
        if (!stage_temporal_native_primitive_identified(
                &record->primitive,
                XG_WORLD_TERRAIN_WATER_NATIVE_ENTRY_PC,
                record->interpolation_primitive_id, &policy)) {
            abort_standalone_submission();
            return false;
        }
    }
    if (!xg_world_terrain_water_shadow_record_native_cutover(
            preparation.record_count)) {
        abort_standalone_submission();
        return false;
    }
    world_terrain_water_native_blocker_detail = 0u;

    for (index = 0u;
         index < XG_WORLD_TERRAIN_WATER_NATIVE_SCRATCH_WORD_COUNT; ++index) {
        const uint32_t mask = preparation.scratch.write_masks[index];

        if (mask != 0u) {
            const uint32_t value =
                (workspace->scratch_words[index] & ~mask) |
                (preparation.scratch.values[index] & mask);
            psx_store_cycle_barrier();
            cpu->write_word(XG_WORLD_TERRAIN_WATER_NATIVE_SCRATCH_ADDRESS +
                                index * 4u,
                            value);
        }
    }
    for (index = 0u; index < preparation.record_count; ++index) {
        const XgWorldTerrainWaterRecord *record = &workspace->records[index];
        const XgRenderIrTriangle *triangle = &record->primitive.triangles[0];
        const uint32_t packet = preparation.packet_base +
            index * XG_WORLD_TERRAIN_WATER_NATIVE_PACKET_STRIDE;
        const uint32_t bucket = record->ordering_bucket;
        const uint32_t ot_address = preparation.ot_base + bucket * 4u;
        const uint32_t previous_head = workspace->ot_heads[bucket];
        uint32_t vertex;

        for (vertex = 0u; vertex < 3u; ++vertex) {
            psx_store_cycle_barrier();
            cpu->write_word(packet + 8u + vertex * 8u,
                            projected_xy(&record->projected_vertices[vertex]));
        }
        psx_store_cycle_barrier();
        cpu->write_word(packet + 12u,
                        fixed_ir_uv(&triangle->vertices[0]) |
                            ((uint32_t)record->encoded_clut << 16u));
        psx_store_cycle_barrier();
        cpu->write_word(packet + 20u,
                        fixed_ir_uv(&triangle->vertices[1]) |
                            ((uint32_t)record->encoded_tpage << 16u));
        psx_store_cycle_barrier();
        cpu->write_word(packet + 28u,
                        workspace->packet_uv2_high[index] |
                            fixed_ir_uv(&triangle->vertices[2]));
        psx_store_cycle_barrier();
        cpu->write_word(packet,
            previous_head | UINT32_C(0x07000000));
        workspace->ot_heads[bucket] =
            packet & UINT32_C(0x00ffffff);
        psx_store_cycle_barrier();
        cpu->write_word(ot_address, workspace->ot_heads[bucket]);
    }
    psx_store_cycle_barrier();
    cpu->write_word(XG_WORLD_TERRAIN_WATER_NATIVE_FINAL_COUNT_ADDRESS,
                    preparation.final_count);
    for (index = 0u; index < XG_WORLD_TERRAIN_WATER_TILE_COUNT; ++index) {
        const XgWorldTerrainWaterTileSource *tile =
            &workspace->source.tiles[index];

        if (tile->active && tile->has_data && tile->grid_index < 81u)
            world_terrain_water_temporal_tiles[tile->grid_index] = *tile;
    }
    cpu->pc = preparation.continuation_pc;
    return true;
}

static bool native_world_entity_shadows_cutover(CPUState *cpu) {
    XgRenderWorldEntityShadowsNativeState *workspace =
        &world_entity_shadows_native_state;
    XgWorldEntityShadowsCaptureRequest request = {0};
    XgWorldEntityShadowsAuthenticatedReader reader;
    XgWorldEntityShadowsCapture capture;
    XgWorldEntityShadowsNativePreparation preparation;
    XgWorldEntityShadowsShadowSnapshot shadow_snapshot;
    GpuDrawState draw = {0};
    uint64_t generation;
    uint32_t packet_base = 0u;
    uint32_t ot_base = 0u;
    uint32_t index;

    if (!native_world_cutover_ready() || cpu == NULL ||
        cpu->read_word == NULL || cpu->read_half == NULL ||
        cpu->read_byte == NULL || cpu->write_word == NULL ||
        !physical_address_equals(cpu->gpr[31],
                                 XG_WORLD_ENTITY_SHADOWS_CONTINUATION_PC) ||
        !world_authentication_generation(&generation))
        return false;
    gpu_get_draw_state(&draw);
    request = (XgWorldEntityShadowsCaptureRequest){
        .authentication_generation = generation,
        .producer_callsite = XG_WORLD_ENTITY_SHADOWS_PRODUCER_CALLSITE,
        .screen_offset_x = (int32_t)cpu->gte_ctrl[24],
        .screen_offset_y = (int32_t)cpu->gte_ctrl[25],
        .projection_distance = (uint16_t)cpu->gte_ctrl[26],
        .raster = {
            .draw_area_left = draw.left,
            .draw_area_top = draw.top,
            .draw_area_right = draw.right,
            .draw_area_bottom = draw.bottom,
            .draw_offset_x = draw.offset_x,
            .draw_offset_y = draw.offset_y,
            .dither = draw.dither,
            .mask_set = draw.mask_set,
            .mask_check = draw.mask_check,
        },
        .projection_state_authenticated = true,
    };
    reader = (XgWorldEntityShadowsAuthenticatedReader){
        .context = cpu,
        .read_u8 = read_native_u8,
        .read_u16 = read_native_u16,
        .read_u32 = read_native_u32,
        .authorize_source_range = authorize_entity_shadow_source_range,
        .authentication_generation = generation,
        .authenticated = true,
    };
    memset(workspace->ot_touched, 0, sizeof(workspace->ot_touched));
    if (xg_world_entity_shadows_source_capture(
            &request, &reader, &capture) !=
            XG_WORLD_ENTITY_SHADOWS_CAPTURE_OK ||
        xg_world_entity_shadows_prepare_native_cutover(
            &capture, generation, workspace->records,
            XG_WORLD_ENTITY_SHADOW_LIST_CAPACITY, &preparation) !=
            XG_WORLD_ENTITY_SHADOWS_OK ||
        !preparation.authenticated || !preparation.sealed ||
        preparation.authentication_generation != generation ||
        preparation.authentication_generation != generation ||
        preparation.record_count > XG_WORLD_ENTITY_SHADOW_LIST_CAPACITY ||
        !physical_address_equals(preparation.continuation_pc, cpu->gpr[31]))
        return false;

    if (preparation.record_count != 0u) {
        const uint32_t buffer_index =
            cpu->read_word(XG_WORLD_ENTITY_SHADOWS_BUFFER_INDEX_ADDRESS);
        const uint32_t context =
            cpu->read_word(XG_WORLD_ENTITY_SHADOWS_CONTEXT_ADDRESS);

        if (buffer_index >= XG_WORLD_ENTITY_SHADOW_PACKET_BUFFER_COUNT ||
            !guest_data_range_is_valid(context,
                XG_WORLD_ENTITY_SHADOWS_CONTEXT_OT_OFFSET + 4u, 4u, false))
            return false;
        packet_base = cpu->read_word(
            XG_WORLD_ENTITY_SHADOWS_PACKET_BASES_ADDRESS + buffer_index * 4u);
        ot_base = cpu->read_word(
            context + XG_WORLD_ENTITY_SHADOWS_CONTEXT_OT_OFFSET);
    }
    xg_world_entity_shadows_shadow_snapshot(&shadow_snapshot);
    if (shadow_snapshot.native_cutover_count == UINT64_MAX ||
        shadow_snapshot.native_primitive_count >
            UINT64_MAX - preparation.accepted_record_count)
        return false;
    for (index = 0u; index < preparation.record_count; ++index) {
        const XgWorldEntityShadowRecord *record = &workspace->records[index];
        const uint32_t packet = packet_base + record->packet_offset;
        uint32_t vertex;

        if (record->packet_word_write_mask == 0u) continue;
        if (!guest_data_range_is_valid(
                packet, XG_WORLD_ENTITY_SHADOW_PACKET_STRIDE, 4u, false) ||
            cpu->read_word(packet + 4u) != record->material_word ||
            (cpu->read_word(packet + 12u) & UINT32_C(0x0000ffff)) !=
                record->uv[0] ||
            (cpu->read_word(packet + 12u) >> 16u) != record->clut ||
            (cpu->read_word(packet + 20u) & UINT32_C(0x0000ffff)) !=
                record->uv[1] ||
            (cpu->read_word(packet + 20u) >> 16u) != record->tpage ||
            (cpu->read_word(packet + 28u) & UINT32_C(0x0000ffff)) !=
                record->uv[2] ||
            (cpu->read_word(packet + 36u) & UINT32_C(0x0000ffff)) !=
                record->uv[3])
            return false;
        workspace->packet_tags[index] = cpu->read_word(packet);
        if ((workspace->packet_tags[index] >> 24u) !=
                XG_WORLD_ENTITY_SHADOW_PACKET_PAYLOAD_WORDS)
            return false;
        for (vertex = 0u; vertex < XG_HOST_3D_VERTEX_COUNT; ++vertex) {
            if ((record->packet_word_write_mask &
                 (UINT32_C(1) << (2u + vertex * 2u))) == 0u)
                return false;
        }
        if (record->accepted) {
            const uint32_t bucket = record->ordering_bucket;
            const uint32_t ot_address = ot_base + bucket * 4u;

            if (bucket >= XG_WORLD_ENTITY_SHADOW_OT_BUCKET_COUNT ||
                !guest_data_range_is_valid(ot_address, 4u, 4u, false))
                return false;
            if (!workspace->ot_touched[bucket]) {
                workspace->ot_heads[bucket] = cpu->read_word(ot_address);
                workspace->ot_touched[bucket] = true;
            }
        }
    }
    for (index = 0u; index < preparation.record_count; ++index) {
        const XgWorldEntityShadowRecord *record = &workspace->records[index];
        const uint32_t packet = packet_base + record->packet_offset;

        if (record->accepted && !stage_standalone_native_primitive_identified(
                &record->primitive, packet,
                UINT32_C(0x64000000) |
                    ((packet & UINT32_C(0x001ffffc)) >> 2u),
                XG_WORLD_ENTITY_SHADOWS_ENTRY_PC, record->source_index)) {
            abort_standalone_submission();
            return false;
        }
        if (!record->accepted && record->primitive.triangle_count != 0u) {
            const GpuRenderTemporalCullPolicy policy = {
                .flags = GPU_RENDER_TEMPORAL_CULL_PROJECTIVE |
                    GPU_RENDER_TEMPORAL_CULL_DEPTH,
                .depth_min_inclusive = 1,
                .depth_max_exclusive = 0x1000,
                .depth_mode = GPU_RENDER_TEMPORAL_DEPTH_MINIMUM,
                .ordering_depth_shift = 4u,
            };

            if (!stage_temporal_native_primitive_identified(
                    &record->primitive, XG_WORLD_ENTITY_SHADOWS_ENTRY_PC,
                    record->source_index, &policy)) {
                abort_standalone_submission();
                return false;
            }
        }
    }
    if (!xg_world_entity_shadows_shadow_record_native_cutover(
            preparation.accepted_record_count)) {
        abort_standalone_submission();
        return false;
    }

    for (index = 0u; index < preparation.record_count; ++index) {
        const XgWorldEntityShadowRecord *record = &workspace->records[index];
        const uint32_t packet = packet_base + record->packet_offset;
        uint32_t vertex;

        if (record->packet_word_write_mask == 0u) continue;
        for (vertex = 0u; vertex < XG_HOST_3D_VERTEX_COUNT; ++vertex) {
            psx_store_cycle_barrier();
            cpu->write_word(packet + 8u + vertex * 8u,
                            projected_xy(&record->vertices[vertex]));
        }
        if (record->accepted) {
            const uint32_t bucket = record->ordering_bucket;
            const uint32_t ot_address = ot_base + bucket * 4u;
            const uint32_t previous_head = workspace->ot_heads[bucket];

            psx_store_cycle_barrier();
            cpu->write_word(packet,
                (workspace->packet_tags[index] & UINT32_C(0xff000000)) |
                (previous_head & UINT32_C(0x00ffffff)));
            workspace->ot_heads[bucket] =
                (previous_head & UINT32_C(0xff000000)) |
                (packet & UINT32_C(0x00ffffff));
            psx_store_cycle_barrier();
            cpu->write_word(ot_address, workspace->ot_heads[bucket]);
        }
    }
    if (preparation.side_effects.pending_count_written) {
        psx_store_cycle_barrier();
        cpu->write_word(XG_WORLD_ENTITY_SHADOWS_PENDING_COUNT_ADDRESS,
                        preparation.side_effects.pending_count_after);
    }
    cpu->pc = preparation.continuation_pc;
    return true;
}

static bool native_world_decorations_cutover(CPUState *cpu) {
    XgRenderWorldDecorationsNativeState *workspace =
        &world_decorations_native_state;
    XgWorldDecorationsNativePreparation preparation;
    XgWorldDecorationsNativeRequest request = {0};
    XgWorldDecorationsAuthenticatedReader reader;
    XgWorldDecorationsShadowSnapshot shadow_snapshot;
    GpuDrawState draw = {0};
    uint64_t generation;
    uint32_t shared_count_before;
    uint32_t native_count = 0u;
    uint32_t index;

    if (!native_world_cutover_ready() || cpu == NULL ||
        cpu->read_word == NULL || cpu->read_half == NULL ||
        cpu->write_word == NULL || cpu->write_half == NULL ||
        !physical_address_equals(
            cpu->gpr[31], XG_WORLD_DECORATIONS_NATIVE_CALLER_RETURN) ||
        !world_authentication_generation(&generation))
        return false;
    gpu_get_draw_state(&draw);
    request = (XgWorldDecorationsNativeRequest){
        .authentication_generation = generation,
        .entry_pc = XG_WORLD_DECORATIONS_NATIVE_ENTRY_PC,
        .caller_return = cpu->gpr[31],
        .screen_offset_x = (int32_t)cpu->gte_ctrl[24],
        .screen_offset_y = (int32_t)cpu->gte_ctrl[25],
        .screen_x_cull_margin = render_screen_x_cull_margin(),
        .projection_distance = (uint16_t)cpu->gte_ctrl[26],
        .raster = {
            .draw_area_left = draw.left,
            .draw_area_top = draw.top,
            .draw_area_right = draw.right,
            .draw_area_bottom = draw.bottom,
            .draw_offset_x = draw.offset_x,
            .draw_offset_y = draw.offset_y,
            .dither = draw.dither,
            .mask_set = draw.mask_set,
            .mask_check = draw.mask_check,
        },
        .projection_state_authenticated = true,
    };
    reader = (XgWorldDecorationsAuthenticatedReader){
        .context = cpu,
        .read_u16 = read_native_u16,
        .read_u32 = read_native_u32,
        .authentication_generation = generation,
        .authenticated = true,
    };
    memset(workspace->ot_touched, 0, sizeof(workspace->ot_touched));
    shared_count_before = cpu->read_word(
        XG_WORLD_DECORATIONS_NATIVE_SHARED_COUNT_ADDRESS);
    if (xg_world_decorations_native_prepare_temporal(
            &request, &reader, workspace->records,
            XG_WORLD_DECORATIONS_PACKET_CAPACITY,
            workspace->temporal_records,
            XG_WORLD_DECORATIONS_TEMPORAL_CAPACITY, &preparation) !=
            XG_WORLD_DECORATIONS_NATIVE_OK ||
        !preparation.authenticated || !preparation.sealed ||
        preparation.record_count > XG_WORLD_DECORATIONS_PACKET_CAPACITY ||
        !physical_address_equals(preparation.continuation, cpu->gpr[31]) ||
        !guest_data_range_is_valid(
            XG_WORLD_DECORATIONS_NATIVE_SCRATCH_ADDRESS, 0x88u, 2u, true) ||
        !guest_data_range_is_valid(
            XG_WORLD_DECORATIONS_NATIVE_SHARED_COUNT_ADDRESS, 4u, 4u,
            false))
        return false;

    for (index = 0u; index < preparation.record_count; ++index) {
        const XgWorldDecorationsRecord *record = &workspace->records[index];
        const uint32_t packet = preparation.packet_base +
            record->packet_index * XG_WORLD_DECORATIONS_NATIVE_PACKET_STRIDE;
        uint32_t payload;

        if (record->tag_payload_word_count == 0u) continue;
        if (record->tag_payload_word_count !=
                XG_WORLD_DECORATIONS_NATIVE_PACKET_TAG_WORD_COUNT ||
            record->packet_index != index ||
            record->ordering_bucket >=
                XG_WORLD_DECORATIONS_NATIVE_OT_BUCKET_COUNT ||
            !guest_data_range_is_valid(
                packet, XG_WORLD_DECORATIONS_NATIVE_PACKET_STRIDE, 4u,
                false) ||
            !guest_data_range_is_valid(
                preparation.ot_base + record->ordering_bucket * 4u,
                4u, 4u, false))
            return false;
        for (payload = 0u;
             payload < XG_WORLD_DECORATIONS_FT4_PAYLOAD_WORD_COUNT;
             ++payload) {
            const uint32_t actual = cpu->read_word(packet + 4u + payload * 4u);
            const bool full_write =
                payload == 1u || payload == 3u ||
                payload == 5u || payload == 7u;
            const bool low_half_semantic =
                payload == 2u || payload == 6u || payload == 8u;

            if (!full_write && !low_half_semantic &&
                actual != record->ft4_payload_words[payload])
                return false;
            if (low_half_semantic &&
                (actual & UINT32_C(0x0000ffff)) !=
                    (record->ft4_payload_words[payload] &
                     UINT32_C(0x0000ffff)))
                return false;
        }
        if (!workspace->ot_touched[record->ordering_bucket]) {
            workspace->ot_heads[record->ordering_bucket] = cpu->read_word(
                preparation.ot_base + record->ordering_bucket * 4u);
            workspace->simulated_ot_heads[record->ordering_bucket] =
                workspace->ot_heads[record->ordering_bucket];
            workspace->ot_touched[record->ordering_bucket] = true;
        }
        if (((workspace->simulated_ot_heads[record->ordering_bucket] |
              UINT32_C(0x09000000)) >> 24u) !=
                XG_WORLD_DECORATIONS_NATIVE_PACKET_TAG_WORD_COUNT)
            return false;
        workspace->simulated_ot_heads[record->ordering_bucket] =
            packet & UINT32_C(0x00ffffff);
        ++native_count;
    }
    if (native_count != preparation.record_count) return false;
    xg_world_decorations_shadow_snapshot(&shadow_snapshot);
    if (shadow_snapshot.native_cutover_count == UINT64_MAX ||
        shadow_snapshot.native_primitive_count > UINT64_MAX - native_count)
        return false;
    for (index = 0u; index < preparation.record_count; ++index) {
        const XgWorldDecorationsRecord *record = &workspace->records[index];
        const uint32_t packet = preparation.packet_base +
            record->packet_index * XG_WORLD_DECORATIONS_NATIVE_PACKET_STRIDE;

        if (!stage_standalone_native_primitive_identified(
                &record->primitive, packet,
                UINT32_C(0x65000000) |
                    ((packet & UINT32_C(0x001ffffc)) >> 2u),
                XG_WORLD_DECORATIONS_NATIVE_ENTRY_PC,
                record->semantic_id)) {
            abort_standalone_submission();
            return false;
        }
    }
    for (index = 0u; index < preparation.temporal_record_count; ++index) {
        const XgWorldDecorationsRecord *record =
            &workspace->temporal_records[index];
        const GpuRenderTemporalCullPolicy policy = {
            .flags = GPU_RENDER_TEMPORAL_CULL_PROJECTIVE |
                GPU_RENDER_TEMPORAL_CULL_SCREEN |
                GPU_RENDER_TEMPORAL_CULL_DEPTH,
            .screen_left = -request.screen_x_cull_margin * INT32_C(65536),
            .screen_top = 0,
            .screen_right_exclusive =
                (320 + request.screen_x_cull_margin) * INT32_C(65536),
            .screen_bottom_exclusive = 216 * INT32_C(65536),
            .depth_min_inclusive = 1,
            .depth_max_exclusive = 0x0e00,
            .depth_mode = GPU_RENDER_TEMPORAL_DEPTH_MAXIMUM,
            .ordering_depth_shift = 4u,
        };

        if (!stage_temporal_native_primitive_identified(
                &record->primitive, XG_WORLD_DECORATIONS_NATIVE_ENTRY_PC,
                record->semantic_id, &policy)) {
            abort_standalone_submission();
            return false;
        }
    }
    if (!xg_world_decorations_shadow_record_native_cutover(native_count)) {
        abort_standalone_submission();
        return false;
    }

    for (index = 0u; index < XG_WORLD_DECORATIONS_FT4_VERTEX_COUNT; ++index) {
        const XgHost3dVector *vertex = &preparation.scratch.vertices[index];
        const uint32_t address = XG_WORLD_DECORATIONS_NATIVE_SCRATCH_ADDRESS +
            index * sizeof(*vertex);

        psx_store_cycle_barrier();
        cpu->write_half(address, (uint16_t)vertex->x);
        psx_store_cycle_barrier();
        cpu->write_half(address + 2u, (uint16_t)vertex->y);
        psx_store_cycle_barrier();
        cpu->write_half(address + 4u, (uint16_t)vertex->z);
    }
    store_full_matrix(
        cpu, XG_WORLD_DECORATIONS_NATIVE_SCRATCH_ADDRESS +
                 XG_WORLD_DECORATIONS_NATIVE_SCRATCH_CAMERA_OFFSET,
        &preparation.scratch.camera_matrix);
    store_full_matrix(
        cpu, XG_WORLD_DECORATIONS_NATIVE_SCRATCH_ADDRESS +
                 XG_WORLD_DECORATIONS_NATIVE_SCRATCH_DECORATION_OFFSET,
        &preparation.scratch.decoration_matrix);
    for (index = 0u; index < XG_WORLD_DECORATIONS_DEPTH_CLUT_COUNT; ++index) {
        psx_store_cycle_barrier();
        cpu->write_half(
            XG_WORLD_DECORATIONS_NATIVE_SCRATCH_ADDRESS +
                XG_WORLD_DECORATIONS_NATIVE_SCRATCH_CLUT_OFFSET + index * 2u,
            preparation.scratch.depth_clut[index]);
    }
    for (index = 0u; index < preparation.record_count; ++index) {
        const XgWorldDecorationsRecord *record = &workspace->records[index];
        const uint32_t packet = preparation.packet_base +
            record->packet_index * XG_WORLD_DECORATIONS_NATIVE_PACKET_STRIDE;
        const uint32_t bucket = record->ordering_bucket;
        const uint32_t ot_address = preparation.ot_base + bucket * 4u;
        const uint32_t previous_head = workspace->ot_heads[bucket];

        psx_store_cycle_barrier();
        cpu->write_word(packet + 8u, record->ft4_payload_words[1]);
        psx_store_cycle_barrier();
        cpu->write_word(packet + 0x10u, record->ft4_payload_words[3]);
        psx_store_cycle_barrier();
        cpu->write_word(packet + 0x18u, record->ft4_payload_words[5]);
        psx_store_cycle_barrier();
        cpu->write_word(packet + 0x20u, record->ft4_payload_words[7]);
        workspace->ot_heads[bucket] =
            packet & UINT32_C(0x00ffffff);
        psx_store_cycle_barrier();
        cpu->write_word(ot_address, workspace->ot_heads[bucket]);
        psx_store_cycle_barrier();
        cpu->write_word(packet, previous_head | UINT32_C(0x09000000));
        psx_store_cycle_barrier();
        cpu->write_half(packet + 0x0eu,
                        (uint16_t)(record->ft4_payload_words[2] >> 16u));
    }
    psx_store_cycle_barrier();
    cpu->write_word(
        XG_WORLD_DECORATIONS_NATIVE_SHARED_COUNT_ADDRESS,
        (shared_count_before & ~preparation.shared_count_write_mask) |
            (preparation.final_shared_count &
             preparation.shared_count_write_mask));
    cpu->pc = preparation.continuation;
    return true;
}

static void world_actor_context_begin(CPUState *cpu) {
    uint64_t generation;

    if (world_actor_context.active) {
        if (world_actor_context.depth != UINT32_MAX)
            ++world_actor_context.depth;
        world_actor_context.poisoned = true;
        world_native_cutover_failed = true;
        clear_world_actor_native_pending();
        return;
    }
    clear_world_actor_native_pending();
    clear_world_actor_context();
    if (!native_world_cutover_ready() || cpu == NULL ||
        !stack_address_is_valid(cpu->gpr[29]) || cpu->gpr[29] < 0x28u ||
        !guest_data_range_is_valid(cpu->gpr[29] - 0x28u, 0x28u, 4u, true) ||
        !physical_address_equals(
            cpu->gpr[31], XG_WORLD_ACTOR_SPRITES_WORLD_CALLER_RETURN) ||
        !world_authentication_generation(&generation))
        return;
    world_actor_context = (XgRenderWorldActorContext){
        .authentication_generation = generation,
        .entry_stack_pointer = cpu->gpr[29],
        .caller_return = cpu->gpr[31],
        .owner_cpu = cpu,
        .depth = 1u,
        .active = true,
    };
}

static void world_actor_context_finish(CPUState *cpu) {
    if (!world_actor_context.active) return;
    if (world_actor_context.depth > 1u) {
        if (world_actor_context.owner_cpu != cpu)
            world_actor_context.poisoned = true;
        --world_actor_context.depth;
        return;
    }
    if (world_actor_context.poisoned ||
        world_actor_context.owner_cpu != cpu ||
        world_actor_sprites_native_state.valid ||
        cpu == NULL || cpu->read_word == NULL ||
        world_actor_context.entry_stack_pointer < 0x28u ||
        cpu->gpr[29] != world_actor_context.entry_stack_pointer - 0x28u ||
        !guest_data_range_is_valid(cpu->gpr[29], 0x28u, 4u, true) ||
        !physical_address_equals(
            cpu->read_word(cpu->gpr[29] + 0x20u),
            world_actor_context.caller_return)) {
        world_native_cutover_failed = true;
        clear_world_actor_native_pending();
        clear_world_actor_context();
        return;
    }
    clear_world_actor_context();
}

static bool actor_scratch_output_matches(
    CPUState *cpu, const XgWorldActorSpritesScratchOutput *scratch) {
    uint32_t vertex;

    if (!scratch->written) return true;
    if (cpu == NULL || cpu->read_half == NULL) return false;
    for (vertex = 0u; vertex < XG_HOST_3D_VERTEX_COUNT; ++vertex) {
        const uint32_t address = scratch->address + vertex * 8u;

        if (cpu->read_half(address) !=
                (uint16_t)scratch->vertices[vertex].x ||
            cpu->read_half(address + 2u) !=
                (uint16_t)scratch->vertices[vertex].y ||
            cpu->read_half(address + 4u) !=
                (uint16_t)scratch->vertices[vertex].z ||
            cpu->read_half(address + 6u) !=
                scratch->vertices[vertex].pad)
            return false;
    }
    return true;
}

static bool native_world_actor_sprites_prepare(CPUState *cpu) {
    XgRenderWorldActorSpritesNativeState *workspace =
        &world_actor_sprites_native_state;
    XgWorldActorSpritesNativePreparation *preparation =
        &workspace->preparation;
    XgWorldActorSpritesNativeRequest request = {0};
    XgWorldActorSpritesAuthenticatedReader reader;
    GpuDrawState draw = {0};
    uint32_t context;
    uint32_t ot_base;
    uint32_t resident_caller_return;
    uint32_t index;

    if (!native_world_cutover_ready() || !world_actor_context.active ||
        world_actor_context.poisoned || world_actor_context.depth != 1u ||
        world_actor_context.owner_cpu != cpu ||
        workspace->valid ||
        cpu == NULL || cpu->read_word == NULL || cpu->read_half == NULL ||
        cpu->read_byte == NULL ||
        world_actor_context.entry_stack_pointer < 0x48u ||
        cpu->gpr[29] != world_actor_context.entry_stack_pointer - 0x48u ||
        !guest_data_range_is_valid(cpu->gpr[29], 0x48u, 4u, true) ||
        !physical_address_equals(
            cpu->gpr[31], XG_WORLD_ACTOR_SPRITES_PREPARED_SEAM) ||
        !physical_address_equals(
            cpu->read_word(cpu->gpr[29] + 0x40u),
            world_actor_context.caller_return) ||
        world_actor_context.authentication_generation == 0u)
        return false;
    resident_caller_return = cpu->read_word(cpu->gpr[29] + 0x18u);
    if (!physical_address_equals(
            resident_caller_return,
            XG_WORLD_ACTOR_SPRITES_RESIDENT_CALLER_RETURN))
        return false;
    context = cpu->read_word(UINT32_C(0x8009be3c));
    if (!guest_data_range_is_valid(context, 0x74u, 4u, false)) return false;
    ot_base = cpu->read_word(context + 0x70u);
    gpu_get_draw_state(&draw);
    request = (XgWorldActorSpritesNativeRequest){
        .authentication_generation =
            world_actor_context.authentication_generation,
        .resident_entry_pc = XG_WORLD_ACTOR_SPRITES_RESIDENT_ENTRY,
        .prepared_seam_pc = XG_WORLD_ACTOR_SPRITES_PREPARED_SEAM,
        .resident_caller_return = resident_caller_return,
        .actor_address = cpu->gpr[16],
        .ordering_table_address = cpu->gpr[17],
        .screen_offset_x = (int32_t)cpu->gte_ctrl[24],
        .screen_offset_y = (int32_t)cpu->gte_ctrl[25],
        .projection_distance = (uint16_t)cpu->gte_ctrl[26],
        .depth_cue_a = low_s16(cpu->gte_ctrl[27]),
        .depth_cue_b = (int32_t)cpu->gte_ctrl[28],
        .average_z_scale4 = low_s16(cpu->gte_ctrl[30]),
        .raster = {
            .draw_area_left = draw.left,
            .draw_area_top = draw.top,
            .draw_area_right = draw.right,
            .draw_area_bottom = draw.bottom,
            .draw_offset_x = draw.offset_x,
            .draw_offset_y = draw.offset_y,
            .texture_window_mask_x = draw.texture_window_mask_x,
            .texture_window_mask_y = draw.texture_window_mask_y,
            .texture_window_offset_x = draw.texture_window_offset_x,
            .texture_window_offset_y = draw.texture_window_offset_y,
            .dither = draw.dither,
            .mask_set = draw.mask_set,
            .mask_check = draw.mask_check,
        },
        .resident_context_authenticated = true,
        .projection_state_authenticated = true,
    };
    reader = (XgWorldActorSpritesAuthenticatedReader){
        .context = cpu,
        .read_u8 = read_native_u8,
        .read_u16 = read_native_u16,
        .read_u32 = read_native_u32,
        .authorize_source_range = authorize_actor_source_range,
        .authentication_generation =
            world_actor_context.authentication_generation,
        .authenticated = true,
    };
    memset(workspace->ot_touched, 0, sizeof(workspace->ot_touched));
    if (xg_world_actor_sprites_native_prepare(
            &request, &reader, workspace->records,
            XG_WORLD_ACTOR_SPRITES_NATIVE_RECORD_CAPACITY, preparation) !=
            XG_WORLD_ACTOR_SPRITES_NATIVE_OK ||
        !preparation->authenticated || !preparation->sealed ||
        preparation->authentication_generation !=
            world_actor_context.authentication_generation ||
        preparation->record_count >
            XG_WORLD_ACTOR_SPRITES_NATIVE_RECORD_CAPACITY ||
        preparation->actor_address != cpu->gpr[16] ||
        !physical_address_equals(
            preparation->continuation_pc,
            XG_WORLD_ACTOR_SPRITES_CONTINUATION) ||
        !native_snapshot_can_record(
            &world_actor_sprites_native_snapshot, preparation->record_count))
        return false;

    if ((preparation->body_scratch.written &&
         !guest_data_range_is_valid(preparation->body_scratch.address, 0x20u,
                                    2u, false)) ||
        (preparation->shadow_scratch.written &&
         !guest_data_range_is_valid(preparation->shadow_scratch.address, 0x20u,
                                    2u, false)) ||
        (preparation->packet_cursor_written &&
         !guest_data_range_is_valid(
              XG_WORLD_ACTOR_SPRITES_PACKET_CURSOR, 4u, 4u, false)))
        return false;
    workspace->ot_base = ot_base;
    for (index = 0u; index < preparation->record_count; ++index) {
        const XgWorldActorSpriteRecord *record = &workspace->records[index];
        const uint32_t packet = record->packet_address;
        const uint32_t ot_address = record->ordering_table_address;
        uint32_t bucket;
        uint32_t payload;

        if (record->tag_payload_word_count !=
                XG_WORLD_ACTOR_SPRITE_TAG_PAYLOAD_WORD_COUNT ||
            !guest_data_range_is_valid(
                packet, XG_WORLD_ACTOR_SPRITE_PACKET_STRIDE, 4u, false) ||
            !guest_data_range_is_valid(ot_address, 4u, 4u, false) ||
            ot_address < ot_base ||
            ot_address > ot_base +
                (XG_WORLD_ACTOR_SPRITE_OT_BUCKET_COUNT - 1u) * 4u ||
            ((ot_address - ot_base) & 3u) != 0u)
            return false;
        bucket = (ot_address - ot_base) / 4u;
        workspace->ot_addresses[index] = ot_address;
        for (payload = 0u;
             payload < XG_WORLD_ACTOR_SPRITE_PACKET_PAYLOAD_WORD_COUNT;
             ++payload) {
            const uint32_t actual = cpu->read_word(
                packet + 4u + payload * 4u);
            const uint32_t mask =
                record->packet_payload_write_masks[payload];

            if (mask != UINT32_MAX && mask != UINT32_C(0x0000ffff))
                return false;
            workspace->payload_words[index][payload] =
                (actual & ~mask) |
                (record->packet_payload_words[payload] & mask);
        }
        if (!workspace->ot_touched[bucket]) {
            workspace->ot_heads[bucket] = cpu->read_word(ot_address);
            workspace->ot_touched[bucket] = true;
        }
        workspace->packet_tags[index] = UINT32_C(0x09000000) |
            (workspace->ot_heads[bucket] & UINT32_C(0x00ffffff));
        workspace->ot_heads[bucket] =
            (workspace->ot_heads[bucket] & UINT32_C(0xff000000)) |
            (record->packet_address & UINT32_C(0x00ffffff));
    }
    workspace->authentication_generation =
        world_actor_context.authentication_generation;
    workspace->entry_stack_pointer = cpu->gpr[29];
    workspace->owner_cpu = cpu;
    workspace->valid = true;
    return true;
}

static bool native_world_actor_sprites_commit(CPUState *cpu) {
    XgRenderWorldActorSpritesNativeState *workspace =
        &world_actor_sprites_native_state;
    const XgWorldActorSpritesNativePreparation *preparation =
        &workspace->preparation;
    uint64_t generation;
    uint32_t index;

    if (!workspace->valid || workspace->owner_cpu != cpu ||
        !world_actor_context.active || world_actor_context.poisoned ||
        world_actor_context.depth != 1u ||
        world_actor_context.owner_cpu != cpu ||
        !native_world_cutover_ready() ||
        !world_authentication_generation(&generation) ||
        generation != workspace->authentication_generation ||
        cpu == NULL || cpu->read_word == NULL || cpu->read_half == NULL ||
        cpu->gpr[29] != workspace->entry_stack_pointer ||
        !physical_address_equals(
            cpu->read_word(cpu->gpr[29] + 0x40u),
            world_actor_context.caller_return) ||
        cpu->read_word(XG_WORLD_ACTOR_SPRITES_PACKET_CURSOR) !=
            preparation->final_packet_cursor ||
        !actor_scratch_output_matches(cpu, &preparation->body_scratch) ||
        !actor_scratch_output_matches(cpu, &preparation->shadow_scratch))
        goto fail;

    for (index = 0u; index < preparation->record_count; ++index) {
        const XgWorldActorSpriteRecord *record = &workspace->records[index];
        uint32_t payload;

        if (cpu->read_word(record->packet_address) !=
                workspace->packet_tags[index])
            goto fail;
        for (payload = 0u;
             payload < XG_WORLD_ACTOR_SPRITE_PACKET_PAYLOAD_WORD_COUNT;
             ++payload) {
            if (cpu->read_word(
                    record->packet_address + 4u + payload * 4u) !=
                workspace->payload_words[index][payload])
                goto fail;
        }
    }
    for (index = 0u; index < XG_WORLD_ACTOR_SPRITE_OT_BUCKET_COUNT;
         ++index) {
        if (workspace->ot_touched[index] &&
            cpu->read_word(workspace->ot_base + index * 4u) !=
                workspace->ot_heads[index])
            goto fail;
    }
    if (preparation->record_count != 0u &&
        !begin_standalone_submission())
        goto fail;
    for (index = 0u; index < preparation->record_count; ++index) {
        const XgWorldActorSpriteRecord *record = &workspace->records[index];
        uint32_t interpolation_primitive_id;

        if (record->descriptor_index > (UINT32_MAX - 1u) / 2u)
            goto fail;
        interpolation_primitive_id =
            record->descriptor_index * 2u + (uint32_t)record->family;
        if (!stage_standalone_native_primitive_identified(
                &record->sprite.primitive, record->packet_address,
                UINT32_C(0x67000000) |
                    ((record->packet_address & UINT32_C(0x001ffffc)) >> 2u),
                preparation->actor_address & UINT32_C(0x1fffffff),
                interpolation_primitive_id))
            goto fail;
    }
    native_snapshot_record(
        &world_actor_sprites_native_snapshot, preparation->record_count);
    clear_world_actor_native_pending();
    return true;

fail:
    clear_world_actor_native_pending();
    return false;
}

static void capture_world_models_gte_state(
    const CPUState *cpu, XgWorldModelsGteState *gte) {
    *gte = (XgWorldModelsGteState){
        .screen_offset_x = (int32_t)cpu->gte_ctrl[24],
        .screen_offset_y = (int32_t)cpu->gte_ctrl[25],
        .projection_distance = (uint16_t)cpu->gte_ctrl[26],
        .depth_cue_a = low_s16(cpu->gte_ctrl[27]),
        .depth_cue_b = (int32_t)cpu->gte_ctrl[28],
        .average_z_scale3 = low_s16(cpu->gte_ctrl[29]),
        .average_z_scale4 = low_s16(cpu->gte_ctrl[30]),
        .far_color = {
            (int32_t)cpu->gte_ctrl[21],
            (int32_t)cpu->gte_ctrl[22],
            (int32_t)cpu->gte_ctrl[23],
        },
    };
}

static bool world_models_begin_submission(void *context) {
    (void)context;
    return begin_standalone_submission();
}

static bool world_models_stage_primitive(
    void *context, const XgRenderIrNativePrimitive *primitive,
    uint32_t packet_address, uint32_t primitive_index) {
    const XgRenderWorldModelsNativeState *workspace =
        (const XgRenderWorldModelsNativeState *)context;
    const XgWorldModelsNativePrimitiveSource *source;
    uint32_t interpolation_primitive_id;

    if (workspace == NULL ||
        primitive_index >= workspace->preparation.primitive_count)
        return false;
    source = &workspace->primitives[primitive_index];
    if (source->source_index >
        (UINT32_MAX - source->primitive_index) / 4096u)
        return false;
    interpolation_primitive_id =
        source->source_index * 4096u + source->primitive_index;
    return stage_standalone_native_primitive_identified(
        primitive, packet_address,
        UINT32_C(0x66000000) |
            ((packet_address & UINT32_C(0x001ffffc)) >> 2u),
        source->model_header_address & UINT32_C(0x1fffffff),
        interpolation_primitive_id);
}

static const XgRenderIrVertex *world_model_source_vertex(
    const XgWorldModelsNativePrimitiveOutput *output, uint32_t source_vertex) {
    if (output == NULL || source_vertex >= XG_HOST_3D_VERTEX_COUNT ||
        output->primitive.triangle_count == 0u)
        return NULL;
    if (source_vertex < 3u)
        return &output->primitive.triangles[0].vertices[source_vertex];
    if (output->primitive.triangle_count < 2u)
        return NULL;
    return &output->primitive.triangles[1].vertices[2];
}

static bool world_models_collect_interpolation_anchors(
    XgRenderWorldModelsNativeState *workspace, uint64_t scene_id,
    uint32_t *out_anchor_count, uint32_t *out_failure_detail) {
    uint32_t anchor_count = 0u;
    uint32_t dispatch_index;

    if (out_failure_detail != NULL) *out_failure_detail = 0u;
    if (workspace == NULL || scene_id == 0u || out_anchor_count == NULL ||
        out_failure_detail == NULL)
        return false;
    *out_anchor_count = 0u;
    for (dispatch_index = 0u;
         dispatch_index < workspace->preparation.dispatch_count;
         ++dispatch_index) {
        const XgWorldModelsNativeDispatch *dispatch =
            &workspace->dispatches[dispatch_index];
        uint32_t primitive_offset;

        if (dispatch->primitive_start > workspace->preparation.primitive_count ||
            dispatch->primitive_count >
                workspace->preparation.primitive_count -
                    dispatch->primitive_start) {
            *out_failure_detail = 1u;
            return false;
        }
        memset(workspace->anchor_seen, 0, sizeof(workspace->anchor_seen));
        for (primitive_offset = 0u;
             primitive_offset < dispatch->primitive_count;
             ++primitive_offset) {
            const uint32_t primitive_index =
                dispatch->primitive_start + primitive_offset;
            const XgWorldModelsNativePrimitiveSource *source =
                &workspace->primitives[primitive_index];
            const XgWorldModelsNativePrimitiveOutput *output =
                &workspace->outputs[primitive_index];
            uint32_t source_vertex;

            if (source->dispatch_index != dispatch_index) {
                *out_failure_detail = 2u;
                return false;
            }
            for (source_vertex = 0u;
                 source_vertex < source->vertex_count; ++source_vertex) {
                const uint32_t topology_vertex = source->topology[source_vertex];
                const uint32_t word = topology_vertex / 32u;
                const uint32_t mask = UINT32_C(1) << (topology_vertex % 32u);
                const XgRenderIrVertex *vertex;

                if ((workspace->anchor_seen[word] & mask) != 0u) continue;
                if (anchor_count == XG_RENDER_WORLD_MODEL_ANCHOR_CAPACITY) {
                    *out_anchor_count = anchor_count;
                    *out_failure_detail = 3u;
                    return false;
                }
                vertex = world_model_source_vertex(output, source_vertex);
                if (vertex == NULL) {
                    *out_anchor_count = anchor_count;
                    *out_failure_detail = 4u;
                    return false;
                }
                {
                    const XgRenderBackendStatus status =
                        xg_render_backend_translate_anchor(
                            &output->primitive.material, vertex, scene_id,
                            source->model_header_address & UINT32_C(0x1fffffff),
                            &workspace->anchors[anchor_count]);

                    if (status != XG_RENDER_BACKEND_OK) {
                        *out_anchor_count = anchor_count;
                        *out_failure_detail = UINT32_C(0x100) + (uint32_t)status;
                        return false;
                    }
                }
                workspace->anchor_seen[word] |= mask;
                ++anchor_count;
            }
        }
    }
    *out_anchor_count = anchor_count;
    return true;
}

static bool native_world_models_prepare(CPUState *cpu) {
    XgRenderWorldModelsNativeState *workspace = &world_models_native_state;
    XgWorldModelsNativePreparation preparation;
    XgWorldModelsNativeCommit commit = {0};
    XgWorldModelsNativeRequest request = {0};
    XgWorldModelsNativeAuthenticatedReader reader;
    XgWorldModelsNativeWorkspace native_workspace;
    XgWorldModelsNativeResult native_result;
    GpuDrawState draw = {0};
    uint64_t generation;
    uint32_t accepted_count = 0u;
    uint32_t failure_detail =
        XG_RENDER_WORLD_MODELS_PREPARE_FAILURE_PRECONDITION;
    uint32_t dispatch_index;
    uint32_t primitive_index;
    uint32_t index;

    if (!native_world_cutover_ready() || workspace->valid || cpu == NULL ||
        cpu->read_word == NULL || cpu->read_half == NULL ||
        cpu->read_byte == NULL || !stack_address_is_valid(cpu->gpr[29]) ||
        cpu->gpr[29] < 0x40u ||
        !guest_data_range_is_valid(cpu->gpr[29] - 0x40u, 0x40u, 4u, false) ||
        !world_authentication_generation(&generation)) {
        native_snapshot_record_failure(
            &world_models_native_snapshot,
            PSX_XG_RENDER_WORLD_NATIVE_FAILURE_PREPARE, failure_detail, 0u);
        return false;
    }
    failure_detail = XG_RENDER_WORLD_MODELS_PREPARE_FAILURE_CALLER_RETURN;
    if (!physical_address_equals(
            cpu->gpr[31], XG_WORLD_MODELS_PRODUCER_CONTINUATION_0) &&
        !physical_address_equals(
            cpu->gpr[31], XG_WORLD_MODELS_PRODUCER_CONTINUATION_1) &&
        !physical_address_equals(
            cpu->gpr[31], XG_WORLD_MODELS_PRODUCER_CONTINUATION_2) &&
        !physical_address_equals(
            cpu->gpr[31], XG_WORLD_MODELS_PRODUCER_CONTINUATION_3))
        goto fail;

    gpu_get_draw_state(&draw);
    request.authentication_generation = generation;
    request.entry_pc = XG_WORLD_MODELS_PRODUCER_ENTRY;
    request.caller_return = cpu->gpr[31];
    request.guest_xclip_bound = psx_ws_xclip_bound(
        cpu->read_word(XG_WORLD_MODELS_SCREEN_RIGHT_GLOBAL));
    capture_world_models_gte_state(cpu, &request.gte);
    request.raster = (XgWorldModelsNativeRasterState){
        .draw_area_left = draw.left,
        .draw_area_top = draw.top,
        .draw_area_right = draw.right,
        .draw_area_bottom = draw.bottom,
        .draw_offset_x = draw.offset_x,
        .draw_offset_y = draw.offset_y,
        .texture_window_mask_x = draw.texture_window_mask_x,
        .texture_window_mask_y = draw.texture_window_mask_y,
        .texture_window_offset_x = draw.texture_window_offset_x,
        .texture_window_offset_y = draw.texture_window_offset_y,
        .dither = draw.dither,
        .mask_set = draw.mask_set,
        .mask_check = draw.mask_check,
    };
    request.projection_state_authenticated = true;
    request.lighting_state_authenticated = true;
    reader = (XgWorldModelsNativeAuthenticatedReader){
        .context = cpu,
        .read_u8 = read_native_u8,
        .read_u16 = read_native_u16,
        .read_u32 = read_native_u32,
        .read_packet_template = read_world_model_packet_template,
        .authorize_range = authorize_world_model_range,
        .authentication_generation = generation,
        .authenticated = true,
    };
    native_workspace = (XgWorldModelsNativeWorkspace){
        .record_sources = workspace->record_sources,
        .record_capacity = XG_RENDER_WORLD_MODEL_RECORD_CAPACITY,
        .transform_nodes = workspace->transform_nodes,
        .transform_node_capacity = XG_RENDER_WORLD_MODEL_NODE_CAPACITY,
    };
    memset(workspace->ot_touched, 0, sizeof(workspace->ot_touched));
    world_model_template_read_failure = 0u;
    native_result = xg_world_models_native_prepare(
        &request, &reader, &native_workspace, workspace->records,
        XG_RENDER_WORLD_MODEL_RECORD_CAPACITY, workspace->node_side_effects,
        XG_RENDER_WORLD_MODEL_NODE_CAPACITY, workspace->dispatches,
        XG_RENDER_WORLD_MODEL_DISPATCH_CAPACITY, workspace->primitives,
        XG_RENDER_WORLD_MODEL_PRIMITIVE_CAPACITY, &preparation);
    if (native_result != XG_WORLD_MODELS_NATIVE_OK) {
        failure_detail = world_model_template_read_failure != 0u
            ? XG_RENDER_WORLD_MODELS_PREPARE_FAILURE_TEMPLATE_READ_BASE +
                  world_model_template_read_failure
            : XG_RENDER_WORLD_MODELS_PREPARE_FAILURE_NATIVE_RESULT_BASE +
                  (uint32_t)native_result;
        goto fail;
    }
    failure_detail = XG_RENDER_WORLD_MODELS_PREPARE_FAILURE_CONTRACT;
    if (!preparation.authenticated || !preparation.sealed ||
        preparation.authentication_generation != generation ||
        preparation.record_count > XG_RENDER_WORLD_MODEL_RECORD_CAPACITY ||
        preparation.transform_node_count > XG_RENDER_WORLD_MODEL_NODE_CAPACITY ||
        preparation.dispatch_count > XG_RENDER_WORLD_MODEL_DISPATCH_CAPACITY ||
        preparation.primitive_count > XG_RENDER_WORLD_MODEL_PRIMITIVE_CAPACITY ||
        !physical_address_equals(preparation.continuation_pc, cpu->gpr[31]))
        goto fail;

    failure_detail = XG_RENDER_WORLD_MODELS_PREPARE_FAILURE_GLOBAL_RANGE;
    if (!guest_data_range_is_valid(XG_WORLD_MODELS_SCALE_X_SCRATCH, 12u, 4u,
                                   true) ||
        !guest_data_range_is_valid(XG_WORLD_MODELS_COARSE_ORIGIN_SCRATCH, 6u,
                                   2u, true) ||
        !guest_data_range_is_valid(
            XG_WORLD_MODELS_RESIDENT_CULL_MODE_GLOBAL, 4u, 4u, false) ||
        !guest_data_range_is_valid(
            XG_WORLD_MODELS_RESIDENT_VERTEX_TOTAL_GLOBAL, 4u, 4u, false) ||
        !guest_data_range_is_valid(
            XG_WORLD_MODELS_RESIDENT_EMITTED_COUNT_GLOBAL, 4u, 4u, false))
        goto fail;
    for (index = 0u; index < preparation.transform_node_count; ++index) {
        failure_detail = XG_RENDER_WORLD_MODELS_PREPARE_FAILURE_NODE_RANGE;
        if (!guest_data_range_is_valid(
                workspace->node_side_effects[index].guest_address +
                    XG_WORLD_MODELS_NODE_WRITEBACK_X_OFFSET,
                12u, 4u, false))
            goto fail;
    }

    primitive_index = 0u;
    for (dispatch_index = 0u;
         dispatch_index < preparation.dispatch_count; ++dispatch_index) {
        const XgWorldModelsNativeDispatch *dispatch =
            &workspace->dispatches[dispatch_index];
        XgWorldModelsNativeDispatchOutput *dispatch_output =
            &workspace->dispatch_outputs[dispatch_index];
        uint32_t dispatch_primitive;

        *dispatch_output = (XgWorldModelsNativeDispatchOutput){0};
        dispatch_output->source_index = dispatch->source_index;
        dispatch_output->bounds_accepted = dispatch->bounds_accepted;
        dispatch_output->guest_bounds_accepted =
            dispatch->guest_bounds_accepted;
        if (!dispatch->bounds_accepted && !dispatch->guest_bounds_accepted) {
            failure_detail =
                XG_RENDER_WORLD_MODELS_PREPARE_FAILURE_CULLED_DISPATCH;
            if (dispatch->primitive_count != 0u) goto fail;
            continue;
        }
        failure_detail =
            XG_RENDER_WORLD_MODELS_PREPARE_FAILURE_DISPATCH_RANGE;
        if (dispatch->primitive_start != primitive_index ||
            dispatch->primitive_count >
                preparation.primitive_count - primitive_index)
            goto fail;
        for (dispatch_primitive = 0u;
             dispatch_primitive < dispatch->primitive_count;
             ++dispatch_primitive, ++primitive_index) {
            const XgWorldModelsNativePrimitiveSource *source =
                &workspace->primitives[primitive_index];
            XgWorldModelsNativePrimitiveOutput *output =
                &workspace->outputs[primitive_index];
            XgRenderWorldModelTemplate *template_entry;
            uint32_t allowed_mask;
            uint32_t word;

            failure_detail =
                XG_RENDER_WORLD_MODELS_PREPARE_FAILURE_SOURCE_IDENTITY;
            if (source->dispatch_index != dispatch_index ||
                source->primitive_index != dispatch_primitive)
                goto fail;
            native_result = xg_world_models_native_build_primitive(
                &preparation, generation, source, output);
            if (native_result != XG_WORLD_MODELS_NATIVE_OK) {
                failure_detail =
                    XG_RENDER_WORLD_MODELS_PREPARE_FAILURE_BUILD_RESULT_BASE +
                    (uint32_t)native_result;
                goto fail;
            }
            if (!dispatch->bounds_accepted) {
                output->passed_screen_cull = false;
                output->accepted = false;
                output->ordering_table_written = false;
            }
            if (!dispatch->guest_bounds_accepted) {
                output->counter_incremented = false;
                output->guest_ordering_table_written = false;
                output->guest_packet_word_write_mask = 0u;
            }
            template_entry = world_model_template_find(
                source->packet_address, false);
            failure_detail =
                XG_RENDER_WORLD_MODELS_PREPARE_FAILURE_TEMPLATE_MISSING;
            if (template_entry == NULL) goto fail;
            failure_detail =
                XG_RENDER_WORLD_MODELS_PREPARE_FAILURE_TEMPLATE_INACTIVE;
            if (!template_entry->active) goto fail;
            failure_detail =
                XG_RENDER_WORLD_MODELS_PREPARE_FAILURE_TEMPLATE_OWNER;
            if (template_entry->owner_cpu != cpu) goto fail;
            failure_detail =
                XG_RENDER_WORLD_MODELS_PREPARE_FAILURE_TEMPLATE_EPOCH;
            if (template_entry->resource_epoch != source->resource_epoch)
                goto fail;
            failure_detail =
                XG_RENDER_WORLD_MODELS_PREPARE_FAILURE_TEMPLATE_MODEL;
            if (template_entry->model_address !=
                    model_ft4_template_key(source->model_header_address))
                goto fail;
            failure_detail =
                XG_RENDER_WORLD_MODELS_PREPARE_FAILURE_TEMPLATE_PACKET_BASE;
            if (template_entry->packet_base !=
                    model_ft4_template_key(source->packet_base_address))
                goto fail;
            failure_detail =
                XG_RENDER_WORLD_MODELS_PREPARE_FAILURE_TEMPLATE_ORDINAL;
            if (template_entry->primitive_ordinal != source->primitive_index)
                goto fail;
            failure_detail =
                XG_RENDER_WORLD_MODELS_PREPARE_FAILURE_TEMPLATE_ATTRIBUTE;
            if (template_entry->attribute_address !=
                    model_ft4_template_key(source->attribute_address))
                goto fail;
            failure_detail =
                XG_RENDER_WORLD_MODELS_PREPARE_FAILURE_TEMPLATE_FAMILY;
            if (template_entry->primitive_family != source->primitive_family)
                goto fail;
            failure_detail =
                XG_RENDER_WORLD_MODELS_PREPARE_FAILURE_TEMPLATE_WORD_COUNT;
            if (template_entry->word_count != source->packet_word_count)
                goto fail;
            failure_detail =
                XG_RENDER_WORLD_MODELS_PREPARE_FAILURE_OUTPUT_PACKET;
            if (output->packet_address != source->packet_address) goto fail;
            failure_detail =
                XG_RENDER_WORLD_MODELS_PREPARE_FAILURE_OUTPUT_WORD_COUNT;
            if (output->packet_word_count != source->packet_word_count)
                goto fail;
            failure_detail =
                XG_RENDER_WORLD_MODELS_PREPARE_FAILURE_OUTPUT_COUNTER;
            if (output->guest_ordering_table_written &&
                !output->counter_incremented)
                goto fail;
            failure_detail =
                XG_RENDER_WORLD_MODELS_PREPARE_FAILURE_PACKET_RANGE;
            if (!guest_data_range_is_valid(
                    source->packet_address,
                    (uint32_t)source->packet_word_count * 4u, 4u, false))
                goto fail;
            workspace->templates[primitive_index] = template_entry;
            allowed_mask =
                (UINT32_C(1) << source->packet_word_count) - 1u;
            failure_detail = XG_RENDER_WORLD_MODELS_PREPARE_FAILURE_OUTPUT_MASK;
            if ((output->packet_word_write_mask & ~allowed_mask) != 0u)
                goto fail;
            failure_detail =
                XG_RENDER_WORLD_MODELS_PREPARE_FAILURE_OUTPUT_ACCEPTED;
            if (output->accepted != output->ordering_table_written) goto fail;
            failure_detail = XG_RENDER_WORLD_MODELS_PREPARE_FAILURE_OUTPUT_TAG;
            if ((output->accepted || output->guest_ordering_table_written) &&
                (output->packet_word_write_mask & 1u) == 0u)
                goto fail;
            for (word = 0u; word < source->packet_word_count; ++word) {
                failure_detail =
                    XG_RENDER_WORLD_MODELS_PREPARE_FAILURE_PACKET_WORD;
                if (cpu->read_word(source->packet_address + word * 4u) !=
                    template_entry->words[word])
                    goto fail;
            }
            ++dispatch_output->processed_primitive_count;
            dispatch_output->emitted_count_delta +=
                output->counter_incremented ? 1u : 0u;
            dispatch_output->packet_cursor_masked |=
                output->packet_cursor_masked;
            dispatch_output->primitive_family_mask |=
                UINT32_C(1) << source->primitive_family;
            if (output->packet_word_write_mask != 0u)
                ++dispatch_output->packet_side_effect_primitive_count;
            if (output->accepted) {
                ++dispatch_output->accepted_primitive_count;
                ++accepted_count;
            }
            if (output->guest_ordering_table_written) {
                const uint32_t bucket = output->ordering_bucket;
                const uint32_t ot_address =
                    dispatch->call.ordering_table_address + bucket * 4u;

                failure_detail = XG_RENDER_WORLD_MODELS_PREPARE_FAILURE_OT_RANGE;
                if (bucket >= XG_WORLD_MODELS_OT_BUCKET_COUNT ||
                    !guest_data_range_is_valid(ot_address, 4u, 4u, false))
                    goto fail;
                if (!workspace->ot_touched[bucket]) {
                    workspace->ot_heads[bucket] = cpu->read_word(ot_address);
                    workspace->ot_touched[bucket] = true;
                }
            }
        }
        dispatch_output->packet_cursor_after = dispatch->packet_cursor_after;
        if (dispatch_output->packet_cursor_masked)
            dispatch_output->packet_cursor_after &= UINT32_C(0x00ffffff);
        dispatch_output->group_cursor_after = dispatch->group_cursor_after;
    }
    failure_detail = XG_RENDER_WORLD_MODELS_PREPARE_FAILURE_PRIMITIVE_TOTAL;
    if (primitive_index != preparation.primitive_count ||
        !native_snapshot_can_record(&world_models_native_snapshot,
                                    accepted_count))
        goto fail;

    commit.entry_side_effects = preparation.world.entry_side_effects;
    commit.completed_dispatch_count = preparation.dispatch_count;
    for (dispatch_index = 0u;
         dispatch_index < preparation.dispatch_count; ++dispatch_index) {
        const XgWorldModelsNativeDispatch *dispatch =
            &workspace->dispatches[dispatch_index];
        const XgWorldModelsNativeDispatchOutput *output =
            &workspace->dispatch_outputs[dispatch_index];

        if (!dispatch->bounds_accepted && !dispatch->guest_bounds_accepted)
            continue;
        failure_detail = XG_RENDER_WORLD_MODELS_PREPARE_FAILURE_COMMIT_OVERFLOW;
        if ((dispatch->guest_bounds_accepted &&
             commit.resident_vertex_total > UINT32_MAX -
                 dispatch->model.vertex_count) ||
            commit.resident_emitted_count > UINT32_MAX -
                output->emitted_count_delta ||
            commit.processed_primitive_count > UINT32_MAX -
                output->processed_primitive_count ||
            commit.accepted_primitive_count > UINT32_MAX -
                output->accepted_primitive_count)
            goto fail;
        if (dispatch->guest_bounds_accepted)
            commit.resident_vertex_total += dispatch->model.vertex_count;
        commit.resident_emitted_count += output->emitted_count_delta;
        commit.processed_primitive_count +=
            output->processed_primitive_count;
        commit.accepted_primitive_count += output->accepted_primitive_count;
        if (dispatch->guest_bounds_accepted) {
            commit.resident_packet_cursor = output->packet_cursor_after;
            commit.resident_group_cursor = output->group_cursor_after;
            commit.resident_model_0c = dispatch->model.auxiliary_vertex_base;
            commit.resident_vertex_base = dispatch->model.vertex_base;
            commit.resident_ot_base = dispatch->call.ordering_table_address;
            commit.resident_model_18 = dispatch->model.model_18;
            commit.resident_dispatch_globals_written = true;
        }
    }
    failure_detail = XG_RENDER_WORLD_MODELS_PREPARE_FAILURE_COMMIT_TOTAL;
    if (commit.processed_primitive_count != preparation.primitive_count ||
        commit.accepted_primitive_count != accepted_count)
        goto fail;

    for (primitive_index = 0u;
         primitive_index < preparation.primitive_count; ++primitive_index) {
        const XgWorldModelsNativePrimitiveSource *source =
            &workspace->primitives[primitive_index];
        XgWorldModelsNativePrimitiveOutput *output =
            &workspace->outputs[primitive_index];
        XgRenderWorldModelTemplate *template_entry =
            workspace->templates[primitive_index];

        if (output->guest_ordering_table_written) {
            const XgWorldModelsNativeDispatch *dispatch =
                &workspace->dispatches[source->dispatch_index];
            const uint32_t bucket = output->ordering_bucket;
            const uint32_t ot_address =
                dispatch->call.ordering_table_address + bucket * 4u;
            const uint32_t previous_head = workspace->ot_heads[bucket];

            output->packet_words[0] =
                (output->packet_words[0] & UINT32_C(0xff000000)) |
                (previous_head & UINT32_C(0x00ffffff));
            workspace->ot_heads[bucket] =
                (previous_head & UINT32_C(0xff000000)) |
                (source->packet_address & UINT32_C(0x00ffffff));
            (void)ot_address;
        }
    }
    commit.authentication_generation = generation;
    commit.continuation_pc = preparation.continuation_pc;
    commit.entry_side_effects.resident_vertex_total =
        commit.resident_vertex_total;
    commit.entry_side_effects.resident_emitted_count =
        commit.resident_emitted_count;
    commit.authenticated = true;
    commit.sealed = true;
    workspace->preparation = preparation;
    workspace->expected_commit = commit;
    workspace->authentication_generation = generation;
    workspace->entry_stack_pointer = cpu->gpr[29];
    workspace->accepted_count = accepted_count;
    workspace->owner_cpu = cpu;
    workspace->valid = true;
    return true;

fail:
    native_snapshot_record_failure(
        &world_models_native_snapshot,
        PSX_XG_RENDER_WORLD_NATIVE_FAILURE_PREPARE, failure_detail, 0u);
    invalidate_world_model_templates();
    return false;
}

static bool native_world_models_commit(CPUState *cpu) {
    XgRenderWorldModelsNativeState *workspace = &world_models_native_state;
    XgWorldModelsNativePreparation *preparation = &workspace->preparation;
    XgWorldModelsNativeCommit commit;
    const XgWorldModelsNativeCommit *expected = &workspace->expected_commit;
    XgWorldModelsNativeResult native_result;
    GpuRenderTransactionStatus transaction_status;
    uint64_t generation;
    uint32_t anchor_count = 0u;
    uint32_t failure_detail = 1u;
    PsxXgRenderWorldNativeFailureStage failure_stage =
        PSX_XG_RENDER_WORLD_NATIVE_FAILURE_COMMIT_PRECONDITION;
    uint32_t primitive_index;
    uint32_t index;

    if (!workspace->valid || workspace->owner_cpu != cpu) {
        goto fail_commit;
    }
    failure_detail = 2u;
    if (!native_world_cutover_ready()) {
        goto fail_commit;
    }
    failure_detail = 3u;
    if (!world_authentication_generation(&generation)) {
        goto fail_commit;
    }
    failure_detail = 4u;
    if (generation != workspace->authentication_generation) {
        goto fail_commit;
    }
    failure_detail = 5u;
    if (cpu == NULL || cpu->read_word == NULL || cpu->read_half == NULL) {
        goto fail_commit;
    }
    failure_detail = 6u;
    if (workspace->entry_stack_pointer < 0x40u) {
        goto fail_commit;
    }
    failure_detail = 7u;
    if (cpu->gpr[29] != workspace->entry_stack_pointer - 0x40u) {
        goto fail_commit;
    }
    failure_detail = 8u;
    if (!physical_address_equals(cpu->read_word(cpu->gpr[29] + 0x38u),
                                 expected->continuation_pc)) {
        goto fail_commit;
    }
    failure_detail = 9u;
    if (cpu->read_word(XG_WORLD_MODELS_SCALE_X_SCRATCH) !=
        expected->entry_side_effects.scratch_scale[0]) {
        goto fail_commit;
    }
    if (cpu->read_word(XG_WORLD_MODELS_SCALE_Y_SCRATCH) !=
        expected->entry_side_effects.scratch_scale[1]) {
        goto fail_commit;
    }
    if (cpu->read_word(XG_WORLD_MODELS_SCALE_Z_SCRATCH) !=
        expected->entry_side_effects.scratch_scale[2]) {
        goto fail_commit;
    }
    if (cpu->read_half(XG_WORLD_MODELS_COARSE_ORIGIN_SCRATCH) !=
        (uint16_t)expected->entry_side_effects.coarse_origin[0]) {
        goto fail_commit;
    }
    if (cpu->read_half(XG_WORLD_MODELS_COARSE_ORIGIN_SCRATCH + 2u) !=
        (uint16_t)expected->entry_side_effects.coarse_origin[1]) {
        goto fail_commit;
    }
    if (cpu->read_half(XG_WORLD_MODELS_COARSE_ORIGIN_SCRATCH + 4u) !=
        (uint16_t)expected->entry_side_effects.coarse_origin[2]) {
        goto fail_commit;
    }
    if (cpu->read_word(XG_WORLD_MODELS_RESIDENT_CULL_MODE_GLOBAL) !=
        expected->entry_side_effects.resident_cull_mode) {
        goto fail_commit;
    }
    if (cpu->read_word(XG_WORLD_MODELS_RESIDENT_VERTEX_TOTAL_GLOBAL) !=
        expected->resident_vertex_total) {
        goto fail_commit;
    }
    if (cpu->read_word(XG_WORLD_MODELS_RESIDENT_EMITTED_COUNT_GLOBAL) !=
        expected->resident_emitted_count) {
        goto fail_commit;
    }

    for (index = 0u; index < preparation->transform_node_count; ++index) {
        const XgWorldModelsNodeSideEffect *effect =
            &workspace->node_side_effects[index];
        uint32_t component;

        for (component = 0u; component < 3u; ++component) {
            if (cpu->read_word(
                    effect->guest_address +
                        XG_WORLD_MODELS_NODE_WRITEBACK_X_OFFSET +
                        component * 4u) !=
                (uint32_t)effect->translation[component])
                goto fail_commit;
        }
    }
    for (primitive_index = 0u;
         primitive_index < preparation->primitive_count; ++primitive_index) {
        const XgWorldModelsNativePrimitiveSource *source =
            &workspace->primitives[primitive_index];
        const XgWorldModelsNativePrimitiveOutput *output =
            &workspace->outputs[primitive_index];
        const XgRenderWorldModelTemplate *template_entry =
            workspace->templates[primitive_index];
        uint32_t word;

        if (template_entry == NULL || !template_entry->active ||
            template_entry->owner_cpu != cpu ||
            template_entry->resource_epoch != source->resource_epoch)
            goto fail_commit;
        for (word = 0u; word < source->packet_word_count; ++word) {
            const uint32_t expected_word =
                (output->guest_packet_word_write_mask &
                 (UINT32_C(1) << word)) != 0u
                    ? output->packet_words[word]
                    : template_entry->words[word];

            if (cpu->read_word(source->packet_address + word * 4u) !=
                expected_word)
                goto fail_commit;
        }
    }
    for (index = 0u; index < XG_WORLD_MODELS_OT_BUCKET_COUNT; ++index) {
        if (workspace->ot_touched[index] &&
            cpu->read_word(expected->resident_ot_base + index * 4u) !=
            workspace->ot_heads[index])
            goto fail_commit;
    }
    if (expected->resident_dispatch_globals_written &&
        (cpu->read_word(XG_WORLD_MODELS_RESIDENT_PACKET_CURSOR_GLOBAL) !=
             expected->resident_packet_cursor ||
         cpu->read_word(XG_WORLD_MODELS_RESIDENT_GROUP_CURSOR_GLOBAL) !=
             expected->resident_group_cursor ||
         cpu->read_word(XG_WORLD_MODELS_RESIDENT_MODEL_0C_GLOBAL) !=
             expected->resident_model_0c ||
         cpu->read_word(XG_WORLD_MODELS_RESIDENT_VERTEX_BASE_GLOBAL) !=
             expected->resident_vertex_base ||
         cpu->read_word(XG_WORLD_MODELS_RESIDENT_OT_BASE_GLOBAL) !=
              expected->resident_ot_base ||
         cpu->read_word(XG_WORLD_MODELS_RESIDENT_MODEL_18_GLOBAL) !=
              expected->resident_model_18))
        goto fail_commit;

    if (!world_models_collect_interpolation_anchors(
            workspace, interpolation_scene_generation(), &anchor_count,
            &failure_detail)) {
        failure_stage = PSX_XG_RENDER_WORLD_NATIVE_FAILURE_ANCHOR_COLLECTION;
        goto fail_commit;
    }
    native_result = xg_world_models_native_finalize(
            preparation, generation, workspace->dispatches,
            workspace->dispatch_outputs, preparation->dispatch_count,
            workspace->primitives, workspace->outputs,
            preparation->primitive_count, workspace,
            world_models_begin_submission, world_models_stage_primitive,
            &commit);
    if (native_result != XG_WORLD_MODELS_NATIVE_OK ||
        !commit.authenticated || !commit.sealed ||
        commit.authentication_generation != generation ||
         commit.accepted_primitive_count != workspace->accepted_count ||
         commit.resident_vertex_total != expected->resident_vertex_total ||
         commit.resident_emitted_count != expected->resident_emitted_count) {
        failure_stage = PSX_XG_RENDER_WORLD_NATIVE_FAILURE_FINALIZE;
        failure_detail = (uint32_t)native_result;
        goto fail_commit;
    }
    for (primitive_index = 0u;
         primitive_index < preparation->primitive_count; ++primitive_index) {
        const XgWorldModelsNativePrimitiveSource *source =
            &workspace->primitives[primitive_index];
        const XgWorldModelsNativePrimitiveOutput *output =
            &workspace->outputs[primitive_index];
        const uint8_t mode = source->dispatch_mode;
        const int32_t margin = render_screen_x_cull_margin();
        const uint32_t interpolation_primitive_id =
            source->source_index * 4096u + source->primitive_index;
        const bool average = source->primitive_family == 16u ||
            mode == 0u || mode == 4u;
        const GpuRenderTemporalCullPolicy policy = {
            .flags = GPU_RENDER_TEMPORAL_CULL_PROJECTIVE |
                GPU_RENDER_TEMPORAL_CULL_SCREEN |
                GPU_RENDER_TEMPORAL_CULL_FRONT_FACE |
                GPU_RENDER_TEMPORAL_CULL_DEPTH,
            .screen_left = margin > 0
                ? -margin * INT32_C(65536) : INT32_MIN,
            .screen_top = 0,
            .screen_right_exclusive =
                ((int32_t)preparation->screen_right + margin) *
                    INT32_C(65536),
            .screen_bottom_exclusive =
                (int32_t)(preparation->packed_screen_bottom >> 16u) *
                    INT32_C(65536),
            .depth_min_inclusive = 1,
            .depth_max_exclusive = 0x10000,
            .depth_mode = average ? GPU_RENDER_TEMPORAL_DEPTH_AVERAGE :
                ((mode == 2u || mode == 5u)
                    ? GPU_RENDER_TEMPORAL_DEPTH_MAXIMUM
                    : GPU_RENDER_TEMPORAL_DEPTH_MINIMUM),
            .front_face = GPU_RENDER_TEMPORAL_FRONT_POSITIVE,
            .ordering_depth_shift = (uint8_t)(average
                ? preparation->ordering_shift
                : ((preparation->ordering_shift + 2u) & 31u)),
        };

        if (output->accepted || output->primitive.triangle_count == 0u)
            continue;
        if (!stage_temporal_native_primitive_identified(
                &output->primitive,
                source->model_header_address & UINT32_C(0x1fffffff),
                interpolation_primitive_id, &policy)) {
            failure_stage = PSX_XG_RENDER_WORLD_NATIVE_FAILURE_FINALIZE;
            failure_detail = UINT32_C(0x1000) + primitive_index;
            goto fail_commit;
        }
    }
    transaction_status = anchor_count != 0u
        ? gr_record_interpolation_anchors(workspace->anchors, anchor_count)
        : GPU_RENDER_TRANSACTION_OK;
    if (transaction_status != GPU_RENDER_TRANSACTION_OK) {
        failure_stage = PSX_XG_RENDER_WORLD_NATIVE_FAILURE_ANCHOR_RECORD;
        failure_detail = (uint32_t)transaction_status;
        goto fail_commit;
    }

    for (primitive_index = 0u;
         primitive_index < preparation->primitive_count; ++primitive_index) {
        const XgWorldModelsNativePrimitiveSource *source =
            &workspace->primitives[primitive_index];
        XgRenderWorldModelTemplate *template_entry =
            workspace->templates[primitive_index];
        uint32_t word;

        for (word = 0u; word < source->packet_word_count; ++word) {
            if ((workspace->outputs[primitive_index].guest_packet_word_write_mask &
                 (UINT32_C(1) << word)) != 0u)
                template_entry->words[word] =
                    workspace->outputs[primitive_index].packet_words[word];
        }
    }
    native_snapshot_record(
        &world_models_native_snapshot, workspace->accepted_count);
    world_models_native_snapshot.last_anchor_count = anchor_count;
    clear_world_models_native_pending();
    return true;

fail_commit:
    native_snapshot_record_failure(
        &world_models_native_snapshot, failure_stage, failure_detail,
        anchor_count);
    abort_standalone_submission();
    clear_world_models_native_pending();
    invalidate_world_model_templates();
    return false;
}

static bool native_world_sky_cutover(CPUState *cpu) {
    enum {
        WORLD_SKY_VERTEX_BASE = 0x8009a280u,
        WORLD_SKY_CAMERA_MATRIX = 0x8009c808u,
        WORLD_SKY_ANGLE = 0x8009bd3au,
        WORLD_SKY_PROJECTION_DISTANCE = 0x8009bcdcu,
        WORLD_SKY_SCREEN_OFFSET_Y = 0x8009be0cu,
        WORLD_SKY_CONTEXT = 0x8009be3cu,
        WORLD_SKY_BUFFER_INDEX = 0x8009d7f0u,
        WORLD_SKY_FOG_MODE = 0x8009d7ccu,
        WORLD_SKY_ORDERING_SHIFT = 0x80050100u,
        WORLD_SKY_OT_BUCKET_COUNT = 0x1000u,
    };
    GuestRenderBridgeSnapshot bridge = { 0 };
    XgWorldSkySource source = { 0 };
    XgWorldSkyRecord records[XG_WORLD_SKY_QUAD_COUNT];
    XgHost3dMatrix camera;
    XgHost3dMatrix local = { 0 };
    XgHost3dMatrix composed;
    GpuDrawState draw = { 0 };
    uint32_t packet_tags[XG_WORLD_SKY_QUAD_COUNT] = { 0 };
    uint32_t ot_addresses[XG_WORLD_SKY_QUAD_COUNT] = { 0 };
    uint32_t angle_word;
    uint32_t context;
    uint32_t ot_base;
    uint32_t projection_distance;
    uint32_t accepted_count = 0u;
    uint32_t quad;
    uint32_t vertex;

    if (state.requested_render_mode != GUEST_RENDER_RENDER_NATIVE ||
        guest_render_bridge_snapshot(&bridge) != GUEST_RENDER_OK ||
        (state.active && bridge.modes.effective_render_mode !=
             GUEST_RENDER_RENDER_NATIVE) ||
        (!state.active && !state.armed))
        return false;
    if (cpu == NULL || cpu->read_word == NULL || cpu->read_half == NULL ||
        cpu->write_word == NULL ||
        !physical_address_equals(cpu->gpr[31], UINT32_C(0x80071b60)) ||
        !capture_particle_matrix(cpu, WORLD_SKY_CAMERA_MATRIX, &camera))
        return false;

    angle_word = cpu->read_word(UINT32_C(0x800523f0) +
        ((uint32_t)cpu->read_half(WORLD_SKY_ANGLE) & 0xfffu) * 4u);
    local.rotation[0][0] = (int16_t)(angle_word >> 16u);
    local.rotation[0][2] = (int16_t)angle_word;
    local.rotation[1][1] = 0x1000;
    local.rotation[2][0] = (int16_t)-(int32_t)(int16_t)angle_word;
    local.rotation[2][2] = (int16_t)(angle_word >> 16u);
    if (!xg_host_3d_comp_matrix(&camera, &local, &composed)) return false;

    memcpy(source.projection.rotation, composed.rotation,
           sizeof(source.projection.rotation));
    memcpy(source.projection.translation, composed.translation,
           sizeof(source.projection.translation));
    source.projection.screen_offset_x = 160 << 16;
    source.projection.screen_offset_y = (int32_t)(
        cpu->read_word(WORLD_SKY_SCREEN_OFFSET_Y) << 16u);
    projection_distance = cpu->read_word(WORLD_SKY_PROJECTION_DISTANCE);
    if (projection_distance == 0u || projection_distance > UINT16_MAX ||
        !world_sky_compute_fog(cpu->read_word(WORLD_SKY_FOG_MODE),
                               (int32_t)projection_distance,
                               &source.projection.depth_cue_a,
                               &source.projection.depth_cue_b))
        return false;
    source.projection.projection_distance = (uint16_t)projection_distance;
    source.ordering_shift = cpu->read_word(WORLD_SKY_ORDERING_SHIFT);
    source.buffer_index = cpu->read_word(WORLD_SKY_BUFFER_INDEX);
    gpu_get_draw_state(&draw);
    apply_draw_state(&source.material, &draw);
    source.material.shading = XG_RENDER_IR_SHADING_GOURAUD;

    for (quad = 0u; quad < XG_WORLD_SKY_QUAD_COUNT; ++quad) {
        for (vertex = 0u; vertex < XG_HOST_3D_VERTEX_COUNT; ++vertex) {
            const uint32_t address = WORLD_SKY_VERTEX_BASE +
                (quad * XG_HOST_3D_VERTEX_COUNT + vertex) * 8u;
            const uint32_t xy = cpu->read_word(address);
            const uint32_t zp = cpu->read_word(address + 4u);

            source.vertices[quad][vertex] = (XgHost3dVector){
                low_s16(xy), low_s16(xy >> 16u), low_s16(zp),
                (uint16_t)(zp >> 16u),
            };
        }
    }
    if (xg_world_sky_build(&source, records) != XG_WORLD_SKY_OK) return false;

    context = cpu->read_word(WORLD_SKY_CONTEXT);
    if (context > UINT32_MAX - 0x70u ||
        !word_address_is_valid(context + 0x70u))
        return false;
    ot_base = cpu->read_word(context + 0x70u);
    for (quad = 0u; quad < XG_WORLD_SKY_QUAD_COUNT; ++quad) {
        if (!records[quad].accepted) continue;
        ++accepted_count;
        if (records[quad].ordering_bucket >= WORLD_SKY_OT_BUCKET_COUNT ||
            ot_base > UINT32_MAX - records[quad].ordering_bucket * 4u ||
            !word_address_is_valid(records[quad].packet_address) ||
            !word_address_is_valid(records[quad].packet_address + 32u))
            return false;
        ot_addresses[quad] =
            ot_base + records[quad].ordering_bucket * 4u;
        if (!word_address_is_valid(ot_addresses[quad])) return false;
        packet_tags[quad] = cpu->read_word(records[quad].packet_address);
        if ((packet_tags[quad] >> 24u) != 8u) return false;
    }
    if (!native_snapshot_can_record(
            &world_sky_native_snapshot, accepted_count))
        return false;
    for (quad = 0u; quad < XG_WORLD_SKY_QUAD_COUNT; ++quad) {
        if (records[quad].accepted) {
            if (!stage_standalone_native_primitive_identified(
                &records[quad].primitive, records[quad].packet_address,
                UINT32_C(0x50000000) |
                    (records[quad].packet_address & UINT32_C(0x001ffffc)),
                UINT32_C(0x800737ec), quad)) {
                abort_standalone_submission();
                return false;
            }
        } else {
            const GpuRenderTemporalCullPolicy policy = {
                .flags = GPU_RENDER_TEMPORAL_CULL_PROJECTIVE,
            };

            if (!stage_temporal_native_primitive_identified(
                    &records[quad].primitive, UINT32_C(0x800737ec), quad,
                    &policy)) {
                abort_standalone_submission();
                return false;
            }
        }
    }
    native_snapshot_record(&world_sky_native_snapshot, accepted_count);

    psx_store_cycle_barrier();
    cpu->write_word(UINT32_C(0x1f800000),
                    (uint32_t)cpu->read_half(WORLD_SKY_ANGLE) << 16u);
    psx_store_cycle_barrier();
    cpu->write_word(UINT32_C(0x1f800004), 0u);
    world_sky_store_matrix(cpu, UINT32_C(0x1f800008), &composed);
    world_sky_store_matrix(cpu, UINT32_C(0x1f800028), &local);
    for (quad = 0u; quad < XG_WORLD_SKY_QUAD_COUNT; ++quad) {
        uint32_t previous_head;

        if (!records[quad].accepted) continue;
        for (vertex = 0u; vertex < XG_HOST_3D_VERTEX_COUNT; ++vertex) {
            const uint32_t xy =
                (uint16_t)records[quad].vertices[vertex].x |
                ((uint32_t)(uint16_t)records[quad].vertices[vertex].y << 16u);

            psx_store_cycle_barrier();
            cpu->write_word(records[quad].packet_address + 8u + vertex * 8u,
                            xy);
        }
        previous_head = cpu->read_word(ot_addresses[quad]);
        psx_store_cycle_barrier();
        cpu->write_word(records[quad].packet_address,
            (packet_tags[quad] & UINT32_C(0xff000000)) |
            (previous_head & UINT32_C(0x00ffffff)));
        psx_store_cycle_barrier();
        cpu->write_word(ot_addresses[quad],
            (previous_head & UINT32_C(0xff000000)) |
            (records[quad].packet_address & UINT32_C(0x00ffffff)));
    }
    psx_store_cycle_barrier();
    cpu->write_word(UINT32_C(0x1f800048),
        records[XG_WORLD_SKY_QUAD_COUNT - 1u].vertices[3].z >> 2u);
    psx_store_cycle_barrier();
    cpu->write_word(UINT32_C(0x1f80004c),
        records[XG_WORLD_SKY_QUAD_COUNT - 1u].projection_flags);
    cpu->gpr[2] = records[XG_WORLD_SKY_QUAD_COUNT - 1u].vertices[3].z >> 2u;
    cpu->pc = cpu->gpr[31];
    return true;
}

static bool native_world_horizon_cutover(CPUState *cpu) {
    enum {
        HORIZON_CONTEXT = 0x8009be3cu,
        HORIZON_SET_WINDOW = 0x8009d3d8u,
        HORIZON_RESET_WINDOW = 0x8009d3e4u,
        HORIZON_OT_BUCKET_COUNT = 0x1000u,
    };
    GuestRenderBridgeSnapshot bridge = { 0 };
    XgWorldHorizonCapture capture;
    XgWorldHorizonRecord records[XG_WORLD_HORIZON_QUAD_COUNT];
    uint32_t packet_addresses[XG_WORLD_HORIZON_QUAD_COUNT];
    uint32_t packet_tags[XG_WORLD_HORIZON_QUAD_COUNT];
    uint32_t window_tags[2];
    uint16_t uv[4];
    uint32_t context;
    uint32_t ot_base;
    uint32_t ot_address;
    uint32_t initial_ot_word;
    uint32_t quad;
    uint32_t vertex;

    if (state.requested_render_mode != GUEST_RENDER_RENDER_NATIVE ||
        guest_render_bridge_snapshot(&bridge) != GUEST_RENDER_OK ||
        (state.active && bridge.modes.effective_render_mode !=
             GUEST_RENDER_RENDER_NATIVE) ||
        (!state.active && !state.armed))
        return false;
    if (cpu == NULL || cpu->read_word == NULL || cpu->read_half == NULL ||
        cpu->write_word == NULL ||
        !physical_address_equals(cpu->gpr[31], UINT32_C(0x80071b58)) ||
        world_horizon_capture_build(cpu, &capture, records) != 0u ||
        records[0].accepted != records[1].accepted ||
        records[0].ordering_bucket != records[1].ordering_bucket ||
        world_horizon_shadow.snapshot.native_cutover_count == UINT64_MAX ||
        (records[0].accepted &&
         world_horizon_shadow.snapshot.native_primitive_count >
             UINT64_MAX - XG_WORLD_HORIZON_QUAD_COUNT))
        return false;
    if (!records[0].accepted) {
        const GpuRenderTemporalCullPolicy policy = {
            .flags = GPU_RENDER_TEMPORAL_CULL_PROJECTIVE,
        };

        for (quad = 0u; quad < XG_WORLD_HORIZON_QUAD_COUNT; ++quad) {
            if (!stage_temporal_native_primitive_identified(
                    &records[quad].primitive, UINT32_C(0x80073b04), quad,
                    &policy)) {
                abort_standalone_submission();
                return false;
            }
        }
        ++world_horizon_shadow.snapshot.native_cutover_count;
        cpu->pc = cpu->gpr[31];
        return true;
    }
    if (records[0].ordering_bucket >= HORIZON_OT_BUCKET_COUNT)
        return false;

    for (quad = 0u; quad < XG_WORLD_HORIZON_QUAD_COUNT; ++quad) {
        packet_addresses[quad] = UINT32_C(0x8009c744) + quad * 0x50u +
            capture.buffer_index * 0x28u;
        if (!word_address_is_valid(packet_addresses[quad]) ||
            !word_address_is_valid(packet_addresses[quad] + 0x24u))
            return false;
        packet_tags[quad] = cpu->read_word(packet_addresses[quad]);
        if ((packet_tags[quad] >> 24u) != 9u) return false;
    }
    window_tags[0] = cpu->read_word(HORIZON_SET_WINDOW);
    window_tags[1] = cpu->read_word(HORIZON_RESET_WINDOW);
    if ((window_tags[0] >> 24u) != 2u ||
        (window_tags[1] >> 24u) != 2u ||
        cpu->read_word(HORIZON_SET_WINDOW + 4u) !=
            UINT32_C(0xe2000010) ||
        cpu->read_word(HORIZON_SET_WINDOW + 8u) != 0u ||
        cpu->read_word(HORIZON_RESET_WINDOW + 4u) !=
            UINT32_C(0xe2000000) ||
        cpu->read_word(HORIZON_RESET_WINDOW + 8u) != 0u)
        return false;

    context = cpu->read_word(HORIZON_CONTEXT);
    if (context > UINT32_MAX - 0x70u ||
        !word_address_is_valid(context + 0x70u))
        return false;
    ot_base = cpu->read_word(context + 0x70u);
    if (ot_base > UINT32_MAX - records[0].ordering_bucket * 4u)
        return false;
    ot_address = ot_base + records[0].ordering_bucket * 4u;
    if (!word_address_is_valid(ot_address)) return false;
    initial_ot_word = cpu->read_word(ot_address);

    for (quad = 0u; quad < XG_WORLD_HORIZON_QUAD_COUNT; ++quad) {
        if (!stage_standalone_native_primitive_identified(
                &records[quad].primitive, packet_addresses[quad],
                UINT32_C(0x61000000) | quad,
                UINT32_C(0x80073b04), quad)) {
            abort_standalone_submission();
            return false;
        }
    }

    uv[0] = (uint8_t)(capture.source.angle >> 2u) & 0x7fu;
    uv[1] = uv[0] | 0x80u;
    uv[2] = uv[0] | 0x3f00u;
    uv[3] = uv[1] | 0x3f00u;
    for (quad = 0u; quad < XG_WORLD_HORIZON_QUAD_COUNT; ++quad) {
        const uint32_t packet = packet_addresses[quad];

        psx_store_cycle_barrier();
        cpu->write_word(packet + 4u, UINT32_C(0x2e303030));
        for (vertex = 0u; vertex < XG_HOST_3D_VERTEX_COUNT; ++vertex) {
            const uint32_t xy = (uint16_t)records[quad].vertices[vertex].x |
                ((uint32_t)(uint16_t)records[quad].vertices[vertex].y << 16u);

            psx_store_cycle_barrier();
            cpu->write_word(packet + 8u + vertex * 8u, xy);
        }
        psx_store_cycle_barrier();
        cpu->write_word(packet + 12u,
                        (uint32_t)uv[0] | UINT32_C(0x7f910000));
        psx_store_cycle_barrier();
        cpu->write_word(packet + 20u,
                        (uint32_t)uv[1] | UINT32_C(0x003e0000));
        psx_store_cycle_barrier();
        cpu->write_word(packet + 28u, uv[2]);
        psx_store_cycle_barrier();
        cpu->write_word(packet + 36u, uv[3]);
    }

    psx_store_cycle_barrier();
    cpu->write_word(HORIZON_SET_WINDOW,
        (window_tags[0] & UINT32_C(0xff000000)) |
        (packet_addresses[1] & UINT32_C(0x00ffffff)));
    psx_store_cycle_barrier();
    cpu->write_word(packet_addresses[1],
        (packet_tags[1] & UINT32_C(0xff000000)) |
        (packet_addresses[0] & UINT32_C(0x00ffffff)));
    psx_store_cycle_barrier();
    cpu->write_word(packet_addresses[0],
        (packet_tags[0] & UINT32_C(0xff000000)) |
        (HORIZON_RESET_WINDOW & UINT32_C(0x00ffffff)));
    psx_store_cycle_barrier();
    cpu->write_word(HORIZON_RESET_WINDOW,
        (window_tags[1] & UINT32_C(0xff000000)) |
        (initial_ot_word & UINT32_C(0x00ffffff)));
    psx_store_cycle_barrier();
    cpu->write_word(ot_address,
        (initial_ot_word & UINT32_C(0xff000000)) |
        (HORIZON_SET_WINDOW & UINT32_C(0x00ffffff)));

    ++world_horizon_shadow.snapshot.native_cutover_count;
    world_horizon_shadow.snapshot.native_primitive_count +=
        XG_WORLD_HORIZON_QUAD_COUNT;
    cpu->pc = cpu->gpr[31];
    return true;
}

static bool native_world_effects_cutover(CPUState *cpu) {
    enum {
        EFFECTS_PACKET_BASES = 0x8009be1cu,
        EFFECTS_BUFFER_INDEX = 0x8009d7f0u,
        EFFECTS_CONTEXT = 0x8009be3cu,
        EFFECTS_PACKET_STRIDE = 0x28u,
        EFFECTS_CAPACITY = 0x100u,
        EFFECTS_OT_BUCKET_COUNT = 0xc0u,
    };
    GuestRenderBridgeSnapshot bridge = { 0 };
    XgWorldEffectsCapture capture;
    XgWorldEffectsRecord records[XG_WORLD_EFFECTS_SOURCE_CAPACITY];
    XgWorldEffectsRecord temporal_records[XG_WORLD_EFFECTS_SOURCE_CAPACITY];
    uint32_t packet_addresses[XG_WORLD_EFFECTS_SOURCE_CAPACITY];
    uint32_t packet_tags[XG_WORLD_EFFECTS_SOURCE_CAPACITY];
    uint32_t ot_addresses[XG_WORLD_EFFECTS_SOURCE_CAPACITY];
    uint32_t buffer_index;
    uint32_t packet_cursor;
    uint32_t context;
    uint32_t ot_base;
    uint32_t count;
    uint32_t temporal_count;
    uint32_t index;
    uint32_t vertex;

    if (state.requested_render_mode != GUEST_RENDER_RENDER_NATIVE ||
        guest_render_bridge_snapshot(&bridge) != GUEST_RENDER_OK ||
        (state.active && bridge.modes.effective_render_mode !=
             GUEST_RENDER_RENDER_NATIVE) ||
        (!state.active && !state.armed))
        return false;
    if (cpu == NULL || cpu->read_word == NULL || cpu->read_half == NULL ||
        cpu->write_word == NULL ||
        !physical_address_equals(cpu->gpr[31], UINT32_C(0x80071aa8)) ||
        world_effects_capture_build(
            cpu, &capture, records, &count, temporal_records,
            &temporal_count) != 0u)
        return false;

    buffer_index = cpu->read_word(EFFECTS_BUFFER_INDEX);
    if (buffer_index >= 2u) return false;
    packet_cursor = cpu->read_word(EFFECTS_PACKET_BASES + buffer_index * 4u);
    if (!word_address_is_valid(packet_cursor) ||
        packet_cursor > UINT32_MAX - EFFECTS_CAPACITY * EFFECTS_PACKET_STRIDE ||
        !word_address_is_valid(packet_cursor +
            EFFECTS_CAPACITY * EFFECTS_PACKET_STRIDE - 4u))
        return false;
    context = cpu->read_word(EFFECTS_CONTEXT);
    if (context > UINT32_MAX - 0x70u ||
        !word_address_is_valid(context + 0x70u))
        return false;
    ot_base = cpu->read_word(context + 0x70u);
    if (!word_address_is_valid(ot_base) ||
        ot_base > UINT32_MAX - (EFFECTS_OT_BUCKET_COUNT - 1u) * 4u)
        return false;
    if (world_effects_shadow.snapshot.native_cutover_count == UINT64_MAX ||
        world_effects_shadow.snapshot.native_primitive_count >
            UINT64_MAX - count)
        return false;

    /* Validate every target before staging or publishing any side effect. */
    for (index = 0u; index < count; ++index) {
        const uint32_t packet = packet_cursor + index * EFFECTS_PACKET_STRIDE;
        const uint32_t bucket = records[index].ordering_bucket;

        if (bucket >= EFFECTS_OT_BUCKET_COUNT ||
            !word_address_is_valid(packet) ||
            !word_address_is_valid(packet + EFFECTS_PACKET_STRIDE - 4u))
            return false;
        packet_addresses[index] = packet;
        packet_tags[index] = cpu->read_word(packet);
        if ((packet_tags[index] >> 24u) != 9u) return false;
        ot_addresses[index] = ot_base + bucket * 4u;
        if (!word_address_is_valid(ot_addresses[index])) return false;
    }
    for (index = 0u; index < count; ++index) {
        if (!stage_standalone_native_primitive_identified(
                &records[index].primitive, packet_addresses[index],
                UINT32_C(0x60000000) | records[index].source_index,
                UINT32_C(0x80089c78), records[index].source_index)) {
            abort_standalone_submission();
            return false;
        }
    }
    for (index = 0u; index < temporal_count; ++index) {
        const XgWorldEffectsRecord *record = &temporal_records[index];
        /* The guest projects these billboards before authoring their screen
         * quad. Keep screen-space interpolation instead of inventing a host
         * projective position merely to reproduce the GTE flag test. */
        const GpuRenderTemporalCullPolicy policy = {
            .flags = GPU_RENDER_TEMPORAL_CULL_SCREEN |
                GPU_RENDER_TEMPORAL_CULL_DEPTH,
            .screen_left = INT32_MIN,
            .screen_top = INT32_MIN,
            .screen_right_exclusive =
                (320 + capture.source.screen_x_cull_margin) * INT32_C(65536),
            .screen_bottom_exclusive = 216 * INT32_C(65536),
            .depth_min_inclusive = 0,
            .depth_max_exclusive = 0xc00,
            .depth_mode = GPU_RENDER_TEMPORAL_DEPTH_MAXIMUM,
            .ordering_depth_shift = 4u,
        };

        if (!stage_temporal_native_primitive_identified(
                &record->primitive, UINT32_C(0x80089c78),
                record->source_index, &policy)) {
            abort_standalone_submission();
            return false;
        }
    }

    for (index = 0u; index < count; ++index) {
        const XgWorldEffectsRecord *record = &records[index];
        const uint32_t packet = packet_addresses[index];
        uint32_t previous_head;

        psx_store_cycle_barrier();
        cpu->write_word(packet + 4u, record->material_word);
        for (vertex = 0u; vertex < XG_HOST_3D_VERTEX_COUNT; ++vertex) {
            const uint32_t xy = (uint16_t)record->vertices[vertex].x |
                ((uint32_t)(uint16_t)record->vertices[vertex].y << 16u);

            psx_store_cycle_barrier();
            cpu->write_word(packet + 8u + vertex * 8u, xy);
        }
        psx_store_cycle_barrier();
        cpu->write_word(packet + 12u,
            (uint32_t)record->uv[0] | ((uint32_t)record->clut << 16u));
        psx_store_cycle_barrier();
        cpu->write_word(packet + 20u,
            (uint32_t)record->uv[1] | ((uint32_t)record->tpage << 16u));
        psx_store_cycle_barrier();
        cpu->write_word(packet + 28u, record->uv[2]);
        psx_store_cycle_barrier();
        cpu->write_word(packet + 36u, record->uv[3]);

        previous_head = cpu->read_word(ot_addresses[index]);
        psx_store_cycle_barrier();
        cpu->write_word(packet,
            (packet_tags[index] & UINT32_C(0xff000000)) |
            (previous_head & UINT32_C(0x00ffffff)));
        psx_store_cycle_barrier();
        cpu->write_word(ot_addresses[index],
            (previous_head & UINT32_C(0xff000000)) |
            (packet & UINT32_C(0x00ffffff)));
    }
    ++world_effects_shadow.snapshot.native_cutover_count;
    world_effects_shadow.snapshot.native_primitive_count += count;
    cpu->pc = cpu->gpr[31];
    return true;
}

static void clear_world_clouds_native_pending(void) {
    if (!world_clouds_native_pending.valid &&
        !world_clouds_native_pending.poisoned)
        return;
    memset(&world_clouds_native_pending, 0,
           sizeof(world_clouds_native_pending));
}

static void poison_world_clouds_native_pending(void) {
    world_clouds_native_pending.valid = false;
    world_clouds_native_pending.poisoned = true;
    world_native_cutover_failed = true;
}

static void world_clouds_expected_packet(
    XgRenderWorldCloudsNativePending *pending, uint32_t index,
    const XgWorldCloudRecord *record, uint32_t tag) {
    uint32_t *words = pending->expected_packets[index];
    uint32_t vertex;

    words[0] = tag;
    words[1] = record->material_word;
    for (vertex = 0u; vertex < XG_HOST_3D_VERTEX_COUNT; ++vertex) {
        words[2u + vertex * 2u] = projected_xy(&record->vertices[vertex]);
        words[3u + vertex * 2u] =
            (words[3u + vertex * 2u] & UINT32_C(0xffff0000)) |
            record->uv[vertex];
    }
    words[3] = (words[3] & UINT32_C(0x0000ffff)) |
        ((uint32_t)record->clut << 16u);
    words[5] = (words[5] & UINT32_C(0x0000ffff)) |
        ((uint32_t)record->tpage << 16u);
}

static void native_world_clouds_prepare(CPUState *cpu) {
    enum {
        CLOUD_BUFFER_INDEX = 0x8009d7f0u,
        CLOUD_PACKET_BASES = 0x8009d7f8u,
        CLOUD_CONTEXT = 0x8009be3cu,
        CLOUD_CALLBACK_POINTER = 0x8009cd40u,
        CLOUD_CALLBACK_PHYSICAL = 0x00086700u,
        CLOUD_PACKET_STRIDE = 0x28u,
        CLOUD_POSITION_STRIDE = 0x10u,
    };
    XgRenderWorldCloudsNativePending *pending =
        &world_clouds_native_pending;
    XgWorldCloudsCapture capture;
    XgWorldCloudsBuildStats stats = {0};
    uint64_t generation;
    uint32_t context;
    uint32_t buffer_index;
    uint32_t index;
    uint32_t word;

    if (pending->depth != 0u) {
        if (pending->depth != UINT32_MAX) ++pending->depth;
        poison_world_clouds_native_pending();
        return;
    }
    clear_world_clouds_native_pending();
    pending->owner_cpu = cpu;
    pending->depth = 1u;
    if (!native_world_cutover_ready() ||
        !world_authentication_generation(&generation)) {
        poison_world_clouds_native_pending();
        return;
    }
    if (cpu == NULL || cpu->read_word == NULL || cpu->read_half == NULL ||
        !physical_address_equals(
            cpu->gpr[31], XG_WORLD_CLOUDS_SHADOW_FULL_RETURN) ||
        !physical_address_equals(
            cpu->read_word(CLOUD_CALLBACK_POINTER), CLOUD_CALLBACK_PHYSICAL) ||
        world_clouds_capture_build(
            cpu, &capture, pending->records, &pending->record_count,
            pending->stepped_positions, &stats, pending->temporal_records,
            &pending->temporal_record_count) != 0u) {
        poison_world_clouds_native_pending();
        return;
    }

    buffer_index = cpu->read_word(CLOUD_BUFFER_INDEX);
    if (buffer_index >= 2u) {
        poison_world_clouds_native_pending();
        return;
    }
    pending->packet_base =
        cpu->read_word(CLOUD_PACKET_BASES + buffer_index * 4u);
    if (!guest_data_range_is_valid(
            pending->packet_base,
            XG_WORLD_CLOUD_PACKET_CAPACITY * CLOUD_PACKET_STRIDE, 4u,
            false) ||
        !guest_data_range_is_valid(
            capture.position_array_address,
            XG_WORLD_CLOUD_COUNT * CLOUD_POSITION_STRIDE, 4u, false) ||
        cpu->gpr[29] < 0x50u ||
        !guest_data_range_is_valid(cpu->gpr[29] - 0x50u, 0x50u, 4u,
                                   false)) {
        poison_world_clouds_native_pending();
        return;
    }
    context = cpu->read_word(CLOUD_CONTEXT);
    if (context > UINT32_MAX - 0x70u ||
        !word_address_is_valid(context + 0x70u)) {
        poison_world_clouds_native_pending();
        return;
    }
    pending->ot_base = cpu->read_word(context + 0x70u);
    if (!guest_data_range_is_valid(
            pending->ot_base, XG_WORLD_CLOUD_OT_BUCKET_COUNT * 4u, 4u,
            false)) {
        poison_world_clouds_native_pending();
        return;
    }

    pending->authentication_generation = generation;
    pending->entry_stack_pointer = cpu->gpr[29];
    pending->position_base = capture.position_array_address;
    pending->expected_attempts =
        stats.quad_attempt_count - stats.far_preinsert_depth_stops -
        stats.far_postinsert_depth_stops;
    for (index = 0u; index < XG_WORLD_CLOUD_OT_BUCKET_COUNT; ++index)
        pending->expected_ot[index] =
            cpu->read_word(pending->ot_base + index * 4u);
    for (index = 0u; index < XG_WORLD_CLOUD_PACKET_CAPACITY; ++index) {
        const uint32_t packet =
            pending->packet_base + index * CLOUD_PACKET_STRIDE;

        for (word = 0u;
             word < XG_WORLD_CLOUDS_SHADOW_PACKET_WORD_COUNT; ++word)
            pending->expected_packets[index][word] =
                cpu->read_word(packet + word * 4u);
    }
    for (index = 0u; index < pending->record_count; ++index) {
        const XgWorldCloudRecord *record = &pending->records[index];
        const uint32_t packet =
            pending->packet_base + index * CLOUD_PACKET_STRIDE;
        const uint32_t bucket = record->ordering_bucket;

        if (bucket >= XG_WORLD_CLOUD_OT_BUCKET_COUNT ||
            !word_address_is_valid(packet) ||
            !word_address_is_valid(packet + CLOUD_PACKET_STRIDE - 4u)) {
            poison_world_clouds_native_pending();
            return;
        }
        if ((pending->expected_packets[index][0] >> 24u) != 9u) {
            poison_world_clouds_native_pending();
            return;
        }
        world_clouds_expected_packet(
            pending, index, record,
            UINT32_C(0x09000000) |
                (pending->expected_ot[bucket] & UINT32_C(0x00ffffff)));
        pending->expected_ot[bucket] = packet & UINT32_C(0x00ffffff);
    }
    for (index = 0u; index < XG_WORLD_CLOUD_COUNT; ++index) {
        const uint32_t address = capture.position_array_address +
            index * CLOUD_POSITION_STRIDE;

        if (!word_address_is_valid(address) ||
            !word_address_is_valid(address + 8u)) {
            poison_world_clouds_native_pending();
            return;
        }
    }
    pending->valid = true;
}

static void native_world_clouds_commit(CPUState *cpu) {
    enum {
        CLOUD_PACKET_STRIDE = 0x28u,
        CLOUD_POSITION_STRIDE = 0x10u,
        CLOUD_STACK_FRAME_SIZE = 0x50u,
        CLOUD_SAVED_RA_OFFSET = 0x4cu,
        CLOUD_SCRATCH_EMITTED = 0x1f8002f0u,
        CLOUD_SCRATCH_ATTEMPTS = 0x1f8002f4u,
    };
    XgRenderWorldCloudsNativePending *pending =
        &world_clouds_native_pending;
    XgWorldCloudsShadowSnapshot snapshot;
    uint64_t generation;
    uint32_t index;
    uint32_t word;

    if (pending->depth == 0u) {
        poison_world_clouds_native_pending();
        clear_world_clouds_native_pending();
        return;
    }
    if (pending->depth > 1u) {
        if (cpu == NULL || pending->owner_cpu != cpu ||
            !world_authentication_generation(&generation) ||
            pending->authentication_generation != generation)
            poison_world_clouds_native_pending();
        --pending->depth;
        return;
    }
    if (pending->poisoned || !pending->valid ||
        pending->owner_cpu != cpu || !native_world_cutover_ready() ||
        !world_authentication_generation(&generation) ||
        cpu == NULL || cpu->read_word == NULL ||
        pending->authentication_generation != generation ||
        pending->entry_stack_pointer < CLOUD_STACK_FRAME_SIZE ||
        cpu->gpr[29] !=
            pending->entry_stack_pointer - CLOUD_STACK_FRAME_SIZE ||
        !guest_data_range_is_valid(
            cpu->gpr[29], CLOUD_STACK_FRAME_SIZE, 4u, false) ||
        !guest_data_range_is_valid(
            pending->packet_base,
            XG_WORLD_CLOUD_PACKET_CAPACITY * CLOUD_PACKET_STRIDE, 4u,
            false) ||
        !guest_data_range_is_valid(
            pending->ot_base, XG_WORLD_CLOUD_OT_BUCKET_COUNT * 4u, 4u,
            false) ||
        !guest_data_range_is_valid(
            pending->position_base,
            XG_WORLD_CLOUD_COUNT * CLOUD_POSITION_STRIDE, 4u, false) ||
        !physical_address_equals(
            cpu->read_word(cpu->gpr[29] + CLOUD_SAVED_RA_OFFSET),
            XG_WORLD_CLOUDS_SHADOW_FULL_RETURN) ||
        cpu->gpr[11] != pending->packet_base +
            pending->record_count * CLOUD_PACKET_STRIDE ||
        cpu->read_word(CLOUD_SCRATCH_EMITTED) != pending->record_count ||
         cpu->read_word(CLOUD_SCRATCH_ATTEMPTS) != pending->expected_attempts) {
        poison_world_clouds_native_pending();
        clear_world_clouds_native_pending();
        return;
    }
    for (index = 0u; index < XG_WORLD_CLOUD_COUNT; ++index) {
        const uint32_t address =
            pending->position_base + index * CLOUD_POSITION_STRIDE;

        if (cpu->read_word(address) !=
                (uint32_t)pending->stepped_positions[index].x ||
            cpu->read_word(address + 4u) !=
                (uint32_t)pending->stepped_positions[index].y ||
            cpu->read_word(address + 8u) !=
                (uint32_t)pending->stepped_positions[index].z) {
            poison_world_clouds_native_pending();
            clear_world_clouds_native_pending();
            return;
        }
    }
    for (index = 0u; index < XG_WORLD_CLOUD_PACKET_CAPACITY; ++index) {
        const uint32_t packet =
            pending->packet_base + index * CLOUD_PACKET_STRIDE;

        for (word = 0u;
             word < XG_WORLD_CLOUDS_SHADOW_PACKET_WORD_COUNT; ++word) {
            if (cpu->read_word(packet + word * 4u) !=
                    pending->expected_packets[index][word]) {
                poison_world_clouds_native_pending();
                clear_world_clouds_native_pending();
                return;
            }
        }
    }
    for (index = 0u; index < XG_WORLD_CLOUD_OT_BUCKET_COUNT; ++index) {
        if (cpu->read_word(pending->ot_base + index * 4u) !=
                pending->expected_ot[index]) {
            poison_world_clouds_native_pending();
            clear_world_clouds_native_pending();
            return;
        }
    }

    xg_world_clouds_shadow_snapshot(&snapshot);
    if (snapshot.native_cutover_count == UINT64_MAX ||
        snapshot.native_primitive_count >
            UINT64_MAX - pending->record_count) {
        poison_world_clouds_native_pending();
        clear_world_clouds_native_pending();
        return;
    }
    if (world_native_cutover_in_progress) {
        poison_world_clouds_native_pending();
        clear_world_clouds_native_pending();
        return;
    }
    if (pending->record_count != 0u && !begin_standalone_submission()) {
        poison_world_clouds_native_pending();
        clear_world_clouds_native_pending();
        return;
    }
    world_native_cutover_in_progress = true;
    for (index = 0u; index < pending->record_count; ++index) {
        const XgWorldCloudRecord *record = &pending->records[index];
        const uint32_t packet =
            pending->packet_base + index * CLOUD_PACKET_STRIDE;
        const uint32_t interpolation_primitive_id =
            ((record->source_index * 3u + (uint32_t)record->lod) * 48u) +
            record->lod_quad_index;

        if (!stage_standalone_native_primitive_identified(
                &record->primitive, packet,
                UINT32_C(0x62000000) | index,
                UINT32_C(0x80086798), interpolation_primitive_id)) {
            abort_standalone_submission();
            world_native_cutover_in_progress = false;
            poison_world_clouds_native_pending();
            clear_world_clouds_native_pending();
            return;
        }
        if (world_native_cutover_failed) {
            world_native_cutover_in_progress = false;
            poison_world_clouds_native_pending();
            clear_world_clouds_native_pending();
            return;
        }
    }
    for (index = 0u; index < pending->temporal_record_count; ++index) {
        const XgWorldCloudRecord *record = &pending->temporal_records[index];
        const uint32_t interpolation_primitive_id =
            ((record->source_index * 3u + (uint32_t)record->lod) * 48u) +
            record->lod_quad_index;
        const int32_t margin = render_screen_x_cull_margin();
        const GpuRenderTemporalCullPolicy policy = {
            .flags = GPU_RENDER_TEMPORAL_CULL_PROJECTIVE |
                GPU_RENDER_TEMPORAL_CULL_SCREEN |
                GPU_RENDER_TEMPORAL_CULL_DEPTH,
            .screen_left = -margin * INT32_C(65536),
            .screen_top = 0,
            .screen_right_exclusive =
                (320 + margin) * INT32_C(65536),
            .screen_bottom_exclusive = 216 * INT32_C(65536),
            .depth_min_inclusive = 1,
            .depth_max_exclusive = 0x0d01,
            .depth_mode = GPU_RENDER_TEMPORAL_DEPTH_MAXIMUM,
            .ordering_depth_shift = 4u,
        };

        if (!stage_temporal_native_primitive_identified(
                &record->primitive, UINT32_C(0x80086798),
                interpolation_primitive_id, &policy)) {
            abort_standalone_submission();
            world_native_cutover_in_progress = false;
            poison_world_clouds_native_pending();
            clear_world_clouds_native_pending();
            return;
        }
    }
    world_native_cutover_in_progress = false;
    if (!xg_world_clouds_shadow_record_native_cutover(
            pending->record_count)) {
        abort_standalone_submission();
        poison_world_clouds_native_pending();
    }
    clear_world_clouds_native_pending();
}

static bool native_world_minimap_cutover(CPUState *cpu) {
    enum {
        MINIMAP_CALLER_RETURN = 0x80071b84u,
        MINIMAP_CONTINUATION = 0x80074298u,
        MINIMAP_CONTEXT = 0x8009be3cu,
        MINIMAP_CONTEXT_OT_OFFSET = 0x70u,
        MINIMAP_STACK_RA_OFFSET = 0x30u,
        MINIMAP_TRIANGLE_BASE = 0x8009a340u,
        MINIMAP_PACKET_BASE = 0x8009c664u,
        MINIMAP_PACKET_BUFFER_STRIDE = 0x70u,
        MINIMAP_PACKET_STRIDE = 0x1cu,
        MINIMAP_WORLD_POSITION = 0x8009d55cu,
    };
    GuestRenderBridgeSnapshot bridge = { 0 };
    XgWorldMinimapCapture capture;
    XgWorldMinimapBuildOutput output;
    XgWorldMinimapShadowSnapshot snapshot;
    GpuRenderSemantic semantics[XG_WORLD_MINIMAP_TRIANGLE_COUNT];
    uint32_t packet_tags[XG_WORLD_MINIMAP_TRIANGLE_COUNT];
    uint32_t scratch_words[10];
    uint32_t context;
    uint32_t ot_address;
    uint32_t initial_ot_word;
    uint32_t expected_packet_base;
    uint32_t triangle;
    uint32_t vertex;

    if (state.requested_render_mode != GUEST_RENDER_RENDER_NATIVE ||
        guest_render_bridge_snapshot(&bridge) != GUEST_RENDER_OK ||
        (state.active && bridge.modes.effective_render_mode !=
             GUEST_RENDER_RENDER_NATIVE) ||
        (!state.active && !state.armed))
        return false;
    if (cpu == NULL || cpu->read_word == NULL || cpu->read_half == NULL ||
        cpu->write_word == NULL ||
        !physical_address_equals(cpu->gpr[31], MINIMAP_CALLER_RETURN) ||
        !stack_address_is_valid(cpu->gpr[29]) ||
        cpu->gpr[29] > UINT32_MAX - MINIMAP_STACK_RA_OFFSET ||
        !word_address_is_valid(cpu->gpr[29] + MINIMAP_STACK_RA_OFFSET) ||
        !physical_address_equals(
            cpu->read_word(cpu->gpr[29] + MINIMAP_STACK_RA_OFFSET),
            MINIMAP_CALLER_RETURN) ||
        world_minimap_capture_build(cpu, &capture, &output) != 0u)
        return false;

    expected_packet_base = MINIMAP_PACKET_BASE +
        capture.source.buffer_index * MINIMAP_PACKET_BUFFER_STRIDE;
    if (!capture.authenticated || !capture.sealed ||
        output.ordering_count < XG_WORLD_MINIMAP_TRIANGLE_COUNT ||
        output.scratch.angle_address !=
            XG_WORLD_MINIMAP_SCRATCH_ANGLE_ADDRESS ||
        output.scratch.rotation_address !=
            XG_WORLD_MINIMAP_SCRATCH_ROTATION_ADDRESS ||
        output.scratch.translation_address !=
            XG_WORLD_MINIMAP_SCRATCH_TRANSLATION_ADDRESS ||
        cpu->gpr[2] != MINIMAP_PACKET_BASE ||
        cpu->gpr[3] !=
            capture.source.buffer_index * MINIMAP_PACKET_BUFFER_STRIDE ||
        cpu->gpr[16] != expected_packet_base || cpu->gpr[17] != 0u ||
        cpu->gpr[18] != MINIMAP_TRIANGLE_BASE ||
        cpu->gpr[19] != UINT32_C(0x1f800000) ||
        cpu->gpr[20] != UINT32_C(0x00ffffff) ||
        cpu->gpr[21] != XG_WORLD_MINIMAP_SCRATCH_ROTATION_ADDRESS ||
        cpu->gpr[22] != UINT32_C(0xff000000) ||
        cpu->gpr[23] != MINIMAP_WORLD_POSITION)
        return false;

    context = cpu->read_word(MINIMAP_CONTEXT);
    if (context > UINT32_MAX - MINIMAP_CONTEXT_OT_OFFSET ||
        !word_address_is_valid(context + MINIMAP_CONTEXT_OT_OFFSET))
        return false;
    ot_address = cpu->read_word(context + MINIMAP_CONTEXT_OT_OFFSET);
    if (!word_address_is_valid(ot_address)) return false;
    initial_ot_word = cpu->read_word(ot_address);

    xg_world_minimap_shadow_snapshot(&snapshot);
    if (snapshot.native_cutover_count == UINT64_MAX ||
        snapshot.native_primitive_count >
            UINT64_MAX - XG_WORLD_MINIMAP_TRIANGLE_COUNT)
        return false;
    for (triangle = 0u; triangle < XG_WORLD_MINIMAP_TRIANGLE_COUNT;
         ++triangle) {
        const XgWorldMinimapTriangleRecord *record =
            &output.triangles[triangle];
        const uint32_t packet =
            expected_packet_base + triangle * MINIMAP_PACKET_STRIDE;

        if (!record->submitted || record->packet_address != packet ||
            !word_address_is_valid(packet) ||
            !word_address_is_valid(packet + 24u) ||
            output.ordering[triangle].kind !=
                XG_WORLD_MINIMAP_ORDER_TRIANGLE ||
            output.ordering[triangle].packet_address != packet ||
            output.ordering[triangle].source_index != triangle ||
            output.ordering[triangle].payload_word_count != 6u ||
            xg_render_backend_translate_primitive(
                &record->primitive, &semantics[triangle]) !=
                XG_RENDER_BACKEND_OK)
            return false;
        for (vertex = 0u;
             vertex < XG_WORLD_MINIMAP_TRIANGLE_VERTEX_COUNT; ++vertex) {
            if (record->screen_xy_address[vertex] !=
                packet + 8u + vertex * 8u)
                return false;
        }
        packet_tags[triangle] = cpu->read_word(packet);
        if ((packet_tags[triangle] >> 24u) != 6u) return false;
    }

    scratch_words[0] = (uint16_t)output.scratch.angle_x |
        ((uint32_t)(uint16_t)output.scratch.angle_y << 16u);
    scratch_words[1] =
        (cpu->read_word(XG_WORLD_MINIMAP_SCRATCH_ANGLE_ADDRESS + 4u) &
         UINT32_C(0xffff0000)) |
        output.scratch.angle_z;
    scratch_words[2] = (uint16_t)output.scratch.rotation[0][0] |
        ((uint32_t)(uint16_t)output.scratch.rotation[0][1] << 16u);
    scratch_words[3] = (uint16_t)output.scratch.rotation[0][2] |
        ((uint32_t)(uint16_t)output.scratch.rotation[1][0] << 16u);
    scratch_words[4] = (uint16_t)output.scratch.rotation[1][1] |
        ((uint32_t)(uint16_t)output.scratch.rotation[1][2] << 16u);
    scratch_words[5] = (uint16_t)output.scratch.rotation[2][0] |
        ((uint32_t)(uint16_t)output.scratch.rotation[2][1] << 16u);
    scratch_words[6] =
        (cpu->read_word(XG_WORLD_MINIMAP_SCRATCH_ROTATION_ADDRESS + 16u) &
         UINT32_C(0xffff0000)) |
        (uint16_t)output.scratch.rotation[2][2];
    scratch_words[7] = (uint32_t)output.scratch.translation[0];
    scratch_words[8] = (uint32_t)output.scratch.translation[1];
    scratch_words[9] = (uint32_t)output.scratch.translation[2];
    for (triangle = 0u; triangle < 10u; ++triangle) {
        const uint32_t address = triangle < 2u
            ? XG_WORLD_MINIMAP_SCRATCH_ANGLE_ADDRESS + triangle * 4u
            : triangle < 7u
                ? XG_WORLD_MINIMAP_SCRATCH_ROTATION_ADDRESS +
                    (triangle - 2u) * 4u
                : XG_WORLD_MINIMAP_SCRATCH_TRANSLATION_ADDRESS +
                    (triangle - 7u) * 4u;

        if (!word_address_is_valid(address)) return false;
    }

    for (triangle = 0u; triangle < XG_WORLD_MINIMAP_TRIANGLE_COUNT;
         ++triangle) {
        if (!stage_standalone_native_primitive_identified(
                 &output.triangles[triangle].primitive,
                 output.triangles[triangle].packet_address,
                 UINT32_C(0x62000000) |
                      (output.triangles[triangle].packet_address &
                       UINT32_C(0x001ffffc)),
                 UINT32_C(0x8007412c), triangle)) {
            abort_standalone_submission();
            return false;
        }
    }
    if (!xg_world_minimap_shadow_record_native_cutover(
            XG_WORLD_MINIMAP_TRIANGLE_COUNT)) {
        abort_standalone_submission();
        return false;
    }

    for (triangle = 0u; triangle < XG_WORLD_MINIMAP_TRIANGLE_COUNT;
         ++triangle) {
        const XgWorldMinimapTriangleRecord *record =
            &output.triangles[triangle];
        uint32_t predecessor = triangle == 0u
            ? initial_ot_word
            : (initial_ot_word & UINT32_C(0xff000000)) |
                (output.triangles[triangle - 1u].packet_address &
                 UINT32_C(0x00ffffff));
        uint32_t word;

        for (word = 0u; word < 10u; ++word) {
            const uint32_t address = word < 2u
                ? XG_WORLD_MINIMAP_SCRATCH_ANGLE_ADDRESS + word * 4u
                : word < 7u
                    ? XG_WORLD_MINIMAP_SCRATCH_ROTATION_ADDRESS +
                        (word - 2u) * 4u
                    : XG_WORLD_MINIMAP_SCRATCH_TRANSLATION_ADDRESS +
                        (word - 7u) * 4u;

            psx_store_cycle_barrier();
            cpu->write_word(address, scratch_words[word]);
        }
        for (vertex = 0u;
             vertex < XG_WORLD_MINIMAP_TRIANGLE_VERTEX_COUNT; ++vertex) {
            const uint32_t xy =
                (uint16_t)record->screen_xy[vertex][0] |
                ((uint32_t)(uint16_t)record->screen_xy[vertex][1] << 16u);

            psx_store_cycle_barrier();
            cpu->write_word(record->screen_xy_address[vertex], xy);
        }
        psx_store_cycle_barrier();
        cpu->write_word(record->packet_address,
            (packet_tags[triangle] & UINT32_C(0xff000000)) |
            (predecessor & UINT32_C(0x00ffffff)));
        psx_store_cycle_barrier();
        cpu->write_word(ot_address,
            (predecessor & UINT32_C(0xff000000)) |
            (record->packet_address & UINT32_C(0x00ffffff)));
    }

    cpu->gpr[2] = 0u;
    cpu->gpr[3] = output.triangles[3].packet_address & UINT32_C(0x00ffffff);
    cpu->gpr[4] = ot_address;
    cpu->gpr[5] = context;
    cpu->gpr[16] = expected_packet_base + MINIMAP_PACKET_BUFFER_STRIDE;
    cpu->gpr[17] = XG_WORLD_MINIMAP_TRIANGLE_COUNT;
    cpu->gpr[18] = MINIMAP_TRIANGLE_BASE + 0x60u;
    cpu->pc = MINIMAP_CONTINUATION;
    return true;
}

static bool reject_zoom(uint32_t blocker) {
    xg_field_zoom_note_rejection(blocker);
    *xg_field_zoom_source() = (XgRenderZoomSource){ 0 };
    *xg_field_zoom_rgb_pending() = (XgRenderZoomRgbPending){ 0 };
    *xg_field_zoom_invocation() = (XgRenderZoomInvocation){ 0 };
    if (state.active && !state.completed) {
        reject_producer_family(blocker);
    } else {
        pre_scene.blocked = true;
        if (pre_scene.blocker == 0u) pre_scene.blocker = blocker;
    }
    return false;
}

static bool stage_zoom_records(const XgRenderZoomNativeRecord records[5],
                               uint32_t *failure_blocker) {
    uint32_t index;

    if (failure_blocker != NULL) *failure_blocker = 0u;
    if (state.active && !state.completed) {
        for (index = 0u; index < XG_RENDER_ZOOM_QUAD_COUNT; ++index) {
            if (!stage_active_native_primitive(
                    &records[index].primitive, records[index].packet_address,
                    UINT32_C(0x30000000) |
                        (records[index].packet_address & UINT32_C(0x001ffffc)),
                    XG_RENDER_ZOOM_OT_BUCKET,
                    XG_FIELD_CHARACTER_PACKET_WORD_COUNT,
                    XG_RENDER_ZOOM_TEMPLATE_STORE_PC, index,
                    failure_blocker))
                return false;
        }
        return true;
    }
    if (pre_scene.blocked ||
        pre_scene.count > XG_RENDER_PRE_SCENE_PRIMITIVE_CAPACITY -
            XG_RENDER_ZOOM_QUAD_COUNT)
        return false;
    for (index = 0u; index < XG_RENDER_ZOOM_QUAD_COUNT; ++index) {
        if (!stage_pre_scene_primitive(&(XgRenderPreScenePrimitive){
            .primitive = records[index].primitive,
            .packet_address = records[index].packet_address,
            .source_primitive_index = UINT32_C(0x30000000) |
                (records[index].packet_address & UINT32_C(0x001ffffc)),
            .ot_bucket = XG_RENDER_ZOOM_OT_BUCKET,
            .interpolation_producer_id = XG_RENDER_ZOOM_TEMPLATE_STORE_PC,
            .interpolation_primitive_id = index,
            .payload_word_count = XG_FIELD_CHARACTER_PACKET_WORD_COUNT,
            .interpolation_identity_valid = true,
        }))
            return false;
    }
    return true;
}

static bool native_zoom_stream_resolve(
    const GuestRenderNativeStreamMissContext *context,
    GpuRenderSemantic *out_semantic) {
    const uint64_t command_id = context != NULL ? context->command_id : 0u;
    XgRenderZoomSource *zoom_source = xg_field_zoom_source();
    GpuDrawState draw = { 0 };
    XgFieldCharacterCapture capture = { 0 };
    XgFieldCharacterCandidate candidate;
    XgRenderIrNativePrimitive primitive;
    uint32_t buffer;
    uint32_t quad;
    uint32_t component;

    if (out_semantic == NULL || context == NULL || command_id > UINT32_MAX ||
        context->opcode != 0x2eu ||
        context->word_count != XG_FIELD_CHARACTER_PACKET_WORD_COUNT ||
        state.requested_render_mode != GUEST_RENDER_RENDER_NATIVE)
        return false;
    for (quad = 0u; quad < XG_RENDER_ZOOM_QUAD_COUNT; ++quad) {
        for (buffer = 0u; buffer < XG_RENDER_ZOOM_BUFFER_COUNT; ++buffer) {
            const uint32_t packet = UINT32_C(0x000b1274) + quad * 0x50u +
                buffer * 0x28u;
            if ((uint32_t)command_id == packet + 4u) goto matched;
        }
    }
    return false;

matched:
    if (!current_artifact_is_authorized() ||
        !replay_container_matches_command(context) ||
        zoom_source->artifact_generation !=
            state.authenticated_artifact_generation ||
        context->visual_id.scene_epoch != state.scene_generation + 1u ||
        (context->source_kind != GUEST_RENDER_NATIVE_STREAM_SOURCE_DMA_BLOCK &&
         context->source_kind !=
             GUEST_RENDER_NATIVE_STREAM_SOURCE_DMA_LINKED_LIST &&
         context->source_kind != GUEST_RENDER_NATIVE_STREAM_SOURCE_DMA_BURST) ||
        !zoom_source->valid ||
        !zoom_source->authenticated ||
        zoom_source->command != 0x2eu)
        return false;
    gpu_get_draw_state(&draw);
    for (component = 0u; component < 4u; ++component) {
        static const uint8_t u[4] = { 0u, 64u, 0u, 64u };
        static const uint8_t v[4] = { 0u, 0u, 223u, 223u };
        const XgRenderZoomQuadSource *source =
            &zoom_source->quads[buffer][quad];

        capture.vertices[component] = (XgFieldCharacterCaptureVertex){
            source->x[component], source->y[component],
            u[component], v[component],
        };
    }
    capture.red = zoom_source->quads[buffer][quad].red;
    capture.green = zoom_source->quads[buffer][quad].green;
    capture.blue = zoom_source->quads[buffer][quad].blue;
    capture.tpage = zoom_source->quads[buffer][quad].tpage;
    capture.draw_area_left = draw.left;
    capture.draw_area_top = draw.top;
    capture.draw_area_right = draw.right;
    capture.draw_area_bottom = draw.bottom;
    capture.draw_offset_x = draw.offset_x;
    capture.draw_offset_y = draw.offset_y;
    capture.mask_set = draw.mask_set;
    capture.mask_check = draw.mask_check;
    capture.semi_transparent = true;
    if (xg_field_character_adapter_build(&capture, &candidate) !=
            XG_FIELD_CHARACTER_ADAPTER_OK ||
        xg_field_character_adapter_build_primitive(&candidate, &primitive) !=
            XG_FIELD_CHARACTER_ADAPTER_OK ||
        xg_render_backend_translate_primitive(&primitive, out_semantic) !=
            XG_RENDER_BACKEND_OK)
        return false;
    set_semantic_interpolation_identity(
        out_semantic, interpolation_scene_generation(),
        zoom_source->producer_store_pc,
        quad);
    if (quad == XG_RENDER_ZOOM_QUAD_COUNT - 1u)
        xg_field_zoom_note_replay_invocation(XG_RENDER_ZOOM_QUAD_COUNT);
    return true;
}

static bool native_field_sprite_stream_resolve(
        const GuestRenderNativeStreamMissContext *context,
        GpuRenderSemantic *out_semantic) {
    const uint64_t command_id = context != NULL ? context->command_id : 0u;
    XgRenderFieldSpriteBuilderRecord *record;
    uint32_t packet_address;

    if (out_semantic == NULL || context == NULL || command_id > UINT32_MAX ||
        command_id < 4u ||
        state.requested_render_mode != GUEST_RENDER_RENDER_NATIVE)
        return false;
    packet_address = (uint32_t)command_id - 4u;
    {
        uint32_t lookup_key;
        const uint32_t indexed = xg_render_lookup_find(
            field_sprite_template_lookup, field_sprite_template_lookup_epoch,
            packet_address, field_sprite_templates.count);

        if (xg_render_lookup_key(packet_address, &lookup_key)) {
            if (indexed == UINT32_MAX) return false;
            record = &field_sprite_templates.records[indexed];
            if (!physical_address_equals(record->packet_address,
                                         packet_address))
                return false;
        } else {
            record = field_sprite_template_find(packet_address);
        }
    }
    if (record == NULL || !replay_container_matches_command(context) ||
        !producer_lifecycle_matches_replay(&record->lifecycle, context) ||
        ((record->packet_address & UINT32_C(0x1fffffff)) + 4u) !=
            (uint32_t)command_id ||
        !translate_native_primitive_cached(
            &record->primitive, &record->semantic, &record->semantic_ready,
            out_semantic))
        return false;
    if (record->interpolation_identity_valid)
        set_semantic_interpolation_identity(
            out_semantic, interpolation_scene_generation(),
            record->interpolation_producer_id,
            record->interpolation_primitive_id);
    ++sprite_ft4_shadow.snapshot.field_builder_dma_replay_primitive_count;
    return true;
}

static XgRenderResidualTemplate *residual_template_find(
        uint32_t command_address) {
    const uint32_t indexed = xg_render_lookup_find(
        residual_template_lookup, residual_template_lookup_epoch,
        command_address, residual_templates.count);

    if (indexed != UINT32_MAX && residual_templates.records[indexed].valid &&
        physical_address_equals(
            residual_templates.records[indexed].command_address,
            command_address))
        return &residual_templates.records[indexed];
    for (uint32_t index = 0u; index < residual_templates.count; ++index) {
        XgRenderResidualTemplate *record = &residual_templates.records[index];
        if (record->valid && physical_address_equals(
                record->command_address, command_address)) {
            xg_render_lookup_put(
                residual_template_lookup, residual_template_lookup_epoch,
                command_address, index);
            return record;
        }
    }
    return NULL;
}

static XgRenderResidualTemplate *residual_template_find_indexed(
        uint32_t command_address) {
    const uint32_t indexed = xg_render_lookup_find(
        residual_template_lookup, residual_template_lookup_epoch,
        command_address, residual_templates.count);

    if (indexed == UINT32_MAX || !residual_templates.records[indexed].valid ||
        !physical_address_equals(
            residual_templates.records[indexed].command_address,
            command_address))
        return NULL;
    return &residual_templates.records[indexed];
}

static bool translate_native_primitive_cached(
        const XgRenderIrNativePrimitive *primitive,
        GpuRenderSemantic *cached_semantic, bool *semantic_ready,
        GpuRenderSemantic *out_semantic) {
    if (primitive == NULL || cached_semantic == NULL ||
        semantic_ready == NULL || out_semantic == NULL)
        return false;
    if (*semantic_ready) {
        *out_semantic = *cached_semantic;
        return true;
    }
    if (xg_render_backend_translate_primitive(primitive, cached_semantic) !=
            XG_RENDER_BACKEND_OK)
        return false;
    *semantic_ready = true;
    *out_semantic = *cached_semantic;
    return true;
}

static bool residual_template_store(
        uint32_t command_address, uint32_t producer_seam,
        uint32_t resource_size, const XgRenderQuadSource *source) {
    XgRenderResidualTemplate *record = NULL;

    XgRenderProducerLifecycle lifecycle;

    if (source == NULL || resource_size == 0u ||
        resource_size > XG_RENDER_RESIDUAL_MAX_RESOURCE_SIZE ||
        !producer_lifecycle_begin(producer_seam, &lifecycle)) return false;
    record = residual_template_find(command_address);
    if (record == NULL) {
        if (residual_templates.count == XG_RENDER_RESIDUAL_TEMPLATE_CAPACITY)
            return false;
        record = &residual_templates.records[residual_templates.count++];
    }
    memset(record, 0, sizeof(*record));
    if (xg_render_quad_build_primitive(source, &record->primitive) !=
            XG_RENDER_QUAD_BUILDER_OK)
        return false;
    record->command_address = command_address & UINT32_C(0x1ffffffc);
    record->producer_seam = producer_seam;
    record->resource_size = resource_size;
    record->lifecycle = lifecycle;
    record->valid = true;
    xg_render_lookup_put(
        residual_template_lookup, residual_template_lookup_epoch,
        record->command_address, (uint32_t)(record - residual_templates.records));
    watch_producer_resource(record->command_address, resource_size);
    return true;
}

static void residual_source_material(
        XgRenderIrMaterialState *material, XgRenderIrShading shading,
        bool semi_transparent) {
    GpuDrawState draw = {0};

    gpu_get_draw_state(&draw);
    apply_draw_state(material, &draw);
    material->shading = shading;
    material->textured = false;
    material->raw_texture = false;
    material->semi_transparent = semi_transparent;
}

static bool residual_capture_tile(
        uint32_t command_address, uint32_t producer_seam,
        int16_t x, int16_t y, int16_t width, int16_t height,
        uint8_t red, uint8_t green, uint8_t blue,
        bool semi_transparent) {
    XgRenderQuadSource source = {0};

    residual_source_material(
        &source.material, XG_RENDER_IR_SHADING_FLAT, semi_transparent);
    source.vertices[0] = (XgRenderQuadSourceVertex){
        x, y, 0, 0, red, green, blue};
    source.vertices[1] = (XgRenderQuadSourceVertex){
        x + width, y, 0, 0, red, green, blue};
    source.vertices[2] = (XgRenderQuadSourceVertex){
        x, y + height, 0, 0, red, green, blue};
    source.vertices[3] = (XgRenderQuadSourceVertex){
        x + width, y + height, 0, 0, red, green, blue};
    return residual_template_store(
        command_address, producer_seam, 0x0cu, &source);
}

static void residual_capture_clear_tile(CPUState *cpu) {
    uint32_t rect;
    uint32_t xy;
    uint32_t wh;
    uint32_t color;

    if (cpu == NULL || cpu->read_word == NULL ||
        state.requested_render_mode != GUEST_RENDER_RENDER_NATIVE)
        return;
    rect = cpu->gpr[8];
    xy = cpu->read_word(rect);
    wh = cpu->read_word(rect + 4u);
    if (((xy & 0x3fu) == 0u) && ((wh & 0x3fu) == 0u)) return;
    color = cpu->gpr[9];
    (void)residual_capture_tile(
        UINT32_C(0x8005a250), UINT32_C(0x80045ed0),
        (int16_t)xy, (int16_t)(xy >> 16u),
        (int16_t)wh, (int16_t)(wh >> 16u),
        (uint8_t)color, (uint8_t)(color >> 8u),
        (uint8_t)(color >> 16u), false);
}

void psx_xg_render_auth_capture_clear_tile(CPUState *cpu) {
    residual_capture_clear_tile(cpu);
}

void psx_xg_render_auth_capture_logo_sprite(
        uint32_t command_address, uint8_t color) {
    XgRenderQuadSource source = {0};
    XgRenderResidualTemplate *record;

    if (state.requested_render_mode != GUEST_RENDER_RENDER_NATIVE) return;
    residual_source_material(
        &source.material, XG_RENDER_IR_SHADING_FLAT, false);
    source.material.tpage = 10u;
    source.material.texture_page_x = 10u;
    source.material.texture_page_y = 0u;
    source.material.texture_depth = XG_RENDER_IR_TEXTURE_4_BIT;
    source.material.blend_mode = XG_RENDER_IR_BLEND_AVERAGE;
    source.material.clut_x = 0u;
    source.material.clut_y = 240u;
    source.material.textured = true;
    source.material.raw_texture = false;
    source.vertices[0] = (XgRenderQuadSourceVertex){
        32, 88, 0, 0, color, color, color};
    source.vertices[1] = (XgRenderQuadSourceVertex){
        288, 88, 0, 0, color, color, color};
    source.vertices[2] = (XgRenderQuadSourceVertex){
        32, 136, 0, 48, color, color, color};
    source.vertices[3] = (XgRenderQuadSourceVertex){
        288, 136, 0, 48, color, color, color};
    if (!residual_template_store(
            command_address, UINT32_C(0x80019d48), 0x10u, &source))
        return;
    record = residual_template_find(command_address);
    if (record != NULL) {
        record->primitive.triangles[0].vertices[1].u = 256 * INT32_C(65536);
        record->primitive.triangles[1].vertices[1].u = 256 * INT32_C(65536);
        record->primitive.triangles[1].vertices[2].u = 256 * INT32_C(65536);
    }
}

static void residual_capture_fullscreen_tile(CPUState *cpu) {
    uint32_t buffer;
    uint8_t color;

    if (cpu == NULL || cpu->read_word == NULL ||
        state.requested_render_mode != GUEST_RENDER_RENDER_NATIVE)
        return;
    buffer = cpu->read_word(UINT32_C(0x800adb08));
    if (buffer > 1u) return;
    color = (uint8_t)(cpu->gpr[4] << 2u);
    (void)residual_capture_tile(
        UINT32_C(0x800afe58) + buffer * 0x10u,
        UINT32_C(0x80079784), 0, 0, 320, 224,
        color, color, color, true);
}

static void residual_capture_fade_tiles(CPUState *cpu) {
    uint32_t buffer;

    if (cpu == NULL || cpu->read_word == NULL || cpu->read_half == NULL ||
        state.requested_render_mode != GUEST_RENDER_RENDER_NATIVE)
        return;
    buffer = cpu->gpr[5];
    if (buffer > 1u) return;
    for (uint32_t tile = 0u; tile < 2u; ++tile) {
        const uint32_t stride = tile * 0x58u;
        if (cpu->read_half(UINT32_C(0x800b2116) + stride) == 0u) continue;
        (void)residual_capture_tile(
            UINT32_C(0x800b20e0) + stride + buffer * 0x10u,
            UINT32_C(0x8007da44), 0, 0, 320, 224,
            (uint8_t)(cpu->read_word(UINT32_C(0x800b20fc) + stride) >> 8u),
            (uint8_t)(cpu->read_word(UINT32_C(0x800b2100) + stride) >> 8u),
            (uint8_t)(cpu->read_word(UINT32_C(0x800b2104) + stride) >> 8u),
            true);
    }
}

void psx_xg_render_auth_capture_tile_write(
        CPUState *cpu, uint32_t command_address, uint32_t writer_pc,
        uint8_t color) {
    if (writer_pc == UINT32_C(0x800797d0)) {
        (void)residual_capture_tile(
            command_address, UINT32_C(0x80079784), 0, 0, 320, 224,
            color, color, color, true);
        return;
    }
    if (writer_pc == UINT32_C(0x8007dbb4) && cpu != NULL &&
        cpu->read_word != NULL && command_address >= UINT32_C(0x000b20e0)) {
        const uint32_t relative = command_address - UINT32_C(0x000b20e0);
        const uint32_t tile = relative / 0x58u;
        const uint32_t stride = tile * 0x58u;

        if (tile >= 2u) return;
        (void)residual_capture_tile(
            command_address, UINT32_C(0x8007da44), 0, 0, 320, 224,
            (uint8_t)(cpu->read_word(UINT32_C(0x800b20fc) + stride) >> 8u),
            (uint8_t)(cpu->read_word(UINT32_C(0x800b2100) + stride) >> 8u),
            color, true);
    }
}

static void residual_capture_static_gouraud(CPUState *cpu) {
    uint32_t global;
    uint32_t packet_base;

    if (cpu == NULL || cpu->read_word == NULL ||
        state.requested_render_mode != GUEST_RENDER_RENDER_NATIVE)
        return;
    global = cpu->read_word(UINT32_C(0x800625a0));
    if (!guest_data_range_is_valid(global, 0x350u, 4u, false)) return;
    packet_base = cpu->read_word(global + 0x34cu);
    for (uint32_t quad = 0u; quad < 2u; ++quad) {
        XgRenderQuadSource source = {0};

        residual_source_material(
            &source.material, XG_RENDER_IR_SHADING_GOURAUD, false);
        source.vertices[0] = (XgRenderQuadSourceVertex){
            0, 0x4a, 0, 0, 0x80, 0x00, 0x80};
        source.vertices[1] = (XgRenderQuadSourceVertex){
            320, 0x4a, 0, 0, 0x00, 0x00, 0x80};
        source.vertices[2] = (XgRenderQuadSourceVertex){
            0, 0x8a, 0, 0, 0x10, 0x00, 0x10};
        source.vertices[3] = (XgRenderQuadSourceVertex){
            320, 0x8a, 0, 0, 0x00, 0x00, 0x10};
        (void)residual_template_store(
            packet_base + 0xa54u + quad * 0x24u,
            UINT32_C(0x801e61a4), 0x24u, &source);
    }
}

static void residual_capture_projected_gouraud(CPUState *cpu) {
    XgHost3dProject4Input input = {0};
    XgHost3dRotTransPers4Output output;
    XgRenderQuadSource source = {0};
    uint32_t first_xy;

    if (cpu == NULL || cpu->read_word == NULL || cpu->read_half == NULL ||
        state.requested_render_mode != GUEST_RENDER_RENDER_NATIVE)
        return;
    capture_shadow_projection(cpu, &input.projection);
    for (uint32_t vertex = 0u; vertex < 4u; ++vertex) {
        const uint32_t address = cpu->gpr[4u + vertex];
        input.vertices[vertex] = (XgHost3dVector){
            (int16_t)cpu->read_half(address),
            (int16_t)cpu->read_half(address + 2u),
            (int16_t)cpu->read_half(address + 4u),
            cpu->read_half(address + 6u),
        };
    }
    if (!xg_host_3d_rot_trans_pers4(&input, &output)) return;
    residual_source_material(
        &source.material, XG_RENDER_IR_SHADING_GOURAUD, true);
    for (uint32_t vertex = 0u; vertex < 4u; ++vertex) {
        source.vertices[vertex] = (XgRenderQuadSourceVertex){
            .red = 0x68u,
            .green = 0x68u,
            .blue = 0x68u,
        };
        xg_render_quad_set_projected_position(
            &source.vertices[vertex], &output.vertices[vertex]);
    }
    first_xy = cpu->read_word(cpu->gpr[29] + 0x10u);
    if (first_xy < 4u) return;
    (void)residual_template_store(
        first_xy - 4u, UINT32_C(0x801d09b0), 0x24u, &source);
}

static bool native_residual_stream_resolve(
        const GuestRenderNativeStreamMissContext *context,
        GpuRenderSemantic *out_semantic) {
    const uint64_t command_id = context != NULL ? context->command_id : 0u;
    XgRenderResidualTemplate *record;

    if (out_semantic == NULL || context == NULL || command_id > UINT32_MAX ||
        state.requested_render_mode != GUEST_RENDER_RENDER_NATIVE)
        return false;
    {
        uint32_t lookup_key;
        if (xg_render_lookup_key((uint32_t)command_id, &lookup_key)) {
            record = residual_template_find_indexed((uint32_t)command_id);
        } else {
            record = residual_template_find((uint32_t)command_id);
        }
    }
    return record != NULL && record->valid &&
        replay_container_matches_command(context) &&
        producer_lifecycle_matches_replay(&record->lifecycle, context) &&
        translate_native_primitive_cached(
            &record->primitive, &record->semantic, &record->semantic_ready,
            out_semantic);
}

#ifndef PSX_XG_RENDER_AUTH_RUNTIME_TESTING
static bool native_shared_packet_resolve(
        const GuestRenderNativeStreamMissContext *context,
        GpuRenderTransactionId *out_visual_id,
        GpuRenderSemantic *out_semantic) {
    GpuNativeDrawEnvironment environment;
    uint32_t words[GPU_GP0_RING_MAX_WORDS];
    int expected_words;

    if (!xg_render_runtime_variant_no_gates_enabled() || context == NULL ||
        out_visual_id == NULL || out_semantic == NULL ||
        context->source_kind == GUEST_RENDER_NATIVE_STREAM_SOURCE_UNKNOWN ||
        context->command_id > UINT32_MAX ||
        (context->command_id & 3u) != 0u ||
        (context->visual_id.scene_epoch != 0u &&
         context->visual_id.scene_epoch != state.scene_generation + 1u) ||
        context->opcode < 0x20u || context->opcode > 0x7fu ||
        context->word_count > GPU_GP0_RING_MAX_WORDS)
        return false;
    expected_words = gpu_gp0_command_word_count(context->opcode);
    if (expected_words == 0 ||
        (expected_words > 0 && (size_t)expected_words != context->word_count))
        return false;
    for (size_t index = 0u; index < context->word_count; ++index)
        words[index] = psx_read_word((uint32_t)context->command_id +
                                     (uint32_t)index * 4u);
    if ((uint8_t)(words[0] >> 24u) != context->opcode)
        return false;
    gpu_native_environment_get(&environment);
    if ((context->opcode >= 0x40u && context->opcode <= 0x5fu
             ? gpu_native_line_semantic_from_gp0(
                   words, context->word_count, &environment, out_semantic)
             : gpu_native_semantic_from_gp0(
                   words, (int)context->word_count, &environment, out_semantic))
            != 1)
        return false;
    *out_visual_id = context->visual_id.scene_epoch != 0u
        ? context->visual_id
        : (GpuRenderTransactionId){ state.scene_generation + 1u, 0u };
    return true;
}
#else
static bool native_shared_packet_resolve(
        const GuestRenderNativeStreamMissContext *context,
        GpuRenderTransactionId *out_visual_id,
        GpuRenderSemantic *out_semantic) {
    (void)context;
    (void)out_visual_id;
    (void)out_semantic;
    return false;
}
#endif

static XgRenderF4SourceRecord *f4_source_upsert(uint32_t packet_address) {
    const uint32_t source_id =
        (packet_address & UINT32_C(0x1fffffff)) + 4u;
    const uint32_t indexed = xg_render_lookup_find(
        f4_source_lookup, f4_source_lookup_epoch, source_id,
        f4_sources.count);

    if (indexed != UINT32_MAX &&
        f4_sources.records[indexed].source_id == source_id) {
        f4_sources.records[indexed].semantic_ready = false;
        return &f4_sources.records[indexed];
    }
    for (uint32_t index = 0u; index < f4_sources.count; ++index) {
        if (f4_sources.records[index].source_id == source_id) {
            f4_sources.records[index].semantic_ready = false;
            xg_render_lookup_put(
                f4_source_lookup, f4_source_lookup_epoch, source_id, index);
            return &f4_sources.records[index];
        }
    }
    if (f4_sources.count == XG_RENDER_F4_SOURCE_CAPACITY) return NULL;
    f4_sources.records[f4_sources.count] = (XgRenderF4SourceRecord){
        .source_id = source_id,
    };
    xg_render_lookup_put(
        f4_source_lookup, f4_source_lookup_epoch, source_id,
        f4_sources.count);
    return &f4_sources.records[f4_sources.count++];
}

static void f4_source_capture_material(XgRenderF4SourceRecord *record,
                                       bool semi_transparent) {
    GpuNativeDrawEnvironment environment;

    gpu_native_environment_get(&environment);
    record->semantic_ready = false;
    record->material = (XgRenderIrMaterialState){0};
    record->material.tpage = environment.tpage & UINT16_C(0x01ff);
    record->material.texture_page_x = record->material.tpage & 0x0fu;
    record->material.texture_page_y = (record->material.tpage >> 4u) & 1u;
    record->material.texture_depth = (XgRenderIrTextureDepth)(
        (record->material.tpage >> 7u) & 3u);
    record->material.blend_mode = (XgRenderIrBlendMode)(
        (record->material.tpage >> 5u) & 3u);
    record->material.shading = XG_RENDER_IR_SHADING_FLAT;
    record->material.textured = false;
    record->material.raw_texture = false;
    record->material.semi_transparent = semi_transparent;
    apply_draw_state(&record->material, &environment.draw);
}

/* The battle fader is an overscanned POLY_F4, not a GP0 rectangle: it spans
 * -32..320 by -32..240 before its OT insertion. Capture the completed packet
 * immediately before addPrim so the normal Native miss resolver owns it. */
static bool capture_battle_fader_source(CPUState *cpu) {
    XgRenderF4SourceRecord *record;
    XgRenderProducerLifecycle lifecycle;
    const uint32_t packet_address = cpu != NULL ? cpu->gpr[18] : 0u;
    uint32_t command;
    uint32_t ot_base;
    uint32_t vertex;

    if (cpu == NULL || cpu->read_word == NULL || cpu->read_half == NULL ||
        cpu->read_byte == NULL ||
        state.requested_render_mode != GUEST_RENDER_RENDER_NATIVE ||
        !producer_lifecycle_begin(UINT32_C(0x800b3878), &lifecycle) ||
        !guest_data_range_is_valid(packet_address, 0x18u, 4u, false))
        return false;
    command = cpu->read_word(packet_address + 4u);
    if ((command >> 24u) != 0x2au) return false;
    ot_base = cpu->read_word(UINT32_C(0x8005956c));
    if (ot_base > UINT32_MAX - 8u || !word_address_is_valid(ot_base + 8u))
        return false;
    record = f4_source_upsert(packet_address);
    if (record == NULL) return false;
    for (vertex = 0u; vertex < XG_RENDER_QUAD_VERTEX_COUNT; ++vertex) {
        const uint32_t xy_address = packet_address + 8u + vertex * 4u;

        record->vertices[vertex] = (XgRenderQuadSourceVertex){
            .x = (int16_t)cpu->read_half(xy_address),
            .y = (int16_t)cpu->read_half(xy_address + 2u),
            .red = cpu->read_byte(packet_address + 4u),
            .green = cpu->read_byte(packet_address + 5u),
            .blue = cpu->read_byte(packet_address + 6u),
        };
    }
    f4_source_capture_material(record, true);
    record->lifecycle = lifecycle;
    record->opcode = 0x2au;
    record->ot_address = ot_base + 8u;
    record->valid = true;
    watch_producer_resource(packet_address, 0x18u);
    return true;
}

static bool capture_fixed_2a_sources(CPUState *cpu) {
    uint32_t global;
    uint32_t packet_base;
    XgRenderProducerLifecycle lifecycle;

    if (cpu == NULL || cpu->read_word == NULL ||
        state.requested_render_mode != GUEST_RENDER_RENDER_NATIVE ||
        !producer_lifecycle_begin(UINT32_C(0x801c6f70), &lifecycle))
        return false;
    global = cpu->read_word(UINT32_C(0x800625a0));
    if (!guest_data_range_is_valid(global, 0x34cu, 4u, false)) return false;
    packet_base = cpu->read_word(global + 0x348u);
    if (packet_base > UINT32_MAX - 0xc8u ||
        !guest_data_range_is_valid(packet_base + 0x98u, 0x30u, 4u, false))
        return false;
    for (uint32_t buffer = 0u; buffer < 2u; ++buffer) {
        XgRenderF4SourceRecord *record =
            f4_source_upsert(packet_base + 0x98u + buffer * 0x18u);
        static const int16_t x[4] = {0, 320, 0, 320};
        static const int16_t y[4] = {0, 0, 224, 224};

        if (record == NULL) return false;
        memset(record->vertices, 0, sizeof(record->vertices));
        for (uint32_t vertex = 0u; vertex < 4u; ++vertex) {
            record->vertices[vertex] = (XgRenderQuadSourceVertex){
                x[vertex], y[vertex], 0u, 0u, 0x80u, 0x80u, 0x80u,
            };
        }
        record->opcode = 0x2au;
        record->lifecycle = lifecycle;
        record->ot_address = 0u;
        record->valid = true;
        watch_producer_resource(packet_base + 0x98u + buffer * 0x18u, 0x18u);
    }
    return true;
}

static bool capture_projected_2a_source(CPUState *cpu) {
    XgHost3dProject4Input input = {0};
    XgHost3dRotTransPers4Output output;
    XgRenderF4SourceRecord *record;
    uint32_t global;
    uint32_t first_xy;
    uint8_t color;
    XgRenderProducerLifecycle lifecycle;

    if (cpu == NULL || cpu->read_word == NULL || cpu->read_half == NULL ||
        cpu->read_byte == NULL ||
        state.requested_render_mode != GUEST_RENDER_RENDER_NATIVE ||
        !producer_lifecycle_begin(UINT32_C(0x801cf550), &lifecycle) ||
        !stack_address_is_valid(cpu->gpr[29]))
        return false;
    first_xy = cpu->read_word(cpu->gpr[29] + 0x10u);
    if (first_xy < 8u || !word_address_is_valid(first_xy)) return false;
    global = cpu->read_word(UINT32_C(0x800625a0));
    if (!guest_data_range_is_valid(global, 0x4d5u, 1u, false)) return false;
    color = cpu->read_byte(global + 0x4d4u);
    capture_shadow_projection(cpu, &input.projection);
    for (uint32_t vertex = 0u; vertex < 4u; ++vertex) {
        const uint32_t address = cpu->gpr[4u + vertex];

        if (!vector_address_is_valid(address)) return false;
        input.vertices[vertex] = (XgHost3dVector){
            (int16_t)cpu->read_half(address),
            (int16_t)cpu->read_half(address + 2u),
            (int16_t)cpu->read_half(address + 4u),
            cpu->read_half(address + 6u),
        };
    }
    if (!xg_host_3d_rot_trans_pers4(&input, &output)) return false;
    record = f4_source_upsert(first_xy - 8u);
    if (record == NULL) return false;
    for (uint32_t vertex = 0u; vertex < 4u; ++vertex) {
        record->vertices[vertex] = (XgRenderQuadSourceVertex){
            .red = color,
            .green = color,
            .blue = color,
        };
        xg_render_quad_set_projected_position(
            &record->vertices[vertex], &output.vertices[vertex]);
    }
    record->opcode = 0x2au;
    record->lifecycle = lifecycle;
    record->ot_address = 0u;
    record->valid = true;
    watch_producer_resource(first_xy - 8u, 0x18u);
    return true;
}

static void observe_2a_ot(CPUState *cpu) {
    XgRenderF4SourceRecord *record;
    uint32_t source_id;

    if (cpu == NULL || state.requested_render_mode !=
            GUEST_RENDER_RENDER_NATIVE)
        return;
    source_id = (cpu->gpr[5] & UINT32_C(0x1fffffff)) + 4u;
    for (uint32_t index = 0u; index < f4_sources.count; ++index) {
        record = &f4_sources.records[index];
        if (!record->valid || !producer_lifecycle_matches(&record->lifecycle) ||
            record->opcode != 0x2au ||
            record->source_id != source_id)
            continue;
        record->ot_address = cpu->gpr[4];
        f4_source_capture_material(record, true);
        return;
    }
}

static bool capture_field_f4_source(CPUState *cpu) {
    GpuNativeDrawEnvironment environment;
    XgRenderF4SourceRecord *record;
    uint32_t global;
    uint32_t packet_base;
    uint32_t state_address;
    uint32_t ot_base;
    uint32_t buffer;
    int16_t right;
    uint8_t red;
    XgRenderProducerLifecycle lifecycle;

    if (cpu == NULL || cpu->read_word == NULL || cpu->read_half == NULL ||
        cpu->read_byte == NULL ||
        state.requested_render_mode != GUEST_RENDER_RENDER_NATIVE ||
        !producer_lifecycle_begin(UINT32_C(0x8001e874), &lifecycle))
        return false;
    global = cpu->read_word(UINT32_C(0x800625a0));
    if (!guest_data_range_is_valid(global, 0x450u, 4u, false)) return false;
    packet_base = cpu->read_word(global + 0x44cu);
    state_address = cpu->read_word(global + 0x33cu);
    ot_base = cpu->read_word(global + 0x1d4u);
    if (!guest_data_range_is_valid(packet_base, 0x7b9u, 1u, false) ||
        !guest_data_range_is_valid(state_address + 0x52u, 1u, 1u, false) ||
        ot_base > UINT32_MAX - 0x80u)
        return false;
    buffer = cpu->read_byte(packet_base + 0x7b8u);
    if (buffer > 1u || packet_base > UINT32_MAX - buffer * 0x18u)
        return false;
    record = f4_source_upsert(packet_base + buffer * 0x18u);
    if (record == NULL) return false;
    right = (int16_t)(cpu->read_half(packet_base + 0x7b0u) + 0x20u);
    red = cpu->read_byte(state_address + 0x52u) == 2u ? 0u : 0xa0u;
    gpu_native_environment_get(&environment);
    record->material.tpage = environment.tpage & UINT16_C(0x01ff);
    record->material.texture_page_x = record->material.tpage & 0x0fu;
    record->material.texture_page_y = (record->material.tpage >> 4u) & 1u;
    record->material.texture_depth = (XgRenderIrTextureDepth)(
        (record->material.tpage >> 7u) & 3u);
    record->material.blend_mode = (XgRenderIrBlendMode)(
        (record->material.tpage >> 5u) & 3u);
    record->material.shading = XG_RENDER_IR_SHADING_FLAT;
    record->material.textured = false;
    record->material.raw_texture = false;
    record->material.semi_transparent = false;
    apply_draw_state(&record->material, &environment.draw);
    record->ot_address = ot_base + 0x80u;
    for (uint32_t vertex = 0u; vertex < 4u; ++vertex) {
        static const int16_t x[4] = {0x20, 0, 0x20, 0};
        static const int16_t y[4] = {0x61, 0x61, 0x68, 0x68};

        record->vertices[vertex] = (XgRenderQuadSourceVertex){
            vertex == 1u || vertex == 3u ? right : x[vertex], y[vertex],
            0u, 0u, red, 0xa0u, 0u,
        };
    }
    record->valid = true;
    record->lifecycle = lifecycle;
    watch_producer_resource(packet_base + buffer * 0x18u, 0x18u);
    return true;
}

static uint32_t native_resolve_hint_index(uint32_t command_id) {
    uint32_t hash = command_id * UINT32_C(2654435761);

    hash ^= hash >> 16u;
    return hash & (XG_NATIVE_RESOLVE_HINT_CAPACITY - 1u);
}

static XgNativeResolveFamily native_resolve_hint_get(
        const GuestRenderNativeStreamMissContext *context) {
    const uint64_t command_id = context != NULL ? context->command_id : 0u;
    const uint32_t key = (uint32_t)command_id & UINT32_C(0x1fffffff);
    const XgNativeResolveHint *hint = &native_resolve_hints[
        native_resolve_hint_index(key)];

    if (command_id > UINT32_MAX || !hint->valid || hint->command_id != key ||
        hint->resource_generation != next_resource_generation)
        return XG_NATIVE_RESOLVE_NONE;
    if (hint->family == XG_NATIVE_RESOLVE_NONSHARED_MISS &&
        (context == NULL || hint->container_id != context->container_id ||
         hint->word_count != context->word_count ||
         hint->opcode != context->opcode ||
         hint->source_kind != (uint8_t)context->source_kind))
        return XG_NATIVE_RESOLVE_NONE;
    return (XgNativeResolveFamily)hint->family;
}

static void native_resolve_hint_put(uint64_t command_id,
                                    XgNativeResolveFamily family) {
    const uint32_t key = (uint32_t)command_id & UINT32_C(0x1fffffff);
    XgNativeResolveHint *hint;

    if (command_id > UINT32_MAX || family == XG_NATIVE_RESOLVE_NONE)
        return;
    hint = &native_resolve_hints[native_resolve_hint_index(key)];
    *hint = (XgNativeResolveHint){
        .command_id = key,
        .resource_generation = next_resource_generation,
        .family = (uint8_t)family,
        .valid = true,
    };
}

static void native_resolve_hint_put_nonshared_miss(
        const GuestRenderNativeStreamMissContext *context) {
    const uint64_t command_id = context != NULL ? context->command_id : 0u;
    const uint32_t key = (uint32_t)command_id & UINT32_C(0x1fffffff);
    XgNativeResolveHint *hint;

    if (context == NULL || command_id > UINT32_MAX)
        return;
    hint = &native_resolve_hints[native_resolve_hint_index(key)];
    *hint = (XgNativeResolveHint){
        .command_id = key,
        .word_count = (uint32_t)context->word_count,
        .container_id = context->container_id,
        .resource_generation = next_resource_generation,
        .family = XG_NATIVE_RESOLVE_NONSHARED_MISS,
        .opcode = context->opcode,
        .source_kind = (uint8_t)context->source_kind,
        .valid = true,
    };
}

static bool native_model_ft3_stream_resolve(
        const GuestRenderNativeStreamMissContext *context,
        GpuRenderSemantic *out_semantic) {
    const uint64_t command_id = context != NULL ? context->command_id : 0u;
    XgRenderModelFt3SourceRecord *record = NULL;
    uint32_t lookup_key;

    if (out_semantic == NULL || context == NULL || command_id > UINT32_MAX ||
        state.requested_render_mode != GUEST_RENDER_RENDER_NATIVE ||
        context->opcode < 0x24u || context->opcode > 0x27u ||
        context->word_count != 7u)
        return false;
    ++model_ft3_shadow.snapshot.replay_attempt_count;
    if (xg_render_lookup_key((uint32_t)command_id, &lookup_key)) {
        const uint32_t indexed = xg_render_lookup_find(
            model_ft3_source_lookup, model_ft3_source_lookup_epoch,
            (uint32_t)command_id, model_ft3_source_count);

        if (indexed != UINT32_MAX) record = &model_ft3_sources[indexed];
    }
    if (record == NULL) {
        bool invalid_record = false;

        for (uint32_t slot = 0u; slot < model_ft3_source_count; ++slot) {
            if (!physical_address_equals(model_ft3_sources[slot].source_id,
                                         (uint32_t)command_id))
                continue;
            if (!model_ft3_sources[slot].valid) {
                invalid_record = true;
                continue;
            }
            record = &model_ft3_sources[slot];
            xg_render_lookup_put(
                model_ft3_source_lookup, model_ft3_source_lookup_epoch,
                record->source_id, slot);
            break;
        }
        if (record == NULL) {
            ++model_ft3_shadow.snapshot.replay_lookup_miss_count;
            if (invalid_record)
                ++model_ft3_shadow.snapshot.replay_lookup_invalid_count;
            else
                ++model_ft3_shadow.snapshot.replay_lookup_absent_count;
            model_ft3_shadow.snapshot.last_replay_lookup_miss_source =
                (uint32_t)command_id;
            return false;
        }
    }
    if (!record->valid || !physical_address_equals(
            record->source_id, (uint32_t)command_id)) {
        ++model_ft3_shadow.snapshot.replay_record_reject_count;
        return false;
    }
    if (!replay_container_matches_command(context)) {
        ++model_ft3_shadow.snapshot.replay_container_reject_count;
        return false;
    }
    if (!producer_lifecycle_matches_replay(&record->lifecycle, context)) {
        ++model_ft3_shadow.snapshot.replay_lifecycle_reject_count;
        return false;
    }
    if (!translate_native_primitive_cached(
            &record->primitive, &record->semantic, &record->semantic_ready,
            out_semantic)) {
        ++model_ft3_shadow.snapshot.replay_translate_reject_count;
        return false;
    }
    if (record->interpolation_identity_valid)
        set_semantic_interpolation_identity(
            out_semantic, interpolation_scene_generation(),
            record->interpolation_producer_id,
            record->interpolation_primitive_id);
    out_semantic->material.raw_texture = (context->opcode & 1u) != 0u;
    out_semantic->material.semi_transparent =
        (context->opcode & 2u) != 0u;
    ++model_ft3_shadow.snapshot.replay_resolved_count;
    return true;
}

static bool native_model_ft4_stream_resolve(
        const GuestRenderNativeStreamMissContext *context,
        GpuRenderSemantic *out_semantic) {
    const uint64_t command_id = context != NULL ? context->command_id : 0u;
    const bool sprite_opcode = context != NULL &&
        (context->opcode & 1u) == 0u;
    uint32_t indexed;
    XgRenderModelFt4SourceRecord *record;

    if (out_semantic == NULL || context == NULL || command_id > UINT32_MAX ||
        state.requested_render_mode != GUEST_RENDER_RENDER_NATIVE ||
        context->opcode < 0x2cu || context->opcode > 0x2fu ||
        context->word_count != 9u)
        return false;
    if (sprite_opcode)
        ++sprite_ft4_shadow.snapshot.resident_replay_attempt_count;
    else
        ++model_ft4_shadow.snapshot.replay_attempt_count;
    indexed = xg_render_lookup_find(
        model_ft4_source_lookup, model_ft4_source_lookup_epoch,
        (uint32_t)command_id, model_ft4_source_count);
    if (indexed == UINT32_MAX) {
        if (sprite_opcode)
            ++sprite_ft4_shadow.snapshot.resident_replay_lookup_miss_count;
        else
            ++model_ft4_shadow.snapshot.replay_lookup_miss_count;
        return false;
    }
    record = &model_ft4_sources[indexed];
    if (!record->valid || !physical_address_equals(
            record->source_id, (uint32_t)command_id) ||
        record->opcode != context->opcode) {
        if (sprite_opcode)
            ++sprite_ft4_shadow.snapshot.resident_replay_record_reject_count;
        else
            ++model_ft4_shadow.snapshot.replay_record_reject_count;
        return false;
    }
    if (!replay_container_matches_command(context)) {
        if (sprite_opcode)
            ++sprite_ft4_shadow.snapshot.resident_replay_container_reject_count;
        else
            ++model_ft4_shadow.snapshot.replay_container_reject_count;
        return false;
    }
    if (!producer_lifecycle_matches_replay(&record->lifecycle, context)) {
        if (sprite_opcode)
            ++sprite_ft4_shadow.snapshot.resident_replay_lifecycle_reject_count;
        else
            ++model_ft4_shadow.snapshot.replay_lifecycle_reject_count;
        return false;
    }
    if (!translate_native_primitive_cached(
            &record->primitive, &record->semantic,
            &record->semantic_ready, out_semantic)) {
        if (sprite_opcode)
            ++sprite_ft4_shadow.snapshot.resident_replay_translate_reject_count;
        else
            ++model_ft4_shadow.snapshot.replay_translate_reject_count;
        return false;
    }
    if (record->interpolation_identity_valid)
        set_semantic_interpolation_identity(
            out_semantic, interpolation_scene_generation(),
            record->interpolation_producer_id,
            record->interpolation_primitive_id);
    if (sprite_opcode)
        ++sprite_ft4_shadow.snapshot.resident_replay_resolved_count;
    else
        ++model_ft4_shadow.snapshot.replay_resolved_count;
    return true;
}

static bool native_f4_stream_resolve(
        const GuestRenderNativeStreamMissContext *context,
        GpuRenderSemantic *out_semantic) {
    const uint64_t command_id = context != NULL ? context->command_id : 0u;
    XgRenderF4SourceRecord *record = NULL;
    const uint32_t indexed = command_id <= UINT32_MAX
        ? xg_render_lookup_find(
              f4_source_lookup, f4_source_lookup_epoch,
              (uint32_t)command_id, f4_sources.count)
        : UINT32_MAX;

    if (out_semantic == NULL || context == NULL || command_id > UINT32_MAX ||
        state.requested_render_mode != GUEST_RENDER_RENDER_NATIVE)
        return false;
    {
        uint32_t lookup_key;
        if (xg_render_lookup_key((uint32_t)command_id, &lookup_key)) {
            if (indexed == UINT32_MAX) return false;
            if (f4_sources.records[indexed].valid &&
                physical_address_equals(f4_sources.records[indexed].source_id,
                                        (uint32_t)command_id))
                record = &f4_sources.records[indexed];
        } else {
            for (uint32_t index = 0u; index < f4_sources.count; ++index) {
                if (f4_sources.records[index].valid &&
                    physical_address_equals(f4_sources.records[index].source_id,
                                            (uint32_t)command_id)) {
                    record = &f4_sources.records[index];
                    xg_render_lookup_put(
                        f4_source_lookup, f4_source_lookup_epoch,
                        (uint32_t)command_id, index);
                    break;
                }
            }
        }
    }
    if (record == NULL || !replay_container_matches_command(context) ||
        !producer_lifecycle_matches_replay(&record->lifecycle, context) ||
        (record->opcode == 0x2au &&
                           record->ot_address == 0u))
        return false;
    if (!record->semantic_ready) {
        XgRenderQuadSource source = {0};
        XgRenderIrNativePrimitive primitive;

        source.material = record->material;
        memcpy(source.vertices, record->vertices, sizeof(source.vertices));
        if (xg_render_quad_build_primitive(&source, &primitive) !=
                XG_RENDER_QUAD_BUILDER_OK)
            return false;
        record->semantic_ready = false;
        if (xg_render_backend_translate_primitive(
                &primitive, &record->semantic) != XG_RENDER_BACKEND_OK)
            return false;
        record->semantic_ready = true;
    }
    *out_semantic = record->semantic;
    return true;
}

static bool native_stream_resolve(
        const GuestRenderNativeStreamMissContext *context,
        GpuRenderTransactionId *out_visual_id,
        GpuRenderSemantic *out_semantic) {
    GuestRenderNativeStreamMissContext resolved;
    XgNativeResolveFamily hinted_family;
    bool hinted_resolved = false;

    if (context == NULL || out_visual_id == NULL ||
        context->source_kind == GUEST_RENDER_NATIVE_STREAM_SOURCE_UNKNOWN ||
        context->word_count == 0u || context->opcode < 0x20u ||
        context->opcode > 0x7fu)
        return false;
    *out_visual_id = (GpuRenderTransactionId){
        state.scene_generation + 1u, f4_sources.count,
    };
    resolved = *context;
    if (resolved.visual_id.scene_epoch == 0u) {
        resolved.visual_id.scene_epoch = state.scene_generation + 1u;
        resolved.visual_id.state_sequence = 0u;
    }
    hinted_family = native_resolve_hint_get(&resolved);
    switch (hinted_family) {
    case XG_NATIVE_RESOLVE_MODEL_FT4:
        hinted_resolved = native_model_ft4_stream_resolve(&resolved,
                                                          out_semantic);
        break;
    case XG_NATIVE_RESOLVE_MODEL_FT3:
        hinted_resolved = native_model_ft3_stream_resolve(&resolved,
                                                          out_semantic);
        break;
    case XG_NATIVE_RESOLVE_ZOOM:
        hinted_resolved = native_zoom_stream_resolve(&resolved, out_semantic);
        break;
    case XG_NATIVE_RESOLVE_FIELD_SPRITE:
        hinted_resolved = native_field_sprite_stream_resolve(&resolved,
                                                             out_semantic);
        break;
    case XG_NATIVE_RESOLVE_RESIDUAL:
        hinted_resolved = native_residual_stream_resolve(&resolved,
                                                         out_semantic);
        break;
    case XG_NATIVE_RESOLVE_F4:
        hinted_resolved = native_f4_stream_resolve(&resolved, out_semantic);
        break;
    case XG_NATIVE_RESOLVE_SHARED:
        hinted_resolved = native_shared_packet_resolve(&resolved,
                                                       out_visual_id,
                                                       out_semantic);
        break;
    case XG_NATIVE_RESOLVE_NONSHARED_MISS:
    case XG_NATIVE_RESOLVE_NONE:
    default:
        break;
    }
    if (hinted_resolved) {
        *out_visual_id = resolved.visual_id;
        return true;
    }
    if (hinted_family != XG_NATIVE_RESOLVE_NONSHARED_MISS)
        hinted_family = XG_NATIVE_RESOLVE_NONE;
    if (hinted_family != XG_NATIVE_RESOLVE_NONSHARED_MISS) {
        if (native_model_ft4_stream_resolve(&resolved, out_semantic))
            hinted_family = XG_NATIVE_RESOLVE_MODEL_FT4;
        else if (native_model_ft3_stream_resolve(&resolved, out_semantic))
            hinted_family = XG_NATIVE_RESOLVE_MODEL_FT3;
        else if (native_zoom_stream_resolve(&resolved, out_semantic))
            hinted_family = XG_NATIVE_RESOLVE_ZOOM;
        else if (native_field_sprite_stream_resolve(&resolved, out_semantic))
            hinted_family = XG_NATIVE_RESOLVE_FIELD_SPRITE;
        else if (native_residual_stream_resolve(&resolved, out_semantic))
            hinted_family = XG_NATIVE_RESOLVE_RESIDUAL;
        else if (native_f4_stream_resolve(&resolved, out_semantic))
            hinted_family = XG_NATIVE_RESOLVE_F4;
    }
    if (hinted_family != XG_NATIVE_RESOLVE_MODEL_FT4 &&
        hinted_family != XG_NATIVE_RESOLVE_MODEL_FT3 &&
        hinted_family != XG_NATIVE_RESOLVE_ZOOM &&
        hinted_family != XG_NATIVE_RESOLVE_FIELD_SPRITE &&
        hinted_family != XG_NATIVE_RESOLVE_RESIDUAL &&
        hinted_family != XG_NATIVE_RESOLVE_F4 &&
        native_shared_packet_resolve(&resolved, out_visual_id,
                                     out_semantic))
        hinted_family = XG_NATIVE_RESOLVE_SHARED;
    if (hinted_family != XG_NATIVE_RESOLVE_MODEL_FT4 &&
        hinted_family != XG_NATIVE_RESOLVE_MODEL_FT3 &&
        hinted_family != XG_NATIVE_RESOLVE_ZOOM &&
        hinted_family != XG_NATIVE_RESOLVE_FIELD_SPRITE &&
        hinted_family != XG_NATIVE_RESOLVE_RESIDUAL &&
        hinted_family != XG_NATIVE_RESOLVE_F4 &&
        hinted_family != XG_NATIVE_RESOLVE_SHARED) {
        native_resolve_hint_put_nonshared_miss(&resolved);
        return false;
    }
    native_resolve_hint_put(resolved.command_id, hinted_family);
    *out_visual_id = resolved.visual_id;
    return true;
}

static bool native_zoom_cutover(CPUState *cpu, uint32_t continuation) {
    GuestRenderBridgeSnapshot bridge = { 0 };
    XgRenderZoomSource *zoom_source = xg_field_zoom_source();
    XgRenderZoomInvocation *zoom_invocation = xg_field_zoom_invocation();
    XgRenderZoomNativeRecord records[XG_RENDER_ZOOM_QUAD_COUNT];
    GpuDrawState draw = { 0 };
    XgHost3dProjection projection = { 0 };
    uint32_t stack_pointer;
    uint32_t scale;
    uint32_t buffer_index;
    uint32_t context_address;
    uint32_t ot_address;
    uint32_t index;
    bool project;

    if (state.requested_render_mode != GUEST_RENDER_RENDER_NATIVE) return false;
    xg_field_zoom_note_cutover_attempt();
    if (guest_render_bridge_snapshot(&bridge) != GUEST_RENDER_OK)
        return reject_zoom(57u);
    if ((state.active && bridge.modes.effective_render_mode !=
             GUEST_RENDER_RENDER_NATIVE) ||
        (!state.active && !state.armed)) {
        *zoom_source = (XgRenderZoomSource){ 0 };
        *zoom_invocation = (XgRenderZoomInvocation){ 0 };
        return false;
    }
    if (cpu == NULL || cpu->read_word == NULL || cpu->write_word == NULL)
        return reject_zoom(71u);
    if (!zoom_source->valid || !zoom_source->authenticated) {
        if (state.active && !state.completed) return reject_zoom(73u);
        *zoom_invocation = (XgRenderZoomInvocation){ 0 };
        return false;
    }
    if (!zoom_invocation->valid) {
        if (state.active && !state.completed) return reject_zoom(74u);
        *zoom_source = (XgRenderZoomSource){ 0 };
        return false;
    }
    if (zoom_invocation->source_generation != zoom_source->generation)
        return reject_zoom(75u);
    if (zoom_invocation->entry_sp < 0x88u ||
        cpu->gpr[29] != zoom_invocation->entry_sp - 0x88u)
        return reject_zoom(76u);
    if (!stack_address_is_valid(cpu->gpr[29])) return reject_zoom(77u);
    if (!physical_address_equals(cpu->gpr[31], UINT32_C(0x800a6444)))
        return reject_zoom(78u);
    if (cpu->read_word(cpu->gpr[29] + 0x84u) !=
        zoom_invocation->return_address)
        return reject_zoom(79u);
    if (!xg_field_zoom_caller_is_authorized(zoom_invocation->return_address))
        return reject_zoom(80u);
    stack_pointer = cpu->gpr[29];
    scale = cpu->gpr[2];
    buffer_index = cpu->read_word(UINT32_C(0x800adb08));
    context_address = cpu->read_word(UINT32_C(0x800c426c));
    if (buffer_index >= XG_RENDER_ZOOM_BUFFER_COUNT ||
        context_address != UINT32_C(0x800b249c) + buffer_index * 0x80f4u ||
        context_address > UINT32_MAX - 0x80d4u ||
        !word_address_is_valid(context_address + 0x80d4u))
        return reject_zoom(53u);
    ot_address = context_address + 0x80d4u;
    project = scale != 0x1000u;
    gpu_get_draw_state(&draw);
    capture_shadow_projection(cpu, &projection);
    if (project) {
        XgHost3dMatrix matrix;
        const XgHost3dLongVector scale_vector = {
            (int32_t)scale, (int32_t)scale, (int32_t)scale,
        };

        if (!capture_particle_matrix(cpu, stack_pointer + 0x28u, &matrix) ||
            !xg_host_3d_scale_matrix(&matrix, &scale_vector))
            return reject_zoom(54u);
        memcpy(projection.rotation, matrix.rotation,
               sizeof(projection.rotation));
        memcpy(projection.translation, matrix.translation,
               sizeof(projection.translation));
    }
    memset(records, 0, sizeof(records));
    for (index = 0u; index < XG_RENDER_ZOOM_QUAD_COUNT; ++index) {
        XgRenderZoomQuadSource *source =
            &zoom_source->quads[buffer_index][index];
        XgHost3dRotAverage4Input input = { 0 };
        XgHost3dRotAverage4Output output;
        XgFieldCharacterCapture capture = { 0 };
        XgFieldCharacterCandidate candidate;
        uint32_t component;

        for (component = 0u; component < 4u; ++component) {
            input.vertices[component].x = source->projection_x[component];
            input.vertices[component].y = source->projection_y[component];
            input.vertices[component].z = source->projection_z[component];
        }
        input.projection = projection;
        if (project) {
            if (!xg_host_3d_rot_average4(&input, &output))
                return reject_zoom(54u);
            for (component = 0u; component < 4u; ++component) {
                records[index].x[component] = output.vertices[component].x;
                records[index].y[component] = output.vertices[component].y;
            }
        } else {
            memcpy(records[index].x, source->x, sizeof(records[index].x));
            memcpy(records[index].y, source->y, sizeof(records[index].y));
        }
        for (component = 0u; component < 4u; ++component) {
            static const uint8_t u[4] = { 0u, 64u, 0u, 64u };
            static const uint8_t v[4] = { 0u, 0u, 223u, 223u };

            capture.vertices[component] = (XgFieldCharacterCaptureVertex){
                records[index].x[component], records[index].y[component],
                u[component], v[component],
            };
        }
        capture.red = source->red;
        capture.green = source->green;
        capture.blue = source->blue;
        capture.tpage = source->tpage;
        capture.clut_x = 0u;
        capture.clut_y = 0u;
        capture.draw_area_left = draw.left;
        capture.draw_area_top = draw.top;
        capture.draw_area_right = draw.right;
        capture.draw_area_bottom = draw.bottom;
        capture.draw_offset_x = draw.offset_x;
        capture.draw_offset_y = draw.offset_y;
        capture.mask_set = draw.mask_set;
        capture.mask_check = draw.mask_check;
        capture.semi_transparent = zoom_source->semi_transparent;
        if (xg_field_character_adapter_build(&capture, &candidate) !=
                XG_FIELD_CHARACTER_ADAPTER_OK ||
            xg_field_character_adapter_build_primitive(
                &candidate, &records[index].primitive) !=
                XG_FIELD_CHARACTER_ADAPTER_OK)
            return reject_zoom(55u);
        if (project)
            apply_projected_quad_positions(
                &records[index].primitive, output.vertices);
        records[index].packet_address = UINT32_C(0x800b1274) +
            index * 0x50u + buffer_index * 0x28u;
        records[index].draw_mode_address = UINT32_C(0x800b11ac) +
            index * 0x18u + buffer_index * 0x0cu;
        if (!word_address_is_valid(records[index].packet_address) ||
            !word_address_is_valid(records[index].packet_address + 0x24u) ||
            !word_address_is_valid(records[index].draw_mode_address) ||
            !word_address_is_valid(records[index].draw_mode_address + 8u))
            return reject_zoom(53u);
        records[index].packet_tag = UINT32_C(0x09000000);
        records[index].draw_mode_tag = UINT32_C(0x02000000);
    }
    {
        uint32_t stage_blocker = 0u;
        if (!stage_zoom_records(records, &stage_blocker))
            return reject_zoom(stage_blocker != 0u ? stage_blocker : 56u);
    }
    psx_store_cycle_barrier();
    cpu->write_word(stack_pointer + 0x4cu, scale);
    psx_store_cycle_barrier();
    cpu->write_word(stack_pointer + 0x50u, scale);
#ifdef PSX_XG_RENDER_AUTH_RUNTIME_TESTING
    for (index = 0u; index < XG_RENDER_ZOOM_QUAD_COUNT; ++index)
        zoom_test_primitives[index] = records[index].primitive;
    zoom_test_primitive_count = XG_RENDER_ZOOM_QUAD_COUNT;
#endif
    for (index = 0u; index < XG_RENDER_ZOOM_QUAD_COUNT; ++index) {
        uint32_t component;
        uint32_t previous_head;
        uint32_t linked_head;

        if (project) {
            for (component = 0u; component < 4u; ++component) {
                const uint32_t xy = (uint16_t)records[index].x[component] |
                    ((uint32_t)(uint16_t)records[index].y[component] << 16u);

                psx_store_cycle_barrier();
                cpu->write_word(records[index].packet_address + 8u +
                    component * 8u, xy);
            }
        }
        previous_head = cpu->read_word(ot_address);
        psx_store_cycle_barrier();
        cpu->write_word(records[index].packet_address,
            (records[index].packet_tag & UINT32_C(0xff000000)) |
            (previous_head & UINT32_C(0x00ffffff)));
        linked_head = (previous_head & UINT32_C(0xff000000)) |
            (records[index].packet_address & UINT32_C(0x00ffffff));
        psx_store_cycle_barrier();
        cpu->write_word(ot_address, linked_head);
        psx_store_cycle_barrier();
        cpu->write_word(records[index].draw_mode_address,
            (records[index].draw_mode_tag & UINT32_C(0xff000000)) |
            (records[index].packet_address & UINT32_C(0x00ffffff)));
        linked_head = (linked_head & UINT32_C(0xff000000)) |
            (records[index].draw_mode_address & UINT32_C(0x00ffffff));
        psx_store_cycle_barrier();
        cpu->write_word(ot_address, linked_head);
        if (project) {
            memcpy(zoom_source->quads[buffer_index][index].x,
                   records[index].x, sizeof(records[index].x));
            memcpy(zoom_source->quads[buffer_index][index].y,
                   records[index].y, sizeof(records[index].y));
        }
    }
    *zoom_invocation = (XgRenderZoomInvocation){ 0 };
    xg_field_zoom_note_native_invocation(XG_RENDER_ZOOM_QUAD_COUNT);
    cpu->gpr[2] = 0u;
    cpu->pc = continuation;
    return true;
}

static bool native_field_actor_cutover(CPUState *cpu, uint32_t continuation) {
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
    XgRenderAuthTier tier;

    if (cpu == NULL || cpu->read_word == NULL || cpu->write_word == NULL ||
        guest_render_bridge_snapshot(&bridge) != GUEST_RENDER_OK ||
        bridge.modes.effective_render_mode != GUEST_RENDER_RENDER_NATIVE)
        return false;
    tier = state.pending_variant_tier;
    if (!producer_family.enabled || producer_family.blocked)
        return false;
    if (!ft4_geometry.enabled || ft4_geometry.blocked || pending->valid ||
        !source_context_matches(tier) ||
        !stack_address_is_valid(cpu->gpr[29])) {
        reject_producer_family(17u);
        return false;
    }
    actor_address = cpu->gpr[18];
    if (actor_address > UINT32_MAX - 8u ||
        !word_address_is_valid(actor_address + 8u)) {
        reject_producer_family(18u);
        return false;
    }
    model_address = cpu->read_word(actor_address + 8u);
    ft4_index = cpu->read_word(cpu->gpr[29] + 0xd8u);
    if (ft4_index >= XG_FIELD_CHARACTER_SOURCE_DYNAMIC_MODEL_FT4_COUNT ||
        ft4_index > (UINT32_MAX - 0x40u) / 0x28u) {
        reject_producer_family(18u);
        return false;
    }
    packet_offset = ft4_index * 0x28u + 0x20u;
    if (model_address > UINT32_MAX - packet_offset - 0x20u) {
        reject_producer_family(18u);
        return false;
    }
    packet_address = model_address + packet_offset;
    *pending = (XgRenderFt4GeometryPending){
        .scene_generation = state.scene_generation,
        .tier = tier,
        .phase = XG_RENDER_FT4_EXPECT_FIRST_PRE,
        .valid = true,
    };
    for (index = 0u; index < 4u; ++index)
        pending->destinations[index] = packet_address + 8u + index * 8u;
    if (!ft4_destinations_are_valid(pending->destinations) ||
        !capture_source_values(cpu, pending, model_address, ft4_index) ||
        xg_field_character_runtime_build_candidate(
            &pending->source_snapshot, &pending->native_candidate) !=
            XG_FIELD_CHARACTER_RUNTIME_OK) {
        block_ft4_geometry(false);
        reject_producer_family(19u);
        return false;
    }
    pending->host_output = pending->native_candidate.source_derived.projection;
    pending->native_ready = true;
    ++ft4_geometry.host_transform_count;

    ot_base = cpu->read_word(cpu->gpr[29] + 0xd0u);
    ot_bucket = pending->native_candidate.source_derived.ordering_bucket;
    if (ot_bucket > (UINT32_MAX - ot_base) / 4u ||
        !word_address_is_valid(packet_address) ||
        !word_address_is_valid(ot_base + ot_bucket * 4u) ||
        ft4_geometry.count == PSX_XG_RENDER_FT4_GEOMETRY_CAPACITY ||
        ft4_geometry.next_sequence == 0u ||
        ft4_geometry.completed_count == UINT64_MAX) {
        block_ft4_geometry(ft4_geometry.count ==
                           PSX_XG_RENDER_FT4_GEOMETRY_CAPACITY);
        reject_producer_family(20u);
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
        process_producer_family_candidate(cpu, ot_bucket);
    cpu->pc = continuation;
    return true;
}

bool psx_xg_render_auth_native_ft4_bypass(
    CPUState *cpu, uint32_t pc, uint32_t instruction_word) {
    GuestRenderBridgeSnapshot bridge = { 0 };
    XgRenderFt4GeometryPending *pending = &ft4_geometry.pending;
    PsxXgRenderSourceSiteMetadata source_metadata = { 0 };
    XgRenderRuntimeVariantCutoverHandler cutover_handler;
    uint32_t continuation;
    uint32_t depth_cue_address;
    uint32_t flags_address;
    uint32_t index;

    observe_world_execution(pc, instruction_word);

    if (!overlay_pc_is_authorized(pc, instruction_word)) return false;

    if (physical_address_equals(pc, UINT32_C(0x801c6f70)) &&
        instruction_word == UINT32_C(0x27bdffb8)) {
        (void)capture_fixed_2a_sources(cpu);
        return false;
    }
    if (physical_address_equals(pc, UINT32_C(0x800b393c)) &&
        instruction_word == UINT32_C(0x3c108006)) {
        (void)capture_battle_fader_source(cpu);
        return false;
    }
    if (physical_address_equals(pc, UINT32_C(0x801cf550)) &&
        instruction_word == UINT32_C(0x0c0129cf)) {
        (void)capture_projected_2a_source(cpu);
        return false;
    }

    if (physical_address_equals(pc, UINT32_C(0x80043b48)) &&
        instruction_word == UINT32_C(0x3c0600ff)) {
        observe_2a_ot(cpu);
        overlay_ft4_observe_add_prim(cpu);
        return false;
    }
    if (physical_address_equals(pc, UINT32_C(0x8007fbe0)) &&
        instruction_word == UINT32_C(0x00a04021)) {
        (void)capture_resident_line_f2_source(cpu);
        return false;
    }
    if (physical_address_equals(pc, UINT32_C(0x80073b64)) &&
        instruction_word == UINT32_C(0x3c03800d)) {
        (void)stage_resident_line_f2(cpu);
        return false;
    }
    if (physical_address_equals(pc, UINT32_C(0x80045ed0)) &&
        instruction_word == UINT32_C(0x95020000)) {
        residual_capture_clear_tile(cpu);
        return false;
    }
    if (physical_address_equals(pc, UINT32_C(0x80079784)) &&
        instruction_word == UINT32_C(0x27bdffe8)) {
        residual_capture_fullscreen_tile(cpu);
        return false;
    }
    if (physical_address_equals(pc, UINT32_C(0x8007da44)) &&
        instruction_word == UINT32_C(0x27bdff88)) {
        residual_capture_fade_tiles(cpu);
        return false;
    }
    if (physical_address_equals(pc, UINT32_C(0x801e7c50)) &&
        instruction_word == UINT32_C(0x27bdffc0)) {
        if (state.requested_render_mode == GUEST_RENDER_RENDER_NATIVE &&
            !overlay_ft4_capture_projected_material(cpu, pc))
            overlay_ft4_2c.substitution_blocker = 7u;
        return false;
    }
    if (physical_address_equals(pc, UINT32_C(0x801e920c)) &&
        instruction_word == UINT32_C(0x00a01821) && cpu != NULL &&
        (overlay_ft4_find_template(cpu->gpr[4]) != NULL ||
         physical_address_equals(cpu->gpr[31], UINT32_C(0x801e63f4)))) {
        if (state.requested_render_mode == GUEST_RENDER_RENDER_NATIVE &&
            !overlay_ft4_capture_rectangle_template(cpu, pc))
            overlay_ft4_2c.substitution_blocker = 5u;
        return false;
    }
    if (physical_address_equals(pc, UINT32_C(0x801ce23c)) &&
        instruction_word == UINT32_C(0x0c0129cf)) {
        if (state.requested_render_mode == GUEST_RENDER_RENDER_NATIVE &&
            !overlay_ft4_capture_projected_geometry(cpu))
            overlay_ft4_2c.substitution_blocker = 8u;
        return false;
    }
    if (physical_address_equals(pc, UINT32_C(0x801cf85c)) &&
        instruction_word == UINT32_C(0x0c073866)) {
        if (state.requested_render_mode == GUEST_RENDER_RENDER_NATIVE &&
            !overlay_ft4_capture_glyph_material(cpu, pc))
            overlay_ft4_2c.substitution_blocker = 10u;
        return false;
    }
    if (physical_address_equals(pc, UINT32_C(0x801d3db0)) &&
        instruction_word == UINT32_C(0x27bdffb8)) {
        overlay_projected_2e_descriptor_scope = true;
        return false;
    }
    if (physical_address_equals(pc, UINT32_C(0x801d3ff0)) &&
        instruction_word == UINT32_C(0x03e00008)) {
        overlay_projected_2e_descriptor_scope = false;
        return false;
    }
    if (physical_address_equals(pc, UINT32_C(0x801d3ff8)) &&
        instruction_word == UINT32_C(0x27bdffc8)) {
        if (!overlay_ft4_capture_projected_2e_material(cpu, 3u, pc))
            overlay_ft4_2c.substitution_blocker = 10u;
        return false;
    }
    if (physical_address_equals(pc, UINT32_C(0x801d433c)) &&
        instruction_word == UINT32_C(0x27bdffc8)) {
        if (!overlay_ft4_capture_projected_2e_material(cpu, 2u, pc))
            overlay_ft4_2c.substitution_blocker = 10u;
        return false;
    }
    if (physical_address_equals(pc, UINT32_C(0x801d4688)) &&
        instruction_word == UINT32_C(0x27bdffc8)) {
        if (!overlay_ft4_capture_projected_2e_material(cpu, 1u, pc))
            overlay_ft4_2c.substitution_blocker = 10u;
        return false;
    }
    if (physical_address_equals(pc, UINT32_C(0x801d49d0)) &&
        instruction_word == UINT32_C(0x27bdffc8)) {
        if (!overlay_ft4_capture_projected_2e_material(cpu, 0u, pc))
            overlay_ft4_2c.substitution_blocker = 10u;
        return false;
    }
    if (physical_address_equals(pc, UINT32_C(0x801d09b0)) &&
        instruction_word == UINT32_C(0x0c0129cf)) {
        residual_capture_projected_gouraud(cpu);
        if (!overlay_ft4_capture_projected_2e_geometry(cpu))
            overlay_ft4_2c.substitution_blocker = 11u;
        return false;
    }
    if (physical_address_equals(pc, UINT32_C(0x801d0bdc)) &&
        instruction_word == UINT32_C(0x0c0129cf)) {
        residual_capture_projected_gouraud(cpu);
        return false;
    }

    if (physical_address_equals(pc, UINT32_C(0x801e927c))) {
        if (instruction_word == UINT32_C(0x27bdffe8)) {
            ++overlay_ft4_2c.producer_entry_count;
            overlay_ft4_2c.last_pc = guest_address(pc);
        } else {
            ++overlay_ft4_2c.rejected_site_count;
        }
    } else if (physical_address_equals(pc, UINT32_C(0x801e92c4))) {
        if (instruction_word == UINT32_C(0x03e00008)) {
            ++overlay_ft4_2c.producer_return_count;
            overlay_ft4_2c.last_pc = guest_address(pc);
        } else {
            ++overlay_ft4_2c.rejected_site_count;
        }
    } else {
        static const uint32_t caller_calls[10] = {
            UINT32_C(0x801cd984), UINT32_C(0x801d3724),
            UINT32_C(0x801d4fb4), UINT32_C(0x801d5fa8),
            UINT32_C(0x801e6304), UINT32_C(0x801e574c),
            UINT32_C(0x801e5be4), UINT32_C(0x801e5eb4),
            UINT32_C(0x801e739c), UINT32_C(0x801e7cb0),
        };
        static const uint32_t caller_finishes[10] = {
            UINT32_C(0x801cdb10), UINT32_C(0x801d3970),
            UINT32_C(0x801d50e0), UINT32_C(0x801d6188),
            UINT32_C(0x801e6444), UINT32_C(0x801e5918),
            UINT32_C(0x801e5e40), UINT32_C(0x801e61a4),
            UINT32_C(0x801e76e0), UINT32_C(0x801e7e5c),
        };
        static const uint32_t finish_instructions[10] = {
            UINT32_C(0x27bd0058), UINT32_C(0x27bd0038),
            UINT32_C(0x27bd0038), UINT32_C(0x27bd0038),
            UINT32_C(0x27bd0040), UINT32_C(0x27bd0048),
            UINT32_C(0x27bd0030), UINT32_C(0x27bd0038),
            UINT32_C(0x27bd0030), UINT32_C(0x27bd0040),
        };
        for (uint32_t caller = 0u; caller < 10u; ++caller) {
            if (physical_address_equals(pc, caller_calls[caller])) {
                if (instruction_word != UINT32_C(0x0c07a49f)) {
                    ++overlay_ft4_2c.rejected_site_count;
                    break;
                }
                ++overlay_ft4_2c.caller_call_count;
                if (caller < 5u)
                    ++overlay_ft4_2c.rectangle_helper_count;
                else if (caller < 9u)
                    ++overlay_ft4_2c.static_quad_count;
                else
                    ++overlay_ft4_2c.dynamic_uv_template_count;
                overlay_ft4_2c.last_pc = guest_address(pc);
                break;
            }
            if (physical_address_equals(pc, caller_finishes[caller])) {
                if (instruction_word == finish_instructions[caller]) {
                    ++overlay_ft4_2c.caller_finish_count;
                    overlay_ft4_2c.last_pc = guest_address(pc);
                    if (caller == 8u && state.requested_render_mode ==
                            GUEST_RENDER_RENDER_NATIVE &&
                        !overlay_ft4_capture_direct_templates(cpu, pc) &&
                        overlay_ft4_2c.substitution_blocker == 0u)
                        overlay_ft4_2c.substitution_blocker = 2u;
                    if (caller == 7u)
                        residual_capture_static_gouraud(cpu);
                } else {
                    ++overlay_ft4_2c.rejected_site_count;
                }
                break;
            }
        }
    }

    if (physical_address_equals(pc, UINT32_C(0x8002c8cc)) &&
        instruction_word == UINT32_C(0x27bdffd0)) {
        world_model_initializer_begin(cpu);
        return false;
    }
    if (physical_address_equals(pc, UINT32_C(0x8002cb4c)) &&
        instruction_word == UINT32_C(0x03e00008)) {
        world_model_initializer_finish(cpu);
        return false;
    }
    if (physical_address_equals(pc, UINT32_C(0x8003f968)) &&
        instruction_word == UINT32_C(0x1080000a)) {
        world_model_begin_packet_buffer_copy(cpu);
        return false;
    }
    if (physical_address_equals(pc, UINT32_C(0x8003f994)) &&
        instruction_word == UINT32_C(0x03e00008)) {
        world_model_finish_packet_buffer_copy(cpu);
        return false;
    }
    if (physical_address_equals(pc, UINT32_C(0x8002caa4)) &&
        instruction_word == UINT32_C(0x3c028006)) {
        world_model_observe_initializer_success(cpu);
        return false;
    }
    if ((physical_address_equals(pc, UINT32_C(0x8004a1ac)) &&
         instruction_word == UINT32_C(0xe8b60000)) ||
        (physical_address_equals(pc, UINT32_C(0x8004a1e8)) &&
         instruction_word == UINT32_C(0xe9360000)) ||
        (physical_address_equals(pc, UINT32_C(0x8004a274)) &&
         instruction_word == UINT32_C(0xe8d60000)) ||
        (physical_address_equals(pc, UINT32_C(0x8004a2b8)) &&
         instruction_word == UINT32_C(0xe9560000))) {
        world_model_observe_color_writes(cpu, pc);
        return false;
    }
    if (physical_address_equals(pc, UINT32_C(0x8002d100)) &&
        instruction_word == UINT32_C(0x3c048006)) {
        model_ft4_template_observe(cpu);
        return false;
    }
    if (physical_address_equals(pc, UINT32_C(0x8002da00)) &&
        instruction_word == UINT32_C(0x8fbf0014)) {
        model_ft3_template_observe(cpu);
        return false;
    }
    if (physical_address_equals(pc, UINT32_C(0x8002e404)) &&
        instruction_word == UINT32_C(0x26520001)) {
        model_ft4_observe_guest_pass(cpu, true);
        return false;
    }
    if (physical_address_equals(pc, UINT32_C(0x8002e5f0)) &&
        instruction_word == UINT32_C(0x02794021)) {
        model_ft3_observe_guest_pass(cpu);
        return false;
    }
    if (physical_address_equals(pc, UINT32_C(0x8002e880)) &&
        instruction_word == UINT32_C(0x15400002)) {
        model_ft4_observe_guest_pass(cpu, false);
        return false;
    }
    if (physical_address_equals(pc, UINT32_C(0x8001e298)) &&
        instruction_word == UINT32_C(0x27bdffe0)) {
        sprite_ft4_shadow_begin(cpu, true);
        return false;
    }
    if (physical_address_equals(pc, UINT32_C(0x8002675c)) &&
        instruction_word == UINT32_C(0x27bdffb0)) {
        (void)field_sprite_builder_begin(cpu);
        return false;
    }
    if (physical_address_equals(pc, UINT32_C(0x800269cc)) &&
        instruction_word == UINT32_C(0x8fa90020)) {
        field_sprite_builder_finish(cpu);
        return false;
    }
    if (physical_address_equals(pc, UINT32_C(0x801ced74)) &&
        instruction_word == UINT32_C(0xa05208c4)) {
        overlay_ft4_observe_field_material(cpu, 5u, true, 0x8c0u);
        return false;
    }
    if (physical_address_equals(pc, UINT32_C(0x801cef58)) &&
        instruction_word == UINT32_C(0xa0520006)) {
        overlay_ft4_observe_field_material(cpu, 4u, true, 0u);
        return false;
    }
    if (physical_address_equals(pc, UINT32_C(0x801cf074)) &&
        instruction_word == UINT32_C(0xa05208c4)) {
        overlay_ft4_observe_field_material(cpu, 5u, false, 0x8c0u);
        return false;
    }
    if (physical_address_equals(pc, UINT32_C(0x801cf258)) &&
        instruction_word == UINT32_C(0xa0520006)) {
        overlay_ft4_observe_field_material(cpu, 4u, false, 0u);
        return false;
    }
    if (physical_address_equals(pc, UINT32_C(0x801cfb48)) &&
        instruction_word == UINT32_C(0x3c038006)) {
        (void)field_polyline_begin(cpu);
        return false;
    }
    if (physical_address_equals(pc, UINT32_C(0x801cff3c)) &&
        instruction_word == UINT32_C(0x8fbf0040)) {
        field_polyline_finish(cpu);
        (void)capture_field_f4_source(cpu);
        return false;
    }
    if (physical_address_equals(pc, UINT32_C(0x8001e3d8)) &&
        instruction_word == UINT32_C(0x27bdff68)) {
        if (!sprite_ft4_shadow.snapshot.context_active)
            sprite_ft4_shadow_begin(cpu, false);
        return false;
    }
    if (physical_address_equals(pc, UINT32_C(0x8001e874)) &&
        instruction_word == UINT32_C(0x92680006)) {
        if (sprite_ft4_shadow.snapshot.context_active &&
            !sprite_ft4_shadow.snapshot.pending &&
            !sprite_ft4_shadow_prepare(cpu))
            block_sprite_ft4_shadow(86u);
        if (sprite_ft4_shadow.snapshot.pending)
            sprite_ft4_shadow_observe_xy(cpu);
        return false;
    }
    if (physical_address_equals(pc, UINT32_C(0x8001e8d8)) &&
        instruction_word == UINT32_C(0x8e82003c)) {
        if (sprite_ft4_shadow.snapshot.context_active)
            sprite_ft4_shadow_observe_material(cpu);
        return false;
    }
    if (physical_address_equals(pc, UINT32_C(0x8001e2c0)) &&
        instruction_word == UINT32_C(0x8e02003c)) {
        if (sprite_ft4_shadow.snapshot.context_active)
            sprite_ft4_shadow_end();
        return false;
    }
    if (physical_address_equals(pc, UINT32_C(0x8001e988)) &&
        instruction_word == UINT32_C(0x8fbf0094) &&
        sprite_ft4_shadow.snapshot.context_active &&
        !sprite_ft4_shadow.wrapper_scope) {
        sprite_ft4_shadow_end();
        return false;
    }
    if (physical_address_equals(pc, UINT32_C(0x8002c700)) &&
        instruction_word == UINT32_C(0x27bdffd0)) {
        model_ft4_shadow_begin(cpu);
        return false;
    }
    if ((physical_address_equals(pc, UINT32_C(0x8002e268)) ||
         physical_address_equals(pc, UINT32_C(0x8002e688))) &&
        instruction_word == UINT32_C(0x34190008)) {
        model_ft4_shadow.snapshot.last_seam_pc = pc;
        if (physical_address_equals(pc, UINT32_C(0x8002e268)))
            ++model_ft4_shadow.snapshot.average_seam_count;
        else
            ++model_ft4_shadow.snapshot.farthest_seam_count;
        if (!model_ft4_shadow.context.valid)
            ++model_ft4_shadow.snapshot.seam_without_context_count;
        if (state.requested_render_mode == GUEST_RENDER_RENDER_NATIVE) {
            if (model_ft4_shadow.context.valid &&
                !native_model_ft4_raw_stage(cpu)) {
                if (model_ft4_shadow.snapshot.prepare_failure_detail != 0u)
                    clear_model_ft4_shadow_pending();
                else
                    block_model_ft4_shadow(76u);
            }
        } else if (model_ft4_shadow.context.valid &&
            !model_ft4_shadow_prepare(cpu))
            block_model_ft4_shadow(75u);
        return false;
    }
    if (physical_address_equals(pc, UINT32_C(0x8002e484)) &&
        instruction_word == UINT32_C(0x34190008)) {
        if (!model_ft3_shadow.snapshot.blocked &&
            model_ft4_shadow.context.valid) {
            if (state.requested_render_mode == GUEST_RENDER_RENDER_NATIVE) {
                if (!native_model_ft3_raw_stage(cpu)) {
                    if (model_ft3_shadow.snapshot.prepare_failure_detail != 0u)
                        clear_model_ft3_shadow_pending();
                    else
                        block_model_ft3_shadow(76u);
                }
            } else if (!model_ft3_shadow_prepare(cpu)) {
                block_model_ft3_shadow(75u);
            }
        }
        return false;
    }
    if (physical_address_equals(pc, UINT32_C(0x8002c86c)) &&
        instruction_word == UINT32_C(0x86230002)) {
        model_ft4_shadow_finish(cpu);
        model_ft3_shadow_finish(cpu);
        return false;
    }
    if (physical_address_equals(pc, UINT32_C(0x800257dc)) &&
        instruction_word == UINT32_C(0x8fbf0044)) {
        model_ft4_shadow_end();
        return false;
    }
    if (physical_address_equals(
            pc, XG_WORLD_ACTOR_SPRITES_WORLD_ENTRY) &&
        instruction_word == UINT32_C(0x3c02800a)) {
        world_actor_context_begin(cpu);
        return false;
    }
    if (physical_address_equals(pc, UINT32_C(0x80085f38)) &&
        instruction_word == UINT32_C(0x8fbf0020)) {
        world_actor_context_finish(cpu);
        return false;
    }
    if (state.requested_render_mode == GUEST_RENDER_RENDER_NATIVE &&
        world_actor_context.active &&
        physical_address_equals(
            pc, XG_WORLD_ACTOR_SPRITES_PREPARED_SEAM) &&
        instruction_word == UINT32_C(0x02002021)) {
        bool prepared;

        if (world_native_cutover_failed || world_native_cutover_in_progress) {
            world_native_cutover_failed = true;
            abort_standalone_submission();
            clear_world_actor_native_pending();
            return false;
        }
        world_native_cutover_in_progress = true;
        prepared = native_world_actor_sprites_prepare(cpu);
        world_native_cutover_in_progress = false;
        if (!prepared) {
            world_native_cutover_failed = true;
            clear_world_actor_native_pending();
        }
        return false;
    }
    if (state.requested_render_mode == GUEST_RENDER_RENDER_NATIVE &&
        world_actor_context.active && world_actor_sprites_native_state.valid &&
        physical_address_equals(pc, UINT32_C(0x8001e2e0)) &&
        instruction_word == UINT32_C(0x8fbf0018)) {
        (void)run_native_world_cutover(
            cpu, native_world_actor_sprites_commit);
        return false;
    }

    if (physical_address_equals(pc, UINT32_C(0x8002709c)) &&
        instruction_word == UINT32_C(0x27bdff60)) {
        ++projected_lifecycle.initializer_begin_count;
        xg_field_projected_observe_initializer_begin(
            cpu, state.requested_render_mode);
        return false;
    }
    if (physical_address_equals(pc, UINT32_C(0x80027390)) &&
        instruction_word == UINT32_C(0x8fbf009c)) {
        xg_field_projected_observe_initializer_commit(cpu);
        if (cpu != NULL && xg_field_projected_find(cpu->gpr[2]) != NULL) {
            projected_lifecycle.last_registered_object = cpu->gpr[2];
            ++projected_lifecycle.initializer_registration_count;
        }
        return false;
    }
    if (physical_address_equals(pc, UINT32_C(0x800273c4)) &&
        instruction_word == UINT32_C(0x27bdff80) && !native_view.enabled)
        return native_projected_effect_cutover(cpu, pc);
    if (physical_address_equals(
            pc, XG_WORLD_TERRAIN_WATER_NATIVE_ENTRY_PC) &&
        instruction_word == UINT32_C(0x27bdffc8)) {
        world_terrain_water_native_last_caller =
            cpu != NULL ? cpu->gpr[31] : 0u;
        world_terrain_water_native_blocker_detail =
            state.requested_render_mode == GUEST_RENDER_RENDER_NATIVE ? 5u : 6u;
        if (state.requested_render_mode == GUEST_RENDER_RENDER_NATIVE)
            return run_native_world_cutover(
                cpu, native_world_terrain_water_cutover);
    }
    if (state.requested_render_mode == GUEST_RENDER_RENDER_SHADOW &&
        physical_address_equals(pc, UINT32_C(0x8009932c)) &&
        instruction_word == UINT32_C(0x27bdffc8)) {
        GpuDrawState draw = { 0 };

        gpu_get_draw_state(&draw);
        (void)xg_world_terrain_water_shadow_begin(
            cpu, state.scene_generation + 1u, &draw,
            render_screen_x_cull_margin());
        return false;
    }
    if (state.requested_render_mode == GUEST_RENDER_RENDER_NATIVE &&
        physical_address_equals(pc, XG_WORLD_ENTITY_SHADOWS_ENTRY_PC) &&
        instruction_word == UINT32_C(0x3c02800a))
        return run_native_world_cutover(cpu, native_world_entity_shadows_cutover);
    if (state.requested_render_mode == GUEST_RENDER_RENDER_SHADOW &&
        physical_address_equals(pc, UINT32_C(0x800996d4)) &&
        instruction_word == UINT32_C(0x8fbf0034)) {
        (void)xg_world_terrain_water_shadow_finish(cpu);
        return false;
    }
    if (state.requested_render_mode == GUEST_RENDER_RENDER_NATIVE &&
        physical_address_equals(
            pc, XG_WORLD_DECORATIONS_NATIVE_ENTRY_PC) &&
        instruction_word == UINT32_C(0x27bdffd8))
        return run_native_world_cutover(cpu, native_world_decorations_cutover);
    if (state.requested_render_mode == GUEST_RENDER_RENDER_SHADOW &&
        physical_address_equals(pc, UINT32_C(0x800747dc)) &&
        instruction_word == UINT32_C(0x3c02800a)) {
        GpuDrawState draw = { 0 };

        gpu_get_draw_state(&draw);
        (void)xg_world_entity_shadows_shadow_begin(
            cpu, state.scene_generation + 1u, &draw);
        return false;
    }
    if (state.requested_render_mode == GUEST_RENDER_RENDER_NATIVE &&
        physical_address_equals(pc, XG_WORLD_MODELS_PRODUCER_ENTRY) &&
        instruction_word == UINT32_C(0x24020800)) {
        bool prepared;

        if (world_native_cutover_failed || world_native_cutover_in_progress) {
            world_native_cutover_failed = true;
            abort_standalone_submission();
            clear_world_models_native_pending();
            return false;
        }
        world_native_cutover_in_progress = true;
        prepared = native_world_models_prepare(cpu);
        world_native_cutover_in_progress = false;
        if (!prepared) {
            world_native_cutover_failed = true;
            clear_world_models_native_pending();
        }
        return false;
    }
    if (state.requested_render_mode == GUEST_RENDER_RENDER_NATIVE &&
        physical_address_equals(pc, UINT32_C(0x80084cd0)) &&
        instruction_word == UINT32_C(0x8fbf0038)) {
        (void)run_native_world_cutover(cpu, native_world_models_commit);
        return false;
    }
    if (state.requested_render_mode == GUEST_RENDER_RENDER_SHADOW &&
        physical_address_equals(pc, UINT32_C(0x80074e24)) &&
        instruction_word == UINT32_C(0x8fbf0064)) {
        (void)xg_world_entity_shadows_shadow_finish(cpu);
        return false;
    }
    if (state.requested_render_mode == GUEST_RENDER_RENDER_SHADOW &&
        physical_address_equals(pc, UINT32_C(0x80074c84)) &&
        instruction_word == UINT32_C(0x8ecc0000)) {
        xg_world_entity_shadows_shadow_observe_transform(cpu);
        return false;
    }
    if (state.requested_render_mode == GUEST_RENDER_RENDER_SHADOW &&
        physical_address_equals(pc, UINT32_C(0x8008615c)) &&
        instruction_word == UINT32_C(0x27bdffd8)) {
        GpuDrawState draw = { 0 };

        gpu_get_draw_state(&draw);
        (void)xg_world_decorations_shadow_outer_begin(
            cpu, state.scene_generation + 1u, &draw,
            render_screen_x_cull_margin());
        return false;
    }
    if (state.requested_render_mode == GUEST_RENDER_RENDER_SHADOW &&
        physical_address_equals(pc, UINT32_C(0x80099bfc)) &&
        instruction_word == UINT32_C(0x27bdfff8)) {
        (void)xg_world_decorations_shadow_helper_begin(cpu);
        return false;
    }
    if (state.requested_render_mode == GUEST_RENDER_RENDER_SHADOW &&
        physical_address_equals(pc, UINT32_C(0x80099e78)) &&
        instruction_word == UINT32_C(0x8fb10008)) {
        (void)xg_world_decorations_shadow_helper_finish(cpu);
        return false;
    }
    if (state.requested_render_mode == GUEST_RENDER_RENDER_SHADOW &&
        physical_address_equals(pc, UINT32_C(0x800863bc)) &&
        instruction_word == UINT32_C(0x8fbf0024)) {
        (void)xg_world_decorations_shadow_outer_finish(cpu);
        return false;
    }
    if (state.requested_render_mode == GUEST_RENDER_RENDER_NATIVE &&
        physical_address_equals(pc, UINT32_C(0x80086798)) &&
        instruction_word == UINT32_C(0x3c02800a)) {
        native_world_clouds_prepare(cpu);
        return false;
    }
    if (state.requested_render_mode == GUEST_RENDER_RENDER_SHADOW &&
        physical_address_equals(pc, UINT32_C(0x80086798)) &&
        instruction_word == UINT32_C(0x3c02800a)) {
        GpuDrawState draw = { 0 };

        gpu_get_draw_state(&draw);
        (void)xg_world_clouds_shadow_begin(
            cpu, state.scene_generation + 1u, &draw,
            render_screen_x_cull_margin());
        return false;
    }
    if (state.requested_render_mode == GUEST_RENDER_RENDER_SHADOW &&
        physical_address_equals(pc, UINT32_C(0x80086e3c)) &&
        instruction_word == UINT32_C(0x8e2202d4)) {
        xg_world_clouds_shadow_observe_anchor(cpu);
        return false;
    }
    if (state.requested_render_mode == GUEST_RENDER_RENDER_SHADOW &&
        physical_address_equals(pc, UINT32_C(0x800876dc)) &&
        instruction_word == UINT32_C(0x8fbf004c)) {
        (void)xg_world_clouds_shadow_finish(cpu);
        return false;
    }
    if (state.requested_render_mode == GUEST_RENDER_RENDER_NATIVE &&
        physical_address_equals(pc, UINT32_C(0x800876dc)) &&
        instruction_word == UINT32_C(0x8fbf004c)) {
        native_world_clouds_commit(cpu);
        return false;
    }
    if (state.requested_render_mode == GUEST_RENDER_RENDER_SHADOW &&
        physical_address_equals(pc, UINT32_C(0x800740b8)) &&
        instruction_word == UINT32_C(0x27bdffc8)) {
        GpuDrawState draw = { 0 };

        gpu_get_draw_state(&draw);
        (void)xg_world_minimap_shadow_begin(
            cpu, state.scene_generation + 1u, &draw);
        return false;
    }
    if (state.requested_render_mode == GUEST_RENDER_RENDER_SHADOW &&
        physical_address_equals(pc, UINT32_C(0x80074564)) &&
        instruction_word == UINT32_C(0x8fbf0030)) {
        (void)xg_world_minimap_shadow_finish(cpu);
        return false;
    }
    if (physical_address_equals(pc, UINT32_C(0x8007412c)) &&
        instruction_word == UINT32_C(0x266400b8) &&
        state.requested_render_mode == GUEST_RENDER_RENDER_NATIVE)
        return run_native_world_cutover(cpu, native_world_minimap_cutover);
    if (physical_address_equals(pc, UINT32_C(0x80073b04)) &&
        instruction_word == UINT32_C(0x27bdffc0) &&
        state.requested_render_mode == GUEST_RENDER_RENDER_NATIVE)
        return run_native_world_cutover(cpu, native_world_horizon_cutover);
    if (physical_address_equals(pc, UINT32_C(0x80073b04)) &&
        instruction_word == UINT32_C(0x27bdffc0)) {
        world_horizon_shadow_begin(cpu);
        return false;
    }
    if (physical_address_equals(pc, UINT32_C(0x80089c78)) &&
        instruction_word == UINT32_C(0x27bdffb0) &&
        state.requested_render_mode == GUEST_RENDER_RENDER_NATIVE)
        return run_native_world_cutover(cpu, native_world_effects_cutover);
    if (physical_address_equals(pc, UINT32_C(0x80089c78)) &&
        instruction_word == UINT32_C(0x27bdffb0)) {
        world_effects_shadow_begin(cpu);
        return false;
    }
    if (physical_address_equals(pc, UINT32_C(0x8008a294)) &&
        instruction_word == UINT32_C(0x8fbf004c)) {
        world_effects_shadow_finish(cpu);
        return false;
    }
    if (physical_address_equals(pc, UINT32_C(0x80073e0c)) &&
        instruction_word == UINT32_C(0x8fbf003c)) {
        world_horizon_shadow_finish(cpu);
        return false;
    }
    if (physical_address_equals(pc, UINT32_C(0x800737ec)) &&
        instruction_word == UINT32_C(0x27bdffb8) &&
        state.requested_render_mode == GUEST_RENDER_RENDER_NATIVE)
        return run_native_world_cutover(cpu, native_world_sky_cutover);
    if (xg_render_runtime_variant_native_cutover_lookup(
            pc, instruction_word, &cutover_handler, &continuation)) {
        switch (cutover_handler) {
        case XG_RENDER_RUNTIME_VARIANT_CUTOVER_ZOOM_INITIALIZER_BEGIN:
            xg_field_zoom_observe_initializer_begin(
                cpu, state.requested_render_mode,
                artifact_generation_for_pc(pc));
            return false;
        case XG_RENDER_RUNTIME_VARIANT_CUTOVER_ZOOM_INITIALIZER_COMMIT:
            xg_field_zoom_observe_initializer_commit(
                cpu, artifact_generation_for_pc(pc));
            if (xg_field_zoom_source()->valid) {
                for (uint32_t quad = 0u;
                     quad < XG_RENDER_ZOOM_QUAD_COUNT; ++quad)
                    for (uint32_t buffer = 0u;
                         buffer < XG_RENDER_ZOOM_BUFFER_COUNT; ++buffer)
                        watch_producer_resource(
                            UINT32_C(0x800b1274) + quad * 0x50u +
                                buffer * 0x28u,
                            0x28u);
            }
            return false;
        case XG_RENDER_RUNTIME_VARIANT_CUTOVER_ZOOM_RGB_BEGIN:
            xg_field_zoom_observe_rgb_begin(
                cpu, state.requested_render_mode,
                artifact_generation_for_pc(pc));
            return false;
        case XG_RENDER_RUNTIME_VARIANT_CUTOVER_ZOOM_RGB_COMMIT:
            xg_field_zoom_observe_rgb_commit(
                cpu, artifact_generation_for_pc(pc));
            return false;
        case XG_RENDER_RUNTIME_VARIANT_CUTOVER_ZOOM_ENTRY:
            xg_field_zoom_observe_entry(
                cpu, state.requested_render_mode,
                artifact_generation_for_pc(pc));
            return false;
        case XG_RENDER_RUNTIME_VARIANT_CUTOVER_ZOOM_NATIVE:
            return native_zoom_cutover(cpu, continuation);
        case XG_RENDER_RUNTIME_VARIANT_CUTOVER_PARTICLE_INITIALIZER:
            (void)xg_field_particles_observe_initializer(
                cpu, state.requested_render_mode,
                authenticated_variant_hook_matches(pc), pc);
            return false;
        case XG_RENDER_RUNTIME_VARIANT_CUTOVER_PARTICLE_NATIVE:
            return native_particle_cutover(cpu, pc);
        case XG_RENDER_RUNTIME_VARIANT_CUTOVER_COMPASS_WORLD:
            return native_compass_cutover(cpu, pc, false);
        case XG_RENDER_RUNTIME_VARIANT_CUTOVER_COMPASS_SCREEN:
            return native_compass_cutover(cpu, pc, true);
        case XG_RENDER_RUNTIME_VARIANT_CUTOVER_ACTOR:
            return native_field_actor_cutover(cpu, continuation);
        default:
            return false;
        }
    }
    if (cpu == NULL || !pending->valid ||
        !xg_render_runtime_variant_source_site_lookup(
            pc, instruction_word, &source_metadata) ||
        source_metadata.operation != PSX_XG_RENDER_SOURCE_OPERATION_CALL ||
        guest_render_bridge_snapshot(&bridge) != GUEST_RENDER_OK ||
        bridge.modes.effective_render_mode != GUEST_RENDER_RENDER_NATIVE)
        return false;
    if (!pending->native_ready || !pending->source_captured ||
        !source_context_matches(pending->tier) || cpu->read_word == NULL ||
        cpu->write_word == NULL || !ft4_destinations_are_valid(
            pending->destinations)) {
        reject_producer_family(15u);
        return false;
    }
    depth_cue_address = cpu->read_word(cpu->gpr[29] + 0x20u);
    flags_address = cpu->read_word(cpu->gpr[29] + 0x24u);
    if (!word_address_is_valid(depth_cue_address) ||
        !word_address_is_valid(flags_address)) {
        reject_producer_family(16u);
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

static XgRenderOverlayFt4Template *overlay_ft4_find_template(
    uint32_t packet_address) {
    for (uint32_t index = 0u; index < overlay_ft4_state.count; ++index) {
        XgRenderOverlayFt4Template *record =
            &overlay_ft4_state.templates[index];
        if (record->valid && physical_address_equals(
                record->packet_address, packet_address))
            return record;
    }
    return NULL;
}

static XgRenderOverlayFt4Template *overlay_ft4_upsert_template(
    uint32_t packet_address, uint32_t producer_pc) {
    XgRenderOverlayFt4Template *record =
        overlay_ft4_find_template(packet_address);

    overlay_ft4_upsert_failure_detail = 0u;
    if (record != NULL) {
        if (producer_lifecycle_matches(&record->lifecycle)) return record;
        overlay_ft4_upsert_failure_detail = 1u;
        return NULL;
    }
    if (overlay_ft4_state.count == XG_RENDER_OVERLAY_FT4_TEMPLATE_CAPACITY) {
        overlay_ft4_upsert_failure_detail = 2u;
        return NULL;
    }
    record = &overlay_ft4_state.templates[overlay_ft4_state.count++];
    *record = (XgRenderOverlayFt4Template){
        .packet_address = packet_address,
    };
    if (!producer_lifecycle_begin(producer_pc, &record->lifecycle)) {
        overlay_ft4_upsert_failure_detail = 3u;
        *record = (XgRenderOverlayFt4Template){0};
        --overlay_ft4_state.count;
        return NULL;
    }
    watch_producer_resource(packet_address, 0x28u);
    return record;
}

static bool overlay_ft4_capture_direct_templates(CPUState *cpu,
                                                  uint32_t producer_pc) {
    GpuDrawState draw = {0};
    uint32_t global;
    uint32_t packet_base;
    uint32_t buffer;
    int16_t origin_x;
    int16_t origin_y;

    if (cpu == NULL || cpu->read_word == NULL || cpu->read_half == NULL ||
        state.requested_render_mode != GUEST_RENDER_RENDER_NATIVE)
        return false;
    global = cpu->read_word(UINT32_C(0x800625a0));
    if (!guest_data_range_is_valid(global, 0x350u, 4u, false)) return false;
    packet_base = cpu->read_word(global + 0x34cu);
    buffer = cpu->read_word(global + 0x308u);
    if (buffer > 1u || !guest_data_range_is_valid(
            UINT32_C(0x801ea04c), 8u, 2u, false))
        return false;
    origin_x = (int16_t)cpu->read_half(UINT32_C(0x801ea04c));
    origin_y = (int16_t)cpu->read_half(UINT32_C(0x801ea050));
    gpu_get_draw_state(&draw);
    for (uint32_t index = 0u; index < 16u; ++index) {
        XgRenderOverlayFt4Template *record;
        XgRenderQuadSource source = {0};
        const int16_t left = (int16_t)(origin_x + index * 12u);
        const int16_t right = (int16_t)(left + 12);
        const int16_t bottom = (int16_t)(origin_y + 16);
        const uint8_t u0 = (uint8_t)(index * 16u);
        const uint8_t u1 = (uint8_t)(u0 + 12u);

        record = overlay_ft4_upsert_template(
            packet_base + (index * 2u + buffer) * 0x28u + 0x277cu,
            producer_pc);
        if (record == NULL) return false;
        record->source_primitive_index = UINT32_C(0x2c730000) | index;
        apply_draw_state(&source.material, &draw);
        source.material.tpage = 5u;
        source.material.texture_page_x = 5u;
        source.material.texture_page_y = 0u;
        source.material.texture_depth = XG_RENDER_IR_TEXTURE_4_BIT;
        source.material.blend_mode = XG_RENDER_IR_BLEND_AVERAGE;
        source.material.clut_x = 0u;
        source.material.clut_y = 0x1c0u;
        source.material.shading = XG_RENDER_IR_SHADING_FLAT;
        source.material.textured = true;
        source.material.raw_texture = false;
        source.vertices[0] = (XgRenderQuadSourceVertex){
            left, origin_y, u0, 0xf0u, 0x80u, 0x80u, 0x80u};
        source.vertices[1] = (XgRenderQuadSourceVertex){
            right, origin_y, u1, 0xf0u, 0x80u, 0x80u, 0x80u};
        source.vertices[2] = (XgRenderQuadSourceVertex){
            left, bottom, u0, 0xffu, 0x80u, 0x80u, 0x80u};
        source.vertices[3] = (XgRenderQuadSourceVertex){
            right, bottom, u1, 0xffu, 0x80u, 0x80u, 0x80u};
        if (xg_render_quad_build_primitive(&source, &record->primitive) !=
                XG_RENDER_QUAD_BUILDER_OK)
            return false;
        record->valid = true;
        record->material = source.material;
        record->material_ready = true;
        record->family = 1u;
    }
    overlay_ft4_2c.direct_template_count += 16u;
    /* The direct family is complete; other producer families remain blocked. */
    return true;
}

static bool overlay_ft4_capture_rectangle_template(CPUState *cpu,
                                                    uint32_t producer_pc) {
    GpuDrawState draw = {0};
    XgRenderQuadSource source = {0};
    XgRenderOverlayFt4Template *record;
    uint32_t stack;
    uint32_t clut;
    uint8_t v;
    uint8_t width;
    uint8_t height;
    const int16_t left = (int16_t)cpu->gpr[5];
    const int16_t top = (int16_t)cpu->gpr[6];
    const uint8_t u = (uint8_t)cpu->gpr[7];

    if (cpu->read_word == NULL || cpu->read_half == NULL ||
        state.requested_render_mode != GUEST_RENDER_RENDER_NATIVE)
        return false;
    stack = cpu->gpr[29];
    v = (uint8_t)cpu->read_word(stack + 0x10u);
    width = (uint8_t)cpu->read_word(stack + 0x14u);
    height = (uint8_t)cpu->read_word(stack + 0x18u);
    record = overlay_ft4_upsert_template(cpu->gpr[4], producer_pc);
    if (record == NULL) return false;
    gpu_get_draw_state(&draw);
    if (record->material_ready) {
        source.material = record->material;
        apply_draw_state(&source.material, &draw);
    } else {
        clut = cpu->read_half(UINT32_C(0x800595d4));
        apply_draw_state(&source.material, &draw);
        source.material.tpage = 6u;
        source.material.texture_page_x = 6u;
        source.material.texture_page_y = 0u;
        source.material.texture_depth = XG_RENDER_IR_TEXTURE_4_BIT;
        source.material.blend_mode = XG_RENDER_IR_BLEND_AVERAGE;
        source.material.clut_x = (clut & 0x3fu) << 4u;
        source.material.clut_y = clut >> 6u;
        source.material.shading = XG_RENDER_IR_SHADING_FLAT;
        source.material.textured = true;
        source.material.raw_texture = false;
        record->material = source.material;
        record->material_ready = true;
    }
    source.vertices[0] = (XgRenderQuadSourceVertex){
        left, top, u, v, 0x80u, 0x80u, 0x80u};
    source.vertices[1] = (XgRenderQuadSourceVertex){
        (int16_t)(left + width), top, (uint8_t)(u + width), v,
        0x80u, 0x80u, 0x80u};
    source.vertices[2] = (XgRenderQuadSourceVertex){
        left, (int16_t)(top + height), u, (uint8_t)(v + height),
        0x80u, 0x80u, 0x80u};
    source.vertices[3] = (XgRenderQuadSourceVertex){
        (int16_t)(left + width), (int16_t)(top + height),
        (uint8_t)(u + width), (uint8_t)(v + height),
        0x80u, 0x80u, 0x80u};
    if (xg_render_quad_build_primitive(&source, &record->primitive) !=
            XG_RENDER_QUAD_BUILDER_OK)
        return false;
    record->source_primitive_index = UINT32_C(0x2c920000) |
        (record->packet_address & UINT32_C(0xffff));
    if (record->family == 0u) record->family = 2u;
    record->valid = true;
    ++overlay_ft4_2c.rectangle_template_count;
    return true;
}

static bool overlay_ft4_capture_projected_material(CPUState *cpu,
                                                    uint32_t producer_pc) {
    GpuDrawState draw = {0};
    uint32_t object;
    uint32_t mode;
    uint32_t clut;
    int32_t half_index;
    int32_t v0;
    uint8_t u0;
    uint8_t u1;
    uint8_t color;

    if (cpu == NULL || cpu->read_byte == NULL || cpu->read_half == NULL ||
        state.requested_render_mode != GUEST_RENDER_RENDER_NATIVE)
        return false;
    object = cpu->gpr[4];
    mode = cpu->gpr[7] & 0xffu;
    if (!guest_data_range_is_valid(object, 0x80u, 1u, false)) return false;
    gpu_get_draw_state(&draw);
    half_index = (int32_t)cpu->gpr[5] / 2;
    if (mode == 0u) {
        const int32_t quarter =
            ((int32_t)cpu->gpr[5] + (int32_t)cpu->gpr[6]) / 4;
        u0 = (uint8_t)(half_index * -128);
        v0 = quarter * 13;
        color = 0x80u;
    } else {
        u0 = (uint8_t)((cpu->gpr[5] & 1u) * 96u);
        v0 = half_index * 13 + (int32_t)cpu->gpr[6];
        color = (mode & 0x80u) == 0u ? 0x20u : 0x80u;
    }
    u1 = (uint8_t)(u0 + cpu->read_byte(object + 0x7eu));
    for (uint32_t buffer = 0u; buffer < 2u; ++buffer) {
        XgRenderOverlayFt4Template *record =
            overlay_ft4_upsert_template(object + buffer * 0x28u, producer_pc);
        XgRenderIrMaterialState material = {0};
        uint32_t clut_selector;

        if (record == NULL) return false;
        clut_selector = mode == 0u ? (cpu->gpr[5] & 1u) :
            ((mode & 0x7fu) - 1u);
        clut = cpu->read_half(clut_selector == 0u ?
            UINT32_C(0x800595d4) : UINT32_C(0x80059414));
        apply_draw_state(&material, &draw);
        material.tpage = mode == 0u ? 5u : 6u;
        if (mode != 0u && (mode & 0x80u) == 0u) {
            material.tpage |= 0x20u;
            material.semi_transparent = true;
        }
        material.texture_page_x = material.tpage & 0xfu;
        material.texture_page_y = (material.tpage >> 4u) & 1u;
        material.texture_depth = (XgRenderIrTextureDepth)(
            (material.tpage >> 7u) & 3u);
        material.blend_mode = (XgRenderIrBlendMode)(
            (material.tpage >> 5u) & 3u);
        material.clut_x = (clut & 0x3fu) << 4u;
        material.clut_y = clut >> 6u;
        material.shading = XG_RENDER_IR_SHADING_FLAT;
        material.textured = true;
        material.raw_texture = false;
        record->material = material;
        record->material_ready = true;
        record->family = 3u;
        record->source_primitive_index = UINT32_C(0x2ca70000) |
            (object & UINT32_C(0xffff));
        {
            XgRenderQuadSource source = {0};
            source.material = material;
            source.vertices[0] = (XgRenderQuadSourceVertex){
                0, 0, u0, (uint8_t)v0, color, color, color};
            source.vertices[1] = (XgRenderQuadSourceVertex){
                0, 0, u1, (uint8_t)v0, color, color, color};
            source.vertices[2] = (XgRenderQuadSourceVertex){
                0, 0, u0, (uint8_t)(v0 + 13), color, color, color};
            source.vertices[3] = (XgRenderQuadSourceVertex){
                0, 0, u1, (uint8_t)(v0 + 13), color, color, color};
            if (xg_render_quad_build_primitive(
                    &source, &record->primitive) != XG_RENDER_QUAD_BUILDER_OK)
                return false;
        }
        record->valid = true;
        ++overlay_ft4_2c.projected_material_count;
    }
    return true;
}

static bool overlay_ft4_capture_glyph_material(CPUState *cpu,
                                                uint32_t producer_pc) {
    GpuDrawState draw = {0};
    XgRenderQuadSource source = {0};
    XgRenderOverlayFt4Template *record;
    uint32_t global;
    uint32_t source_base;
    uint32_t source_address;
    uint32_t row;
    uint32_t clut;
    uint8_t u0;
    uint8_t u1;
    uint8_t v0;

    if (cpu == NULL || cpu->read_word == NULL || cpu->read_byte == NULL ||
        state.requested_render_mode != GUEST_RENDER_RENDER_NATIVE)
        return false;
    global = cpu->read_word(UINT32_C(0x800625a0));
    if (!guest_data_range_is_valid(global, 0x4d0u, 4u, false)) return false;
    source_base = cpu->read_word(global + 0x32cu);
    row = cpu->gpr[17];
    source_address = source_base + row * 0x5cu +
        cpu->read_word(global + 0x4ccu) * 4u;
    if (!guest_data_range_is_valid(source_address, 1u, 1u, false))
        return false;
    record = overlay_ft4_upsert_template(
        cpu->gpr[16] + (cpu->gpr[7] & 1u) * 0x28u, producer_pc);
    if (record == NULL) return false;
    clut = cpu->gpr[2] & 0xffffu;
    u0 = (uint8_t)cpu->gpr[20];
    u1 = (uint8_t)(u0 + 16u);
    v0 = cpu->read_byte(source_address);
    gpu_get_draw_state(&draw);
    apply_draw_state(&source.material, &draw);
    source.material.tpage = 5u;
    source.material.texture_page_x = 5u;
    source.material.texture_page_y = 0u;
    source.material.texture_depth = XG_RENDER_IR_TEXTURE_4_BIT;
    source.material.blend_mode = XG_RENDER_IR_BLEND_AVERAGE;
    source.material.clut_x = (clut & 0x3fu) << 4u;
    source.material.clut_y = clut >> 6u;
    source.material.shading = XG_RENDER_IR_SHADING_FLAT;
    source.material.textured = true;
    source.material.raw_texture = false;
    source.vertices[0] = (XgRenderQuadSourceVertex){
        0, 0, u0, v0, 0x80u, 0x80u, 0x80u};
    source.vertices[1] = (XgRenderQuadSourceVertex){
        0, 0, u1, v0, 0x80u, 0x80u, 0x80u};
    source.vertices[2] = (XgRenderQuadSourceVertex){
        0, 0, u0, (uint8_t)(v0 + 16u), 0x80u, 0x80u, 0x80u};
    source.vertices[3] = (XgRenderQuadSourceVertex){
        0, 0, u1, (uint8_t)(v0 + 16u), 0x80u, 0x80u, 0x80u};
    if (xg_render_quad_build_primitive(&source, &record->primitive) !=
            XG_RENDER_QUAD_BUILDER_OK)
        return false;
    record->material = source.material;
    record->material_ready = true;
    record->valid = true;
    record->family = 3u;
    record->source_primitive_index = UINT32_C(0x2ca80000) |
        (record->packet_address & UINT32_C(0xffff));
    ++overlay_ft4_2c.projected_material_count;
    return true;
}

static bool overlay_ft4_capture_projected_2e_material(
    CPUState *cpu, uint32_t template_family, uint32_t producer_pc) {
    GpuDrawState draw = {0};
    uint32_t global;
    uint32_t object;
    uint32_t buffer;
    uint32_t packet_offset;
    uint8_t u0;
    uint8_t u1;
    uint8_t v0;
    uint8_t v1;
    uint16_t tpage;
    uint16_t clut;

    if (cpu == NULL || cpu->read_word == NULL ||
        state.requested_render_mode != GUEST_RENDER_RENDER_NATIVE)
        return false;
    global = cpu->read_word(UINT32_C(0x800625a0));
    if (!guest_data_range_is_valid(global, 0x4ccu, 4u, false)) return false;
    object = cpu->read_word(global + 0x364u + (cpu->gpr[4] & 0xffu) * 4u);
    buffer = cpu->read_word(global + 0x308u);
    if (buffer > 1u || !guest_data_range_is_valid(object, 0x720u, 4u, false))
        return false;
    tpage = field_sprite_tpage(
        (uint16_t)cpu->read_word(global + 0x4b8u),
        (int16_t)cpu->read_word(global + 0x4c4u),
        (int16_t)cpu->read_word(global + 0x4c8u));
    clut = field_sprite_clut(
        (int16_t)cpu->read_word(global + 0x4bcu),
        (int16_t)cpu->read_word(global + 0x4c0u));
    if (template_family > 3u) return false;
    packet_offset = template_family == 0u ? 0x320u :
        template_family == 1u ? 0x280u :
        template_family == 2u ? 0x1e0u : 0x140u;
    u0 = template_family == 2u ? 0x08u :
        template_family == 3u ? 0x00u : 0x10u;
    u1 = template_family == 2u ? 0x0fu :
        template_family == 3u ? 0x07u : 0x20u;
    v0 = template_family == 0u ? 0x8cu : 0x84u;
    v1 = template_family == 2u ? 0x94u : (uint8_t)(v0 + 7u);
    gpu_get_draw_state(&draw);
    for (uint32_t quad = 0u; quad < 2u; ++quad) {
        XgRenderOverlayFt4Template *record = overlay_ft4_upsert_template(
            object + (quad * 2u + buffer) * 0x28u + packet_offset,
            producer_pc);
        XgRenderQuadSource source = {0};

        if (record == NULL) return false;
        apply_draw_state(&source.material, &draw);
        source.material.tpage = tpage;
        source.material.texture_page_x = tpage & 0x0fu;
        source.material.texture_page_y = (tpage >> 4u) & 1u;
        source.material.texture_depth = (XgRenderIrTextureDepth)(
            (tpage >> 7u) & 3u);
        source.material.blend_mode = (XgRenderIrBlendMode)(
            (tpage >> 5u) & 3u);
        source.material.clut_x = (clut & 0x3fu) << 4u;
        source.material.clut_y = clut >> 6u;
        source.material.shading = XG_RENDER_IR_SHADING_FLAT;
        source.material.textured = true;
        source.material.raw_texture = false;
        source.material.semi_transparent = true;
        source.vertices[0] = (XgRenderQuadSourceVertex){
            0, 0, u0, v0, 0x80u, 0x80u, 0x80u};
        source.vertices[1] = (XgRenderQuadSourceVertex){
            0, 0, u1, v0, 0x80u, 0x80u, 0x80u};
        source.vertices[2] = (XgRenderQuadSourceVertex){
            0, 0, u0, v1, 0x80u, 0x80u, 0x80u};
        source.vertices[3] = (XgRenderQuadSourceVertex){
            0, 0, u1, v1, 0x80u, 0x80u, 0x80u};
        if (xg_render_quad_build_primitive(&source, &record->primitive) !=
                XG_RENDER_QUAD_BUILDER_OK)
            return false;
        record->material = source.material;
        record->material_ready = true;
        record->valid = true;
        record->family = (uint8_t)(6u + template_family);
        record->source_primitive_index = UINT32_C(0x2e000000) |
            ((UINT32_C(0x49) - template_family * 3u) << 16u) | quad;
        ++overlay_ft4_2c.projected_2e_material_count;
    }
    return true;
}

static void overlay_ft4_set_projected_position(
    XgRenderIrNativePrimitive *primitive,
    const XgHost3dRotTransPers4Output *projection) {
    static const uint8_t split[2][3] = {{0u, 1u, 2u}, {2u, 1u, 3u}};
    for (uint32_t triangle = 0u; triangle < 2u; ++triangle) {
        for (uint32_t vertex = 0u; vertex < 3u; ++vertex) {
            const XgHost3dProjectedVertex *projected =
                &projection->vertices[split[triangle][vertex]];
            XgRenderQuadSourceVertex source_vertex = {0};
            XgRenderIrVertex *target =
                &primitive->triangles[triangle].vertices[vertex];

            xg_render_quad_set_projected_position(
                &source_vertex, projected);
            target->x = (int32_t)source_vertex.x * INT32_C(65536);
            target->y = (int32_t)source_vertex.y * INT32_C(65536);
            target->native_view_x = source_vertex.native_view_x_16_16;
            target->native_view_y = source_vertex.native_view_y_16_16;
            target->native_view_position = source_vertex.native_view_position;
            target->projective_view_x = source_vertex.projective_view_x;
            target->projective_view_y = source_vertex.projective_view_y;
            target->projective_view_z = source_vertex.projective_view_z;
            target->projective_offset_x =
                source_vertex.projective_offset_x_16_16;
            target->projective_offset_y =
                source_vertex.projective_offset_y_16_16;
            target->projective_native_offset_x =
                source_vertex.projective_native_offset_x_16_16;
            target->projective_native_offset_y =
                source_vertex.projective_native_offset_y_16_16;
            target->projective_distance = source_vertex.projective_distance;
            target->projective_position = source_vertex.projective_position;
        }
    }
}

static bool overlay_ft4_capture_projected_geometry(CPUState *cpu) {
    XgHost3dProject4Input input = {0};
    XgHost3dRotTransPers4Output output;
    XgRenderOverlayFt4Template *record;
    uint32_t packet;

    if (cpu == NULL || cpu->read_word == NULL || cpu->read_half == NULL ||
        state.requested_render_mode != GUEST_RENDER_RENDER_NATIVE)
        return false;
    packet = cpu->read_word(cpu->gpr[29] + 0x10u) - 8u;
    record = overlay_ft4_find_template(packet);
    if (record == NULL || !record->material_ready) {
        const uint32_t outer_return = cpu->read_word(cpu->gpr[29] + 0x64u);
        uint32_t slot;

        ++overlay_ft4_2c.projected_missing_material_count;
        for (slot = 0u; slot < overlay_ft4_2c.projected_missing_outer_count;
             ++slot) {
            if (physical_address_equals(
                    overlay_ft4_2c.projected_missing_outer_returns[slot],
                    outer_return))
                break;
        }
        if (slot < overlay_ft4_2c.projected_missing_outer_count) {
            ++overlay_ft4_2c.projected_missing_outer_counts[slot];
        } else if (slot < 16u) {
            overlay_ft4_2c.projected_missing_outer_returns[slot] =
                guest_address(outer_return);
            overlay_ft4_2c.projected_missing_outer_counts[slot] = 1u;
            ++overlay_ft4_2c.projected_missing_outer_count;
        } else {
            ++overlay_ft4_2c.projected_missing_outer_overflow;
        }
        return false;
    }
    capture_shadow_projection(cpu, &input.projection);
    for (uint32_t vertex = 0u; vertex < 4u; ++vertex) {
        const uint32_t address = cpu->gpr[4u + vertex];
        input.vertices[vertex] = (XgHost3dVector){
            (int16_t)cpu->read_half(address),
            (int16_t)cpu->read_half(address + 2u),
            (int16_t)cpu->read_half(address + 4u),
            cpu->read_half(address + 6u),
        };
    }
    if (!xg_host_3d_rot_trans_pers4(&input, &output)) return false;
    if (!record->valid) {
        XgRenderQuadSource source = {0};
        source.material = record->material;
        for (uint32_t vertex = 0u; vertex < 4u; ++vertex) {
            source.vertices[vertex] = (XgRenderQuadSourceVertex){
                .red = 0x80u,
                .green = 0x80u,
                .blue = 0x80u,
            };
            xg_render_quad_set_projected_position(
                &source.vertices[vertex], &output.vertices[vertex]);
        }
        if (xg_render_quad_build_primitive(&source, &record->primitive) !=
                XG_RENDER_QUAD_BUILDER_OK)
            return false;
    } else {
        overlay_ft4_set_projected_position(&record->primitive, &output);
    }
    record->valid = true;
    record->family = 3u;
    ++overlay_ft4_2c.projected_geometry_count;
    return true;
}

static bool overlay_ft4_capture_projected_2e_geometry(CPUState *cpu) {
    uint32_t packet;
    XgRenderOverlayFt4Template *record;

    if (cpu == NULL || cpu->read_word == NULL) return false;
    packet = cpu->read_word(cpu->gpr[29] + 0x10u) - 8u;
    record = overlay_ft4_find_template(packet);
    if (record == NULL || record->family < 6u || record->family > 10u)
        return false;
    const uint8_t family = record->family;
    if (!overlay_ft4_capture_projected_geometry(cpu)) return false;
    record = overlay_ft4_find_template(packet);
    if (record == NULL) return false;
    record->family = family;
    ++overlay_ft4_2c.projected_2e_geometry_count;
    return true;
}

static void overlay_ft4_observe_add_prim(CPUState *cpu) {
    XgRenderOverlayFt4Template *record;

    if (cpu == NULL || state.requested_render_mode !=
            GUEST_RENDER_RENDER_NATIVE)
        return;
    record = overlay_ft4_find_template(cpu->gpr[5]);
    if (record == NULL || !producer_lifecycle_matches(&record->lifecycle))
        return;
    if (record->family >= 4u && !record->material_ready) {
        overlay_ft4_2c.substitution_blocker = 9u;
        return;
    }
    if (record->family == 1u)
        ++overlay_ft4_2c.direct_add_prim_count;
    else if (record->family == 2u)
        ++overlay_ft4_2c.rectangle_add_prim_count;
    else if (record->family == 3u)
        ++overlay_ft4_2c.projected_add_prim_count;
    else if (record->family >= 6u && record->family <= 10u)
        ++overlay_ft4_2c.projected_2e_add_prim_count;
    else
        ++overlay_ft4_2c.field_add_prim_count;
    overlay_ft4_2c.last_packet = guest_address(cpu->gpr[5]);
    overlay_ft4_2c.last_ot = guest_address(cpu->gpr[4]);
    if (!stage_standalone_native_primitive_identified(
            &record->primitive, record->packet_address,
            record->source_primitive_index,
            record->interpolation_producer_id,
            record->interpolation_primitive_id)) {
        if (record->family == 1u)
            ++overlay_ft4_2c.direct_stage_failure_count;
        else if (record->family == 2u)
            ++overlay_ft4_2c.rectangle_stage_failure_count;
        else if (record->family == 3u)
            ++overlay_ft4_2c.projected_stage_failure_count;
        else if (record->family >= 6u && record->family <= 10u)
            ++overlay_ft4_2c.projected_2e_stage_failure_count;
        else
            ++overlay_ft4_2c.field_stage_failure_count;
        overlay_ft4_2c.substitution_blocker = 4u;
        return;
    }
    if (record->family == 1u)
        ++overlay_ft4_2c.direct_native_count;
    else if (record->family == 2u) {
        ++overlay_ft4_2c.rectangle_native_count;
    } else if (record->family == 3u)
        ++overlay_ft4_2c.projected_native_count;
    else if (record->family >= 6u && record->family <= 10u)
        ++overlay_ft4_2c.projected_2e_native_count;
    else
        ++overlay_ft4_2c.field_native_count;
    overlay_ft4_2c.substitution_blocker = 0u;
}

static void overlay_ft4_observe_field_material(CPUState *cpu, uint32_t family,
                                                bool semi_transparent,
                                                uint32_t packet_offset) {
    XgRenderOverlayFt4Template *record;
    uint32_t packet;
    uint8_t color;

    if (cpu == NULL || state.requested_render_mode !=
            GUEST_RENDER_RENDER_NATIVE)
        return;
    packet = cpu->gpr[2] + packet_offset;
    record = overlay_ft4_find_template(packet);
    if (record == NULL || record->family != family) return;
    field_sprite_template_invalidate(packet);
    color = (uint8_t)cpu->gpr[18];
    record->primitive.material.tpage |= UINT16_C(0x20);
    record->primitive.material.blend_mode = (XgRenderIrBlendMode)(
        (record->primitive.material.tpage >> 5u) & 3u);
    record->primitive.material.raw_texture = false;
    record->primitive.material.semi_transparent = semi_transparent;
    record->material = record->primitive.material;
    for (uint32_t triangle = 0u;
         triangle < record->primitive.triangle_count; ++triangle) {
        for (uint32_t vertex = 0u; vertex < 3u; ++vertex) {
            XgRenderIrVertex *target =
                &record->primitive.triangles[triangle].vertices[vertex];
            target->r = color;
            target->g = color;
            target->b = color;
        }
    }
    record->material_ready = true;
    ++overlay_ft4_2c.field_material_count;
}

#include "xg_render_overlay_cutovers.inc"

bool psx_xg_render_auth_native_cutover_pc_relevant(uint32_t pc) {
    static const uint32_t cutover_pcs[] = {
        UINT32_C(0x01e298), UINT32_C(0x01e2b4), UINT32_C(0x01e2c0),
        UINT32_C(0x01e2e0), UINT32_C(0x01e3d8), UINT32_C(0x01e874),
        UINT32_C(0x01e8d8), UINT32_C(0x01e988), UINT32_C(0x0257dc),
        UINT32_C(0x02675c), UINT32_C(0x0269cc), UINT32_C(0x02709c),
        UINT32_C(0x027390), UINT32_C(0x0273c4), UINT32_C(0x02c700),
        UINT32_C(0x02c86c), UINT32_C(0x02c8cc), UINT32_C(0x02caa4),
        UINT32_C(0x02cb4c), UINT32_C(0x02e268), UINT32_C(0x043b48),
        UINT32_C(0x045ed0), UINT32_C(0x071a58), UINT32_C(0x0737ec),
        UINT32_C(0x073b04), UINT32_C(0x073b64), UINT32_C(0x073e0c),
        UINT32_C(0x0740b8),
        UINT32_C(0x07412c), UINT32_C(0x074564), UINT32_C(0x0747dc),
        UINT32_C(0x074c84), UINT32_C(0x074e24), UINT32_C(0x0765dc),
        UINT32_C(0x079784), UINT32_C(0x07ab6c), UINT32_C(0x07ac58),
        UINT32_C(0x07da44), UINT32_C(0x07fbe0), UINT32_C(0x084778),
        UINT32_C(0x0848f4),
        UINT32_C(0x084cd0), UINT32_C(0x085cdc), UINT32_C(0x085f38),
        UINT32_C(0x08615c), UINT32_C(0x0863bc), UINT32_C(0x087014),
        UINT32_C(0x087288), UINT32_C(0x087614), UINT32_C(0x089c78),
        UINT32_C(0x08a294), UINT32_C(0x0983a0), UINT32_C(0x09932c),
        UINT32_C(0x0996d4), UINT32_C(0x099bfc), UINT32_C(0x099e78),
        UINT32_C(0x0a5600), UINT32_C(0x0a5694), UINT32_C(0x0a6408),
        UINT32_C(0x0a6490), UINT32_C(0x0a663c), UINT32_C(0x0a68f0),
        UINT32_C(0x0a8eac), UINT32_C(0x0a9b54), UINT32_C(0x1c6f70),
        UINT32_C(0x1ced74), UINT32_C(0x1cef58), UINT32_C(0x1cf074),
        UINT32_C(0x1cf258), UINT32_C(0x1cf550), UINT32_C(0x1cfb48),
        UINT32_C(0x1cff3c), UINT32_C(0x1d09b0), UINT32_C(0x1d0bdc),
        UINT32_C(0x1d3db0), UINT32_C(0x1d3ff0), UINT32_C(0x1d3ff8),
        UINT32_C(0x1d433c), UINT32_C(0x1d4688), UINT32_C(0x1d49d0),
    };
    const uint32_t normalized = pc & UINT32_C(0x1fffffff);
    static uint32_t lookup[256];
    static bool lookup_initialized;

    if (!lookup_initialized) {
        for (size_t index = 0u;
             index < sizeof(cutover_pcs) / sizeof(cutover_pcs[0]); ++index) {
            uint32_t slot = ((cutover_pcs[index] >> 2u) *
                             UINT32_C(0x9e3779b1)) & 255u;

            while (lookup[slot] != 0u)
                slot = (slot + 1u) & 255u;
            lookup[slot] = cutover_pcs[index];
        }
        lookup_initialized = true;
    }
    {
        uint32_t slot = ((normalized >> 2u) * UINT32_C(0x9e3779b1)) & 255u;

        for (uint32_t probe = 0u; probe < 256u; ++probe) {
            if (lookup[slot] == 0u) return false;
            if (lookup[slot] == normalized) return true;
            slot = (slot + 1u) & 255u;
        }
    }
    return false;
}

bool psx_xg_render_auth_overlay_cutover_relevant(
    uint32_t pc, uint32_t instruction_word) {
    return xg_render_overlay_cutover_relevant(pc, instruction_word);
}

bool psx_xg_render_auth_native_cutover_post_pc_relevant(uint32_t pc) {
    return physical_address_equals(pc, UINT32_C(0x8004a1ac)) ||
           physical_address_equals(pc, UINT32_C(0x8004a1e8)) ||
           physical_address_equals(pc, UINT32_C(0x8004a274)) ||
           physical_address_equals(pc, UINT32_C(0x8004a2b8));
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
    record->semantic_template =
        PSX_XG_RENDER_FT4_SEMANTIC_DYNAMIC_ACTOR;
    record->tier = ft4_geometry.pending.tier;
    record->source_snapshot = ft4_geometry.pending.source_snapshot;
    record->source_capture_result = ft4_geometry.pending.source_capture_result;
    record->source_captured = ft4_geometry.pending.source_captured;
    record->packet_guest_address =
        guest_address(ft4_geometry.pending.destinations[0] - 8u);
    if (ft4_geometry.pending.shadow_oracle) {
        record->pre_transform = ft4_geometry.pending.pre_transform;
        record->ordering_depth =
            ft4_geometry.pending.host_output.ordering_depth;
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

bool psx_xg_render_auth_resident_ft4_observe(
    CPUState *cpu, uint32_t stage, uint32_t pc, uint32_t instruction_word) {
    XgRenderFt4GeometryPending *pending = &ft4_geometry.pending;

    if (!ft4_geometry.enabled || !pending->valid) return false;
    if (!ft4_context_matches(cpu) ||
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
            if (!physical_address_equals(pending->destinations[3], cpu->gpr[8]) ||
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

bool psx_xg_render_auth_ft4_geometry_pop(
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

void psx_xg_render_auth_ft4_geometry_snapshot(
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

void psx_xg_render_auth_zoom_template_contract_snapshot(
    PsxXgRenderZoomTemplateContractSnapshot *out_snapshot) {
    const XgRenderZoomSource *source = xg_field_zoom_source();
    XgRenderZoomCounters counters = { 0 };

    if (out_snapshot == NULL) return;
    xg_field_zoom_counters_snapshot(&counters);
    *out_snapshot = (PsxXgRenderZoomTemplateContractSnapshot){
        .generation = source->generation,
        .producer_store_pc = source->producer_store_pc,
        .template_count = XG_RENDER_ZOOM_QUAD_COUNT,
        .buffer_count = XG_RENDER_ZOOM_BUFFER_COUNT,
        .opcode = source->command,
        .authenticated = source->valid && source->command == 0x2eu &&
            source->authenticated &&
            source->producer_store_pc == XG_RENDER_ZOOM_TEMPLATE_STORE_PC,
        .initializer_begin_count = counters.initializer_begin_count,
        .initializer_commit_count = counters.initializer_commit_count,
        .initializer_2e_count = counters.initializer_2e_count,
        .rgb_update_count = counters.rgb_update_count,
        .invocation_count = counters.invocation_count,
        .cutover_attempt_count = counters.cutover_attempt_count,
        .native_invocation_count = counters.native_invocation_count,
        .native_primitive_count = counters.native_primitive_count,
        .replay_invocation_count = counters.replay_invocation_count,
        .replay_primitive_count = counters.replay_primitive_count,
        .rejection_count = counters.rejection_count,
        .last_rejection_blocker = counters.last_rejection_blocker,
    };
}

void psx_xg_render_auth_overlay_ft4_snapshot(
    PsxXgRenderOverlayFt4Snapshot *out_snapshot) {
    if (out_snapshot != NULL) *out_snapshot = overlay_ft4_2c;
}

void psx_xg_render_auth_producer_family_enable(bool enabled) {
    psx_xg_render_auth_source_reset();
    producer_family = (PsxXgRenderProducerFamilySnapshot){
        .enabled = enabled,
    };
    ft4_geometry.enabled = enabled;
}

void psx_xg_render_auth_producer_family_snapshot(
    PsxXgRenderProducerFamilySnapshot *out_snapshot) {
    if (out_snapshot != NULL) *out_snapshot = producer_family;
}

void psx_xg_render_auth_projected_lifecycle_snapshot(
    PsxXgRenderProjectedLifecycleSnapshot *out_snapshot) {
    if (out_snapshot != NULL) *out_snapshot = projected_lifecycle;
}

void psx_xg_render_auth_model_ft4_shadow_snapshot(
    PsxXgRenderModelFt4ShadowSnapshot *out_snapshot) {
    if (out_snapshot != NULL) *out_snapshot = model_ft4_shadow.snapshot;
}

void psx_xg_render_auth_model_ft3_shadow_snapshot(
    PsxXgRenderModelFt3ShadowSnapshot *out_snapshot) {
    if (out_snapshot != NULL) {
        *out_snapshot = model_ft3_shadow.snapshot;
        out_snapshot->source_count = model_ft3_source_count;
    }
}

void psx_xg_render_auth_sprite_ft4_shadow_snapshot(
    PsxXgRenderSpriteFt4ShadowSnapshot *out_snapshot) {
    if (out_snapshot != NULL) *out_snapshot = sprite_ft4_shadow.snapshot;
}

void psx_xg_render_auth_field_polyline_snapshot(
    PsxXgRenderFieldPolylineSnapshot *out_snapshot) {
    if (out_snapshot != NULL) *out_snapshot = field_polyline.snapshot;
}

void psx_xg_render_auth_world_horizon_shadow_snapshot(
    PsxXgRenderWorldHorizonShadowSnapshot *out_snapshot) {
    if (out_snapshot != NULL) *out_snapshot = world_horizon_shadow.snapshot;
}

void psx_xg_render_auth_world_effects_shadow_snapshot(
    PsxXgRenderWorldEffectsShadowSnapshot *out_snapshot) {
    if (out_snapshot != NULL) *out_snapshot = world_effects_shadow.snapshot;
}

void psx_xg_render_auth_world_terrain_water_shadow_snapshot(
    PsxXgRenderWorldTerrainWaterShadowSnapshot *out_snapshot) {
    (void)xg_world_terrain_water_shadow_snapshot(out_snapshot);
    if (out_snapshot != NULL) {
        xg_world_terrain_water_build_diagnostics(
            &out_snapshot->build_diagnostics);
        out_snapshot->mesh_duplicate_vertices =
            world_terrain_mesh_duplicate_vertices;
        out_snapshot->mesh_cross_tile_duplicate_vertices =
            world_terrain_mesh_cross_tile_duplicate_vertices;
        out_snapshot->mesh_canonical_raster_conflicts =
            world_terrain_mesh_canonical_raster_conflicts;
        out_snapshot->mesh_native_raster_conflicts =
            world_terrain_mesh_native_raster_conflicts;
        out_snapshot->mesh_cross_tile_native_raster_conflicts =
            world_terrain_mesh_cross_tile_native_raster_conflicts;
    }
    if (out_snapshot != NULL && out_snapshot->native_cutover_count == 0u) {
        out_snapshot->last_caller_return =
            world_terrain_water_native_last_caller;
        out_snapshot->blocker_detail =
            world_terrain_water_native_blocker_detail;
    }
}

void psx_xg_render_auth_world_entity_shadows_shadow_snapshot(
    PsxXgRenderWorldEntityShadowsShadowSnapshot *out_snapshot) {
    (void)xg_world_entity_shadows_shadow_snapshot(out_snapshot);
}

void psx_xg_render_auth_world_decorations_shadow_snapshot(
    PsxXgRenderWorldDecorationsShadowSnapshot *out_snapshot) {
    (void)xg_world_decorations_shadow_snapshot(out_snapshot);
}

void psx_xg_render_auth_world_clouds_shadow_snapshot(
    PsxXgRenderWorldCloudsShadowSnapshot *out_snapshot) {
    xg_world_clouds_shadow_snapshot(out_snapshot);
}

void psx_xg_render_auth_world_minimap_shadow_snapshot(
    PsxXgRenderWorldMinimapShadowSnapshot *out_snapshot) {
    xg_world_minimap_shadow_snapshot(out_snapshot);
}

void psx_xg_render_auth_world_models_native_snapshot(
    PsxXgRenderWorldNativeSnapshot *out_snapshot) {
    if (out_snapshot != NULL) *out_snapshot = world_models_native_snapshot;
}

void psx_xg_render_auth_world_actor_sprites_native_snapshot(
    PsxXgRenderWorldNativeSnapshot *out_snapshot) {
    if (out_snapshot != NULL)
        *out_snapshot = world_actor_sprites_native_snapshot;
}

void psx_xg_render_auth_world_sky_native_snapshot(
    PsxXgRenderWorldNativeSnapshot *out_snapshot) {
    if (out_snapshot != NULL) *out_snapshot = world_sky_native_snapshot;
}

void psx_xg_render_auth_world_execution_snapshot(
    PsxXgRenderWorldExecutionSnapshot *out_snapshot) {
    if (out_snapshot != NULL) *out_snapshot = world_execution;
}

void psx_xg_render_auth_note_code_write(uint64_t previous_generation,
                                          uint64_t current_generation,
                                          uint32_t guest_pc,
                                          uint32_t write_size) {
    if (xg_render_runtime_variant_no_gates_enabled()) return;
    const uint32_t variant_code_write_mask =
        xg_render_runtime_variant_code_write_overlap_mask(
            guest_pc, write_size);
    const uint32_t data_dependency_mask =
        (UINT32_C(1) << PSX_XG_RENDER_CODE_WRITE_MODEL_DISPATCH_DATA) |
        (UINT32_C(1) << PSX_XG_RENDER_CODE_WRITE_SHARED_TRIG_DATA);
    const bool variant_watched_write = variant_code_write_mask != 0u;
    const bool variant_code_write =
        (variant_code_write_mask & ~data_dependency_mask) != 0u;
    const bool artifact_code_write =
        state.authenticated_artifact_candidate_valid &&
        !state.authenticated_artifact_candidate.runtime_variant_bound &&
        current_artifact_code_range_overlaps(guest_pc, write_size);
    const bool protected_auth_code_write =
        protected_code_write_overlaps(guest_pc, write_size,
                                      variant_code_write_mask);
    const bool producer_resource_write =
        producer_resource_write_needs_invalidation(guest_pc, write_size);
    const bool authority_code_write = artifact_code_write ||
        (variant_code_write_mask &
         ((UINT32_C(1) << PSX_XG_RENDER_CODE_WRITE_DESCRIPTOR) |
          (UINT32_C(1) << PSX_XG_RENDER_CODE_WRITE_DIRECT))) != 0u;
    const bool zoom_code_write = authority_code_write ||
        (variant_code_write_mask &
         (UINT32_C(1) << PSX_XG_RENDER_CODE_WRITE_ZOOM)) != 0u;
    const bool projected_code_write = authority_code_write ||
        (variant_code_write_mask &
         (UINT32_C(1) << PSX_XG_RENDER_CODE_WRITE_PROJECTED)) != 0u;
    const bool shared_trig_data_write =
        (variant_code_write_mask &
         (UINT32_C(1) << PSX_XG_RENDER_CODE_WRITE_SHARED_TRIG_DATA)) != 0u;
    const bool model_dispatch_data_write =
        (variant_code_write_mask &
         (UINT32_C(1) <<
          PSX_XG_RENDER_CODE_WRITE_MODEL_DISPATCH_DATA)) != 0u;
    const bool world_sky_code_write = authority_code_write ||
        (variant_code_write_mask &
         (UINT32_C(1) << PSX_XG_RENDER_CODE_WRITE_WORLD_SKY)) != 0u;
    const bool world_horizon_code_write = authority_code_write ||
        (variant_code_write_mask &
         (UINT32_C(1) << PSX_XG_RENDER_CODE_WRITE_WORLD_HORIZON)) != 0u;
    const bool world_effects_code_write = authority_code_write ||
        (variant_code_write_mask &
         (UINT32_C(1) << PSX_XG_RENDER_CODE_WRITE_WORLD_EFFECTS)) != 0u;
    const bool model_ft4_code_write =
        (variant_code_write_mask &
         (UINT32_C(1) << PSX_XG_RENDER_CODE_WRITE_MODEL_FT4)) != 0u;
    const bool sprite_ft4_code_write =
        (variant_code_write_mask &
         (UINT32_C(1) << PSX_XG_RENDER_CODE_WRITE_SPRITE_FT4)) != 0u;

    if (producer_resource_write)
        invalidate_producer_resources_overlapping(guest_pc, write_size);

    if (!variant_watched_write && !artifact_code_write &&
        !protected_auth_code_write && !producer_resource_write) {
        if (state.completed) retire_completed_auth_proof();
        return;
    }

    if (authority_code_write || protected_auth_code_write) {
        if (state.authenticated_artifact_candidate_valid)
            clear_authenticated_artifact_candidate();
        state.authenticated_producer_entry = 0u;
        state.authenticated_producer_scene_generation = 0u;
    }
    if (variant_watched_write || artifact_code_write) {
        const uint32_t code_write_mask = variant_code_write_mask |
            (artifact_code_write
                 ? UINT32_C(1) << PSX_XG_RENDER_CODE_WRITE_ARTIFACT : 0u);

        if (projected_lifecycle.code_write_reset_count == 0u) {
            projected_lifecycle.first_code_write_address =
                guest_address(guest_pc);
            projected_lifecycle.first_code_write_size = write_size;
            projected_lifecycle.first_code_write_mask = code_write_mask;
        }
        projected_lifecycle.last_code_write_address = guest_address(guest_pc);
        projected_lifecycle.last_code_write_size = write_size;
        projected_lifecycle.last_code_write_mask = code_write_mask;
        for (uint32_t code_class = 0u;
             code_class < PSX_XG_RENDER_CODE_WRITE_CLASS_COUNT; ++code_class) {
            if (code_write_mask & (UINT32_C(1) << code_class)) {
                if (projected_lifecycle.code_write_class_counts[code_class] ==
                        0u)
                    projected_lifecycle
                        .code_write_class_first_address[code_class] =
                            guest_address(guest_pc);
                projected_lifecycle
                    .code_write_class_last_address[code_class] =
                        guest_address(guest_pc);
                ++projected_lifecycle.code_write_class_counts[code_class];
            }
        }
        if (variant_code_write || artifact_code_write)
            advance_interpolation_scene();
        abort_standalone_submission();
        if (authority_code_write || shared_trig_data_write)
            clear_particle_sources();
        if (zoom_code_write) clear_zoom_source();
        ++projected_lifecycle.code_write_reset_count;
        if (projected_code_write) clear_projected_source();
        if (authority_code_write) {
            clear_f4_sources();
            retain_resident_residual_templates();
            if (overlay_ft4_state.count != 0u)
                overlay_ft4_state.count = 0u;
        }
        if (model_ft4_code_write) {
            if (model_ft4_shadow.context.valid ||
                model_ft4_shadow.snapshot.pending)
                block_model_ft4_shadow(76u);
            else
                clear_model_ft4_shadow_pending();
            if (model_ft3_shadow.snapshot.pending)
                block_model_ft3_shadow(76u);
            else
                clear_model_ft3_shadow_pending();
        } else if (model_dispatch_data_write) {
            clear_model_ft4_shadow_pending();
            clear_model_ft3_shadow_pending();
        }
        if (model_ft4_code_write) {
            clear_model_ft4_sources();
            clear_model_ft3_sources();
            invalidate_world_model_templates();
        } else if (model_dispatch_data_write) {
            invalidate_world_model_templates();
        }
        if (sprite_ft4_code_write) {
            if (sprite_ft4_shadow.snapshot.context_active ||
                sprite_ft4_shadow.snapshot.pending)
                block_sprite_ft4_shadow(87u);
            else
                clear_sprite_ft4_shadow_context();
            if (sprite_ft4_shadow.snapshot.field_builder_pending)
                block_field_sprite_builder(7u);
            else
                clear_field_sprite_builder();
        } else if (shared_trig_data_write) {
            clear_sprite_ft4_shadow_context();
            clear_field_sprite_builder();
        }
        if (sprite_ft4_code_write || shared_trig_data_write)
            clear_field_sprite_templates();
        if (authority_code_write) {
            if (field_polyline.snapshot.pending)
                block_field_polyline(11u);
            else
                clear_field_polyline_pending();
        }
        if (world_horizon_code_write) {
            if (world_horizon_shadow.snapshot.pending)
                block_world_horizon_shadow(99u);
            else
                clear_world_horizon_shadow_pending();
        } else if (shared_trig_data_write) {
            clear_world_horizon_shadow_pending();
        }
        if (world_effects_code_write) {
            if (world_effects_shadow.snapshot.pending)
                block_world_effects_shadow(105u);
            else
                clear_world_effects_shadow_pending();
        } else if (shared_trig_data_write) {
            clear_world_effects_shadow_pending();
        }
        if (world_sky_code_write || world_horizon_code_write ||
            world_effects_code_write || shared_trig_data_write)
            invalidate_world_semantic_shadows();
    }
    if (state.completed) {
        retire_completed_auth_proof();
        return;
    }
    if (!state.active) {
        if (!variant_code_write)
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
            xg_render_runtime_variant_reset();
        }
        return;
    }
    if (!protected_auth_code_write) return;
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
    if (xg_render_runtime_variant_no_gates_enabled()) return;
    if (field_range_contains(pc) || current_artifact_range_contains_pc(pc)) {
        advance_interpolation_scene();
        clear_authenticated_artifact_candidate();
        state.authenticated_producer_entry = 0u;
        state.authenticated_producer_scene_generation = 0u;
        clear_particle_sources();
        clear_zoom_source();
        ++projected_lifecycle.loader_reset_count;
        clear_projected_source();
        clear_field_sprite_templates();
        retain_resident_residual_templates();
        overlay_ft4_state.count = 0u;
        clear_f4_sources();
        clear_model_ft4_sources();
        clear_model_ft3_sources();
        clear_world_horizon_shadow_pending();
        clear_world_effects_shadow_pending();
        /* A rehash miss demotes compiled host code to authoritative guest
         * execution. It aborts the active proof, not resources observed from
         * that guest execution; code writes own their invalidation separately. */
        abort_active(XG_RENDER_AUTH_REJECT_VALIDATION_MISMATCH,
                     PSX_XG_RENDER_AUTH_REJECTION_SOURCE_LOADER_MISMATCH,
                     false, PSX_XG_RENDER_AUTH_HOOK_ENTRY, pc);
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

void psx_xg_render_auth_note_artifact_candidate(
    const PsxXgRenderAuthCandidate *candidate) {
    bool candidate_matches;

    if (candidate == NULL) return;
    candidate_matches =
        xg_render_runtime_variant_artifact_candidate_matches(candidate) ||
        (!candidate->runtime_variant_bound &&
         xg_render_authoritative_overlay_artifact_candidate_matches(candidate));
    if (!candidate_matches) {
        if (current_artifact_memory_contains_pc(candidate->dispatch_pc) &&
            !artifact_binary_identity_matches(
                &state.authenticated_artifact_candidate, candidate)) {
            invalidate_nonresident_producer_resources();
            clear_authenticated_artifact_candidate();
            state.authenticated_producer_entry = 0u;
            state.authenticated_producer_scene_generation = 0u;
        }
        return;
    }
    if (state.authenticated_artifact_candidate_valid &&
        state.authenticated_artifact_scene_generation == state.scene_generation &&
        artifact_binary_identity_matches(
            &state.authenticated_artifact_candidate, candidate)) {
        state.authenticated_artifact_candidate = *candidate;
        return;
    }
    if (state.authenticated_artifact_generation == UINT64_MAX) {
        clear_authenticated_artifact_candidate();
        state.authenticated_producer_entry = 0u;
        state.authenticated_producer_scene_generation = 0u;
        return;
    }
    if (state.authenticated_artifact_candidate_valid) {
        invalidate_nonresident_producer_resources();
        state.authenticated_producer_entry = 0u;
        state.authenticated_producer_scene_generation = 0u;
    }
    ++state.authenticated_artifact_generation;
    state.authenticated_artifact_candidate = *candidate;
    state.authenticated_artifact_candidate_valid = true;
    state.authenticated_artifact_scene_generation = state.scene_generation;
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

    psx_xg_render_auth_note_artifact_candidate(candidate);
    clear_pending_candidate();
    if (candidate == NULL || state.active || !state.armed)
        return;
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
    if (out_instrumentation == NULL) return;
    lock_instrumentation();
    *out_instrumentation = instrumentation;
    unlock_instrumentation();
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
    out_snapshot->modes.requested_timing_mode = state.requested_timing_mode;
    out_snapshot->modes.effective_timing_mode = state.requested_timing_mode;
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
            out_snapshot->modes.requested_timing_mode =
                state.requested_timing_mode;
            out_snapshot->modes.effective_timing_mode =
                state.requested_timing_mode;
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

#ifdef PSX_XG_RENDER_AUTH_RUNTIME_TESTING
bool psx_xg_render_auth_runtime_test_materialize_world_models_original(
    CPUState *cpu) {
    XgRenderWorldModelsNativeState *workspace = &world_models_native_state;
    const XgWorldModelsNativePreparation *preparation =
        &workspace->preparation;
    const XgWorldModelsNativeCommit *expected = &workspace->expected_commit;
    uint32_t index;

    if (!workspace->valid || workspace->owner_cpu != cpu || cpu == NULL ||
        cpu->write_word == NULL || cpu->write_half == NULL ||
        workspace->entry_stack_pointer < 0x40u)
        return false;
    cpu->gpr[29] = workspace->entry_stack_pointer - 0x40u;
    cpu->write_word(cpu->gpr[29] + 0x38u, expected->continuation_pc);
    for (index = 0u; index < 3u; ++index) {
        cpu->write_word(
            XG_WORLD_MODELS_SCALE_X_SCRATCH + index * 4u,
            expected->entry_side_effects.scratch_scale[index]);
        cpu->write_half(
            XG_WORLD_MODELS_COARSE_ORIGIN_SCRATCH + index * 2u,
            (uint16_t)expected->entry_side_effects.coarse_origin[index]);
    }
    cpu->write_word(
        XG_WORLD_MODELS_RESIDENT_CULL_MODE_GLOBAL,
        expected->entry_side_effects.resident_cull_mode);
    cpu->write_word(
        XG_WORLD_MODELS_RESIDENT_VERTEX_TOTAL_GLOBAL,
        expected->resident_vertex_total);
    cpu->write_word(
        XG_WORLD_MODELS_RESIDENT_EMITTED_COUNT_GLOBAL,
        expected->resident_emitted_count);
    for (index = 0u; index < preparation->transform_node_count; ++index) {
        const XgWorldModelsNodeSideEffect *effect =
            &workspace->node_side_effects[index];
        uint32_t component;

        for (component = 0u; component < 3u; ++component)
            cpu->write_word(
                effect->guest_address +
                    XG_WORLD_MODELS_NODE_WRITEBACK_X_OFFSET + component * 4u,
                (uint32_t)effect->translation[component]);
    }
    for (index = 0u; index < preparation->primitive_count; ++index) {
        const XgWorldModelsNativePrimitiveSource *source =
            &workspace->primitives[index];
        const XgWorldModelsNativePrimitiveOutput *output =
            &workspace->outputs[index];
        uint32_t word;

        for (word = 0u; word < source->packet_word_count; ++word) {
            if ((output->packet_word_write_mask & (UINT32_C(1) << word)) != 0u)
                cpu->write_word(
                    source->packet_address + word * 4u,
                    output->packet_words[word]);
        }
    }
    for (index = 0u; index < XG_WORLD_MODELS_OT_BUCKET_COUNT; ++index) {
        if (workspace->ot_touched[index])
            cpu->write_word(
                expected->resident_ot_base + index * 4u,
                workspace->ot_heads[index]);
    }
    if (expected->resident_dispatch_globals_written) {
        cpu->write_word(XG_WORLD_MODELS_RESIDENT_PACKET_CURSOR_GLOBAL,
                        expected->resident_packet_cursor);
        cpu->write_word(XG_WORLD_MODELS_RESIDENT_GROUP_CURSOR_GLOBAL,
                        expected->resident_group_cursor);
        cpu->write_word(XG_WORLD_MODELS_RESIDENT_MODEL_0C_GLOBAL,
                        expected->resident_model_0c);
        cpu->write_word(XG_WORLD_MODELS_RESIDENT_VERTEX_BASE_GLOBAL,
                        expected->resident_vertex_base);
        cpu->write_word(XG_WORLD_MODELS_RESIDENT_OT_BASE_GLOBAL,
                        expected->resident_ot_base);
        cpu->write_word(XG_WORLD_MODELS_RESIDENT_MODEL_18_GLOBAL,
                        expected->resident_model_18);
    }
    return true;
}

bool psx_xg_render_auth_runtime_test_materialize_world_actor_original(
    CPUState *cpu) {
    XgRenderWorldActorSpritesNativeState *workspace =
        &world_actor_sprites_native_state;
    const XgWorldActorSpritesNativePreparation *preparation =
        &workspace->preparation;
    uint32_t index;

    if (!workspace->valid || workspace->owner_cpu != cpu || cpu == NULL ||
        cpu->write_word == NULL || cpu->write_half == NULL)
        return false;
    if (preparation->packet_cursor_written)
        cpu->write_word(XG_WORLD_ACTOR_SPRITES_PACKET_CURSOR,
                        preparation->final_packet_cursor);
    for (index = 0u; index < preparation->record_count; ++index) {
        const XgWorldActorSpriteRecord *record = &workspace->records[index];
        uint32_t payload;

        cpu->write_word(record->packet_address, workspace->packet_tags[index]);
        for (payload = 0u;
             payload < XG_WORLD_ACTOR_SPRITE_PACKET_PAYLOAD_WORD_COUNT;
             ++payload)
            cpu->write_word(
                record->packet_address + 4u + payload * 4u,
                workspace->payload_words[index][payload]);
    }
    for (index = 0u; index < XG_WORLD_ACTOR_SPRITE_OT_BUCKET_COUNT; ++index) {
        if (workspace->ot_touched[index])
            cpu->write_word(workspace->ot_base + index * 4u,
                            workspace->ot_heads[index]);
    }
    if (preparation->body_scratch.written) {
        for (index = 0u; index < XG_HOST_3D_VERTEX_COUNT; ++index) {
            const XgHost3dVector *vertex =
                &preparation->body_scratch.vertices[index];
            const uint32_t address =
                preparation->body_scratch.address + index * 8u;

            cpu->write_half(address, (uint16_t)vertex->x);
            cpu->write_half(address + 2u, (uint16_t)vertex->y);
            cpu->write_half(address + 4u, (uint16_t)vertex->z);
            cpu->write_half(address + 6u, vertex->pad);
        }
    }
    if (preparation->shadow_scratch.written) {
        for (index = 0u; index < XG_HOST_3D_VERTEX_COUNT; ++index) {
            const XgHost3dVector *vertex =
                &preparation->shadow_scratch.vertices[index];
            const uint32_t address =
                preparation->shadow_scratch.address + index * 8u;

            cpu->write_half(address, (uint16_t)vertex->x);
            cpu->write_half(address + 2u, (uint16_t)vertex->y);
            cpu->write_half(address + 4u, (uint16_t)vertex->z);
            cpu->write_half(address + 6u, vertex->pad);
        }
    }
    return true;
}

#ifdef PSX_XG_RENDER_AUTH_RUNTIME_TESTING
uint64_t psx_xg_render_auth_runtime_test_interpolation_scene(void) {
    return interpolation_scene_generation();
}

uint64_t psx_xg_render_auth_runtime_test_artifact_generation(void) {
    return state.authenticated_artifact_generation;
}

bool psx_xg_render_auth_runtime_test_artifact_active(void) {
    return current_artifact_is_authorized();
}

uint32_t psx_xg_render_auth_runtime_test_pre_scene_count(void) {
    return pre_scene.count;
}

bool psx_xg_render_auth_runtime_test_resource_write_may_overlap(
        uint32_t address, uint32_t size) {
    return producer_resource_write_may_overlap(address, size);
}

uint64_t psx_xg_render_auth_runtime_test_particle_generation(
        uint32_t particle_address) {
    const XgRenderParticleSource *source =
        xg_field_particles_find(particle_address);

    return source != NULL ? source->generation : 0u;
}

bool psx_xg_render_auth_runtime_test_model_ft4_packet_template_present(
        uint32_t packet_address) {
    return model_ft4_template_find_current_in(
        model_ft4_templates, packet_address, false) != NULL;
}

bool psx_xg_render_auth_runtime_test_model_ft4_descriptor_template_present(
        uint32_t descriptor_address) {
    return model_ft4_template_find_current_in(
        model_ft4_descriptor_templates, descriptor_address, true) != NULL;
}

void psx_xg_render_auth_runtime_test_watch_resource(
        uint32_t address, uint32_t size) {
    watch_producer_resource(address, size);
}
#endif

void psx_xg_render_auth_runtime_test_reset(void) {
    guest_render_native_stream_set_enabled(false);
    end_gte_attribution_producer();
    if (state.auth != NULL) (void)xg_render_auth_scene_reset(state.auth);
    state = (XgRenderAuthRuntimeState){
        .armed = true,
        .scene_generation = 1u,
        .interpolation_scene_generation = 1u,
        .requested_timing_mode = GUEST_RENDER_TIMING_ORIGINAL,
        .requested_render_mode = GUEST_RENDER_RENDER_ORIGINAL,
    };
    gpu_native_interpolation_scene_boundary(interpolation_scene_generation());
    (void)xg_native_view_configure(&native_view, false, 0u, 0u, 0u, 0u);
    source_state = (XgRenderSourceState){
        .aggregate = { .next_sequence = 1u },
        .next_auth_sequence = 1u,
    };
    ui_ot_pending = false;
    ui_ot_pending_frame = 0u;
    ui_ot_visual_sequence = 1u;
    ui_ot_snapshot = (PsxXgRenderUiOtSnapshot){ 0 };
    reset_render_transaction();
    reset_ft4_geometry(false);
    clear_pre_scene();
    clear_standalone_submission();
    clear_particle_sources();
    clear_zoom_source();
    clear_projected_source();
    clear_model_ft4_sources();
    model_ft4_shadow = (XgRenderModelFt4ShadowState){ 0 };
    invalidate_world_model_templates();
    clear_model_ft3_sources();
    sprite_ft4_shadow = (XgRenderSpriteFt4ShadowState){ 0 };
    field_sprite_builder = (XgRenderFieldSpriteBuilderState){ 0 };
    field_sprite_templates = (XgRenderFieldSpriteTemplateState){ 0 };
    clear_residual_templates();
    clear_f4_sources();
    clear_resident_line_f2_source();
    next_resource_generation = 1u;
    memset(native_resolve_hints, 0, sizeof(native_resolve_hints));
    world_horizon_shadow = (XgRenderWorldHorizonShadowState){ 0 };
    world_effects_shadow = (XgRenderWorldEffectsShadowState){ 0 };
    clear_world_models_native_pending();
    world_terrain_water_native_state =
        (XgRenderWorldTerrainWaterNativeState){0};
    memset(world_terrain_water_temporal_tiles, 0,
           sizeof(world_terrain_water_temporal_tiles));
    world_terrain_water_temporal_scene = 0u;
    world_terrain_water_native_last_caller = 0u;
    world_terrain_water_native_blocker_detail = 0u;
    world_entity_shadows_native_state =
        (XgRenderWorldEntityShadowsNativeState){0};
    world_decorations_native_state =
        (XgRenderWorldDecorationsNativeState){0};
    clear_world_clouds_native_pending();
    world_actor_sprites_native_state =
        (XgRenderWorldActorSpritesNativeState){0};
    clear_world_actor_context();
    world_models_native_snapshot = (PsxXgRenderWorldNativeSnapshot){0};
    world_actor_sprites_native_snapshot =
        (PsxXgRenderWorldNativeSnapshot){0};
    world_sky_native_snapshot = (PsxXgRenderWorldNativeSnapshot){0};
    xg_world_terrain_water_shadow_reset();
    xg_world_entity_shadows_shadow_reset();
    xg_world_decorations_shadow_reset();
    xg_world_clouds_shadow_reset();
    xg_world_minimap_shadow_reset();
    world_execution = (PsxXgRenderWorldExecutionSnapshot){ 0 };
    world_execution_active = false;
    world_native_cutover_in_progress = false;
    world_native_cutover_failed = false;
    producer_family = (PsxXgRenderProducerFamilySnapshot){ 0 };
    completed_proof = (PsxXgRenderAuthCompletedProofReceipt){ 0 };
    completed_proof_scene_generation = 0u;
    instrumentation = (PsxXgRenderAuthInstrumentation){ .revision = 1u };
    instrumentation_next_sequence = 1u;
    particle_test_primitive = (XgRenderIrNativePrimitive){ 0 };
    particle_test_primitive_valid = false;
    memset(zoom_test_primitives, 0, sizeof(zoom_test_primitives));
    zoom_test_primitive_count = 0u;
    memset(projected_test_primitives, 0,
           sizeof(projected_test_primitives));
    projected_test_primitive_count = 0u;
    projected_lifecycle = (PsxXgRenderProjectedLifecycleSnapshot){ 0 };
    memset(producer_resource_watch_bitmap, 0,
           sizeof(producer_resource_watch_bitmap));
    memset(producer_resource_invalidated_byte_masks, 0,
           sizeof(producer_resource_invalidated_byte_masks));
    overlay_ft4_2c = (PsxXgRenderOverlayFt4Snapshot){ 0 };
    overlay_projected_2e_descriptor_scope = false;
    overlay_ft4_state.count = 0u;
    xg_render_runtime_variant_reset();
}

bool psx_xg_render_auth_particle_test_primitive(
    XgRenderIrNativePrimitive *out_primitive) {
    if (out_primitive == NULL || !particle_test_primitive_valid) return false;
    *out_primitive = particle_test_primitive;
    return true;
}

bool psx_xg_render_auth_particle_test_source_present(
    uint32_t particle_address) {
    return find_particle_source(particle_address) != NULL;
}

bool psx_xg_render_auth_projected_test_source_present(
    uint32_t object_address) {
    return xg_field_projected_find(object_address) != NULL;
}

uint32_t psx_xg_render_auth_zoom_test_primitives(
    XgRenderIrNativePrimitive *out_primitives, uint32_t capacity) {
    uint32_t count = zoom_test_primitive_count;

    if (out_primitives == NULL || capacity < count) return 0u;
    memcpy(out_primitives, zoom_test_primitives,
           count * sizeof(zoom_test_primitives[0]));
    return count;
}

uint32_t psx_xg_render_auth_projected_test_primitives(
    XgRenderIrNativePrimitive *out_primitives, uint32_t capacity) {
    uint32_t count = projected_test_primitive_count;

    if (out_primitives == NULL || capacity < count) return 0u;
    memcpy(out_primitives, projected_test_primitives,
           count * sizeof(projected_test_primitives[0]));
    return count;
}
#endif

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
