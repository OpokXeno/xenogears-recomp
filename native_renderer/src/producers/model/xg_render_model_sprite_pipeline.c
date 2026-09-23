#include "xg_render_model_sprite_pipeline.h"
#include "xg_model_primitive_layout.h"

#include "gpu.h"
#include "xg_field_render_services.h"
#include "xg_model_ft4_raw.h"
#include "xg_render_gear_motion.h"
#include "xg_render_backend.h"
#include "xg_render_depth_policy.h"
#include "xg_render_array.h"
#include "xg_render_field_sprite.h"
#include "xg_render_manifest_generated.h"
#include "xg_render_primitive_utils.h"
#include "xg_render_runtime_variant_auth.h"
#include "xg_render_battle_geometry.h"
#include "xg_sprite_ft4.h"

#include <math.h>
#include <limits.h>
#include <stddef.h>
#include <string.h>

enum {
    MODEL_PRIMITIVE_MAXIMUM = UINT16_MAX,
    SPRITE_CAPACITY = 64u,
    MODEL_PRECONDITION_CONTEXT = 1u << 0,
    MODEL_PRECONDITION_CPU = 1u << 1,
    MODEL_PRECONDITION_CALLBACKS = 1u << 2,
    MODEL_PRECONDITION_RETURN = 1u << 3,
    MODEL_PRECONDITION_TARGET_ZERO = 1u << 4,
    MODEL_PRECONDITION_TARGET_CAPACITY = 1u << 5,
    MODEL_PRECONDITION_VERTEX_BASE = 1u << 6,
    MODEL_PRECONDITION_OT_BASE = 1u << 7,
    FT4_PAYLOAD_MATERIAL = 1u << 0,
    FT4_PAYLOAD_UV0 = 1u << 1,
    FT4_PAYLOAD_TPAGE = 1u << 5,
    FT4_PAYLOAD_CLUT = 1u << 6,
    MODEL_DISPATCH_CALLER_BATTLE = XG_RENDER_MODEL_DISPATCH_CALLER_GEAR_HELPER + 1u,
};

typedef struct ModelContext {
    XgHost3dProjection projection;
    XgRenderMotionRef motion;
    XgRenderMotionRef projection_motion;
    uint32_t motion_part;
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
} ModelContext;

typedef struct GearHelperMode1Proof {
    uint32_t caller_window_start;
    uint32_t return_address;
    bool armed;
} GearHelperMode1Proof;

typedef struct ModelFt4Record {
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
} ModelFt4Record;

typedef struct ModelFt4State {
    ModelFt4Record *records;
    uint32_t capacity;
    ModelContext context;
    PsxXgRenderModelFt4ShadowSnapshot snapshot;
    uint32_t initial_packet_cursor;
    uint32_t initial_counter;
    uint32_t expected_counter_delta;
    uint32_t descriptor_base;
    uint32_t count;
    bool pre_scene_staged;
} ModelFt4State;

typedef struct ModelFt3Record {
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
} ModelFt3Record;

typedef struct ModelFt3State {
    ModelFt3Record *records;
    uint32_t capacity;
    PsxXgRenderModelFt3ShadowSnapshot snapshot;
    uint32_t initial_packet_cursor;
    uint32_t initial_counter;
    uint32_t expected_counter_delta;
    uint32_t descriptor_base;
    uint32_t count;
} ModelFt3State;

typedef struct SpriteStageRecord {
    XgRenderIrNativePrimitive primitive;
    uint32_t packet_address;
    uint32_t source_primitive_index;
    uint32_t ot_bucket;
    uint32_t interpolation_producer_id;
    uint32_t interpolation_primitive_id;
    uint8_t payload_word_count;
    bool interpolation_identity_valid;
} SpriteStageRecord;

typedef enum SpritePhase {
    SPRITE_IDLE = 0,
    SPRITE_EXPECT_XY,
    SPRITE_EXPECT_MATERIAL,
} SpritePhase;

typedef struct SpriteState {
    XgSpriteFt4Record native;
    XgRenderMotionRef motion;
    XgRenderMotionDrawBinding motion_binding;
    XgHost3dProjection motion_projection;
    bool motion_attempted;
    SpriteStageRecord native_records[SPRITE_CAPACITY];
    XgRenderProducerLifecycle native_lifecycles[SPRITE_CAPACITY];
    uint8_t native_opcodes[SPRITE_CAPACITY];
    PsxXgRenderSpriteFt4ShadowSnapshot snapshot;
    uint32_t sprite_address;
    uint32_t packet_address;
    uint32_t descriptor_address;
    uint32_t material_word;
    uint16_t tpage;
    uint16_t clut;
    uint8_t uv[4][2];
    SpritePhase phase;
    uint32_t native_record_count;
    bool geometry_matches;
    bool payload_matches;
    bool invocation_matches;
    bool wrapper_scope;
} SpriteState;

static ModelFt4State model_ft4;
static ModelFt3State model_ft3;
static SpriteState sprite_ft4;
static struct {
    XgRenderTemporalSample *samples;
    XgRenderTemporalCommandBinding *bindings;
    uint32_t sample_capacity, binding_capacity, vertex_count, binding_count;
    uint64_t geometry_digest;
    bool complete;
} model_coverage;
static struct { uint64_t published, rejected; } model_coverage_diagnostics;
/* Guest-owned scratch, invalidated at every model capture. Shared mesh vertices
 * have the same projection throughout that capture, including deformed locals;
 * no camera, pose or vertex result survives into the next invocation. */
#define MODEL_PROJECTION_CACHE_CAPACITY 256u
static struct {
    uint32_t vertex_ids[MODEL_PROJECTION_CACHE_CAPACITY];
    XgHost3dVector locals[MODEL_PROJECTION_CACHE_CAPACITY];
    XgHost3dProjectedVertex projected[MODEL_PROJECTION_CACHE_CAPACITY];
} model_projection_cache;
static GearHelperMode1Proof gear_helper_mode1_proof;

typedef struct XgRenderModelSpriteStageRequest {
    const XgRenderIrNativePrimitive *primitive;
    const GpuRenderTemporalCullPolicy *temporal_cull;
    uint32_t packet_address;
    uint32_t source_primitive_index;
    uint32_t ot_bucket;
    uint32_t interpolation_producer_id;
    uint32_t interpolation_primitive_id;
    uint8_t payload_word_count;
    bool interpolation_identity_valid;
    bool temporal_only;
} XgRenderModelSpriteStageRequest;

typedef struct XgRenderModelSpriteRange {
    uint32_t start;
    uint32_t size;
} XgRenderModelSpriteRange;

static const XgRenderModelSpriteRange model_code_ranges[] = {
    { UINT32_C(0x800257b0), 0x2cu },
    { UINT32_C(0x8002c700), 0x4ea0u },
    { UINT32_C(0x80043a1c), 0x54u },
    { UINT32_C(0x80043c24), 0x50u },
    { UINT32_C(0x8004a19c), 0x104u },
    { UINT32_C(0x8004a7bc), 0x7cu },
};

static const XgRenderModelSpriteRange sprite_code_ranges[] = {
    { UINT32_C(0x8001e148), 0x150u },
    { UINT32_C(0x8001e298), 0x060u },
    { UINT32_C(0x8001e3d8), 0x5e4u },
    { UINT32_C(0x8001e9bc), 0x4acu },
    { UINT32_C(0x8001f6b0), 0x0a0u },
    { UINT32_C(0x80024ff4), 0x050u },
    { UINT32_C(0x8002675c), 0x2b0u },
    { UINT32_C(0x8003f738), 0x178u },
    { UINT32_C(0x8004974c), 0x124u },
    { UINT32_C(0x8004987c), 0x110u },
    { UINT32_C(0x80049cec), 0x050u },
    { UINT32_C(0x80049efc), 0x030u },
    { UINT32_C(0x80049f8c), 0x020u },
    { UINT32_C(0x8004a73c), 0x078u },
    { UINT32_C(0x8004a7bc), 0x07cu },
};

static const XgRenderModelSpriteRange model_data_ranges[] = {
    { UINT32_C(0x8004fe50), 0x2a8u },
};

static bool write_overlaps_ranges(
        const XgRenderModelSpriteRange *ranges, uint32_t range_count,
        uint32_t address, uint32_t size) {
    const uint64_t begin = address & UINT32_C(0x1fffffff);
    const uint64_t end = begin + size;

    if (size == 0u) return false;
    for (uint32_t index = 0u; index < range_count; ++index) {
        const uint64_t range_begin =
            ranges[index].start & UINT32_C(0x1fffffff);
        if (range_begin < end && begin < range_begin + ranges[index].size)
            return true;
    }
    return false;
}

void xg_render_model_sprite_pipeline_classify_code_write(
        uint32_t address, uint32_t size,
        XgRenderMutationClassification *out_classification) {
    uint32_t mask = 0u;
    bool executable_mutation = false;
    bool shared_data_mutation = false;

    if (write_overlaps_ranges(
            model_code_ranges,
            (uint32_t)(sizeof(model_code_ranges) /
                       sizeof(model_code_ranges[0])), address, size)) {
        mask |= UINT32_C(1) << PSX_XG_RENDER_CODE_WRITE_MODEL_FT4;
        executable_mutation = true;
    }
    if (write_overlaps_ranges(
            sprite_code_ranges,
            (uint32_t)(sizeof(sprite_code_ranges) /
                        sizeof(sprite_code_ranges[0])), address, size)) {
        mask |= UINT32_C(1) << PSX_XG_RENDER_CODE_WRITE_SPRITE_FT4;
        executable_mutation = true;
    }
    if (write_overlaps_ranges(
            model_data_ranges,
            (uint32_t)(sizeof(model_data_ranges) /
                        sizeof(model_data_ranges[0])), address, size)) {
        mask |= UINT32_C(1) << PSX_XG_RENDER_CODE_WRITE_MODEL_DISPATCH_DATA;
        shared_data_mutation = true;
    }
    if (out_classification == NULL) return;
    *out_classification = (XgRenderMutationClassification){
        .properties = {
            .watched_range_mutation = mask != 0u,
            .runtime_variant_mutation = executable_mutation,
            .executable_mutation = executable_mutation,
            .shared_data_mutation = shared_data_mutation,
            .authentication_mutation = executable_mutation,
            .authority_loss = executable_mutation,
            .interpolation_reset = executable_mutation,
            .reset_runtime_variant = executable_mutation,
        },
        .code_write_mask = mask,
    };
}

static void register_ranges(
        const XgRenderModelSpriteRange *ranges, uint32_t range_count,
        void (*set_range)(uint32_t physical_address, uint32_t size)) {
    for (uint32_t index = 0u; index < range_count; ++index)
        set_range(ranges[index].start & UINT32_C(0x1fffffff),
                  ranges[index].size);
}

void xg_render_model_sprite_pipeline_register_code_watches(
        void (*set_range)(uint32_t physical_address, uint32_t size)) {
    if (set_range == NULL) return;
    register_ranges(model_code_ranges,
        (uint32_t)(sizeof(model_code_ranges) / sizeof(model_code_ranges[0])),
        set_range);
    register_ranges(sprite_code_ranges,
        (uint32_t)(sizeof(sprite_code_ranges) / sizeof(sprite_code_ranges[0])),
        set_range);
    register_ranges(model_data_ranges,
        (uint32_t)(sizeof(model_data_ranges) / sizeof(model_data_ranges[0])),
        set_range);
}

static bool physical_address_equals(uint32_t left, uint32_t right) {
    return (left & UINT32_C(0x1fffffff)) ==
        (right & UINT32_C(0x1fffffff));
}

bool xg_render_model_sprite_pipeline_resident_lifecycle_pc(uint32_t pc) {
    static const uint32_t lifecycle_pcs[] = {
        UINT32_C(0x8001e874),
        UINT32_C(0x8002c700),
        UINT32_C(0x8002d100),
        UINT32_C(0x8002da00),
    };

    for (uint32_t index = 0u;
         index < sizeof(lifecycle_pcs) / sizeof(lifecycle_pcs[0]); ++index)
        if (physical_address_equals(pc, lifecycle_pcs[index])) return true;
    return false;
}

static const uint32_t model_dispatch_resident_caller_instructions[] = {
    UINT32_C(0x3c068006), UINT32_C(0x8cc6956c),
    UINT32_C(0x8e020020), UINT32_C(0x8f830188),
    UINT32_C(0x96070042), UINT32_C(0x00031880),
    UINT32_C(0x00621821), UINT32_C(0x8c440034),
    UINT32_C(0x8c65002c), UINT32_C(0x0c00b1c0),
    UINT32_C(0x30e70004),
};

static const uint32_t model_dispatch_gear_helper_caller_instructions[][13] = {
    {
        UINT32_C(0x8fa6004c), UINT32_C(0x8fa80050),
        UINT32_C(0x8ee30000), UINT32_C(0x00081080),
        UINT32_C(0x00511021), UINT32_C(0x8c450068),
        UINT32_C(0x96420000), UINT32_C(0x8fa70048),
        UINT32_C(0x00021080), UINT32_C(0x00431021),
        UINT32_C(0x8c440000), UINT32_C(0x0c00b1c0),
        UINT32_C(0x00000000),
    },
    {
        UINT32_C(0x8fa600f4), UINT32_C(0x8faa00f8),
        UINT32_C(0x8fc30000), UINT32_C(0x000a1080),
        UINT32_C(0x00521021), UINT32_C(0x8c450068),
        UINT32_C(0x96220000), UINT32_C(0x8fa70070),
        UINT32_C(0x00021080), UINT32_C(0x00431021),
        UINT32_C(0x8c440000), UINT32_C(0x0c00b1c0),
        UINT32_C(0x00000000),
    },
};

static const uint32_t model_dispatch_gear_helper_returns[] = {
    UINT32_C(0x801dcd48), UINT32_C(0x801dd43c),
};

static bool model_dispatch_instruction_window_matches(
        CPUState *cpu, uint32_t start, const uint32_t *instructions,
        uint32_t instruction_count) {
    if (cpu == NULL || cpu->read_word == NULL || instructions == NULL ||
        instruction_count == 0u)
        return false;
    for (uint32_t index = 0u; index < instruction_count; ++index)
        if (cpu->read_word(start + index * 4u) != instructions[index])
            return false;
    return true;
}

static bool model_dispatch_gear_helper_window_matches(
        CPUState *cpu, uint32_t return_address,
        uint32_t *out_window_start) {
    const uint32_t instruction_count = (uint32_t)(
        sizeof(model_dispatch_gear_helper_caller_instructions[0]) /
        sizeof(model_dispatch_gear_helper_caller_instructions[0][0]));

    for (uint32_t index = 0u;
         index < sizeof(model_dispatch_gear_helper_returns) /
             sizeof(model_dispatch_gear_helper_returns[0]); ++index) {
        const uint32_t window_start =
            return_address - instruction_count * 4u;

        if (!physical_address_equals(
                return_address, model_dispatch_gear_helper_returns[index]) ||
            !model_dispatch_instruction_window_matches(
                cpu, window_start,
                model_dispatch_gear_helper_caller_instructions[index],
                instruction_count))
            continue;
        if (out_window_start != NULL) *out_window_start = window_start;
        return true;
    }
    return false;
}

static bool normalized_range_contains(
        uint32_t start, uint32_t size, uint32_t address,
        uint32_t address_size) {
    const uint64_t range_start = start & UINT32_C(0x1fffffff);
    const uint64_t range_end = range_start + size;
    const uint64_t value_start = address & UINT32_C(0x1fffffff);

    return size != 0u && address_size != 0u &&
        value_start >= range_start && value_start + address_size <= range_end;
}

static bool gear_helper_artifact_proof_matches(
        const PsxXgRenderAuthCandidate *proof, uint32_t caller_window_start,
        uint32_t return_address) {
    static const uint32_t instruction_window_size =
        sizeof(model_dispatch_gear_helper_caller_instructions[0]);

    return proof != NULL && proof->authority_provenance && proof->pair_bound &&
        proof->pair_id != 0u && !proof->runtime_variant_bound &&
        physical_address_equals(proof->artifact_base, UINT32_C(0x801dc000)) &&
        proof->artifact_size == 51200u &&
        memcmp(proof->artifact_sha256,
               (const uint8_t[32]){
                   0x14, 0x39, 0x5a, 0x9f, 0x54, 0xc1, 0x24, 0xfe,
                   0xd0, 0x16, 0xf0, 0x79, 0x06, 0xcc, 0x88, 0x2b,
                   0x2a, 0x92, 0x56, 0xd5, 0xad, 0xe2, 0xd1, 0x0e,
                   0xef, 0x99, 0xe1, 0x0f, 0xe9, 0x36, 0x65, 0x23,
               }, sizeof(proof->artifact_sha256)) == 0 &&
        memcmp(proof->identity.game_sha256, xg_render_game_identity,
               sizeof(proof->identity.game_sha256)) == 0 &&
        memcmp(proof->identity.manifest_sha256, xg_render_manifest_identity,
               sizeof(proof->identity.manifest_sha256)) == 0 &&
        normalized_range_contains(
            proof->range_start, proof->range_size,
            caller_window_start, instruction_window_size) &&
        normalized_range_contains(
            proof->artifact_base, proof->artifact_size,
            caller_window_start, instruction_window_size) &&
        normalized_range_contains(
            proof->artifact_base, proof->artifact_size,
            return_address, 4u);
}

bool xg_render_model_sprite_pipeline_accept_gear_helper_mode1_proof(
        CPUState *cpu, const PsxXgRenderAuthCandidate *proof) {
    uint32_t caller_window_start = 0u;
    const uint32_t return_address = cpu != NULL ? cpu->gpr[31] : 0u;

    gear_helper_mode1_proof = (GearHelperMode1Proof){0};
    if (cpu == NULL ||
        cpu->gpr[7] != XG_MODEL_FT4_RAW_DISPATCH_RELIT ||
        !model_dispatch_gear_helper_window_matches(
            cpu, return_address, &caller_window_start) ||
        !gear_helper_artifact_proof_matches(
            proof, caller_window_start, return_address))
        return false;
    gear_helper_mode1_proof = (GearHelperMode1Proof){
        .caller_window_start = caller_window_start,
        .return_address = return_address,
        .armed = true,
    };
    return true;
}

static bool consume_gear_helper_mode1_proof(
        CPUState *cpu, uint32_t return_address, uint32_t *out_window_start) {
    const GearHelperMode1Proof proof = gear_helper_mode1_proof;
    uint32_t caller_window_start = 0u;

    gear_helper_mode1_proof = (GearHelperMode1Proof){0};
    if (!proof.armed || cpu == NULL ||
        cpu->gpr[7] != XG_MODEL_FT4_RAW_DISPATCH_RELIT ||
        !physical_address_equals(return_address, proof.return_address) ||
        !model_dispatch_gear_helper_window_matches(
            cpu, return_address, &caller_window_start) ||
        !physical_address_equals(
            caller_window_start, proof.caller_window_start))
        return false;
    if (out_window_start != NULL) *out_window_start = caller_window_start;
    return true;
}

