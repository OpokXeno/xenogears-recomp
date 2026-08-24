#include "xg_render_model_sprite_pipeline.h"

#include "gpu.h"
#include "xg_field_render_services.h"
#include "xg_model_ft4_raw.h"
#include "xg_render_field_sprite.h"
#include "xg_render_primitive_utils.h"
#include "xg_render_runtime_variant_auth.h"
#include "xg_sprite_ft4.h"

#include <limits.h>
#include <stddef.h>
#include <string.h>

enum {
    MODEL_CAPACITY = XG_RENDER_IR_ITEM_CAPACITY,
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
};

typedef struct ModelContext {
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
} ModelContext;

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
    ModelFt4Record records[MODEL_CAPACITY];
    ModelContext context;
    PsxXgRenderModelFt4ShadowSnapshot snapshot;
    uint32_t initial_packet_cursor;
    uint32_t initial_counter;
    uint32_t expected_counter_delta;
    uint32_t descriptor_base;
    uint32_t count;
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
    ModelFt3Record records[MODEL_CAPACITY];
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
    { UINT32_C(0x8002675c), 0x274u },
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
        model_ft4.count == 0u)
        return;
    model_ft4.context = (ModelContext){0};
    model_ft4.initial_packet_cursor = 0u;
    model_ft4.initial_counter = 0u;
    model_ft4.expected_counter_delta = 0u;
    model_ft4.descriptor_base = 0u;
    model_ft4.count = 0u;
    model_ft4.snapshot.pending = false;
}

static void block_model_ft4(uint32_t blocker) {
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
        cpu->gpr[7] != XG_MODEL_FT4_RAW_DISPATCH_FARTHEST) {
        ++model_ft4.snapshot.dispatch_mode_reject_count;
        return;
    }
    context.model_address = cpu->gpr[4];
    context.instance_address = cpu->gpr[16];
    context.packet_base = cpu->gpr[5];
    context.ot_base = cpu->gpr[6];
    if (!word_address_is_valid(context.model_address) ||
        !word_address_is_valid(context.model_address + 0x14u) ||
        !word_address_is_valid(context.packet_base) ||
        !word_address_is_valid(context.ot_base)) {
        block_model_ft4(72u);
        return;
    }
    context.vertex_base = cpu->read_word(context.model_address + 8u);
    context.topology_base = cpu->read_word(context.model_address + 0x10u);
    context.material_base = cpu->read_word(context.model_address + 0x14u);
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
    context.tpage = cpu->read_half(UINT32_C(0x80059308));
    context.clut = cpu->read_half(UINT32_C(0x8005930c));
    context.dispatch_mode = (uint8_t)cpu->gpr[7];
    context.caller_window_start = caller_window_start;
    context.caller_contract = caller_contract;
    context.resident_dispatch =
        caller_contract == XG_RENDER_MODEL_DISPATCH_CALLER_RESIDENT;
    context.valid = true;
    model_ft4.context = context;
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
    if (cpu->gpr[5] > MODEL_CAPACITY)
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

        if (row >= 17u || primitive_count > 4096u ||
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
                    primitive >= MODEL_CAPACITY) {
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
                    ++model_ft4.snapshot.template_hit_count;
                    if (!apply_ft4_material(&source, record)) {
                        model_ft4.snapshot.prepare_failure_detail = 6u;
                        return false;
                    }
                } else {
                    ++model_ft4.snapshot.template_miss_count;
                    if (!capture_ft4_packet_material(
                            cpu, source.packet_address, &source, record) &&
                        !decode_ft4_material(
                            cpu, attribute_address, tpage, clut, &source,
                            record)) {
                        model_ft4.snapshot.prepare_failure_detail = 6u;
                        return false;
                    }
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
    }
    ++model_ft4.snapshot.native_cutover_count;
    model_ft4.snapshot.native_primitive_count += accepted_count;
    return true;
}