static bool model_dispatch_overlay_window_matches(
        CPUState *cpu, uint32_t return_address,
        uint32_t *out_window_start, uint32_t *out_matrix_stack_offset) {
    const uint32_t contract_count =
        xg_render_runtime_variant_model_dispatch_contract_count();

    for (uint32_t index = 0u; index < contract_count; ++index) {
        XgRenderRuntimeVariantModelDispatchContract contract = {0};
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

static bool model_dispatch_caller_contract_matches(
        CPUState *cpu, uint32_t return_address, uint8_t *out_contract,
        uint32_t *out_window_start, uint32_t *out_matrix_stack_offset) {
    const uint32_t resident_count = (uint32_t)(
        sizeof(model_dispatch_resident_caller_instructions) /
        sizeof(model_dispatch_resident_caller_instructions[0]));
    uint32_t window_start;

    if (return_address >= 8u &&
        xg_render_battle_geometry_authorizes_call(return_address - 8u)) {
        if (out_contract != NULL) *out_contract = MODEL_DISPATCH_CALLER_BATTLE;
        if (out_window_start != NULL) *out_window_start = return_address - 8u;
        return true;
    }
    if (gear_helper_mode1_proof.armed) {
        if (consume_gear_helper_mode1_proof(
                cpu, return_address, &window_start)) {
            if (out_contract != NULL)
                *out_contract = XG_RENDER_MODEL_DISPATCH_CALLER_GEAR_HELPER;
            if (out_window_start != NULL) *out_window_start = window_start;
            return true;
        }
    }
    if (return_address >= resident_count * 4u) {
        window_start = return_address - resident_count * 4u;
        if (model_dispatch_instruction_window_matches(
                cpu, window_start,
                model_dispatch_resident_caller_instructions,
                resident_count)) {
            if (out_contract != NULL)
                *out_contract = XG_RENDER_MODEL_DISPATCH_CALLER_RESIDENT;
            if (out_window_start != NULL) *out_window_start = window_start;
            if (out_matrix_stack_offset != NULL)
                *out_matrix_stack_offset = 0x10u;
            return true;
        }
    }
    if (!model_dispatch_overlay_window_matches(
            cpu, return_address, &window_start, out_matrix_stack_offset))
        return false;
    if (out_contract != NULL)
        *out_contract = XG_RENDER_MODEL_DISPATCH_CALLER_OVERLAY;
    if (out_window_start != NULL) *out_window_start = window_start;
    return true;
}

static bool model_dispatch_context_contract_matches(
        CPUState *cpu, uint8_t caller_contract,
        uint32_t caller_window_start) {
    if (caller_contract == MODEL_DISPATCH_CALLER_BATTLE)
        return xg_render_battle_geometry_authorizes_call(caller_window_start);
    if (caller_contract == XG_RENDER_MODEL_DISPATCH_CALLER_RESIDENT) {
        return model_dispatch_instruction_window_matches(
            cpu, caller_window_start,
            model_dispatch_resident_caller_instructions,
            (uint32_t)(sizeof(model_dispatch_resident_caller_instructions) /
                       sizeof(model_dispatch_resident_caller_instructions[0])));
    }
    if (caller_contract == XG_RENDER_MODEL_DISPATCH_CALLER_OVERLAY) {
        const uint32_t contract_count =
            xg_render_runtime_variant_model_dispatch_contract_count();

        for (uint32_t index = 0u; index < contract_count; ++index) {
            XgRenderRuntimeVariantModelDispatchContract contract = {0};

            if (xg_render_runtime_variant_model_dispatch_contract_at(
                    index, &contract) &&
                model_dispatch_instruction_window_matches(
                    cpu, caller_window_start, contract.instructions,
                    contract.instruction_count))
                return true;
        }
    }
    if (caller_contract == XG_RENDER_MODEL_DISPATCH_CALLER_GEAR_HELPER) {
        for (uint32_t index = 0u;
             index < sizeof(model_dispatch_gear_helper_caller_instructions) /
                 sizeof(model_dispatch_gear_helper_caller_instructions[0]);
             ++index) {
            if (physical_address_equals(
                    caller_window_start,
                    model_dispatch_gear_helper_returns[index] -
                        sizeof(model_dispatch_gear_helper_caller_instructions[0])) &&
                model_dispatch_instruction_window_matches(
                    cpu, caller_window_start,
                    model_dispatch_gear_helper_caller_instructions[index],
                    (uint32_t)(
                        sizeof(model_dispatch_gear_helper_caller_instructions[0]) /
                        sizeof(model_dispatch_gear_helper_caller_instructions[0][0]))))
                return true;
        }
    }
    return false;
}

static uint32_t normalized_word_address(uint32_t address) {
    return address & UINT32_C(0x001ffffc);
}

static int16_t low_s16(uint32_t value) {
    return xg_render_runtime_low_s16(value);
}

static bool word_address_is_valid(uint32_t address) {
    return xg_render_runtime_word_address_is_valid(address);
}

static bool lifecycle_begin(
        const XgRenderModelSpritePipelineServices *services,
        uint32_t producer_pc, XgRenderProducerLifecycle *out_lifecycle) {
    return services != NULL && services->lifecycle != NULL &&
        services->lifecycle->begin != NULL &&
        services->lifecycle->begin(producer_pc, out_lifecycle);
}

static bool lifecycle_matches(
        const XgRenderModelSpritePipelineServices *services,
        const XgRenderProducerLifecycle *lifecycle) {
    return services != NULL && services->lifecycle != NULL &&
        services->lifecycle->matches != NULL &&
        services->lifecycle->matches(lifecycle);
}

static void watch_resource(
        const XgRenderModelSpritePipelineServices *services,
        uint32_t address, uint32_t size) {
    if (services != NULL && services->watch_resource != NULL)
        services->watch_resource(address, size);
}

static void clear_model_ft4_pending(void) {
    if (!model_ft4.context.valid && !model_ft4.snapshot.pending &&
        model_ft4.context.caller_contract == XG_RENDER_MODEL_DISPATCH_CALLER_NONE &&
        model_ft4.count == 0u && !model_ft4.pre_scene_staged)
        return;
    model_ft4.context = (ModelContext){0};
    model_ft4.initial_packet_cursor = 0u;
    model_ft4.initial_counter = 0u;
    model_ft4.expected_counter_delta = 0u;
    model_ft4.descriptor_base = 0u;
    model_ft4.count = 0u;
    model_ft4.pre_scene_staged = false;
    model_ft4.snapshot.pending = false;
}

static void block_model_ft4(uint32_t blocker) {
    if (model_ft4.pre_scene_staged)
        xg_render_submission_pre_scene_block(blocker, true);
    clear_model_ft4_pending();
    model_ft4.snapshot.blocked = true;
    if (model_ft4.snapshot.blocker == 0u) model_ft4.snapshot.blocker = blocker;
}

static void clear_model_ft3_pending(void) {
    if (!model_ft3.snapshot.pending && model_ft3.count == 0u) return;
    model_ft3.initial_packet_cursor = 0u;
    model_ft3.initial_counter = 0u;
    model_ft3.expected_counter_delta = 0u;
    model_ft3.descriptor_base = 0u;
    model_ft3.count = 0u;
    model_ft3.snapshot.pending = false;
}

static void block_model_ft3(uint32_t blocker) {
    clear_model_ft3_pending();
    model_ft3.snapshot.blocked = true;
    if (model_ft3.snapshot.blocker == 0u) model_ft3.snapshot.blocker = blocker;
}

static void clear_sprite(void) {
    if (!sprite_ft4.snapshot.context_active && !sprite_ft4.snapshot.pending)
        return;
    sprite_ft4.snapshot.context_active = false;
    sprite_ft4.snapshot.pending = false;
    sprite_ft4.sprite_address = 0u;
    sprite_ft4.packet_address = 0u;
    sprite_ft4.descriptor_address = 0u;
    sprite_ft4.material_word = 0u;
    sprite_ft4.tpage = 0u;
    sprite_ft4.clut = 0u;
    sprite_ft4.phase = SPRITE_IDLE;
    sprite_ft4.native_record_count = 0u;
    sprite_ft4.geometry_matches = false;
    sprite_ft4.payload_matches = false;
    sprite_ft4.invocation_matches = false;
    sprite_ft4.wrapper_scope = false;
    sprite_ft4.motion = (XgRenderMotionRef){0};
    sprite_ft4.motion_binding = (XgRenderMotionDrawBinding){0};
    sprite_ft4.motion_attempted = false;
}

static void block_sprite(uint32_t blocker) {
    clear_sprite();
    sprite_ft4.snapshot.blocked = true;
    if (sprite_ft4.snapshot.blocker == 0u) sprite_ft4.snapshot.blocker = blocker;
}

static bool consume_controls(
        CPUState *cpu, uint32_t *cursor, uint16_t *tpage, uint16_t *clut) {
    if (cpu == NULL || cursor == NULL || tpage == NULL || clut == NULL ||
        cpu->read_byte == NULL || cpu->read_half == NULL)
        return false;
    for (uint32_t count = 0u; count < 32u; ++count) {
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

static bool compare_ft4_payload(
        CPUState *cpu, uint32_t packet_address, uint32_t descriptor_address,
        uint32_t expected_material_word, const uint16_t expected_uv[4],
        uint16_t expected_tpage, uint16_t expected_clut,
        PsxXgRenderFt4PayloadMismatch *first_mismatch) {
    PsxXgRenderFt4PayloadMismatch mismatch = {0};

    mismatch.packet_address = packet_address;
    mismatch.descriptor_address = descriptor_address;
    mismatch.expected_material_word = expected_material_word;
    mismatch.actual_material_word = cpu->read_word(packet_address + 4u);
    mismatch.expected_tpage = expected_tpage;
    mismatch.actual_tpage = cpu->read_half(packet_address + 22u);
    mismatch.expected_clut = expected_clut;
    mismatch.actual_clut = cpu->read_half(packet_address + 14u);
    if (mismatch.actual_material_word != expected_material_word)
        mismatch.field_bits |= FT4_PAYLOAD_MATERIAL;
    if (mismatch.actual_tpage != expected_tpage)
        mismatch.field_bits |= FT4_PAYLOAD_TPAGE;
    if (mismatch.actual_clut != expected_clut)
        mismatch.field_bits |= FT4_PAYLOAD_CLUT;
    for (uint32_t vertex = 0u; vertex < 4u; ++vertex) {
        mismatch.expected_uv[vertex] = expected_uv[vertex];
        mismatch.actual_uv[vertex] = cpu->read_half(
            packet_address + 12u + vertex * 8u);
        if (mismatch.actual_uv[vertex] != expected_uv[vertex])
            mismatch.field_bits |= FT4_PAYLOAD_UV0 << vertex;
    }
    if (mismatch.field_bits != 0u && first_mismatch != NULL &&
        first_mismatch->field_bits == 0u)
        *first_mismatch = mismatch;
    return mismatch.field_bits == 0u;
}

void xg_render_model_sprite_pipeline_begin_packet_copy(
        CPUState *cpu, GuestRenderRenderMode render_mode) {
    xg_render_model_repository_begin_packet_copy(cpu, render_mode);
}

void xg_render_model_sprite_pipeline_finish_packet_copy(
        CPUState *cpu, GuestRenderRenderMode render_mode,
        const XgRenderModelSpritePipelineServices *services) {
    xg_render_model_repository_finish_packet_copy(
        cpu, render_mode, services != NULL ? services->repository : NULL);
}

void xg_render_model_sprite_pipeline_observe_ft4_template(
        CPUState *cpu, GuestRenderRenderMode render_mode,
        const XgRenderModelSpritePipelineServices *services) {
    XgRenderModelFt4Template captured = {0};
    const uint32_t descriptor = cpu != NULL ? cpu->gpr[16] : 0u;
    const uint32_t packet = cpu != NULL && cpu->read_word != NULL
        ? cpu->read_word(UINT32_C(0x80059424)) : 0u;

    if (cpu == NULL || cpu->read_word == NULL || cpu->read_half == NULL ||
        (render_mode == GUEST_RENDER_RENDER_NATIVE && !lifecycle_begin(
            services, UINT32_C(0x8002d100), &captured.lifecycle)) ||
        !word_address_is_valid(descriptor) ||
        !word_address_is_valid(descriptor + 8u) ||
        !word_address_is_valid(packet))
        return;
    captured = (XgRenderModelFt4Template){
        .packet_address = normalized_word_address(packet),
        .descriptor_address = normalized_word_address(descriptor),
        .material_word = cpu->read_word(descriptor),
        .uv = {
            cpu->read_half(descriptor + 4u), cpu->read_half(descriptor + 6u),
            cpu->read_half(descriptor + 8u), cpu->read_half(descriptor + 10u),
        },
        .tpage = cpu->read_half(UINT32_C(0x80059308)),
        .clut = cpu->read_half(UINT32_C(0x8005930c)),
        .valid = true,
    };
    if (!xg_render_model_repository_store_template(&captured)) return;
    watch_resource(services, packet, 0x28u);
    watch_resource(services, descriptor, 12u);
    ++model_ft4.snapshot.template_capture_count;
}

static void capture_ft3_template(
        CPUState *cpu, uint32_t descriptor, uint32_t packet,
        GuestRenderRenderMode render_mode,
        const XgRenderModelSpritePipelineServices *services) {
    XgRenderModelFt4Template captured = {0};

    if (cpu == NULL || cpu->read_word == NULL || cpu->read_half == NULL ||
        cpu->read_byte == NULL ||
        (render_mode == GUEST_RENDER_RENDER_NATIVE && !lifecycle_begin(
            services, UINT32_C(0x8002da00), &captured.lifecycle)) ||
        !word_address_is_valid(descriptor) ||
        !word_address_is_valid(descriptor + 8u) ||
        !word_address_is_valid(packet))
        return;
    captured.packet_address = normalized_word_address(packet);
    captured.descriptor_address = normalized_word_address(descriptor);
    captured.material_word = UINT32_C(0x00808080) |
        ((uint32_t)cpu->read_byte(descriptor + 3u) << 24u);
    captured.uv[0] = cpu->read_half(descriptor + 4u);
    captured.uv[1] = cpu->read_half(descriptor + 6u);
    captured.uv[2] = cpu->read_half(descriptor);
    captured.tpage = cpu->read_half(UINT32_C(0x80059308));
    captured.clut = cpu->read_half(UINT32_C(0x8005930c));
    captured.valid = true;
    if (!xg_render_model_repository_store_template(&captured)) return;
    if (render_mode == GUEST_RENDER_RENDER_NATIVE) {
        const XgRenderModelFt3SourceRecord source = {
            .lifecycle = captured.lifecycle,
            .source_id = captured.packet_address + 4u,
            .descriptor_address = captured.descriptor_address,
            .material_word = captured.material_word,
            .uv = {captured.uv[0], captured.uv[1], captured.uv[2]},
            .tpage = captured.tpage,
            .clut = captured.clut,
            .interpolation_producer_id =
                model_ft4.context.instance_address & UINT32_C(0x1fffffff),
            .interpolation_primitive_id =
                (captured.descriptor_address & UINT32_C(0x1fffffff)) |
                ((uint32_t)(model_ft4.context.dispatch_mode & 7u) << 29u) |
                UINT32_C(1),
            .interpolation_identity_valid =
                (model_ft4.context.instance_address &
                 UINT32_C(0x1fffffff)) != 0u,
            .valid = true,
        };
        (void)xg_render_model_repository_store_ft3_source(
            &source, NULL, services->repository);
    }
    watch_resource(services, packet, 0x20u);
    if (services != NULL && services->repository != NULL &&
        services->repository->resources.watch_ft3_descriptor != NULL)
        services->repository->resources.watch_ft3_descriptor(descriptor, 10u);
    ++model_ft3.snapshot.template_capture_count;
}

void xg_render_model_sprite_pipeline_observe_ft3_template(
        CPUState *cpu, GuestRenderRenderMode render_mode,
        const XgRenderModelSpritePipelineServices *services) {
    capture_ft3_template(
        cpu, cpu != NULL ? cpu->gpr[16] : 0u,
        cpu != NULL ? cpu->gpr[4] : 0u, render_mode, services);
}

static bool publish_model_motion(CPUState *cpu, ModelContext *context,
        const XgRenderModelSpritePipelineServices *services,
        const XgRenderMotionSource *source, XgRenderMotionPose *pose, uint32_t owner) {
    const uint32_t vertices = cpu->read_half(context->model_address + 2);
    const uint32_t groups = cpu->read_half(context->model_address + 6);
    uint32_t end = context->topology_base;
    if (!vertices || groups > 256 ||
        !services->lifecycle->guest_data_range_is_valid(context->vertex_base, vertices * 8, 4,
                                                        false))
        return false;
    for (uint32_t i = 0; i < groups; ++i) {
        if (!services->lifecycle->guest_data_range_is_valid(end, 4, 2, false))
            return false;
        const uint32_t bytes = 4u + cpu->read_half(end + 2) * 8u;
        if (end > UINT32_MAX - bytes ||
            !services->lifecycle->guest_data_range_is_valid(end, bytes, 2, false))
            return false;
        end += bytes;
    }
    const uint32_t key[4] = {context->model_address, context->vertex_base, context->topology_base,
                             vertices};
    pose->geometry_id = xg_render_resource_digest(key, sizeof(key));
    pose->geometry_generation = 1;
    pose->screen_offset[0] = context->projection.screen_offset_x / 65536.0;
    pose->screen_offset[1] = context->projection.screen_offset_y / 65536.0;
    pose->projection_distance = context->projection.projection_distance;
    if (pose->discontinuity)
        xg_render_motion_note(XG_MOTION_TRANSLATION_STAGE_DISCRETE, owner);
    if (!xg_render_motion_publish(source, pose, &context->motion))
        return false;
    const uint32_t addresses[4] = {owner, context->model_address, context->vertex_base,
                                   context->topology_base};
    const uint32_t sizes[4] = {4, 0x38, vertices * 8, end - context->topology_base};
    for (unsigned i = 0; i < 4; ++i) {
        if (!sizes[i])
            continue;
        if (!xg_render_motion_watch(context->motion, addresses[i], sizes[i]))
            return false;
        watch_resource(services, addresses[i], sizes[i]);
    }
    return true;
}

/* Resident actors can draw a 3D model (for example a treasure chest) instead
 * of sprite quads. Their wrapper composes the shared Field camera with a
 * LOCAL matrix at actor_data + 0xc, rounding the result back to GTE integers.
 * Recover that same authored pose before composition loses its fractions. */
static bool capture_resident_field_motion(CPUState *cpu, ModelContext *context,
        const XgRenderModelSpritePipelineServices *services) {
    XgRenderMotionSource source;
    XgRenderMotionPose pose = {0};
    XgHost3dMatrix camera, field_camera, local, combined;
    if (!context->resident_dispatch ||
        !physical_address_equals(cpu->gpr[31], UINT32_C(0x800257dc)) ||
        !services || !services->motion_source || !services->lifecycle ||
        !services->lifecycle->guest_data_range_is_valid ||
        !services->motion_source(UINT32_C(0x80075b44), &source)) return false;
    const uint32_t actor = context->instance_address;
    if (!services->lifecycle->guest_data_range_is_valid(actor, 0x44, 4, false) ||
        (cpu->read_byte(actor + 0x3f) & 1u)) return false;
    const uint32_t data = cpu->read_word(actor + 0x20);
    if (!services->lifecycle->guest_data_range_is_valid(data, 0x38, 4, false) ||
        !services->lifecycle->guest_data_range_is_valid(context->model_address, 0x38, 4, false) ||
        !physical_address_equals(cpu->read_word(data + 0x34), context->model_address) ||
        !xg_render_runtime_capture_matrix(cpu, data + 0xc, &local) ||
        !xg_render_runtime_capture_matrix(cpu, 0x8004fbb8u, &camera) ||
        !xg_render_runtime_capture_matrix(cpu, 0x800afa64u, &field_camera)) return false;
    /* Use Field placement authority only while this resident invocation uses
     * its camera, and only after proving the captured GTE matrix exactly. */
    if (memcmp(camera.rotation, field_camera.rotation, sizeof(camera.rotation)) ||
        memcmp(camera.translation, field_camera.translation, sizeof(camera.translation)) ||
        !xg_host_3d_comp_matrix(&camera, &local, &combined) ||
        memcmp(combined.rotation, context->projection.rotation, sizeof(combined.rotation)) ||
        memcmp(combined.translation, context->projection.translation, sizeof(combined.translation)))
        return false;
    for (unsigned axis = 0; axis < 3; ++axis)
        local.translation[axis] = xg_render_motion_s16_translation(local.translation[axis]);
    if (!xg_render_motion_camera_from_view(&camera, &pose.camera) ||
        !xg_render_motion_decompose(&local, &pose.nodes[0].local)) return false;
    pose.node_count = 1;
    pose.translation_stage = XG_RENDER_MOTION_TRANSLATION_FIELD;
    pose.entity_id = UINT64_C(0x53504d4400000000) | (actor & 0x1fffffffu);
    pose.camera_id = UINT64_C(0x4649454c800afa64);
    pose.geometry_scale = 1;
    pose.nodes[0].id = actor & 0x1fffffffu;
    pose.nodes[0].parent = -1;
    pose.nodes[0].translation_s16 = 1;
    pose.nodes[0].source_matrix_valid = 1;
    pose.nodes[0].source_model_to_view = combined;
    context->motion_part = 0;
    return publish_model_motion(cpu, context, services, &source, &pose, actor + 0x20);
}

static bool capture_field_motion(CPUState *cpu, ModelContext *context,
                                 const XgRenderModelSpritePipelineServices *services) {
    XgRenderMotionSource source;
    XgRenderMotionPose pose = {0};
    XgHost3dMatrix local, camera, extra, combined;
    xg_render_motion_note(XG_MOTION_FIELD_CAPTURE,cpu->gpr[31]);
    if (services == NULL || services->motion_source == NULL || services->lifecycle == NULL ||
        services->lifecycle->guest_data_range_is_valid == NULL ||
        !physical_address_equals(cpu->gpr[31], UINT32_C(0x8007519c)) ||
        !services->motion_source(UINT32_C(0x800748e8), &source)) {
        xg_render_motion_note(XG_MOTION_FIELD_CALLER_REJECT,cpu->gpr[31]);
        return false;
    }
    const uint32_t base = cpu->read_word(0x800afb10u);
    const uint32_t count = cpu->read_word(0x800afb0cu);
    const uint32_t record = cpu->gpr[17]; /* s1, not s0 (shared wrapper). */
    const uint32_t index = cpu->gpr[22];
    if (count > 4096 || index >= count || base > UINT32_MAX - index * 0x5cu ||
        !physical_address_equals(record, base + index * 0x5cu) ||
        !services->lifecycle->guest_data_range_is_valid(record, 0x5c, 4, false) ||
        !services->lifecycle->guest_data_range_is_valid(context->model_address, 0x38, 4, false))
        return false;
    const uint32_t flags = cpu->read_half(record + 0x58);
    if ((flags & (0x60u | 3u)) || cpu->read_word(record) != cpu->gpr[16]) {
        xg_render_motion_note(XG_MOTION_FIELD_UNSUPPORTED,record);
        return false;
    }
    if (!xg_render_runtime_capture_matrix(cpu, 0x800afa64u, &camera))
        return false;
    const uint32_t actor_count = cpu->read_word(0x800adbfcu);
    XgHost3dMatrix locals[XG_RENDER_MOTION_NODE_CAPACITY];
    uint32_t chain[XG_RENDER_MOTION_NODE_CAPACITY], depth = 0, current = index;
    for (;;) {
        if (depth == XG_RENDER_MOTION_NODE_CAPACITY || current >= count ||
            base > UINT32_MAX - current * 0x5cu)
            return false;
        const uint32_t node_address = base + current * 0x5cu;
        if (!services->lifecycle->guest_data_range_is_valid(node_address, 0x5c, 4, false) ||
            !xg_render_runtime_capture_matrix(cpu, node_address + 0xc, &locals[depth]))
            return false;
        chain[depth++] = node_address;
        const uint32_t node_flags = cpu->read_half(node_address + 0x58);
        /* A sprite parent copies local -> +2c and skips model hierarchy work. */
        if (current >= actor_count || (node_flags & 0x40u))
            break;
        const uint32_t state = cpu->read_word(node_address + 0x4c);
        if (!services->lifecycle->guest_data_range_is_valid(state, 0x130, 4, false) ||
            cpu->read_half(state + 0x128) != 0xffff || (cpu->read_word(state + 0x12c) & 3u))
            return false;
        const uint32_t parent = cpu->read_byte(state + 0x75);
        if (parent == 0xff)
            break;
        /* Later records contain last-frame caches; they are not a current local pose. */
        if (parent >= current)
            return false;
        current = parent;
    }
    pose.node_count = depth;
    pose.translation_stage = XG_RENDER_MOTION_TRANSLATION_FIELD;
    XgHost3dMatrix accumulated = locals[depth - 1];
    XgHost3dMatrix canonical_accumulated;
    for (uint32_t j = 0; j < depth; ++j) {
        const uint32_t reversed = depth - 1 - j;
        XgRenderMotionNode *node = &pose.nodes[j];
        node->id = chain[reversed] & 0x1fffffffu;
        node->parent = (int32_t)j - 1;
        /* MATRIX.t is stored as s32, but every local RHS reaches GTE V0 as
         * signed 16-bit. Keep raw locals for the original cache validation. */
        XgHost3dMatrix effective = locals[reversed];
        for (unsigned axis = 0; axis < 3; ++axis) {
            effective.translation[axis] =
                xg_render_motion_s16_translation(effective.translation[axis]);
            if (effective.translation[axis] != locals[reversed].translation[axis])
                xg_render_motion_note(XG_MOTION_TRANSLATION_CANONICALIZED, chain[reversed]);
        }
        node->translation_s16 = 1;
        if (!xg_render_motion_decompose(&effective, &node->local))
            return false;
        if (!j)
            canonical_accumulated = effective;
        else if (j + 1 < depth && !xg_host_3d_comp_matrix(&canonical_accumulated, &effective,
                                                          &canonical_accumulated))
            return false;
        if (j && j + 1 < depth &&
            !xg_host_3d_comp_matrix(&accumulated, &locals[reversed], &accumulated))
            return false;
        if (j + 1 < depth) {
            /* Field renders (camera * parent cache) * leaf, not camera *
             * s16(final leaf world matrix). An additional parent-cache wrap
             * is a discontinuity unless canonical locals reproduce it. */
            for (unsigned axis = 0; axis < 3; ++axis)
                if (canonical_accumulated.translation[axis] !=
                    xg_render_motion_s16_translation(accumulated.translation[axis]))
                    pose.discontinuity = 1;
            XgHost3dMatrix cached;
            if (!xg_render_runtime_capture_matrix(cpu, chain[reversed] + 0x2c, &cached) ||
                memcmp(cached.rotation, accumulated.rotation, sizeof(cached.rotation)) ||
                memcmp(cached.translation, accumulated.translation, sizeof(cached.translation)))
                return false;
        }
    }
    local = locals[0];
    if (depth == 1) {
        if (!xg_render_runtime_capture_matrix(cpu, 0x800afc30u, &extra) ||
            !xg_host_3d_comp_matrix(&camera, &extra, &camera) ||
            !xg_host_3d_comp_matrix(&camera, &local, &combined))
            return false;
    } else {
        XgHost3dMatrix parent_view;
        if (!xg_host_3d_comp_matrix(&camera, &accumulated, &parent_view) ||
            !xg_host_3d_comp_matrix(&parent_view, &local, &combined))
            return false;
    }
    if (memcmp(combined.rotation, context->projection.rotation, sizeof(combined.rotation)) ||
        memcmp(combined.translation, context->projection.translation,
               sizeof(combined.translation)) ||
        !xg_render_motion_camera_from_view(&camera, &pose.camera)) {
        xg_render_motion_note(XG_MOTION_FIELD_MATRIX_REJECT,record);
        return false;
    }
    pose.entity_id = UINT64_C(0x4649454c00000000) | (record & 0x1fffffffu);
    pose.camera_id = depth == 1 ? UINT64_C(0x4649454c800afc30) : UINT64_C(0x4649454c800afa64);
    pose.geometry_scale = 1;
    context->motion_part = depth - 1;
    pose.nodes[context->motion_part].source_model_to_view = combined;
    pose.nodes[context->motion_part].source_matrix_valid = 1;
    if (!publish_model_motion(cpu, context, services, &source, &pose, record)) return false;
    if (flags & 0x2000u) {
        /* The callback has already deformed the current LOCAL vertices. Share
         * the terrain's precise camera/model transform, but keep vertex-sample
         * interpolation: rigid pose interpolation would freeze the deformation. */
        context->projection_motion = context->motion;
        context->motion = (XgRenderMotionRef){0};
    }
    return true;
}

static bool bind_model_motion(uint32_t packet, const XgHost3dVector *vertices,
                              const uint32_t *indices, uint32_t count, uint32_t attribute,
                              bool triangle) {
    const ModelContext *context = &model_ft4.context;
    const uint8_t split[2][3] = {{0, 1, 2}, {2, 1, 3}};
    XgRenderMotionDrawBinding binding = {.motion = context->motion,
                                         .motion_part_index = context->motion_part,
                                         .triangle_count = count == 3 ? 1u : 2u};
    if (!context->motion.handle.resource_id)
        return false;
    for (uint32_t t = 0; t < binding.triangle_count; ++t)
        for (unsigned v = 0; v < 3; ++v) {
            binding.local[t][v] = vertices[split[t][v]];
            binding.local[t][v].pad = 0;
            binding.vertex_ids[t][v] = indices[split[t][v]];
        }
    return xg_render_motion_register_command(
        packet + 4, &binding, context->instance_address & 0x1fffffffu,
        (attribute & 0x1fffffffu) | ((uint32_t)(context->dispatch_mode & 7u) << 29u) |
            (triangle ? 1u : 0u));
}

static bool capture_model_geometry(uint32_t packet, uint32_t descriptor,
        uint32_t family, const XgHost3dVector vertices[4], const uint32_t indices[4]) {
    const ModelContext *context = &model_ft4.context;
    const XgRenderMotionPose *pose = NULL;
    XgHost3dProjectedVertex projected[4];
    GpuRenderSemantic semantic = {0};
    const uint8_t split[2][3] = {{0, 1, 2}, {2, 1, 3}};
    const uint32_t corners = family & 8u ? 4u : 3u;
    if (context->motion.handle.resource_id && !xg_render_motion_view(context->motion, &pose)) return false;
    for (uint32_t corner = 0; corner < corners; ++corner) {
        const uint32_t slot = indices[corner] % MODEL_PROJECTION_CACHE_CAPACITY;
        if (model_projection_cache.vertex_ids[slot] != indices[corner] + 1u ||
            memcmp(&model_projection_cache.locals[slot], &vertices[corner], sizeof(vertices[corner]))) {
            XgHost3dProjectedVertex result;
            uint32_t flags;
            /* Capture consumes vertex coordinates only, not RTPT's accumulated
             * flags/depth FIFO. A scalar RTPS gives the identical vertex. */
            if (!xg_host_3d_rtps(&context->projection, &vertices[corner], &result, &flags)) return false;
            if (context->projection_motion.handle.resource_id && result.native_view_position &&
                !xg_render_motion_refine_native_vertex(context->projection_motion,
                    context->motion_part, &vertices[corner],
                    &result.native_view_x_16_16, &result.native_view_y_16_16,
                    &result.native_view_depth_q12)) return false;
            model_projection_cache.projected[slot] = result;
            model_projection_cache.locals[slot] = vertices[corner];
            model_projection_cache.vertex_ids[slot] = indices[corner] + 1u;
        }
        projected[corner] = model_projection_cache.projected[slot];
    }
    semantic.material.textured = (family & 1u) != 0u || family == 16u;
    semantic.material.shading = family & 2u
        ? GPU_RENDER_SHADING_GOURAUD : GPU_RENDER_SHADING_FLAT;
    semantic.triangle_count = corners - 2u;
    const uint32_t producer = context->instance_address & 0x1fffffffu;
    xg_render_semantic_set_interpolation_identity(&semantic,
        pose ? pose->continuity_generation : xg_render_submission_temporal_scene(),
        producer, (descriptor & 0x1fffffffu) |
            ((uint32_t)(context->dispatch_mode & 7u) << 29u) | (corners == 3u));
    for (uint32_t t = 0; t < semantic.triangle_count; ++t) {
        semantic.triangles[t].split_index = (uint8_t)t;
        semantic.triangles[t].split_count = (uint8_t)semantic.triangle_count;
        for (uint32_t v = 0; v < 3u; ++v) {
            const uint32_t corner = split[t][v];
            const XgHost3dProjectedVertex *source = &projected[corner];
            GpuRenderSemanticVertex *target = &semantic.triangles[t].vertices[v];
            target->x = (int32_t)source->x * 65536;
            target->y = (int32_t)source->y * 65536;
            target->native_view_x = source->native_view_x_16_16;
            target->native_view_y = source->native_view_y_16_16;
            target->native_view_position = source->native_view_position;
            target->native_view_depth = source->native_view_depth_q12;
            target->projective_view_x = source->projective_view_x;
            target->projective_view_y = source->projective_view_y;
            target->projective_view_z = source->projective_view_z;
            target->projective_offset_x = source->projective_offset_x_16_16;
            target->projective_offset_y = source->projective_offset_y_16_16;
            target->projective_native_offset_x = source->projective_native_offset_x_16_16;
            target->projective_native_offset_y = source->projective_native_offset_y_16_16;
            target->projective_distance = source->projective_distance;
            target->projective_position = source->projective_position;
            target->interpolation_group_id = producer;
            target->interpolation_vertex_id = indices[corner];
            target->interpolation_vertex_identity_valid = 1u;
            if (!pose && indices[corner] < model_coverage.vertex_count)
                model_coverage.samples[indices[corner]] = (XgRenderTemporalSample){producer, *target};
        }
    }
    /* As in the resident Battle model capture, this binds geometry to an output
     * slot; acceptance checks exact GTE XY and takes final material/OT order from
     * the actual draw. A culled slot does not publish a polygon. */
    xg_render_depth_policy_stamp_semantic(&semantic, XG_RENDER_DEPTH_FAMILY_FIELD_MODELS);
    if (xg_render_submission_stage_exact((GpuRenderTransactionId){0},
        (packet & 0x1fffffffu) + 4u, &semantic) != GUEST_RENDER_TRANSACTION_OK) return false;
    if (!pose) {
        XgRenderTemporalCommandBinding *bindings = xg_render_array_reserve(model_coverage.bindings,
            sizeof(*bindings), &model_coverage.binding_capacity, model_coverage.binding_count + 1u,
            XG_RENDER_TEMPORAL_SAMPLE_CAPACITY);
        if (!bindings) return false;
        model_coverage.bindings = bindings;
        bindings[model_coverage.binding_count++] = (XgRenderTemporalCommandBinding){
            (packet & 0x1fffffffu) + 4u, producer};
    }
    return true;
}

/* Capture every family's geometry at the authenticated model entry, including
 * FT3/FT4. Later observers may have lost initializer templates after a module
 * transition, or be absent altogether for the Gear helper's relit dispatch. */
static void capture_model_geometry_bindings(CPUState *cpu,
                                    const XgRenderModelSpritePipelineServices *services) {
    ModelContext *context = &model_ft4.context;
    uint32_t topology = context->topology_base, attribute = context->material_base;
    uint32_t packet = context->packet_base;
    uint16_t tpage = context->tpage, clut = context->clut;
    if (!services || !services->lifecycle ||
        !services->lifecycle->guest_data_range_is_valid)
        return;
    const uint32_t count = cpu->read_half(context->model_address + 6);
    const uint32_t vertex_count = cpu->read_half(context->model_address + 2);
    memset(model_projection_cache.vertex_ids, 0, sizeof(model_projection_cache.vertex_ids));
    model_coverage.complete = false;
    model_coverage.binding_count = model_coverage.vertex_count = 0u;
    if (!context->motion.handle.resource_id) {
        XgRenderTemporalSample *samples = xg_render_array_reserve(model_coverage.samples,
            sizeof(*samples), &model_coverage.sample_capacity, vertex_count,
            XG_RENDER_TEMPORAL_SAMPLE_CAPACITY);
        if (!samples) goto fail;
        model_coverage.samples = samples;
        model_coverage.vertex_count = vertex_count;
        memset(samples, 0, vertex_count * sizeof(*samples));
        const uint32_t key[] = {context->model_address, context->vertex_base,
                               context->topology_base, vertex_count};
        model_coverage.geometry_digest = xg_render_resource_digest(key, sizeof(key));
    }
    uint32_t packet_bytes = cpu->read_word(context->model_address + 0x34);
    if (!services->lifecycle->guest_data_range_is_valid(packet, packet_bytes, 4, false))
        goto fail;
    for (uint32_t g = 0; g < count; ++g) {
        if (!services->lifecycle->guest_data_range_is_valid(topology, 4, 2, false))
            goto fail;
        const uint32_t family = cpu->read_byte(topology), n = cpu->read_half(topology + 2);
        if (!context->motion.handle.resource_id) {
            const uint64_t key[] = {model_coverage.geometry_digest, family, n};
            model_coverage.geometry_digest = xg_render_resource_digest(key, sizeof(key));
        }
        XgModelPrimitiveLayout layout;
        if (!xg_model_primitive_layout(family, &layout) ||
            n > packet_bytes / layout.packet_size ||
            cpu->read_word(0x8004fe6cu + family * 40u) != 8u ||
            cpu->read_word(0x8004fe74u + family * 40u) != layout.packet_size ||
            !services->lifecycle->guest_data_range_is_valid(topology, 4 + n * 8, 2, false)) {
            xg_render_motion_note(XG_MOTION_BIND_GEOMETRY_UNSUPPORTED,family);
            goto fail;
        }
        for (uint32_t p = 0; p < n; ++p) {
            XgHost3dVector vertices[4] = {0};
            uint32_t indices[4] = {0};
            const uint32_t size = layout.vertex_count;
            if (!consume_controls(cpu, &attribute, &tpage, &clut))
                goto fail;
            for (uint32_t v = 0; v < size; ++v) {
                indices[v] = cpu->read_half(topology + 4 + p * 8 + v * 2);
                if (indices[v] >= vertex_count)
                    goto fail;
                const uint32_t address = context->vertex_base + indices[v] * 8;
                if (!services->lifecycle->guest_data_range_is_valid(address, 8, 4, false))
                    goto fail;
                const uint32_t xy = cpu->read_word(address), z = cpu->read_word(address + 4);
                vertices[v] = (XgHost3dVector){low_s16(xy), low_s16(xy >> 16), low_s16(z), 0};
            }
            const XgRenderModelFt4Template *material =
                family == 5u || family == 13u
                    ? xg_render_model_repository_find_packet_template(packet,
                        GUEST_RENDER_RENDER_NATIVE, services->repository) : NULL;
            const uint32_t descriptor =
                material && material->descriptor_address ? material->descriptor_address : attribute;
            if (!context->motion.handle.resource_id) {
                const uint64_t key[] = {model_coverage.geometry_digest,
                    indices[0], indices[1], indices[2], size == 4u ? indices[3] : 0u};
                model_coverage.geometry_digest = xg_render_resource_digest(key, sizeof(key));
            }
            if (context->motion.handle.resource_id &&
                !bind_model_motion(packet, vertices, indices, size, descriptor, size == 3u))
                goto fail;
            if (!capture_model_geometry(packet, descriptor, family, vertices, indices))
                goto fail;
            packet += layout.packet_size;
            packet_bytes -= layout.packet_size;
            attribute += layout.attribute_size;
        }
        topology += 4 + n * 8;
    }
    model_coverage.complete = !context->motion.handle.resource_id;
    return;
fail:
    if (context->caller_contract == XG_RENDER_MODEL_DISPATCH_CALLER_GEAR_HELPER) {
        const XgRenderMotionPose *pose;
        if (xg_render_motion_view(context->motion, &pose))
            xg_render_motion_forget_entity(pose->entity_id);
    }
    xg_render_motion_forget_range(context->packet_base,
                                  cpu->read_word(context->model_address + 0x34));
    context->motion = (XgRenderMotionRef){0};
}

void xg_render_model_sprite_pipeline_model_begin(
        CPUState *cpu, GuestRenderRenderMode render_mode,
        const XgRenderModelSpritePipelineServices *services) {
    ModelContext context = {0};
    XgHost3dMatrix matrix;
    uint32_t matrix_stack_offset = 0u;
    uint32_t matrix_mismatch_mask = 0u;
    uint8_t caller_contract = XG_RENDER_MODEL_DISPATCH_CALLER_NONE;
    uint32_t caller_window_start = 0u;
    const bool caller_authenticated = model_dispatch_caller_contract_matches(
        cpu, cpu != NULL ? cpu->gpr[31] : 0u, &caller_contract,
        &caller_window_start, &matrix_stack_offset);
    model_coverage.complete = false;

    if (render_mode != GUEST_RENDER_RENDER_SHADOW &&
        render_mode != GUEST_RENDER_RENDER_NATIVE) {
        clear_model_ft4_pending();
        clear_model_ft3_pending();
        return;
    }
    if (render_mode == GUEST_RENDER_RENDER_NATIVE && !caller_authenticated) {
        ++model_ft4.snapshot.dispatch_begin_count;
        ++model_ft4.snapshot.dispatch_caller_reject_count;
        model_ft4.snapshot.last_dispatch_caller =
            cpu != NULL ? cpu->gpr[31] : 0u;
        model_ft4.snapshot.last_dispatch_mode = cpu != NULL ? cpu->gpr[7] : 0u;
        clear_model_ft4_pending();
        clear_model_ft3_pending();
        return;
    }
    if (caller_contract == MODEL_DISPATCH_CALLER_BATTLE) {
        clear_model_ft4_pending();
        clear_model_ft3_pending();
        model_ft4.context.caller_contract = MODEL_DISPATCH_CALLER_BATTLE;
        ++model_ft4.snapshot.dispatch_begin_count;
        model_ft4.snapshot.last_dispatch_caller = cpu != NULL ? cpu->gpr[31] : 0u;
        model_ft4.snapshot.last_dispatch_mode = cpu != NULL ? cpu->gpr[7] : 0u;
        (void)xg_render_battle_geometry_capture(cpu, services ? services->lifecycle : NULL);
        return;
    }
    if (model_ft4.snapshot.blocked) return;
    if (model_ft4.snapshot.pending) {
        block_model_ft4(70u);
        return;
    }
    if (model_ft3.snapshot.pending) {
        block_model_ft3(70u);
        return;
    }
    if (cpu == NULL || cpu->read_word == NULL || cpu->read_half == NULL ||
        cpu->read_byte == NULL) {
        block_model_ft4(71u);
        return;
    }
    ++model_ft4.snapshot.dispatch_begin_count;
    model_ft4.snapshot.last_dispatch_caller = cpu->gpr[31];
    model_ft4.snapshot.last_dispatch_mode = cpu->gpr[7];
    if (!caller_authenticated) {
        ++model_ft4.snapshot.dispatch_caller_reject_count;
        return;
    }
    if (cpu->gpr[7] != XG_MODEL_FT4_RAW_DISPATCH_AVERAGE &&
        cpu->gpr[7] != XG_MODEL_FT4_RAW_DISPATCH_FARTHEST &&
        cpu->gpr[7] != XG_MODEL_FT4_RAW_DISPATCH_RELIT) {
        ++model_ft4.snapshot.dispatch_mode_reject_count;
        return;
    }
    context.model_address = cpu->gpr[4];
    context.instance_address = cpu->gpr[16];
    context.packet_base = cpu->gpr[5];
    context.ot_base = cpu->gpr[6];
    if (!word_address_is_valid(context.model_address) ||
        context.model_address > UINT32_MAX - 0x34u ||
        !word_address_is_valid(context.model_address + 0x34u) ||
        !word_address_is_valid(context.packet_base) ||
        !word_address_is_valid(context.ot_base)) {
        block_model_ft4(72u);
        return;
    }
    context.vertex_base = cpu->read_word(context.model_address + 8u);
    context.topology_base = cpu->read_word(context.model_address + 0x10u);
    context.material_base = cpu->read_word(context.model_address + 0x14u);
    /* A reused output buffer must not inherit an older eligible instance's pose. */
    xg_render_motion_forget_range(context.packet_base,
        cpu->read_word(context.model_address + 0x34u));
    model_ft4.snapshot.last_model_address = context.model_address;
    model_ft4.snapshot.last_topology_base = context.topology_base;
    model_ft4.snapshot.last_material_base = context.material_base;
    if (!word_address_is_valid(context.vertex_base) ||
        !word_address_is_valid(context.topology_base) ||
        !word_address_is_valid(context.material_base)) {
        block_model_ft4(72u);
        return;
    }
    xg_render_runtime_capture_shadow_projection(cpu, &context.projection);
    if (caller_contract != XG_RENDER_MODEL_DISPATCH_CALLER_GEAR_HELPER) {
        if (!xg_render_runtime_stack_address_is_valid(cpu->gpr[29]) ||
            !xg_render_runtime_capture_matrix(
                cpu, cpu->gpr[29] + matrix_stack_offset, &matrix)) {
            block_model_ft4(71u);
            return;
        }
        if (memcmp(context.projection.rotation, matrix.rotation,
                   sizeof(context.projection.rotation)) != 0)
            matrix_mismatch_mask |= 1u;
        if (memcmp(context.projection.translation, matrix.translation,
                   sizeof(context.projection.translation)) != 0)
            matrix_mismatch_mask |= 2u;
        model_ft4.snapshot.last_projection_matrix_mismatch_mask =
            matrix_mismatch_mask;
        if (matrix_mismatch_mask != 0u)
            ++model_ft4.snapshot.projection_matrix_mismatch_count;
        memcpy(context.projection.rotation, matrix.rotation,
               sizeof(context.projection.rotation));
        memcpy(context.projection.translation, matrix.translation,
               sizeof(context.projection.translation));
    }
    context.tpage = cpu->read_half(UINT32_C(0x80059308));
    context.clut = cpu->read_half(UINT32_C(0x8005930c));
    context.dispatch_mode = (uint8_t)cpu->gpr[7];
    context.caller_window_start = caller_window_start;
    context.caller_contract = caller_contract;
    context.resident_dispatch =
        caller_contract == XG_RENDER_MODEL_DISPATCH_CALLER_RESIDENT;
    context.valid = true;
    if (render_mode == GUEST_RENDER_RENDER_NATIVE) {
        if (caller_contract == XG_RENDER_MODEL_DISPATCH_CALLER_GEAR_HELPER) {
            if (!xg_render_gear_motion_bind(cpu,&context.motion,&context.motion_part))
                context.motion=(XgRenderMotionRef){0};
        } else if (!(context.resident_dispatch
                ? capture_resident_field_motion(cpu, &context, services)
                : capture_field_motion(cpu, &context, services)))
            context.motion=(XgRenderMotionRef){0};
    }
    model_ft4.context = context;
    /* Endpoint geometry is independent of rigid-pose eligibility. In particular
     * lit Field props and billboard/deformable models still need subpixel XY. */
    if (render_mode == GUEST_RENDER_RENDER_NATIVE) capture_model_geometry_bindings(cpu,services);
}

static bool apply_ft4_material(
        XgModelFt4RawSource *source, ModelFt4Record *record) {
    GpuDrawState draw = {0};

    if ((record->material_word >> 24u) != 0x2du &&
        (record->material_word >> 24u) != 0x2fu)
        return false;
    gpu_get_draw_state(&draw);
    xg_render_material_apply_draw_state(&source->material, &draw);
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
    for (uint32_t vertex = 0u; vertex < 4u; ++vertex) {
        source->uv[vertex][0] = (uint8_t)record->uv[vertex];
        source->uv[vertex][1] = (uint8_t)(record->uv[vertex] >> 8u);
    }
    return true;
}

static bool decode_ft4_material(
        CPUState *cpu, uint32_t attribute, uint16_t tpage, uint16_t clut,
        XgModelFt4RawSource *source, ModelFt4Record *record) {
    if (cpu == NULL || source == NULL || record == NULL ||
        cpu->read_word == NULL || cpu->read_half == NULL ||
        cpu->read_byte == NULL || !word_address_is_valid(attribute) ||
        !word_address_is_valid(attribute + 8u))
        return false;
    record->material_word = cpu->read_word(attribute);
    record->uv[0] = cpu->read_half(attribute + 4u);
    record->uv[1] = cpu->read_half(attribute + 6u);
    record->uv[2] = cpu->read_half(attribute + 8u);
    record->uv[3] = cpu->read_half(attribute + 10u);
    record->tpage = tpage;
    record->clut = clut;
    return apply_ft4_material(source, record);
}

static bool capture_ft4_packet_material(
        CPUState *cpu, uint32_t packet, XgModelFt4RawSource *source,
        ModelFt4Record *record) {
    if (cpu == NULL || source == NULL || record == NULL ||
        cpu->read_word == NULL || cpu->read_half == NULL ||
        !word_address_is_valid(packet) || !word_address_is_valid(packet + 36u))
        return false;
    record->material_word = cpu->read_word(packet + 4u);
    if ((record->material_word >> 24u) != 0x2du &&
        (record->material_word >> 24u) != 0x2fu)
        return false;
    record->uv[0] = cpu->read_half(packet + 12u);
    record->uv[1] = cpu->read_half(packet + 20u);
    record->uv[2] = cpu->read_half(packet + 28u);
    record->uv[3] = cpu->read_half(packet + 36u);
    record->tpage = cpu->read_half(packet + 22u);
    record->clut = cpu->read_half(packet + 14u);
    return apply_ft4_material(source, record);
}

static uint32_t prepare_precondition_mask(CPUState *cpu) {
    const ModelContext *context = &model_ft4.context;
    uint32_t mask = 0u;

    if (!context->valid) mask |= MODEL_PRECONDITION_CONTEXT;
    if (cpu == NULL) return mask | MODEL_PRECONDITION_CPU;
    if (cpu->read_word == NULL || cpu->read_half == NULL ||
        cpu->read_byte == NULL)
        mask |= MODEL_PRECONDITION_CALLBACKS;
    if (!physical_address_equals(cpu->gpr[31], UINT32_C(0x8002c86c)))
        mask |= MODEL_PRECONDITION_RETURN;
    if (cpu->gpr[5] == 0u) mask |= MODEL_PRECONDITION_TARGET_ZERO;
    if (cpu->gpr[5] > MODEL_PRIMITIVE_MAXIMUM)
        mask |= MODEL_PRECONDITION_TARGET_CAPACITY;
    if (context->valid && context->resident_dispatch &&
        cpu->read_word != NULL) {
        if (cpu->read_word(UINT32_C(0x8005953c)) != context->vertex_base)
            mask |= MODEL_PRECONDITION_VERTEX_BASE;
        if (cpu->read_word(UINT32_C(0x80059568)) != context->ot_base)
            mask |= MODEL_PRECONDITION_OT_BASE;
    }
    return mask;
}

static bool prepare_model_ft4(
        CPUState *cpu, GuestRenderRenderMode render_mode,
        const XgRenderModelSpritePipelineServices *services) {
    static const uint8_t attribute_sizes[17] = {
        4u, 8u, 4u, 8u, 4u, 8u, 4u, 8u, 4u,
        12u, 4u, 12u, 4u, 12u, 4u, 12u, 4u,
    };
    static const uint8_t split[2][3] = {{0u, 1u, 2u}, {2u, 1u, 3u}};
    ModelContext *context = &model_ft4.context;
    uint32_t group_address;
    uint32_t attribute_address;
    uint32_t group_count;
    uint32_t target_count;
    uint16_t tpage;
    uint16_t clut;
    bool found = false;

    model_ft4.snapshot.prepare_failure_detail = 0u;
    model_ft4.snapshot.last_target_count = cpu != NULL ? cpu->gpr[5] : 0u;
    model_ft4.snapshot.prepare_precondition_failure_mask =
        prepare_precondition_mask(cpu);
    if (model_ft4.snapshot.prepare_precondition_failure_mask != 0u) {
        model_ft4.snapshot.prepare_failure_detail = 1u;
        return false;
    }
    target_count = cpu->gpr[5];
    ModelFt4Record *grown = xg_render_array_reserve(model_ft4.records, sizeof(*grown),
        &model_ft4.capacity, target_count, MODEL_PRIMITIVE_MAXIMUM);
    if (!grown) { model_ft4.snapshot.prepare_failure_detail = 1u; return false; }
    model_ft4.records = grown;
    group_address = context->topology_base;
    attribute_address = context->material_base;
    group_count = cpu->read_half(context->model_address + 6u);
    model_ft4.snapshot.last_group_count = group_count;
    model_ft4.snapshot.last_target_count = target_count;
    tpage = context->tpage;
    clut = context->clut;
    if (group_count == 0u || group_count > 256u) {
        model_ft4.snapshot.prepare_failure_detail = 2u;
        return false;
    }
    for (uint32_t group = 0u; group < group_count; ++group) {
        const uint8_t row = cpu->read_byte(group_address);
        const uint32_t primitive_count = cpu->read_half(group_address + 2u);
        const uint32_t descriptors = group_address + 4u;

        if (row >= 17u ||
            descriptors > UINT32_MAX - primitive_count * 8u) {
            model_ft4.snapshot.prepare_failure_detail = 3u;
            return false;
        }
        for (uint32_t primitive = 0u; primitive < primitive_count;
             ++primitive) {
            if (!consume_controls(cpu, &attribute_address, &tpage, &clut)) {
                model_ft4.snapshot.prepare_failure_detail = 4u;
                return false;
            }
            if (descriptors == cpu->gpr[4]) {
                ModelFt4Record *record;
                XgModelFt4RawSource source = {0};

                if (row != 13u || primitive_count != target_count ||
                    primitive >= model_ft4.capacity) {
                    model_ft4.snapshot.prepare_failure_detail = 5u;
                    return false;
                }
                found = true;
                record = &model_ft4.records[primitive];
                *record = (ModelFt4Record){0};
                record->attribute_address = attribute_address;
                model_ft4.snapshot.last_attribute_address = attribute_address;
                model_ft4.snapshot.last_material_word =
                    cpu->read_word(attribute_address);
                source.projection = context->projection;
                source.screen_right =
                    (int16_t)cpu->read_word(UINT32_C(0x800500f8));
                source.screen_x_cull_margin =
                    services->screen_x_cull_margin();
                source.packed_screen_bottom =
                    cpu->read_word(UINT32_C(0x800500fc));
                source.packet_address =
                    cpu->read_word(UINT32_C(0x80059424)) + primitive * 0x28u;
                source.ordering_shift =
                    cpu->read_word(UINT32_C(0x80050100));
                source.dispatch_mode = context->dispatch_mode;
                record->packet_address = source.packet_address;
                const XgRenderModelFt4Template *material =
                    xg_render_model_repository_find_packet_template(
                        source.packet_address, render_mode, services->repository);
                if (material == NULL)
                    material =
                        xg_render_model_repository_find_descriptor_template(
                            attribute_address, render_mode,
                            services->repository);
                if (material != NULL) {
                    record->attribute_address = material->descriptor_address;
                    record->material_word = material->material_word;
                    memcpy(record->uv, material->uv, sizeof(record->uv));
                    record->tpage = material->tpage;
                    record->clut = material->clut;
                    source.material_word = record->material_word;
                    source.relit_color_source =
                        XG_MODEL_FT4_RAW_RELIT_COLOR_CAPTURED;
                    ++model_ft4.snapshot.template_hit_count;
                    if (!apply_ft4_material(&source, record)) {
                        model_ft4.snapshot.prepare_failure_detail = 6u;
                        return false;
                    }
                } else {
                    ++model_ft4.snapshot.template_miss_count;
                    if (capture_ft4_packet_material(
                            cpu, source.packet_address, &source, record)) {
                        source.relit_color_source =
                            XG_MODEL_FT4_RAW_RELIT_COLOR_CAPTURED;
                    } else if (decode_ft4_material(
                                   cpu, attribute_address, tpage, clut,
                                   &source, record)) {
                        source.relit_color_source =
                            XG_MODEL_FT4_RAW_RELIT_COLOR_RESOLVED;
                    } else {
                        model_ft4.snapshot.prepare_failure_detail = 6u;
                        return false;
                    }
                    source.material_word = record->material_word;
                }
                for (uint32_t vertex = 0u; vertex < 4u; ++vertex) {
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
                    model_ft4.snapshot.prepare_failure_detail = 7u;
                    return false;
                }
                bind_model_motion(record->packet_address, source.vertices,
                    record->source_vertex_indices, 4, record->attribute_address, false);
                if (context->projection_motion.handle.resource_id)
                    for (uint32_t t = 0; t < 2u; ++t)
                        for (uint32_t v = 0; v < 3u; ++v) {
                            XgRenderIrVertex *target = &record->native.primitive.triangles[t].vertices[v];
                            if (target->native_view_position &&
                                !xg_render_motion_refine_native_vertex(context->projection_motion,
                                    context->motion_part, &source.vertices[split[t][v]],
                                    &target->native_view_x, &target->native_view_y,
                                    &target->native_view_depth)) return false;
                        }
                for (uint32_t triangle = 0u; triangle < 2u; ++triangle) {
                    for (uint32_t vertex = 0u; vertex < 3u; ++vertex) {
                        XgRenderIrVertex *destination =
                            &record->native.primitive.triangles[triangle]
                                 .vertices[vertex];
                        const uint32_t group_id =
                            context->instance_address & UINT32_C(0x1fffffff);
                        destination->interpolation_group_id = group_id;
                        destination->interpolation_vertex_id =
                            record->source_vertex_indices[split[triangle][vertex]];
                        destination->interpolation_vertex_identity_valid =
                            group_id != 0u;
                    }
                }
                xg_render_depth_policy_stamp_primitive(
                    &record->native.primitive, XG_RENDER_DEPTH_FAMILY_FIELD_MODELS);
            }
            attribute_address += attribute_sizes[row];
        }
        group_address = descriptors + primitive_count * 8u;
        if (found) break;
    }
    if (!found) {
        model_ft4.snapshot.prepare_failure_detail = 8u;
        return false;
    }
    model_ft4.initial_packet_cursor =
        cpu->read_word(UINT32_C(0x80059424));
    model_ft4.initial_counter = cpu->read_word(UINT32_C(0x80059578));
    model_ft4.descriptor_base = cpu->gpr[4];
    model_ft4.count = target_count;
    for (uint32_t index = 0u; index < target_count; ++index) {
        ModelFt4Record *record = &model_ft4.records[index];
        if (!record->native.accepted) continue;
        if (record->native.ordering_bucket >= 0x1000u ||
            context->ot_base > UINT32_MAX -
                record->native.ordering_bucket * 4u ||
            !word_address_is_valid(context->ot_base +
                record->native.ordering_bucket * 4u)) {
            model_ft4.snapshot.prepare_failure_detail = 9u;
            return false;
        }
    }
    model_ft4.snapshot.pending = true;
    return true;
}

static bool stage_request(
        const XgRenderModelSpritePipelineServices *services,
        const XgRenderModelSpriteStageRequest *request) {
    XgRenderPreScenePrimitive record;

    if (services == NULL || services->stage_pre_scene == NULL ||
        request == NULL || request->primitive == NULL)
        return false;
    record = (XgRenderPreScenePrimitive){
        .primitive = *request->primitive,
        .packet_address = request->packet_address,
        .source_primitive_index = request->source_primitive_index,
        .ot_bucket = request->ot_bucket,
        .interpolation_producer_id = request->interpolation_producer_id,
        .interpolation_primitive_id = request->interpolation_primitive_id,
        .payload_word_count = request->payload_word_count,
        .interpolation_identity_valid = request->interpolation_identity_valid,
        .temporal_only = request->temporal_only,
        .temporal_cull = request->temporal_cull != NULL
            ? *request->temporal_cull : (GpuRenderTemporalCullPolicy){0},
    };
    return services->stage_pre_scene(&record);
}

static bool discard_staged_model_ft4_record(const ModelFt4Record *record) {
    XgRenderPreScenePrimitive key = {0};

    if (record == NULL) return false;
    if (record->native.accepted) {
        key.packet_address = record->packet_address;
    } else if (xg_render_primitive_all_projective(
                   &record->native.primitive)) {
        key.interpolation_producer_id = record->interpolation_producer_id;
        key.interpolation_primitive_id = record->interpolation_primitive_id;
        key.temporal_only = true;
    } else {
        return true;
    }
    return xg_render_submission_pre_scene_discard(&key);
}

static bool stage_model_ft4(
        CPUState *cpu, GuestRenderRenderMode render_mode,
        const XgRenderModelSpritePipelineServices *services) {
    uint32_t accepted_count = 0u;
    uint32_t staged_count = 0u;
    XgRenderProducerLifecycle lifecycle;

    if (render_mode != GUEST_RENDER_RENDER_NATIVE || services == NULL ||
        !model_dispatch_context_contract_matches(
            cpu, model_ft4.context.caller_contract,
            model_ft4.context.caller_window_start) ||
        cpu == NULL || !model_ft4.context.valid ||
        !prepare_model_ft4(cpu, render_mode, services) ||
        !lifecycle_begin(services, UINT32_C(0x8002c700), &lifecycle))
        return false;
    for (uint32_t index = 0u; index < model_ft4.count; ++index) {
        accepted_count += model_ft4.records[index].native.accepted;
        staged_count += model_ft4.records[index].native.accepted ||
            xg_render_primitive_all_projective(
                &model_ft4.records[index].native.primitive);
    }
    if (services->pre_scene_available == NULL ||
        !services->pre_scene_available(staged_count) ||
        model_ft4.snapshot.native_cutover_count == UINT64_MAX ||
        model_ft4.snapshot.native_primitive_count >
            UINT64_MAX - accepted_count) {
        block_model_ft4(77u);
        return false;
    }
    for (uint32_t index = 0u; index < model_ft4.count; ++index) {
        ModelFt4Record *record = &model_ft4.records[index];
        const uint32_t producer_id =
            model_ft4.context.instance_address & UINT32_C(0x1fffffff);
        const uint32_t primitive_id =
            (record->attribute_address & UINT32_C(0x1fffffff)) |
            ((uint32_t)(model_ft4.context.dispatch_mode & 7u) << 29u);
        const bool identity_valid = producer_id != 0u;
        const int32_t margin = services->screen_x_cull_margin();
        const uint32_t ordering_shift =
            cpu->read_word(UINT32_C(0x80050100));
        const bool average_depth =
            model_ft4.context.dispatch_mode ==
                XG_MODEL_FT4_RAW_DISPATCH_AVERAGE ||
            model_ft4.context.dispatch_mode ==
                XG_MODEL_FT4_RAW_DISPATCH_RELIT ||
            model_ft4.context.dispatch_mode ==
                XG_MODEL_FT4_RAW_DISPATCH_AVERAGE_DEPTH_CUE;
        const GpuRenderTemporalCullPolicy temporal_cull = {
            .flags = GPU_RENDER_TEMPORAL_CULL_PROJECTIVE |
                GPU_RENDER_TEMPORAL_CULL_SCREEN |
                GPU_RENDER_TEMPORAL_CULL_FRONT_FACE |
                GPU_RENDER_TEMPORAL_CULL_DEPTH,
            .screen_left = margin > 0
                ? -margin * INT32_C(65536) : INT32_MIN,
            .screen_top = INT32_MIN,
            .screen_right_exclusive =
                ((int32_t)(uint16_t)cpu->read_word(UINT32_C(0x800500f8)) +
                 margin) * INT32_C(65536),
            .screen_bottom_exclusive =
                (int32_t)(uint16_t)(
                    cpu->read_word(UINT32_C(0x800500fc)) >> 16u) *
                INT32_C(65536),
            .depth_min_inclusive = 1,
            .depth_max_exclusive = 0x10000,
            .depth_mode = average_depth ? GPU_RENDER_TEMPORAL_DEPTH_AVERAGE :
                (model_ft4.context.dispatch_mode ==
                     XG_MODEL_FT4_RAW_DISPATCH_NEAREST
                 ? GPU_RENDER_TEMPORAL_DEPTH_MINIMUM
                 : GPU_RENDER_TEMPORAL_DEPTH_MAXIMUM),
            .front_face = GPU_RENDER_TEMPORAL_FRONT_POSITIVE,
            .ordering_depth_shift = (uint8_t)(average_depth
                ? ordering_shift : ((ordering_shift + 2u) & 31u)),
        };

        record->lifecycle = lifecycle;
        record->interpolation_producer_id = producer_id;
        record->interpolation_primitive_id = primitive_id;
        record->interpolation_identity_valid = identity_valid;
        /* Native Work publishes only the validated endpoint at handler exit. */
        if (xg_render_submission_native_work_mode()) continue;
        if (!record->native.accepted) {
            if (!identity_valid || !xg_render_primitive_all_projective(
                    &record->native.primitive))
                continue;
            if (!stage_request(services, &(XgRenderModelSpriteStageRequest){
                    .primitive = &record->native.primitive,
                    .temporal_cull = &temporal_cull,
                    .interpolation_producer_id = producer_id,
                    .interpolation_primitive_id = primitive_id,
                    .interpolation_identity_valid = true,
                    .temporal_only = true,
                })) {
                block_model_ft4(78u);
                return false;
            }
            model_ft4.pre_scene_staged = true;
            continue;
        }
        if (!stage_request(services, &(XgRenderModelSpriteStageRequest){
                .primitive = &record->native.primitive,
                .packet_address = record->packet_address,
                .source_primitive_index = UINT32_C(0x50000000) |
                    (record->packet_address & UINT32_C(0x001ffffc)),
                .ot_bucket = record->native.ordering_bucket,
                .interpolation_producer_id = producer_id,
                .interpolation_primitive_id = primitive_id,
                .payload_word_count = 9u,
                .interpolation_identity_valid = identity_valid,
            })) {
            block_model_ft4(78u);
            return false;
        }
        model_ft4.pre_scene_staged = true;
    }
    ++model_ft4.snapshot.native_cutover_count;
    model_ft4.snapshot.native_primitive_count += accepted_count;
    return true;
}

void xg_render_model_sprite_pipeline_observe_ft4_guest_pass(
        CPUState *cpu, bool average_mode) {
    uint32_t descriptor_base;
    uint32_t next_descriptor;
    uint32_t descriptor_offset;
    uint32_t primitive_index;
    uint32_t ordering_depth;
    uint32_t ordering_bucket;
    ModelFt4Record *record;

    if (!model_ft4.snapshot.pending || cpu == NULL ||
        cpu->read_word == NULL)
        return;
    if ((average_mode && model_ft4.context.dispatch_mode !=
             XG_MODEL_FT4_RAW_DISPATCH_AVERAGE &&
         model_ft4.context.dispatch_mode !=
             XG_MODEL_FT4_RAW_DISPATCH_RELIT) ||
        (!average_mode && model_ft4.context.dispatch_mode !=
             XG_MODEL_FT4_RAW_DISPATCH_FARTHEST)) {
        block_model_ft4(79u);
        return;
    }
    descriptor_base = model_ft4.descriptor_base & UINT32_C(0x1fffffff);
    next_descriptor = cpu->gpr[4] & UINT32_C(0x1fffffff);
    if (next_descriptor < descriptor_base + 8u) {
        block_model_ft4(79u);
        return;
    }
    descriptor_offset = next_descriptor - descriptor_base - 8u;
    if ((descriptor_offset & 7u) != 0u) {
        block_model_ft4(79u);
        return;
    }
    primitive_index = descriptor_offset / 8u;
    if (primitive_index >= model_ft4.count) {
        block_model_ft4(79u);
        return;
    }
    record = &model_ft4.records[primitive_index];
    if (record->guest_observed_passed_screen_cull) {
        block_model_ft4(80u);
        return;
    }
    record->guest_observed_passed_screen_cull = true;
    ++model_ft4.expected_counter_delta;
    ++model_ft4.snapshot.guest_pass_observation_count;
    if (!record->native.passed_screen_cull)
        ++model_ft4.snapshot.guest_pass_projection_disagreement_count;
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
        const uint32_t ot_base = model_ft4.context.ot_base;
        if (ordering_bucket >= 0x1000u ||
            ot_base > UINT32_MAX - ordering_bucket * 4u ||
            !word_address_is_valid(ot_base + ordering_bucket * 4u)) {
            block_model_ft4(81u);
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
    }
}

static bool publish_pose_endpoint(const XgRenderIrNativePrimitive *primitive,
    uint32_t command_id, uint32_t producer_id, uint32_t primitive_id,
    XgRenderMotionRef motion) {
    if (!xg_render_submission_native_work_mode()) return true;
    GpuRenderSemantic semantic;
    const XgRenderMotionPose *pose = NULL;
    if ((motion.handle.resource_id && !xg_render_motion_view(motion, &pose)) ||
        xg_render_backend_translate_primitive(primitive, &semantic) != XG_RENDER_BACKEND_OK) return false;
    /* Preserve source vertex IDs for unposed meshes too. The pre-scene bridge
     * assigns corner IDs when consumed, which discards their shared topology. */
    xg_render_semantic_set_interpolation_identity(&semantic,
        pose ? pose->continuity_generation : xg_render_submission_temporal_scene(), producer_id, primitive_id);
    return xg_render_submission_stage_exact((GpuRenderTransactionId){0}, command_id, &semantic) ==
        GUEST_RENDER_TRANSACTION_OK;
}

static bool publish_model_endpoint(const XgRenderIrNativePrimitive *primitive,
    uint32_t command_id, uint32_t producer_id, uint32_t primitive_id) {
    return publish_pose_endpoint(primitive, command_id, producer_id, primitive_id,
        model_ft4.context.motion);
}

static bool publish_model_ft4(
        const XgRenderModelSpritePipelineServices *services) {
    for (uint32_t index = 0u; index < model_ft4.count; ++index) {
        const ModelFt4Record *record = &model_ft4.records[index];
        const XgRenderModelFt4SourceRecord source = {
            .primitive = record->native.primitive,
            .lifecycle = record->lifecycle,
            .source_id =
                (record->packet_address & UINT32_C(0x1fffffff)) + 4u,
            .interpolation_producer_id = record->interpolation_producer_id,
            .interpolation_primitive_id = record->interpolation_primitive_id,
            .opcode = (uint8_t)(record->material_word >> 24u),
            .interpolation_identity_valid =
                record->interpolation_identity_valid,
            .valid = true,
        };
        if (!record->guest_observed_accepted || !record->output_validated)
            continue;
        if (!xg_render_model_repository_store_ft4_source(
                &source, &(XgRenderModelSourcePublication){
                    .resource_address = record->packet_address + 4u,
                    .resource_size = 0x24u,
                    .register_replay = true,
                }, services->repository))
            return false;
        if (!publish_model_endpoint(&source.primitive,source.source_id,
            source.interpolation_producer_id,source.interpolation_primitive_id)) return false;
        ++model_ft4.snapshot.publish_source_count;
    }
    return true;
}

static void finish_model_ft4(
        CPUState *cpu, GuestRenderRenderMode render_mode,
        const XgRenderModelSpritePipelineServices *services) {
    ModelContext context;
    bool framing_matches = true;

    if (!model_ft4.snapshot.pending) return;
    if (cpu == NULL || cpu->read_word == NULL || cpu->read_half == NULL) {
        block_model_ft4(73u);
        return;
    }
    ++model_ft4.snapshot.invocation_count;
    model_ft4.snapshot.last_primitive_count = model_ft4.count;
    model_ft4.snapshot.primitive_count += model_ft4.count;
    for (uint32_t index = 0u; index < model_ft4.count; ++index) {
        ModelFt4Record *record = &model_ft4.records[index];
        const bool payload_matches = compare_ft4_payload(
            cpu, record->packet_address, record->attribute_address,
            record->material_word, record->uv, record->tpage, record->clut,
            &model_ft4.snapshot.first_payload_mismatch);
        bool geometry_matches = true;
        bool tag_matches = true;
        bool ot_matches = true;

        if (record->guest_observed_accepted) {
            bool last_in_bucket = true;
            tag_matches = cpu->read_word(record->packet_address) ==
                record->expected_tag;
            for (uint32_t vertex = 0u; vertex < 4u; ++vertex) {
                geometry_matches &= cpu->read_word(
                    record->packet_address + 8u + vertex * 8u) ==
                    record->observed_xy[vertex];
                geometry_matches &= record->native.vertices[vertex].x ==
                        low_s16(record->observed_xy[vertex]) &&
                    record->native.vertices[vertex].y ==
                        low_s16(record->observed_xy[vertex] >> 16u);
            }
            for (uint32_t later = index + 1u; later < model_ft4.count; ++later) {
                if (model_ft4.records[later].guest_observed_accepted &&
                    model_ft4.records[later].native.ordering_bucket ==
                        record->native.ordering_bucket)
                    last_in_bucket = false;
            }
            if (last_in_bucket)
                ot_matches = cpu->read_word(model_ft4.context.ot_base +
                    record->native.ordering_bucket * 4u) ==
                    (record->packet_address & UINT32_C(0x00ffffff));
        }
        const bool matches = payload_matches && geometry_matches &&
            tag_matches && ot_matches;
        record->output_validated = record->guest_observed_accepted && matches;
        if (record->guest_observed_accepted && !matches)
            ++model_ft4.snapshot.validation_rejected_source_count;
        if (!payload_matches) ++model_ft4.snapshot.payload_mismatch_count;
        if (!geometry_matches) ++model_ft4.snapshot.geometry_mismatch_count;
        if (!tag_matches) ++model_ft4.snapshot.tag_mismatch_count;
        if (!ot_matches) ++model_ft4.snapshot.ot_mismatch_count;
        if (matches) {
            ++model_ft4.snapshot.match_count;
        } else {
            if (model_ft4.snapshot.mismatch_count == 0u) {
                model_ft4.snapshot.first_mismatch_primitive = index;
                model_ft4.snapshot.first_mismatch_packet =
                    record->packet_address;
            }
            ++model_ft4.snapshot.mismatch_count;
        }
    }
    if ((cpu->read_word(UINT32_C(0x80059424)) & UINT32_C(0x00ffffff)) !=
        ((model_ft4.initial_packet_cursor + model_ft4.count * 0x28u) &
         UINT32_C(0x00ffffff))) {
        framing_matches = false;
        ++model_ft4.snapshot.cursor_mismatch_count;
        ++model_ft4.snapshot.mismatch_count;
    }
    const uint32_t actual_counter_delta =
        cpu->read_word(UINT32_C(0x80059578)) - model_ft4.initial_counter;
    model_ft4.snapshot.last_expected_counter_delta =
        model_ft4.expected_counter_delta;
    model_ft4.snapshot.last_actual_counter_delta = actual_counter_delta;
    if (actual_counter_delta != model_ft4.expected_counter_delta) {
        framing_matches = false;
        ++model_ft4.snapshot.counter_mismatch_count;
        ++model_ft4.snapshot.mismatch_count;
    }
    if (render_mode == GUEST_RENDER_RENDER_NATIVE) {
        for (uint32_t index = 0u; index < model_ft4.count; ++index) {
            const ModelFt4Record *record = &model_ft4.records[index];
            const bool output_matches =
                record->native.accepted == record->guest_observed_accepted &&
                (!record->guest_observed_accepted || record->output_validated);

            if (!xg_render_submission_native_work_mode() &&
                (!framing_matches || !output_matches) &&
                !discard_staged_model_ft4_record(record)) {
                block_model_ft4(82u);
                return;
            }
        }
        if (framing_matches) {
            ++model_ft4.snapshot.publish_invocation_count;
            if (!publish_model_ft4(services)) {
                block_model_ft4(78u);
                return;
            }
        } else {
            ++model_ft4.snapshot.framing_rejected_invocation_count;
        }
    }
    context = model_ft4.context;
    clear_model_ft4_pending();
    model_ft4.context = context;
}

static int32_t model_ft3_nclip(const XgHost3dProjectedVertex vertices[3]) {
    return xg_host_3d_nclip(vertices);
}

static bool decode_ft3_material(
        CPUState *cpu, uint32_t attribute, uint16_t tpage, uint16_t clut,
        XgRenderModelFt4Template *material) {
    if (cpu == NULL || material == NULL || cpu->read_word == NULL ||
        cpu->read_half == NULL || cpu->read_byte == NULL ||
        !word_address_is_valid(attribute) ||
        !word_address_is_valid(attribute + 4u))
        return false;
    const uint8_t opcode = cpu->read_byte(attribute + 3u);
    if (opcode < 0x24u || opcode > 0x27u) return false;
    *material = (XgRenderModelFt4Template){
        .descriptor_address = normalized_word_address(attribute),
        .material_word = UINT32_C(0x00808080) |
            ((uint32_t)opcode << 24u),
        .uv = {
            cpu->read_half(attribute + 4u), cpu->read_half(attribute + 6u),
            cpu->read_half(attribute),
        },
        .tpage = tpage,
        .clut = clut,
        .valid = true,
    };
    return true;
}

static bool build_ft3_record(
        CPUState *cpu, const XgHost3dVector vertices[3],
        const XgRenderModelFt4Template *material, uint32_t packet,
        ModelFt3Record *record,
        const XgRenderModelSpritePipelineServices *services) {
    XgHost3dProject4Input input = {0};
    XgHost3dRotTransPers4Output output;
    GpuDrawState draw = {0};
    const int32_t margin = services->screen_x_cull_margin();
    uint16_t max_depth = 0u;
    bool all_below = true;
    bool guest_all_horizontal_outside = true;
    bool all_left = true;
    bool all_right = true;

    if (cpu == NULL || vertices == NULL || material == NULL || record == NULL ||
        (material->material_word >> 24u) < 0x24u ||
        (material->material_word >> 24u) > 0x27u)
        return false;
    memcpy(input.vertices, vertices, 3u * sizeof(vertices[0]));
    input.vertices[3] = vertices[2];
    input.projection = model_ft4.context.projection;
    if (!xg_host_3d_rot_trans_pers4(&input, &output)) return false;
    memcpy(record->vertices, output.vertices, sizeof(record->vertices));
    for (uint32_t vertex = 0u; vertex < 3u; ++vertex) {
        const uint32_t packed = (uint16_t)output.vertices[vertex].x |
            ((uint32_t)(uint16_t)output.vertices[vertex].y << 16u);
        all_below &= packed >= cpu->read_word(UINT32_C(0x800500fc));
        guest_all_horizontal_outside &=
            (uint16_t)output.vertices[vertex].x >=
                (uint16_t)cpu->read_word(UINT32_C(0x800500f8));
        if (margin > 0) {
            all_left &= (int32_t)output.vertices[vertex].x < -margin;
            all_right &= (int32_t)output.vertices[vertex].x >=
                (int32_t)(uint16_t)cpu->read_word(UINT32_C(0x800500f8)) +
                margin;
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
    record->guest_passed_screen_cull =
        record->nclip_positive && record->guest_screen_accepted;
    record->guest_accepted =
        record->guest_passed_screen_cull && max_depth != 0u;
    record->passed_screen_cull = (int32_t)output.rtpt_flags >= 0 &&
        model_ft3_nclip(output.vertices) > 0 && !all_below &&
        !all_left && !all_right;
    record->accepted = record->passed_screen_cull && max_depth != 0u;
    record->packet_address = packet;
    record->material_word = material->material_word;
    memcpy(record->uv, material->uv, sizeof(record->uv));
    record->tpage = material->tpage;
    record->clut = material->clut;
    gpu_get_draw_state(&draw);
    xg_render_material_apply_draw_state(&record->primitive.material, &draw);
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
    for (uint32_t vertex = 0u; vertex < 3u; ++vertex) {
        XgRenderIrVertex *destination =
            &record->primitive.triangles[0].vertices[vertex];
        destination->x =
            (int32_t)record->vertices[vertex].x * INT32_C(65536);
        destination->y =
            (int32_t)record->vertices[vertex].y * INT32_C(65536);
        destination->u =
            (int32_t)(uint8_t)record->uv[vertex] * INT32_C(65536);
        destination->v =
            (int32_t)(uint8_t)(record->uv[vertex] >> 8u) * INT32_C(65536);
        destination->r = (uint8_t)record->material_word;
        destination->g = (uint8_t)(record->material_word >> 8u);
        destination->b = (uint8_t)(record->material_word >> 16u);
        destination->native_view_x =
            record->vertices[vertex].native_view_x_16_16;
        destination->native_view_y =
            record->vertices[vertex].native_view_y_16_16;
        destination->native_view_position =
            record->vertices[vertex].native_view_position != 0u;
        destination->native_view_depth =
            record->vertices[vertex].native_view_depth_q12;
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
        destination->projective_native_offset_x =
            record->vertices[vertex].projective_native_offset_x_16_16;
        destination->projective_native_offset_y =
            record->vertices[vertex].projective_native_offset_y_16_16;
        destination->projective_distance =
            record->vertices[vertex].projective_distance;
        destination->projective_position =
            record->vertices[vertex].projective_position != 0u;
    }
    return true;
}

static bool prepare_model_ft3(
        CPUState *cpu, GuestRenderRenderMode render_mode,
        const XgRenderModelSpritePipelineServices *services) {
    static const uint8_t attribute_sizes[17] = {
        4u, 8u, 4u, 8u, 4u, 8u, 4u, 8u, 4u,
        12u, 4u, 12u, 4u, 12u, 4u, 12u, 4u,
    };
    ModelContext *context = &model_ft4.context;
    uint32_t group_address;
    uint32_t attribute_address;
    uint32_t group_count;
    uint32_t target_count;
    uint16_t tpage;
    uint16_t clut;
    bool found = false;
    XgHost3dProjection handler_projection = {0};
    uint32_t projection_mismatch_mask = 0u;

    model_ft3.snapshot.prepare_failure_detail = 0u;
    model_ft3.snapshot.last_target_count = cpu != NULL ? cpu->gpr[5] : 0u;
    model_ft3.snapshot.last_nclip_positive_count = 0u;
    model_ft3.snapshot.last_guest_screen_accepted_count = 0u;
    model_ft3.snapshot.last_guest_vertical_accepted_count = 0u;
    model_ft3.snapshot.last_guest_horizontal_accepted_count = 0u;
    model_ft3.snapshot.last_projection_flag_negative_count = 0u;
    model_ft3.snapshot.last_handler_projection_mismatch_mask = 0u;
    model_ft3.snapshot.prepare_precondition_failure_mask =
        prepare_precondition_mask(cpu);
    if (model_ft3.snapshot.prepare_precondition_failure_mask != 0u) {
        model_ft3.snapshot.prepare_failure_detail = 1u;
        return false;
    }
    xg_render_runtime_capture_shadow_projection(cpu, &handler_projection);
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
    model_ft3.snapshot.last_handler_projection_mismatch_mask =
        projection_mismatch_mask;
    if (projection_mismatch_mask != 0u)
        ++model_ft3.snapshot.handler_projection_mismatch_count;
    target_count = cpu->gpr[5];
    ModelFt3Record *grown = xg_render_array_reserve(model_ft3.records, sizeof(*grown),
        &model_ft3.capacity, target_count, MODEL_PRIMITIVE_MAXIMUM);
    if (!grown) { model_ft3.snapshot.prepare_failure_detail = 1u; return false; }
    model_ft3.records = grown;
    group_address = context->topology_base;
    attribute_address = context->material_base;
    group_count = cpu->read_half(context->model_address + 6u);
    model_ft3.snapshot.last_group_count = group_count;
    model_ft3.snapshot.last_target_count = target_count;
    tpage = context->tpage;
    clut = context->clut;
    if (group_count == 0u || group_count > 256u) {
        model_ft3.snapshot.prepare_failure_detail = 2u;
        return false;
    }
    for (uint32_t group = 0u; group < group_count; ++group) {
        const uint8_t row = cpu->read_byte(group_address);
        const uint32_t primitive_count = cpu->read_half(group_address + 2u);
        const uint32_t descriptors = group_address + 4u;
        if (row >= 17u ||
            descriptors > UINT32_MAX - primitive_count * 8u) {
            model_ft3.snapshot.prepare_failure_detail = 3u;
            return false;
        }
        for (uint32_t primitive = 0u; primitive < primitive_count;
             ++primitive) {
            if (!consume_controls(cpu, &attribute_address, &tpage, &clut)) {
                model_ft3.snapshot.prepare_failure_detail = 4u;
                return false;
            }
            if (descriptors == cpu->gpr[4]) {
                ModelFt3Record *record;
                XgRenderModelFt4Template decoded = {0};
                const XgRenderModelFt4Template *material;
                XgHost3dVector vertices[3];
                const uint32_t packet =
                    cpu->read_word(UINT32_C(0x80059424)) + primitive * 0x20u;
                if (row != 5u || primitive_count != target_count ||
                    primitive >= model_ft3.capacity) {
                    model_ft3.snapshot.prepare_failure_detail = 5u;
                    return false;
                }
                found = true;
                record = &model_ft3.records[primitive];
                *record = (ModelFt3Record){0};
                record->attribute_address = attribute_address;
                material = xg_render_model_repository_find_packet_template(
                    packet, render_mode, services->repository);
                if (material == NULL)
                    material =
                        xg_render_model_repository_find_descriptor_template(
                            attribute_address, render_mode,
                            services->repository);
                if (material == NULL) {
                    ++model_ft3.snapshot.template_miss_count;
                    if (!decode_ft3_material(
                            cpu, attribute_address, tpage, clut, &decoded)) {
                        model_ft3.snapshot.prepare_failure_detail = 6u;
                        return false;
                    }
                    material = &decoded;
                } else {
                    ++model_ft3.snapshot.template_hit_count;
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
                if (!build_ft3_record(
                        cpu, vertices, material, packet, record, services)) {
                    model_ft3.snapshot.prepare_failure_detail = 7u;
                    return false;
                }
                bind_model_motion(packet, vertices, record->source_vertex_indices,
                    3, record->attribute_address, true);
                if (context->projection_motion.handle.resource_id)
                    for (uint32_t v = 0; v < 3u; ++v) {
                        XgRenderIrVertex *target = &record->primitive.triangles[0].vertices[v];
                        if (target->native_view_position &&
                            !xg_render_motion_refine_native_vertex(context->projection_motion,
                                context->motion_part, &vertices[v],
                                &target->native_view_x, &target->native_view_y,
                                &target->native_view_depth)) return false;
                    }
                for (uint32_t vertex = 0u; vertex < 3u; ++vertex) {
                    XgRenderIrVertex *destination =
                        &record->primitive.triangles[0].vertices[vertex];
                    const uint32_t group_id =
                        context->instance_address & UINT32_C(0x1fffffff);
                    destination->interpolation_group_id = group_id;
                    destination->interpolation_vertex_id =
                        record->source_vertex_indices[vertex];
                    destination->interpolation_vertex_identity_valid =
                        group_id != 0u;
                }
                xg_render_depth_policy_stamp_primitive(
                    &record->primitive, XG_RENDER_DEPTH_FAMILY_FIELD_MODELS);
                model_ft3.snapshot.last_nclip_positive_count +=
                    record->nclip_positive;
                model_ft3.snapshot.last_guest_screen_accepted_count +=
                    record->guest_screen_accepted;
                model_ft3.snapshot.last_guest_vertical_accepted_count +=
                    record->guest_vertical_accepted;
                model_ft3.snapshot.last_guest_horizontal_accepted_count +=
                    record->guest_horizontal_accepted;
                model_ft3.snapshot.last_projection_flag_negative_count +=
                    record->projection_flag_negative;
            }
            attribute_address += attribute_sizes[row];
        }
        group_address = descriptors + primitive_count * 8u;
        if (found) break;
    }
    if (!found) {
        model_ft3.snapshot.prepare_failure_detail = 8u;
        return false;
    }
    model_ft3.initial_packet_cursor =
        cpu->read_word(UINT32_C(0x80059424));
    model_ft3.initial_counter = cpu->read_word(UINT32_C(0x80059578));
    model_ft3.descriptor_base = cpu->gpr[4];
    model_ft3.count = target_count;
    for (uint32_t index = 0u; index < target_count; ++index) {
        ModelFt3Record *record = &model_ft3.records[index];
        if (!record->accepted) continue;
        if (record->ordering_bucket >= 0x1000u ||
            context->ot_base > UINT32_MAX - record->ordering_bucket * 4u ||
            !word_address_is_valid(
                context->ot_base + record->ordering_bucket * 4u)) {
            model_ft3.snapshot.prepare_failure_detail = 9u;
            return false;
        }
    }
    model_ft3.snapshot.pending = true;
    return true;
}

static bool stage_model_ft3(
        CPUState *cpu, GuestRenderRenderMode render_mode,
        const XgRenderModelSpritePipelineServices *services) {
    uint32_t accepted_count = 0u;
    uint32_t staged_count = 0u;
    XgRenderProducerLifecycle lifecycle;

    if (render_mode != GUEST_RENDER_RENDER_NATIVE || services == NULL ||
        !model_dispatch_context_contract_matches(
            cpu, model_ft4.context.caller_contract,
            model_ft4.context.caller_window_start) ||
        !prepare_model_ft3(cpu, render_mode, services) ||
        !lifecycle_begin(services, UINT32_C(0x8002c700), &lifecycle))
        return false;
    for (uint32_t index = 0u; index < model_ft3.count; ++index) {
        accepted_count += model_ft3.records[index].accepted;
        staged_count += model_ft3.records[index].accepted ||
            xg_render_primitive_all_projective(&model_ft3.records[index].primitive);
    }
    if (services->pre_scene_available == NULL ||
        !services->pre_scene_available(staged_count))
        return false;
    for (uint32_t index = 0u; index < model_ft3.count; ++index) {
        ModelFt3Record *record = &model_ft3.records[index];
        const uint32_t producer_id =
            model_ft4.context.instance_address & UINT32_C(0x1fffffff);
        const uint32_t primitive_id =
            (record->attribute_address & UINT32_C(0x1fffffff)) |
            ((uint32_t)(model_ft4.context.dispatch_mode & 7u) << 29u) |
            UINT32_C(1);
        const bool identity_valid = producer_id != 0u;
        const int32_t margin = services->screen_x_cull_margin();
        const GpuRenderTemporalCullPolicy temporal_cull = {
            .flags = GPU_RENDER_TEMPORAL_CULL_PROJECTIVE |
                GPU_RENDER_TEMPORAL_CULL_SCREEN |
                GPU_RENDER_TEMPORAL_CULL_FRONT_FACE |
                GPU_RENDER_TEMPORAL_CULL_DEPTH,
            .screen_left = margin > 0
                ? -margin * INT32_C(65536) : INT32_MIN,
            .screen_top = INT32_MIN,
            .screen_right_exclusive =
                ((int32_t)(uint16_t)cpu->read_word(UINT32_C(0x800500f8)) +
                 margin) * INT32_C(65536),
            .screen_bottom_exclusive =
                (int32_t)(uint16_t)(
                    cpu->read_word(UINT32_C(0x800500fc)) >> 16u) *
                INT32_C(65536),
            .depth_min_inclusive = 1,
            .depth_max_exclusive = 0x10000,
            .depth_mode = GPU_RENDER_TEMPORAL_DEPTH_MAXIMUM,
            .front_face = GPU_RENDER_TEMPORAL_FRONT_POSITIVE,
            .ordering_depth_shift = (uint8_t)(
                (cpu->read_word(UINT32_C(0x80050100)) + 2u) & 31u),
        };
        record->lifecycle = lifecycle;
        record->interpolation_producer_id = producer_id;
        record->interpolation_primitive_id = primitive_id;
        record->interpolation_identity_valid = identity_valid;
        if (xg_render_submission_native_work_mode()) continue;
        if (!record->accepted) {
            if (!identity_valid ||
                !xg_render_primitive_all_projective(&record->primitive))
                continue;
            if (!stage_request(services, &(XgRenderModelSpriteStageRequest){
                    .primitive = &record->primitive,
                    .temporal_cull = &temporal_cull,
                    .interpolation_producer_id = producer_id,
                    .interpolation_primitive_id = primitive_id,
                    .interpolation_identity_valid = true,
                    .temporal_only = true,
                }))
                return false;
            continue;
        }
        if (!stage_request(services, &(XgRenderModelSpriteStageRequest){
                .primitive = &record->primitive,
                .packet_address = record->packet_address,
                .source_primitive_index = UINT32_C(0x51000000) |
                    (record->packet_address & UINT32_C(0x001ffffc)),
                .ot_bucket = record->ordering_bucket,
                .interpolation_producer_id = producer_id,
                .interpolation_primitive_id = primitive_id,
                .payload_word_count = 7u,
                .interpolation_identity_valid = identity_valid,
            }))
            return false;
    }
    ++model_ft3.snapshot.native_cutover_count;
    model_ft3.snapshot.native_primitive_count += accepted_count;
    return true;
}

void xg_render_model_sprite_pipeline_observe_ft3_guest_pass(CPUState *cpu) {
    uint32_t descriptor_base;
    uint32_t next_descriptor;
    uint32_t descriptor_offset;
    uint32_t primitive_index;
    uint32_t max_depth;
    uint32_t ordering_bucket;
    ModelFt3Record *record;

    if (!model_ft3.snapshot.pending || cpu == NULL ||
        cpu->read_word == NULL)
        return;
    descriptor_base = model_ft3.descriptor_base & UINT32_C(0x1fffffff);
    next_descriptor = cpu->gpr[4] & UINT32_C(0x1fffffff);
    if (next_descriptor < descriptor_base + 8u) {
        block_model_ft3(75u);
        return;
    }
    descriptor_offset = next_descriptor - descriptor_base - 8u;
    if ((descriptor_offset & 7u) != 0u) {
        block_model_ft3(75u);
        return;
    }
    primitive_index = descriptor_offset / 8u;
    if (primitive_index >= model_ft3.count) {
        block_model_ft3(75u);
        return;
    }
    record = &model_ft3.records[primitive_index];
    if (record->guest_observed_passed_screen_cull) {
        block_model_ft3(76u);
        return;
    }
    record->guest_observed_passed_screen_cull = true;
    ++model_ft3.expected_counter_delta;
    ++model_ft3.snapshot.guest_pass_observation_count;
    if (!record->guest_passed_screen_cull)
        ++model_ft3.snapshot.guest_pass_projection_disagreement_count;
    max_depth = (uint16_t)cpu->gte_data[17];
    for (uint32_t depth = 18u; depth <= 19u; ++depth) {
        if ((uint16_t)cpu->gte_data[depth] > max_depth)
            max_depth = (uint16_t)cpu->gte_data[depth];
    }
    ordering_bucket = max_depth >>
        ((cpu->read_word(UINT32_C(0x80050100)) + 2u) & 31u);
    if (max_depth != 0u) {
        const uint32_t ot_base = model_ft4.context.ot_base;
        if (ordering_bucket >= 0x1000u ||
            ot_base > UINT32_MAX - ordering_bucket * 4u ||
            !word_address_is_valid(ot_base + ordering_bucket * 4u)) {
            block_model_ft3(77u);
            return;
        }
        record->ordering_bucket = (uint16_t)ordering_bucket;
        record->expected_tag = cpu->read_word(
            ot_base + ordering_bucket * 4u) | UINT32_C(0x07000000);
        record->guest_observed_accepted = true;
    }
    for (uint32_t vertex = 0u; vertex < 3u; ++vertex) {
        record->observed_xy[vertex] = cpu->gte_data[12u + vertex];
    }
}

static bool publish_model_ft3(
        const XgRenderModelSpritePipelineServices *services) {
    for (uint32_t index = 0u; index < model_ft3.count; ++index) {
        const ModelFt3Record *record = &model_ft3.records[index];
        const XgRenderModelFt3SourceRecord source = {
            .primitive = record->primitive,
            .lifecycle = record->lifecycle,
            .source_id =
                (record->packet_address & UINT32_C(0x1fffffff)) + 4u,
            .interpolation_producer_id = record->interpolation_producer_id,
            .interpolation_primitive_id = record->interpolation_primitive_id,
            .interpolation_identity_valid =
                record->interpolation_identity_valid,
            .geometry_ready = true,
            .valid = true,
        };
        if (!record->guest_observed_accepted || !record->output_validated)
            continue;
        if (!xg_render_model_repository_store_ft3_source(
                &source, &(XgRenderModelSourcePublication){
                    .resource_address = record->packet_address,
                    .resource_size = 0x20u,
                    .register_replay = true,
                }, services->repository))
            return false;
        if (!publish_model_endpoint(&source.primitive,source.source_id,
            source.interpolation_producer_id,source.interpolation_primitive_id)) return false;
        ++model_ft3.snapshot.publish_source_count;
    }
    return true;
}

static void finish_model_ft3(
        CPUState *cpu, GuestRenderRenderMode render_mode,
        const XgRenderModelSpritePipelineServices *services) {
    bool framing_matches = true;

    if (!model_ft3.snapshot.pending) return;
    if (cpu == NULL || cpu->read_word == NULL || cpu->read_half == NULL) {
        block_model_ft3(73u);
        return;
    }
    ++model_ft3.snapshot.invocation_count;
    model_ft3.snapshot.primitive_count += model_ft3.count;
    for (uint32_t index = 0u; index < model_ft3.count; ++index) {
        ModelFt3Record *record = &model_ft3.records[index];
        const uint32_t actual_material =
            cpu->read_word(record->packet_address + 4u);
        const uint8_t actual_opcode = (uint8_t)(actual_material >> 24u);
        const bool payload_matches =
            (actual_material & UINT32_C(0xff000000)) ==
                (record->material_word & UINT32_C(0xff000000)) &&
            cpu->read_half(record->packet_address + 12u) == record->uv[0] &&
            cpu->read_half(record->packet_address + 20u) == record->uv[1] &&
            cpu->read_half(record->packet_address + 28u) == record->uv[2] &&
            cpu->read_half(record->packet_address + 22u) == record->tpage &&
            cpu->read_half(record->packet_address + 14u) == record->clut;
        bool geometry_matches = true;
        bool tag_matches = true;
        bool ot_matches = true;

        if ((actual_material & UINT32_C(0x00ffffff)) !=
            (record->material_word & UINT32_C(0x00ffffff)))
            ++model_ft3.snapshot.raw_color_difference_count;
        record->primitive.material.raw_texture = (actual_opcode & 1u) != 0u;
        record->primitive.material.semi_transparent =
            (actual_opcode & 2u) != 0u;
        for (uint32_t vertex = 0u; vertex < 3u; ++vertex) {
            XgRenderIrVertex *destination =
                &record->primitive.triangles[0].vertices[vertex];
            destination->r = (uint8_t)actual_material;
            destination->g = (uint8_t)(actual_material >> 8u);
            destination->b = (uint8_t)(actual_material >> 16u);
        }
        if (record->guest_observed_accepted) {
            bool last_in_bucket = true;
            tag_matches = cpu->read_word(record->packet_address) ==
                record->expected_tag;
            for (uint32_t vertex = 0u; vertex < 3u; ++vertex) {
                geometry_matches &= cpu->read_word(
                    record->packet_address + 8u + vertex * 8u) ==
                    record->observed_xy[vertex];
                geometry_matches &= record->vertices[vertex].x ==
                        low_s16(record->observed_xy[vertex]) &&
                    record->vertices[vertex].y ==
                        low_s16(record->observed_xy[vertex] >> 16u);
            }
            for (uint32_t later = index + 1u; later < model_ft3.count; ++later) {
                if (model_ft3.records[later].guest_observed_accepted &&
                    model_ft3.records[later].ordering_bucket ==
                        record->ordering_bucket)
                    last_in_bucket = false;
            }
            if (last_in_bucket)
                ot_matches = cpu->read_word(model_ft4.context.ot_base +
                    record->ordering_bucket * 4u) ==
                    (record->packet_address & UINT32_C(0x00ffffff));
        }
        if (!payload_matches) ++model_ft3.snapshot.payload_mismatch_count;
        if (!payload_matches &&
            model_ft3.snapshot.first_payload_mismatch.field_bits == 0u) {
            PsxXgRenderFt4PayloadMismatch *mismatch =
                &model_ft3.snapshot.first_payload_mismatch;
            mismatch->packet_address = record->packet_address;
            mismatch->descriptor_address = record->attribute_address;
            mismatch->expected_material_word = record->material_word;
            mismatch->actual_material_word = actual_material;
            mismatch->expected_tpage = record->tpage;
            mismatch->actual_tpage =
                cpu->read_half(record->packet_address + 22u);
            mismatch->expected_clut = record->clut;
            mismatch->actual_clut =
                cpu->read_half(record->packet_address + 14u);
            if (mismatch->actual_material_word != record->material_word)
                mismatch->field_bits |= FT4_PAYLOAD_MATERIAL;
            if (mismatch->actual_tpage != record->tpage)
                mismatch->field_bits |= FT4_PAYLOAD_TPAGE;
            if (mismatch->actual_clut != record->clut)
                mismatch->field_bits |= FT4_PAYLOAD_CLUT;
            for (uint32_t vertex = 0u; vertex < 3u; ++vertex) {
                mismatch->expected_uv[vertex] = record->uv[vertex];
                mismatch->actual_uv[vertex] = cpu->read_half(
                    record->packet_address + 12u + vertex * 8u);
                if (mismatch->actual_uv[vertex] != record->uv[vertex])
                    mismatch->field_bits |= FT4_PAYLOAD_UV0 << vertex;
            }
        }
        if (!geometry_matches) ++model_ft3.snapshot.geometry_mismatch_count;
        if (!tag_matches) ++model_ft3.snapshot.tag_mismatch_count;
        if (!ot_matches) ++model_ft3.snapshot.ot_mismatch_count;
        const bool matches = payload_matches && geometry_matches &&
            tag_matches && ot_matches;
        record->output_validated = record->guest_observed_accepted && matches;
        if (record->guest_observed_accepted && !matches)
            ++model_ft3.snapshot.validation_rejected_source_count;
        if (matches) {
            ++model_ft3.snapshot.match_count;
        } else {
            if (model_ft3.snapshot.mismatch_count == 0u)
                model_ft3.snapshot.first_mismatch_packet =
                    record->packet_address;
            ++model_ft3.snapshot.mismatch_count;
        }
    }
    if ((cpu->read_word(UINT32_C(0x80059424)) & UINT32_C(0x00ffffff)) !=
        ((model_ft3.initial_packet_cursor + model_ft3.count * 0x20u) &
         UINT32_C(0x00ffffff))) {
        framing_matches = false;
        ++model_ft3.snapshot.cursor_mismatch_count;
        ++model_ft3.snapshot.mismatch_count;
    }
    const uint32_t actual_counter_delta =
        cpu->read_word(UINT32_C(0x80059578)) - model_ft3.initial_counter;
    model_ft3.snapshot.last_expected_counter_delta =
        model_ft3.expected_counter_delta;
    model_ft3.snapshot.last_actual_counter_delta = actual_counter_delta;
    if (actual_counter_delta != model_ft3.expected_counter_delta) {
        framing_matches = false;
        model_ft3.snapshot.last_mismatch_expected_counter_delta =
            model_ft3.expected_counter_delta;
        model_ft3.snapshot.last_mismatch_actual_counter_delta =
            actual_counter_delta;
        model_ft3.snapshot.last_mismatch_target_count = model_ft3.count;
        model_ft3.snapshot.last_mismatch_nclip_positive_count =
            model_ft3.snapshot.last_nclip_positive_count;
        model_ft3.snapshot.last_mismatch_guest_screen_accepted_count =
            model_ft3.snapshot.last_guest_screen_accepted_count;
        model_ft3.snapshot.last_mismatch_guest_vertical_accepted_count =
            model_ft3.snapshot.last_guest_vertical_accepted_count;
        model_ft3.snapshot.last_mismatch_guest_horizontal_accepted_count =
            model_ft3.snapshot.last_guest_horizontal_accepted_count;
        model_ft3.snapshot.last_mismatch_projection_flag_negative_count =
            model_ft3.snapshot.last_projection_flag_negative_count;
        model_ft3.snapshot.last_mismatch_screen_right =
            cpu->read_word(UINT32_C(0x800500f8));
        model_ft3.snapshot.last_mismatch_screen_bottom =
            cpu->read_word(UINT32_C(0x800500fc));
        if (actual_counter_delta > model_ft3.expected_counter_delta)
            ++model_ft3.snapshot.counter_actual_greater_count;
        else
            ++model_ft3.snapshot.counter_actual_less_count;
        ++model_ft3.snapshot.counter_mismatch_count;
        ++model_ft3.snapshot.mismatch_count;
    }
    if (render_mode == GUEST_RENDER_RENDER_NATIVE) {
        if (framing_matches) {
            if (!publish_model_ft3(services)) {
                block_model_ft3(78u);
                return;
            }
            ++model_ft3.snapshot.publish_invocation_count;
        } else {
            ++model_ft3.snapshot.framing_rejected_invocation_count;
        }
    }
    clear_model_ft3_pending();
}

void xg_render_model_sprite_pipeline_model_ft4_seam(
        CPUState *cpu, uint32_t pc, GuestRenderRenderMode render_mode,
        const XgRenderModelSpritePipelineServices *services) {
    model_ft4.snapshot.last_seam_pc = pc;
    if (physical_address_equals(pc, UINT32_C(0x8002e268)))
        ++model_ft4.snapshot.average_seam_count;
    else
        ++model_ft4.snapshot.farthest_seam_count;
    if (!model_ft4.context.valid)
        ++model_ft4.snapshot.seam_without_context_count;
    if (model_ft4.context.valid &&
        model_ft4.context.dispatch_mode == XG_MODEL_FT4_RAW_DISPATCH_RELIT &&
        !physical_address_equals(pc, UINT32_C(0x8002e268))) {
        block_model_ft4(76u);
        return;
    }
    if (render_mode == GUEST_RENDER_RENDER_NATIVE) {
        if (model_ft4.context.valid &&
            !stage_model_ft4(cpu, render_mode, services)) {
            if (model_ft4.snapshot.prepare_failure_detail != 0u)
                clear_model_ft4_pending();
            else
                block_model_ft4(76u);
        }
    } else if (model_ft4.context.valid &&
               !prepare_model_ft4(cpu, render_mode, services)) {
        block_model_ft4(75u);
    }
}

void xg_render_model_sprite_pipeline_model_ft3_seam(
        CPUState *cpu, GuestRenderRenderMode render_mode,
        const XgRenderModelSpritePipelineServices *services) {
    if (model_ft3.snapshot.blocked || !model_ft4.context.valid) return;
    /* Relit geometry is captured at the common model entry. Its final lighting
     * is taken from the consumed packet, not from the raw FT3 seam's material. */
    if (model_ft4.context.dispatch_mode == XG_MODEL_FT4_RAW_DISPATCH_RELIT)
        return;
    if (render_mode == GUEST_RENDER_RENDER_NATIVE) {
        if (!stage_model_ft3(cpu, render_mode, services)) {
            if (model_ft3.snapshot.prepare_failure_detail != 0u)
                clear_model_ft3_pending();
            else
                block_model_ft3(76u);
        }
    } else if (!prepare_model_ft3(cpu, render_mode, services)) {
        block_model_ft3(75u);
    }
}

static void publish_model_coverage(void) {
    if (model_coverage.complete && model_ft4.context.valid &&
        !model_ft4.snapshot.pending && !model_ft3.snapshot.pending) {
        const uint32_t producer = model_ft4.context.instance_address & 0x1fffffffu;
        uint32_t count = 0;
        for (uint32_t i = 0; i < model_coverage.vertex_count; ++i)
            if (model_coverage.samples[i].component_id)
                model_coverage.samples[count++] = model_coverage.samples[i];
        const XgRenderTemporalComponent component = {producer, model_coverage.geometry_digest,
            xg_render_submission_temporal_scene(), producer};
        /* Full source topology, including culled faces, supplies the old vertex
         * when visibility changes. It adds metadata, not hidden draw calls. */
        if (xg_render_submission_publish_temporal_coverage(producer, &component, 1u,
            model_coverage.samples, count, model_coverage.bindings, model_coverage.binding_count))
            ++model_coverage_diagnostics.published;
        else ++model_coverage_diagnostics.rejected;
    }
    model_coverage.complete = false;
}

void xg_render_model_sprite_pipeline_model_finish(
        CPUState *cpu, GuestRenderRenderMode render_mode,
        const XgRenderModelSpritePipelineServices *services) {
    finish_model_ft4(cpu, render_mode, services);
    finish_model_ft3(cpu, render_mode, services);
    /* At 8002c86c, s3 is the remaining-group counter after JALR's delay
     * slot. -1 is the last group. Field calls this dispatcher directly;
     * the 800257dc wrapper-end seam does not run for those invocations. */
    if (cpu && cpu->gpr[19] == UINT32_MAX) publish_model_coverage();
}

void xg_render_model_sprite_pipeline_model_end(void) {
    if (model_ft4.snapshot.pending) block_model_ft4(74u);
    if (model_ft3.snapshot.pending) block_model_ft3(74u);
    publish_model_coverage();
    if (!model_ft4.snapshot.pending && !model_ft3.snapshot.pending)
        model_ft4.context = (ModelContext){0};
}

void xg_render_model_sprite_pipeline_capture_ft3_link(
        CPUState *cpu, GuestRenderRenderMode render_mode,
        const XgRenderModelSpritePipelineServices *services) {
    if (model_ft4.context.caller_contract == MODEL_DISPATCH_CALLER_BATTLE) return;
    const uint32_t packet = cpu != NULL ? cpu->gpr[19] : 0u;
    const uint32_t material_word = cpu != NULL && cpu->read_word != NULL
        ? cpu->read_word(packet + 4u) : 0u;
    const uint8_t opcode = (uint8_t)(material_word >> 24u);
    XgRenderModelFt4Template decoded = {0};
    const XgRenderModelFt4Template *material;
    XgRenderIrNativePrimitive primitive = {0};
    const uint32_t source_id = normalized_word_address(packet) + 4u;
    const XgRenderModelFt3SourceRecord *existing;
    GpuDrawState draw = {0};
    XgRenderProducerLifecycle lifecycle;

    if (cpu == NULL || render_mode != GUEST_RENDER_RENDER_NATIVE ||
        cpu->read_word == NULL || cpu->read_half == NULL ||
        cpu->read_byte == NULL || !word_address_is_valid(packet) ||
        opcode < 0x24u || opcode > 0x27u ||
        !lifecycle_begin(services, UINT32_C(0x8002da00), &lifecycle))
        return;
    existing = xg_render_model_repository_find_ft3_source(source_id);
    if (existing != NULL && !existing->geometry_ready &&
        lifecycle_matches(services, &existing->lifecycle)) {
        decoded = (XgRenderModelFt4Template){
            .descriptor_address = existing->descriptor_address,
            .material_word = existing->material_word,
            .uv = {existing->uv[0], existing->uv[1], existing->uv[2]},
            .tpage = existing->tpage,
            .clut = existing->clut,
            .valid = true,
        };
        material = &decoded;
    } else {
        material = xg_render_model_repository_find_packet_template(
            packet, render_mode, services->repository);
        if (material == NULL) {
            /* At this render-loop seam s0 is the vertex pool, not a material
             * descriptor. Model-entry capture already provides authenticated
             * geometry independently of this optional initializer template. */
            ++model_ft3.snapshot.template_miss_count;
            return;
        }
    }
    if (!material->valid) return;
    for (uint32_t index = 0u; index < model_ft3.count; ++index) {
        if (physical_address_equals(
                model_ft3.records[index].packet_address, packet)) {
            primitive = model_ft3.records[index].primitive;
            for (uint32_t vertex = 0u; vertex < 3u; ++vertex) {
                const uint32_t xy = cpu->gpr[9u + vertex];
                const XgRenderIrVertex *projected =
                    &primitive.triangles[0].vertices[vertex];
                if (projected->x != (int32_t)low_s16(xy) * INT32_C(65536) ||
                    projected->y != (int32_t)low_s16(xy >> 16u) * INT32_C(65536)) {
                    ++model_ft3.snapshot.geometry_mismatch_count;
                    ++model_ft3.snapshot.validation_rejected_source_count;
                    return;
                }
            }
            break;
        }
    }
    gpu_get_draw_state(&draw);
    xg_render_material_apply_draw_state(&primitive.material, &draw);
    primitive.material.tpage = material->tpage;
    primitive.material.texture_page_x = material->tpage & 0x0fu;
    primitive.material.texture_page_y = (material->tpage >> 4u) & 1u;
    primitive.material.texture_depth =
        (XgRenderIrTextureDepth)((material->tpage >> 7u) & 3u);
    primitive.material.blend_mode =
        (XgRenderIrBlendMode)((material->tpage >> 5u) & 3u);
    primitive.material.clut_x = (material->clut & 0x3fu) << 4u;
    primitive.material.clut_y = material->clut >> 6u;
    primitive.material.shading = XG_RENDER_IR_SHADING_FLAT;
    primitive.material.textured = true;
    primitive.material.raw_texture = (opcode & 1u) != 0u;
    primitive.material.semi_transparent = (opcode & 2u) != 0u;
    primitive.triangle_count = 1u;
    primitive.triangles[0].split_count = 1u;
    for (uint32_t vertex = 0u; vertex < 3u; ++vertex) {
        const uint32_t xy = cpu->gpr[9u + vertex];
        XgRenderIrVertex *destination =
            &primitive.triangles[0].vertices[vertex];
        destination->x = (int32_t)low_s16(xy) * INT32_C(65536);
        destination->y =
            (int32_t)low_s16(xy >> 16u) * INT32_C(65536);
        destination->u =
            (int32_t)(uint8_t)material->uv[vertex] * INT32_C(65536);
        destination->v =
            (int32_t)(uint8_t)(material->uv[vertex] >> 8u) * INT32_C(65536);
        destination->r = (uint8_t)material_word;
        destination->g = (uint8_t)(material_word >> 8u);
        destination->b = (uint8_t)(material_word >> 16u);
    }
    xg_render_depth_policy_stamp_primitive(&primitive, XG_RENDER_DEPTH_FAMILY_FIELD_MODELS);
    const XgRenderModelFt3SourceRecord captured = {
        .primitive = primitive,
        .lifecycle = lifecycle,
        .source_id = source_id,
        .descriptor_address = material->descriptor_address,
        .material_word = material->material_word,
        .uv = {material->uv[0], material->uv[1], material->uv[2]},
        .tpage = material->tpage,
        .clut = material->clut,
        .interpolation_producer_id =
            model_ft4.context.instance_address & UINT32_C(0x1fffffff),
        .interpolation_primitive_id =
            ((material->descriptor_address != 0u
                  ? material->descriptor_address : cpu->gpr[16]) &
             UINT32_C(0x1fffffff)) |
            ((uint32_t)(model_ft4.context.dispatch_mode & 7u) << 29u) |
            UINT32_C(1),
        .interpolation_identity_valid =
            (model_ft4.context.instance_address & UINT32_C(0x1fffffff)) != 0u,
        .geometry_ready = true,
        .link_pending = true,
        .valid = true,
    };
    (void)xg_render_model_repository_store_ft3_source(
        &captured, &(XgRenderModelSourcePublication){
            .resource_address = packet,
            .resource_size = 0x20u,
            .descriptor_address = captured.descriptor_address,
            .descriptor_size = captured.descriptor_address != 0u ? 10u : 0u,
            .register_replay = true,
        }, services->repository);
}

void xg_render_model_sprite_pipeline_finish_ft3_link(
        CPUState *cpu, const XgRenderModelSpritePipelineServices *services) {
    if (model_ft4.context.caller_contract == MODEL_DISPATCH_CALLER_BATTLE) return;
    const uint32_t packet = cpu != NULL ? cpu->gpr[19] : 0u;
    const uint32_t source_id = normalized_word_address(packet) + 4u;
    const XgRenderModelFt3SourceRecord *source =
        xg_render_model_repository_find_ft3_source(source_id);

    if (source == NULL || !source->link_pending || cpu == NULL ||
        cpu->read_word == NULL)
        return;
    const XgRenderModelFt3SourceRecord endpoint = *source;
    bool geometry_matches = true;
    for (uint32_t vertex = 0u; vertex < 3u; ++vertex) {
        const uint32_t xy = cpu->read_word(packet + 8u + vertex * 8u);
        const XgRenderIrVertex *projected =
            &endpoint.primitive.triangles[0].vertices[vertex];
        geometry_matches &= projected->x == (int32_t)low_s16(xy) * INT32_C(65536) &&
            projected->y == (int32_t)low_s16(xy >> 16u) * INT32_C(65536);
    }
    const bool linked = cpu->gpr[13] != 0u && (cpu->read_word(packet) >> 24u) == 7u;
    const bool accepted = linked && geometry_matches;
    if (linked && !geometry_matches) {
        ++model_ft3.snapshot.geometry_mismatch_count;
        ++model_ft3.snapshot.validation_rejected_source_count;
    }
    xg_render_model_repository_finish_ft3_link(
        source_id, accepted,
        &(XgRenderModelSourcePublication){
            .resource_address = packet,
            .resource_size = 0x20u,
            .register_replay = true,
        }, services->repository);
    if (accepted)
        (void)publish_model_endpoint(&endpoint.primitive,source_id,
            endpoint.interpolation_producer_id,endpoint.interpolation_primitive_id);
}

void xg_render_model_sprite_pipeline_sprite_begin(
        CPUState *cpu, bool wrapper_scope,
        GuestRenderRenderMode render_mode) {
    uint32_t data_address;
    uint32_t descriptor_address;
    uint32_t primitive_count;

    if (render_mode != GUEST_RENDER_RENDER_SHADOW &&
        render_mode != GUEST_RENDER_RENDER_NATIVE) {
        clear_sprite();
        return;
    }
    if (!wrapper_scope && sprite_ft4.snapshot.context_active) return;
    if (sprite_ft4.snapshot.blocked) return;
    if (sprite_ft4.snapshot.context_active || sprite_ft4.snapshot.pending) {
        block_sprite(80u);
        return;
    }
    if (cpu == NULL || cpu->read_word == NULL || cpu->read_byte == NULL) {
        block_sprite(81u);
        return;
    }
    ++sprite_ft4.snapshot.caller_count;
    sprite_ft4.snapshot.last_caller = cpu->gpr[31];
    sprite_ft4.snapshot.last_sprite_address = cpu->gpr[4];
    if (!word_address_is_valid(cpu->gpr[4])) {
        sprite_ft4.snapshot.blocker_detail = 1u;
        block_sprite(81u);
        return;
    }
    data_address = cpu->read_word(cpu->gpr[4] + 0x20u);
    primitive_count = (cpu->read_byte(cpu->gpr[4] + 0x40u) >> 2u) & 0x3fu;
    sprite_ft4.snapshot.last_data_address = data_address;
    sprite_ft4.snapshot.last_primitive_count = primitive_count;
    if (primitive_count == 0u) {
        ++sprite_ft4.snapshot.empty_caller_count;
        sprite_ft4.snapshot.blocker_detail = 0u;
        return;
    }
    if (!word_address_is_valid(data_address)) {
        sprite_ft4.snapshot.blocker_detail = 2u;
        block_sprite(82u);
        return;
    }
    descriptor_address = cpu->read_word(data_address + 0x30u);
    sprite_ft4.snapshot.last_descriptor_address = descriptor_address;
    if (!word_address_is_valid(descriptor_address) ||
        descriptor_address > UINT32_MAX - primitive_count * 0x18u) {
        sprite_ft4.snapshot.blocker_detail =
            (!word_address_is_valid(descriptor_address) ? 8u : 0u) |
            (descriptor_address > UINT32_MAX - primitive_count * 0x18u
                 ? 16u : 0u);
        block_sprite(82u);
        return;
    }
    sprite_ft4.sprite_address = cpu->gpr[4];
    sprite_ft4.invocation_matches = true;
    sprite_ft4.wrapper_scope = wrapper_scope;
    sprite_ft4.snapshot.blocker_detail = 0u;
    sprite_ft4.snapshot.context_active = true;
}

/* Field actor cards are billboarded in view space around their feet (the node
 * origin), so every corner has the feet's view Z and the top of the card leans
 * back into whatever stands behind the character. For the host depth test they
 * take the depth of an upright figure instead: a view-Y step up the card moves
 * along the Field camera's world-up axis (-column 1), whose view Z per view Y
 * is R[2][1] / R[1][1]. Zero (flat card) outside the Field actor renderer or
 * for a camera looking almost straight down. */
static double sprite_upright_depth_slope(CPUState *cpu) {
    XgHost3dMatrix camera;
    if ((sprite_ft4.snapshot.last_caller & 0x1fffffffu) != 0x7622cu ||
        !xg_render_runtime_capture_matrix(cpu, 0x800afa64u, &camera)) return 0.0;
    const double x = camera.rotation[0][1], y = camera.rotation[1][1], z = camera.rotation[2][1];
    const double norm = sqrt(x * x + y * y + z * z);
    return norm > 0.0 && y >= 0.25 * norm ? z / y : 0.0;
}

/* The same upright depth for the endpoint card (bound cards are re-derived
 * from the motion pose, see XgRenderMotionPose.upright_depth_slope). */
static void sprite_upright_depth(XgRenderIrNativePrimitive *primitive,
        const XgHost3dProjection *projection, double slope) {
    const double distance = projection->projection_distance;
    if (slope == 0.0 || distance <= 0.0) return;
    const double origin_y = projection->native_transform_valid
        ? projection->native_translation[1] : projection->translation[1];
    for (uint32_t t = 0u; t < primitive->triangle_count; ++t)
        for (uint32_t v = 0u; v < 3u; ++v) {
            XgRenderIrVertex *vertex = &primitive->triangles[t].vertices[v];
            if (!vertex->native_view_position || vertex->native_view_depth <= 0) continue;
            const double z = vertex->native_view_depth / 4096.0;
            const double y = (vertex->native_view_y - (double)projection->screen_offset_y) /
                65536.0 * z / distance;
            const int32_t depth = xg_host_3d_native_depth_q12(z + slope * (y - origin_y),
                (uint32_t)distance);
            if (depth > 0) vertex->native_view_depth = depth;
        }
}

static void capture_sprite_motion(CPUState *cpu, const XgSpriteFt4Source *sprite,
        const XgRenderModelSpritePipelineServices *services) {
    const uint32_t producer = sprite_ft4.sprite_address & 0x1fffffffu;
    const uint64_t entity = UINT64_C(0x5350524900000000) | producer;
    sprite_ft4.motion_binding = (XgRenderMotionDrawBinding){0};
    if (!sprite_ft4.motion_attempted) {
        XgRenderMotionSource source;
        XgRenderMotionPose pose = {0};
        XgHost3dMatrix matrix = {0};
        sprite_ft4.motion_attempted = true;
        /* This is the Field actor renderer's resident billboard invocation.
         * Its authenticated update owns placement; the changing cel descriptors
         * own the current local quads, not a previous frame's mesh topology. */
        const uint32_t caller = sprite_ft4.snapshot.last_caller & 0x1fffffffu;
        if (caller != 0x7622cu ||
            !services || !services->motion_source ||
            !services->motion_source(0x80075b44u, &source)) return;
        memcpy(matrix.rotation, sprite->projection.rotation, sizeof(matrix.rotation));
        memcpy(matrix.translation, sprite->projection.translation, sizeof(matrix.translation));
        pose.node_count = 1;
        pose.entity_id = entity;
        const uint32_t identity[3] = {producer,
            cpu->read_word(sprite_ft4.sprite_address + 0x20u), sprite_ft4.wrapper_scope};
        pose.geometry_id = xg_render_resource_digest(identity, sizeof(identity));
        pose.geometry_generation = 1;
        /* The game has already billboarded this transform in view space. */
        pose.camera_id = UINT64_C(0x5350524956494557);
        pose.camera.rotation[3] = 1;
        pose.camera.scale[0] = pose.camera.scale[1] = pose.camera.scale[2] = 1;
        pose.geometry_scale = 1;
        pose.upright_depth_slope = sprite_upright_depth_slope(cpu);
        pose.nodes[0].id = producer;
        pose.nodes[0].parent = -1;
        pose.nodes[0].source_matrix_valid = 1;
        pose.nodes[0].source_model_to_view = matrix;
        if (!xg_render_motion_decompose(&matrix, &pose.nodes[0].local)) return;
        pose.screen_offset[0] = sprite->projection.screen_offset_x / 65536.0;
        pose.screen_offset[1] = sprite->projection.screen_offset_y / 65536.0;
        pose.projection_distance = sprite->projection.projection_distance;
        if (!xg_render_motion_publish(&source, &pose, &sprite_ft4.motion)) return;
        if (!xg_render_motion_watch(sprite_ft4.motion, sprite_ft4.sprite_address + 0x20u, 4u)) {
            xg_render_motion_forget_entity(entity);
            sprite_ft4.motion = (XgRenderMotionRef){0};
            return;
        }
        sprite_ft4.motion_projection = sprite->projection;
    }
    if (!sprite_ft4.motion.handle.resource_id) return;
    const XgHost3dProjection *prior = &sprite_ft4.motion_projection;
    const XgHost3dProjection *current = &sprite->projection;
    if (memcmp(prior->rotation, current->rotation, sizeof(prior->rotation)) ||
        memcmp(prior->translation, current->translation, sizeof(prior->translation)) ||
        prior->screen_offset_x != current->screen_offset_x ||
        prior->screen_offset_y != current->screen_offset_y ||
        prior->projection_distance != current->projection_distance) {
        /* All pieces must describe the same placement within an invocation. */
        xg_render_motion_forget_entity(entity);
        sprite_ft4.motion = (XgRenderMotionRef){0};
        return;
    }
    XgRenderMotionDrawBinding *binding = &sprite_ft4.motion_binding;
    binding->motion = sprite_ft4.motion;
    binding->triangle_count = 2;
    const uint8_t split[2][3] = {{0, 1, 2}, {2, 1, 3}};
    const uint32_t primitive = (sprite_ft4.descriptor_address & 0x1ffffffeu) |
        (sprite_ft4.wrapper_scope ? 1u : 0u);
    for (uint32_t t = 0; t < 2; ++t)
        for (uint32_t v = 0; v < 3; ++v)
            for (uint32_t p = 0; p < 4; ++p)
                if (sprite->packet_vertex_for_projection[p] == split[t][v]) {
                    binding->local[t][v] = sprite->vertices[p];
                    binding->local[t][v].pad = 0;
                    binding->vertex_ids[t][v] = primitive * 4u + split[t][v];
                }
}

static bool prepare_sprite(CPUState *cpu,
        const XgRenderModelSpritePipelineServices *services) {
    XgSpriteFt4Source source = {0};
    XgSpriteFt4Record projected;
    GpuDrawState draw = {0};
    uint32_t data_address;
    uint32_t descriptor_base;
    uint32_t primitive_count;
    uint32_t descriptor_address;
    uint32_t packet_address;

    if (!sprite_ft4.snapshot.context_active || sprite_ft4.snapshot.pending ||
        cpu == NULL || cpu->read_word == NULL || cpu->read_half == NULL ||
        cpu->read_byte == NULL || cpu->gpr[20] != sprite_ft4.sprite_address)
        return false;
    data_address = cpu->read_word(sprite_ft4.sprite_address + 0x20u);
    descriptor_base = cpu->read_word(data_address + 0x30u);
    primitive_count =
        (cpu->read_byte(sprite_ft4.sprite_address + 0x40u) >> 2u) & 0x3fu;
    descriptor_address = cpu->gpr[19];
    packet_address = cpu->gpr[16];
    if (cpu->gpr[18] != UINT32_C(0x8004fb98) ||
        descriptor_address < descriptor_base ||
        descriptor_address >= descriptor_base + primitive_count * 0x18u ||
        (descriptor_address - descriptor_base) % 0x18u != 0u ||
        !word_address_is_valid(packet_address) ||
        !word_address_is_valid(packet_address + 36u))
        return false;
    xg_render_runtime_capture_shadow_projection(cpu, &source.projection);
    for (uint32_t vertex = 0u; vertex < 4u; ++vertex) {
        const uint32_t address = cpu->gpr[18] + vertex * 8u;
        const uint32_t xy = cpu->read_word(address);
        const uint32_t zp = cpu->read_word(address + 4u);
        source.vertices[vertex] = (XgHost3dVector){
            low_s16(xy), low_s16(xy >> 16u), low_s16(zp),
            (uint16_t)(zp >> 16u),
        };
    }
    sprite_ft4.material_word = cpu->read_word(descriptor_address + 0x10u);
    source.color[0] = (uint8_t)sprite_ft4.material_word;
    source.color[1] = (uint8_t)(sprite_ft4.material_word >> 8u);
    source.color[2] = (uint8_t)(sprite_ft4.material_word >> 16u);
    sprite_ft4.tpage = cpu->read_half(descriptor_address + 0x0au);
    sprite_ft4.clut = cpu->read_half(descriptor_address + 0x0cu);
    if (((sprite_ft4.material_word >> 24u) & 0xfcu) != 0x2cu) return false;
    gpu_get_draw_state(&draw);
    xg_render_material_apply_draw_state(&source.material, &draw);
    source.material.tpage = sprite_ft4.tpage;
    source.material.texture_page_x = sprite_ft4.tpage & 0x0fu;
    source.material.texture_page_y = (sprite_ft4.tpage >> 4u) & 1u;
    source.material.texture_depth =
        (XgRenderIrTextureDepth)((sprite_ft4.tpage >> 7u) & 3u);
    source.material.blend_mode =
        (XgRenderIrBlendMode)((sprite_ft4.tpage >> 5u) & 3u);
    source.material.clut_x = (sprite_ft4.clut & 0x3fu) << 4u;
    source.material.clut_y = sprite_ft4.clut >> 6u;
    source.material.shading = XG_RENDER_IR_SHADING_FLAT;
    source.material.textured = true;
    source.material.raw_texture =
        ((sprite_ft4.material_word >> 24u) & 1u) != 0u;
    source.material.semi_transparent =
        ((sprite_ft4.material_word >> 24u) & 2u) != 0u;
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
    if (xg_sprite_ft4_build(&source, &sprite_ft4.native) != XG_SPRITE_FT4_OK)
        return false;
    sprite_upright_depth(&sprite_ft4.native.primitive, &source.projection,
        sprite_upright_depth_slope(cpu));
    memcpy(sprite_ft4.uv, source.uv, sizeof(source.uv));
    sprite_ft4.packet_address = packet_address;
    sprite_ft4.descriptor_address = descriptor_address;
    xg_render_motion_forget_range(packet_address + 4u, 0x24u);
    capture_sprite_motion(cpu, &source, services);
    sprite_ft4.phase = SPRITE_EXPECT_XY;
    sprite_ft4.geometry_matches = true;
    sprite_ft4.payload_matches = true;
    sprite_ft4.snapshot.pending = true;
    return true;
}

static bool stage_sprite(
        CPUState *cpu, GuestRenderRenderMode render_mode,
        const XgRenderModelSpritePipelineServices *services) {
    SpriteStageRecord *record;
    XgRenderProducerLifecycle lifecycle;
    uint32_t record_index;

    if (render_mode != GUEST_RENDER_RENDER_NATIVE || cpu == NULL ||
        !sprite_ft4.snapshot.pending ||
        sprite_ft4.native_record_count == SPRITE_CAPACITY ||
        !lifecycle_begin(services, UINT32_C(0x8001e874), &lifecycle))
        return false;
    record_index = sprite_ft4.native_record_count++;
    record = &sprite_ft4.native_records[record_index];
    sprite_ft4.native_lifecycles[record_index] = lifecycle;
    sprite_ft4.native_opcodes[record_index] =
        (uint8_t)(sprite_ft4.material_word >> 24u);
    *record = (SpriteStageRecord){
        .primitive = sprite_ft4.native.primitive,
        .packet_address = sprite_ft4.packet_address,
        .source_primitive_index = UINT32_C(0x52000000) |
            (sprite_ft4.packet_address & UINT32_C(0x001ffffc)),
        .interpolation_producer_id =
            sprite_ft4.sprite_address & UINT32_C(0x1fffffff),
        .interpolation_primitive_id =
            (sprite_ft4.descriptor_address & UINT32_C(0x1ffffffe)) |
            (sprite_ft4.wrapper_scope ? 1u : 0u),
        .payload_word_count = 9u,
        .interpolation_identity_valid = true,
    };
    /* A character card is projected corner by corner (xg_sprite_ft4_build):
     * its view depth is real, so it hides behind nearer certified surfaces. */
    xg_render_depth_policy_stamp_primitive(&record->primitive, XG_RENDER_DEPTH_FAMILY_SPRITES);
    if (sprite_ft4.geometry_matches && sprite_ft4.motion_binding.motion.handle.resource_id)
        (void)xg_render_motion_register_command(record->packet_address + 4u,
            &sprite_ft4.motion_binding, record->interpolation_producer_id,
            record->interpolation_primitive_id);
    if (sprite_ft4.wrapper_scope) {
        uint32_t ot_address;
        if (cpu->read_word == NULL) return true;
        const uint32_t base_ot_address =
            cpu->read_word(cpu->gpr[29] + 0x9cu);
        if (xg_sprite_ft4_select_ot_address(
                base_ot_address,
                cpu->read_word(sprite_ft4.sprite_address + 0x3cu),
                cpu->read_word(sprite_ft4.descriptor_address + 0x14u),
                &ot_address) != XG_SPRITE_FT4_OK)
            return true;
        const uint32_t ot_base = cpu->read_word(UINT32_C(0x8005956c));
        if (ot_address < ot_base || (ot_address - ot_base) % 4u != 0u ||
            (ot_address - ot_base) / 4u >= 0x1000u)
            return true;
        record->ot_bucket = (ot_address - ot_base) / 4u;
        (void)stage_request(services, &(XgRenderModelSpriteStageRequest){
            .primitive = &record->primitive,
            .packet_address = record->packet_address,
            .source_primitive_index = record->source_primitive_index,
            .ot_bucket = record->ot_bucket,
            .interpolation_producer_id = record->interpolation_producer_id,
            .interpolation_primitive_id = record->interpolation_primitive_id,
            .payload_word_count = record->payload_word_count,
            .interpolation_identity_valid =
                record->interpolation_identity_valid,
        });
    } else {
        uint32_t failure_detail = 0u;
        if (services == NULL || services->stage_standalone == NULL ||
            !services->stage_standalone(
                &record->primitive, record->packet_address,
                record->source_primitive_index,
                record->interpolation_producer_id,
                record->interpolation_primitive_id, &failure_detail)) {
            --sprite_ft4.native_record_count;
            return false;
        }
    }
    return true;
}

void xg_render_model_sprite_pipeline_sprite_geometry_seam(
        CPUState *cpu, GuestRenderRenderMode render_mode,
        const XgRenderModelSpritePipelineServices *services) {
    (void)render_mode;
    (void)services;
    if (sprite_ft4.snapshot.context_active && !sprite_ft4.snapshot.pending &&
        !prepare_sprite(cpu, services)) {
        block_sprite(86u);
        return;
    }
    if (!sprite_ft4.snapshot.pending) return;
    static const uint8_t split[2][3] = {{0u, 1u, 2u}, {2u, 1u, 3u}};
    if (sprite_ft4.phase != SPRITE_EXPECT_XY || cpu == NULL ||
        cpu->read_word == NULL) {
        block_sprite(83u);
        return;
    }
    for (uint32_t vertex = 0u; vertex < 4u; ++vertex) {
        const uint32_t expected_xy =
            (uint16_t)sprite_ft4.native.vertices[vertex].x |
            ((uint32_t)(uint16_t)sprite_ft4.native.vertices[vertex].y << 16u);
        const uint32_t observed_xy = cpu->read_word(
            sprite_ft4.packet_address + 8u + vertex * 8u);
        sprite_ft4.geometry_matches &= observed_xy == expected_xy;
        sprite_ft4.native.vertices[vertex].x = low_s16(observed_xy);
        sprite_ft4.native.vertices[vertex].y = low_s16(observed_xy >> 16u);
    }
    for (uint32_t triangle = 0u; triangle < 2u; ++triangle) {
        for (uint32_t vertex = 0u; vertex < 3u; ++vertex) {
            const uint32_t source = split[triangle][vertex];
            XgRenderIrVertex *destination =
                &sprite_ft4.native.primitive.triangles[triangle]
                     .vertices[vertex];
            destination->x =
                (int32_t)sprite_ft4.native.vertices[source].x * INT32_C(65536);
            destination->y =
                (int32_t)sprite_ft4.native.vertices[source].y * INT32_C(65536);
        }
    }
    sprite_ft4.phase = SPRITE_EXPECT_MATERIAL;
}

void xg_render_model_sprite_pipeline_sprite_material_seam(
        CPUState *cpu, GuestRenderRenderMode render_mode,
        const XgRenderModelSpritePipelineServices *services) {
    uint16_t expected_uv[4];

    if (!sprite_ft4.snapshot.context_active) return;
    if (sprite_ft4.phase != SPRITE_EXPECT_MATERIAL || cpu == NULL ||
        cpu->read_word == NULL || cpu->read_half == NULL ||
        cpu->read_byte == NULL) {
        block_sprite(84u);
        return;
    }
    for (uint32_t vertex = 0u; vertex < 4u; ++vertex)
        expected_uv[vertex] = sprite_ft4.uv[vertex][0] |
            ((uint16_t)sprite_ft4.uv[vertex][1] << 8u);
    sprite_ft4.payload_matches &= compare_ft4_payload(
        cpu, sprite_ft4.packet_address, sprite_ft4.descriptor_address,
        sprite_ft4.material_word, expected_uv, sprite_ft4.tpage,
        sprite_ft4.clut, &sprite_ft4.snapshot.first_payload_mismatch);
    ++sprite_ft4.snapshot.projection_count;
    if (!sprite_ft4.geometry_matches)
        ++sprite_ft4.snapshot.geometry_mismatch_count;
    if (!sprite_ft4.payload_matches)
        ++sprite_ft4.snapshot.payload_mismatch_count;
    if (sprite_ft4.payload_matches) {
        ++sprite_ft4.snapshot.match_count;
        if (render_mode == GUEST_RENDER_RENDER_NATIVE &&
            !stage_sprite(cpu, render_mode, services)) {
            sprite_ft4.snapshot.blocker_detail = 880000u +
                xg_render_submission_standalone_failure_detail();
            block_sprite(88u);
            return;
        }
    } else {
        sprite_ft4.invocation_matches = false;
        if (sprite_ft4.snapshot.mismatch_count == 0u) {
            sprite_ft4.snapshot.first_mismatch_packet =
                sprite_ft4.packet_address;
            sprite_ft4.snapshot.first_mismatch_descriptor =
                sprite_ft4.descriptor_address;
        }
        ++sprite_ft4.snapshot.mismatch_count;
    }
    sprite_ft4.snapshot.pending = false;
    sprite_ft4.phase = SPRITE_IDLE;
}

void xg_render_model_sprite_pipeline_sprite_end(
        bool nonwrapper_only, GuestRenderRenderMode render_mode,
        const XgRenderModelSpritePipelineServices *services) {
    if (!sprite_ft4.snapshot.context_active ||
        (nonwrapper_only && sprite_ft4.wrapper_scope))
        return;
    if (sprite_ft4.snapshot.pending) {
        block_sprite(85u);
        return;
    }
    if (render_mode == GUEST_RENDER_RENDER_NATIVE &&
        !sprite_ft4.invocation_matches) {
        block_sprite(89u);
        return;
    }
    if (render_mode == GUEST_RENDER_RENDER_NATIVE &&
        sprite_ft4.native_record_count != 0u) {
        XgRenderModelFt4SourceRecord sources[SPRITE_CAPACITY];
        XgRenderModelSourcePublication publications[SPRITE_CAPACITY];
        for (uint32_t index = 0u;
             index < sprite_ft4.native_record_count; ++index) {
            const SpriteStageRecord *record =
                &sprite_ft4.native_records[index];
            sources[index] = (XgRenderModelFt4SourceRecord){
                .primitive = record->primitive,
                .lifecycle = sprite_ft4.native_lifecycles[index],
                .source_id =
                    (record->packet_address & UINT32_C(0x1fffffff)) + 4u,
                .interpolation_producer_id =
                    record->interpolation_producer_id,
                .interpolation_primitive_id =
                    record->interpolation_primitive_id,
                .opcode = sprite_ft4.native_opcodes[index],
                .interpolation_identity_valid =
                    record->interpolation_identity_valid,
                .valid = true,
            };
            publications[index] = (XgRenderModelSourcePublication){
                .resource_address = record->packet_address + 4u,
                .resource_size = 0x24u,
                .register_replay = true,
            };
        }
        if (!xg_render_model_repository_store_ft4_sources(
                sources, publications, sprite_ft4.native_record_count,
                services->repository)) {
            block_sprite(88u);
            return;
        }
        if (sprite_ft4.motion.handle.resource_id)
            for (uint32_t index = 0u; index < sprite_ft4.native_record_count; ++index) {
                const SpriteStageRecord *record = &sprite_ft4.native_records[index];
                if (!publish_pose_endpoint(&record->primitive,
                        (record->packet_address & 0x1fffffffu) + 4u,
                        record->interpolation_producer_id, record->interpolation_primitive_id,
                        sprite_ft4.motion)) {
                    block_sprite(88u);
                    return;
                }
            }
        sprite_ft4.snapshot.resident_publish_source_count +=
            sprite_ft4.native_record_count;
        ++sprite_ft4.snapshot.native_cutover_count;
        sprite_ft4.snapshot.native_primitive_count +=
            sprite_ft4.native_record_count;
    }
    clear_sprite();
}

void xg_render_model_sprite_pipeline_clear_model(void) {
    gear_helper_mode1_proof = (GearHelperMode1Proof){0};
    clear_model_ft4_pending();
    clear_model_ft3_pending();
}

void xg_render_model_sprite_pipeline_clear_sprite(void) {
    clear_sprite();
}

void xg_render_model_sprite_pipeline_invalidate_model_code(void) {
    gear_helper_mode1_proof = (GearHelperMode1Proof){0};
    if (model_ft4.context.valid || model_ft4.snapshot.pending)
        block_model_ft4(76u);
    else
        clear_model_ft4_pending();
    if (model_ft3.snapshot.pending)
        block_model_ft3(76u);
    else
        clear_model_ft3_pending();
}

void xg_render_model_sprite_pipeline_invalidate_model_data(void) {
    gear_helper_mode1_proof = (GearHelperMode1Proof){0};
    clear_model_ft4_pending();
    clear_model_ft3_pending();
}

void xg_render_model_sprite_pipeline_invalidate_sprite_code(void) {
    if (sprite_ft4.snapshot.context_active || sprite_ft4.snapshot.pending)
        block_sprite(87u);
    else
        clear_sprite();
}

void xg_render_model_sprite_pipeline_record_ft3_replay(
        XgRenderModelReplayResult result,
        const GuestRenderNativeStreamMissContext *context) {
    if (result == XG_RENDER_MODEL_REPLAY_NOT_APPLICABLE) return;
    ++model_ft3.snapshot.replay_attempt_count;
    switch (result) {
    case XG_RENDER_MODEL_REPLAY_LOOKUP_INVALID:
        ++model_ft3.snapshot.replay_lookup_invalid_count;
        /* fall through */
    case XG_RENDER_MODEL_REPLAY_LOOKUP_ABSENT:
        ++model_ft3.snapshot.replay_lookup_miss_count;
        if (result == XG_RENDER_MODEL_REPLAY_LOOKUP_ABSENT)
            ++model_ft3.snapshot.replay_lookup_absent_count;
        model_ft3.snapshot.last_replay_lookup_miss_source =
            context != NULL ? (uint32_t)context->command_id : 0u;
        break;
    case XG_RENDER_MODEL_REPLAY_RECORD_REJECTED:
        ++model_ft3.snapshot.replay_record_reject_count;
        break;
    case XG_RENDER_MODEL_REPLAY_CONTAINER_REJECTED:
        ++model_ft3.snapshot.replay_container_reject_count;
        break;
    case XG_RENDER_MODEL_REPLAY_LIFECYCLE_REJECTED:
        ++model_ft3.snapshot.replay_lifecycle_reject_count;
        break;
    case XG_RENDER_MODEL_REPLAY_TRANSLATE_REJECTED:
        ++model_ft3.snapshot.replay_translate_reject_count;
        break;
    case XG_RENDER_MODEL_REPLAY_RESOLVED:
        ++model_ft3.snapshot.replay_resolved_count;
        break;
    default:
        break;
    }
}

void xg_render_model_sprite_pipeline_record_ft4_replay(
        XgRenderModelReplayResult result, bool sprite_opcode,
        uint32_t miss_source_id) {
    if (result == XG_RENDER_MODEL_REPLAY_NOT_APPLICABLE) return;
    if (sprite_opcode)
        ++sprite_ft4.snapshot.resident_replay_attempt_count;
    else
        ++model_ft4.snapshot.replay_attempt_count;
    switch (result) {
    case XG_RENDER_MODEL_REPLAY_LOOKUP_ABSENT:
    case XG_RENDER_MODEL_REPLAY_LOOKUP_INVALID:
        if (sprite_opcode) {
            ++sprite_ft4.snapshot.resident_replay_lookup_miss_count;
            sprite_ft4.snapshot.last_resident_miss_source = miss_source_id;
        } else
            ++model_ft4.snapshot.replay_lookup_miss_count;
        break;
    case XG_RENDER_MODEL_REPLAY_RECORD_REJECTED:
        if (sprite_opcode)
            ++sprite_ft4.snapshot.resident_replay_record_reject_count;
        else
            ++model_ft4.snapshot.replay_record_reject_count;
        break;
    case XG_RENDER_MODEL_REPLAY_CONTAINER_REJECTED:
        if (sprite_opcode)
            ++sprite_ft4.snapshot.resident_replay_container_reject_count;
        else
            ++model_ft4.snapshot.replay_container_reject_count;
        break;
    case XG_RENDER_MODEL_REPLAY_LIFECYCLE_REJECTED:
        if (sprite_opcode)
            ++sprite_ft4.snapshot.resident_replay_lifecycle_reject_count;
        else
            ++model_ft4.snapshot.replay_lifecycle_reject_count;
        break;
    case XG_RENDER_MODEL_REPLAY_TRANSLATE_REJECTED:
        if (sprite_opcode)
            ++sprite_ft4.snapshot.resident_replay_translate_reject_count;
        else
            ++model_ft4.snapshot.replay_translate_reject_count;
        break;
    case XG_RENDER_MODEL_REPLAY_RESOLVED:
        if (sprite_opcode)
            ++sprite_ft4.snapshot.resident_replay_resolved_count;
        else
            ++model_ft4.snapshot.replay_resolved_count;
        break;
    default:
        break;
    }
}

void xg_render_model_sprite_pipeline_ft4_snapshot(
        PsxXgRenderModelFt4ShadowSnapshot *out_snapshot) {
    if (out_snapshot != NULL) *out_snapshot = model_ft4.snapshot;
}

void xg_render_model_sprite_pipeline_ft3_snapshot(
        PsxXgRenderModelFt3ShadowSnapshot *out_snapshot) {
    if (out_snapshot == NULL) return;
    *out_snapshot = model_ft3.snapshot;
    out_snapshot->source_count = xg_render_model_repository_ft3_source_count();
}

void xg_render_model_sprite_pipeline_sprite_snapshot(
        PsxXgRenderSpriteFt4ShadowSnapshot *out_snapshot) {
    if (out_snapshot == NULL) return;
    *out_snapshot = sprite_ft4.snapshot;
    xg_render_field_sprite_diagnostics_update_snapshot(out_snapshot);
}

void xg_render_model_sprite_pipeline_reset(
        const XgRenderModelSpritePipelineServices *services) {
    xg_render_motion_reset();
    xg_render_gear_motion_reset();
    gear_helper_mode1_proof = (GearHelperMode1Proof){0};
    free(model_ft4.records);
    free(model_ft3.records);
    free(model_coverage.samples);
    free(model_coverage.bindings);
    memset(&model_coverage, 0, sizeof(model_coverage));
    model_ft4 = (ModelFt4State){0};
    model_ft3 = (ModelFt3State){0};
    sprite_ft4 = (SpriteState){0};
    xg_render_model_repository_clear_ft4_sources();
    xg_render_model_repository_clear_ft3_sources(
        services != NULL ? services->repository : NULL);
}

void xg_render_model_sprite_pipeline_handle_invalidation(
        const XgRenderInvalidationEvent *event,
        const XgRenderInvalidationServices *services) {
    const bool model_code = xg_render_invalidation_has_code_class(
        event, PSX_XG_RENDER_CODE_WRITE_MODEL_FT4);
    const bool model_data = xg_render_invalidation_has_code_class(
        event, PSX_XG_RENDER_CODE_WRITE_MODEL_DISPATCH_DATA);
    const bool sprite_code = xg_render_invalidation_has_code_class(
        event, PSX_XG_RENDER_CODE_WRITE_SPRITE_FT4);
    const bool shared_data = xg_render_invalidation_has_code_class(
        event, PSX_XG_RENDER_CODE_WRITE_SHARED_TRIG_DATA);
    /* A legacy proof can be disarmed while independently authenticated source
     * producers continue. Actual code/resource writes and scene boundaries
     * still invalidate their own inputs through the paths below. */
    if (event->kind == XG_RENDER_INVALIDATION_AUTHORITY_LOST &&
        xg_render_submission_native_work_mode()) {
        xg_render_motion_prune_authority();
        return;
    }
    if (event->kind == XG_RENDER_INVALIDATION_CODE_WRITE) {
        xg_render_gear_motion_invalidate(event->address,event->size);
        if (gear_helper_mode1_proof.armed ||
            model_ft4.context.caller_contract == XG_RENDER_MODEL_DISPATCH_CALLER_GEAR_HELPER) {
            const uint64_t begin = event->address & UINT32_C(0x1fffffff);
            const uint64_t end = begin + event->size;
            if (event->size && begin < 0x1e8638u && end > 0x1dc000u) {
                gear_helper_mode1_proof = (GearHelperMode1Proof){0};
                /* A snapshot may still retain this pose. Do not let a pending
                 * Gear capture re-register its retired command binding. */
                if (model_ft4.context.caller_contract ==
                    XG_RENDER_MODEL_DISPATCH_CALLER_GEAR_HELPER)
                    xg_render_model_sprite_pipeline_invalidate_model_code();
            }
        }
    } else if (event->kind != XG_RENDER_INVALIDATION_RESOURCE_OVERLAP)
        xg_render_gear_motion_reset();

    if (event->kind == XG_RENDER_INVALIDATION_RESOURCE_OVERLAP ||
        (event->kind == XG_RENDER_INVALIDATION_CODE_WRITE && event->mutation.resource_mutation))
        xg_render_motion_invalidate_range(event->address,event->size);
    if ((event->kind != XG_RENDER_INVALIDATION_CODE_WRITE &&
         event->kind != XG_RENDER_INVALIDATION_RESOURCE_OVERLAP) || model_code || model_data)
        xg_render_motion_reset();

    if (event->kind == XG_RENDER_INVALIDATION_CODE_WRITE) {
        if (model_code)
            xg_render_model_sprite_pipeline_invalidate_model_code();
        else if (model_data)
            xg_render_model_sprite_pipeline_invalidate_model_data();
        if (sprite_code)
            xg_render_model_sprite_pipeline_invalidate_sprite_code();
        else if (shared_data)
            xg_render_model_sprite_pipeline_clear_sprite();
    } else if (event->kind == XG_RENDER_INVALIDATION_DISABLE ||
               event->kind == XG_RENDER_INVALIDATION_SCENE_BOUNDARY) {
        xg_render_model_sprite_pipeline_clear_model();
        xg_render_model_sprite_pipeline_clear_sprite();
    } else if (event->kind == XG_RENDER_INVALIDATION_RESET) {
        xg_render_model_sprite_pipeline_reset(
            services->model_sprite_pipeline);
    }
}