void xg_render_model_sprite_pipeline_observe_ft4_guest_pass(
        CPUState *cpu, bool average_mode) {
    static const uint8_t split[2][3] = {{0u, 1u, 2u}, {2u, 1u, 3u}};
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
             XG_MODEL_FT4_RAW_DISPATCH_AVERAGE) ||
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
            for (uint32_t vertex = 0u; vertex < 4u; ++vertex)
                geometry_matches &= cpu->read_word(
                    record->packet_address + 8u + vertex * 8u) ==
                    record->observed_xy[vertex];
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
    return (int32_t)vertices[0].x * vertices[1].y +
        (int32_t)vertices[1].x * vertices[2].y +
        (int32_t)vertices[2].x * vertices[0].y -
        (int32_t)vertices[0].x * vertices[2].y -
        (int32_t)vertices[1].x * vertices[0].y -
        (int32_t)vertices[2].x * vertices[1].y;
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
        if (row >= 17u || primitive_count > 4096u ||
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
                    primitive >= MODEL_CAPACITY) {
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
        XgRenderIrVertex *destination =
            &record->primitive.triangles[0].vertices[vertex];
        record->observed_xy[vertex] = cpu->gte_data[12u + vertex];
        destination->x =
            (int32_t)low_s16(record->observed_xy[vertex]) * INT32_C(65536);
        destination->y =
            (int32_t)low_s16(record->observed_xy[vertex] >> 16u) *
            INT32_C(65536);
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
            for (uint32_t vertex = 0u; vertex < 3u; ++vertex)
                geometry_matches &= cpu->read_word(
                    record->packet_address + 8u + vertex * 8u) ==
                    record->observed_xy[vertex];
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

void xg_render_model_sprite_pipeline_model_finish(
        CPUState *cpu, GuestRenderRenderMode render_mode,
        const XgRenderModelSpritePipelineServices *services) {
    finish_model_ft4(cpu, render_mode, services);
    finish_model_ft3(cpu, render_mode, services);
}

void xg_render_model_sprite_pipeline_model_end(void) {
    if (model_ft4.snapshot.pending) block_model_ft4(74u);
    if (model_ft3.snapshot.pending) block_model_ft3(74u);
    if (!model_ft4.snapshot.pending && !model_ft3.snapshot.pending)
        model_ft4.context = (ModelContext){0};
}

void xg_render_model_sprite_pipeline_capture_ft3_link(
        CPUState *cpu, GuestRenderRenderMode render_mode,
        const XgRenderModelSpritePipelineServices *services) {
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
        if (material == NULL && !decode_ft3_material(
                cpu, cpu->gpr[16], cpu->read_half(UINT32_C(0x80059308)),
                cpu->read_half(UINT32_C(0x8005930c)), &decoded)) {
            ++model_ft3.snapshot.template_miss_count;
            return;
        }
        if (material == NULL) {
            ++model_ft3.snapshot.template_miss_count;
            material = &decoded;
        }
    }
    if (!material->valid) return;
    for (uint32_t index = 0u; index < model_ft3.count; ++index) {
        if (physical_address_equals(
                model_ft3.records[index].packet_address, packet)) {
            primitive = model_ft3.records[index].primitive;
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
    const uint32_t packet = cpu != NULL ? cpu->gpr[19] : 0u;
    const uint32_t source_id = normalized_word_address(packet) + 4u;
    const XgRenderModelFt3SourceRecord *source =
        xg_render_model_repository_find_ft3_source(source_id);

    if (source == NULL || !source->link_pending || cpu == NULL ||
        cpu->read_word == NULL)
        return;
    xg_render_model_repository_finish_ft3_link(
        source_id,
        cpu->gpr[13] != 0u && (cpu->read_word(packet) >> 24u) == 7u,
        &(XgRenderModelSourcePublication){
            .resource_address = packet,
            .resource_size = 0x20u,
            .register_replay = true,
        }, services->repository);
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

static bool prepare_sprite(CPUState *cpu) {
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
    memcpy(sprite_ft4.uv, source.uv, sizeof(source.uv));
    sprite_ft4.packet_address = packet_address;
    sprite_ft4.descriptor_address = descriptor_address;
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
        !prepare_sprite(cpu)) {
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
        sprite_ft4.snapshot.resident_publish_source_count +=
            sprite_ft4.native_record_count;
        ++sprite_ft4.snapshot.native_cutover_count;
        sprite_ft4.snapshot.native_primitive_count +=
            sprite_ft4.native_record_count;
    }
    clear_sprite();
}

void xg_render_model_sprite_pipeline_clear_model(void) {
    clear_model_ft4_pending();
    clear_model_ft3_pending();
}

void xg_render_model_sprite_pipeline_clear_sprite(void) {
    clear_sprite();
}

void xg_render_model_sprite_pipeline_invalidate_model_code(void) {
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
        XgRenderModelReplayResult result, bool sprite_opcode) {
    if (result == XG_RENDER_MODEL_REPLAY_NOT_APPLICABLE) return;
    if (sprite_opcode)
        ++sprite_ft4.snapshot.resident_replay_attempt_count;
    else
        ++model_ft4.snapshot.replay_attempt_count;
    switch (result) {
    case XG_RENDER_MODEL_REPLAY_LOOKUP_ABSENT:
    case XG_RENDER_MODEL_REPLAY_LOOKUP_INVALID:
        if (sprite_opcode)
            ++sprite_ft4.snapshot.resident_replay_lookup_miss_count;
        else
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
    model_ft4 = (ModelFt4State){0};
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
