#include "xg_field_zoom.h"

#include "cpu_state.h"
#include "gpu.h"
#include "psx_cyc.h"
#include "xg_field_character_adapter.h"
#include "xg_field_render_services.h"
#include "xg_render_backend.h"
#include "xg_render_primitive_utils.h"

#include <stddef.h>
#include <string.h>

typedef struct XgRenderZoomInitializerPending {
    uint64_t artifact_generation;
    uint32_t entry_sp;
    uint32_t return_address;
    uint8_t semi_transparent;
    uint8_t abr;
    bool artifact_authorized;
    bool valid;
} XgRenderZoomInitializerPending;

typedef struct XgRenderZoomRgbPending {
    uint64_t generation;
    uint8_t intensity;
    uint8_t buffer_index;
    bool valid;
} XgRenderZoomRgbPending;

typedef struct XgRenderZoomInvocation {
    uint64_t source_generation;
    uint32_t entry_sp;
    uint32_t return_address;
    bool valid;
} XgRenderZoomInvocation;

typedef struct XgRenderZoomCounters {
    uint64_t initializer_begin_count;
    uint64_t initializer_commit_count;
    uint64_t initializer_2e_count;
    uint64_t rgb_update_count;
    uint64_t invocation_count;
    uint64_t cutover_attempt_count;
    uint64_t native_invocation_count;
    uint64_t native_primitive_count;
    uint64_t replay_invocation_count;
    uint64_t replay_primitive_count;
    uint64_t rejection_count;
    uint32_t last_rejection_blocker;
} XgRenderZoomCounters;

static XgRenderZoomSource zoom_source;
static XgRenderZoomInitializerPending zoom_initializer_pending;
static XgRenderZoomRgbPending zoom_rgb_pending;
static XgRenderZoomInvocation zoom_invocation;
static XgRenderZoomCounters zoom_counters;
static uint64_t zoom_next_generation = 1u;

typedef struct XgFieldZoomRange {
    uint32_t start;
    uint32_t size;
} XgFieldZoomRange;

static const XgFieldZoomRange zoom_code_ranges[] = {
    { UINT32_C(0x8003f738), 0x178u },
    { UINT32_C(0x80043a1c), 0x3cu },
    { UINT32_C(0x80043bfc), 0x28u },
    { UINT32_C(0x80043cb0), 0x14u },
    { UINT32_C(0x800454dc), 0x58u },
    { UINT32_C(0x800459dc), 0x50u },
    { UINT32_C(0x80045c10), 0x78u },
    { UINT32_C(0x80049dcc), 0x124u },
    { UINT32_C(0x80049efc), 0x30u },
    { UINT32_C(0x80049f8c), 0x20u },
    { UINT32_C(0x8004a7bc), 0x7cu },
    { UINT32_C(0x80078ef8), 8u }, { UINT32_C(0x80078fb8), 8u },
    { UINT32_C(0x80079168), 8u }, { UINT32_C(0x800791fc), 8u },
    { UINT32_C(0x800a58a4), 8u }, { UINT32_C(0x800a58f0), 8u },
    { UINT32_C(0x800a5ac0), 8u }, { UINT32_C(0x800a5b8c), 8u },
    { UINT32_C(0x800a5bec), 8u }, { UINT32_C(0x800a5e60), 8u },
    { UINT32_C(0x800a5f6c), 8u }, { UINT32_C(0x800a6088), 8u },
    { UINT32_C(0x800a6150), 8u }, { UINT32_C(0x800a61ec), 8u },
    { UINT32_C(0x800a6278), 8u }, { UINT32_C(0x800a62c0), 8u },
    { UINT32_C(0x800a637c), 8u },
    { UINT32_C(0x800a5de8), 8u }, { UINT32_C(0x800a5e90), 8u },
    { UINT32_C(0x800abc44), 8u }, { UINT32_C(0x800abc68), 8u },
    { UINT32_C(0x800abca4), 8u },
};

static bool ranges_overlap(uint32_t left_start, uint32_t left_size,
                           uint32_t right_start, uint32_t right_size) {
    const uint64_t left = left_start & UINT32_C(0x1fffffff);
    const uint64_t right = right_start & UINT32_C(0x1fffffff);

    return left_size != 0u && right_size != 0u &&
        left < right + right_size && right < left + left_size;
}

bool xg_field_zoom_code_write_overlaps(uint32_t address, uint32_t size) {
    for (uint32_t index = 0u;
         index < sizeof(zoom_code_ranges) / sizeof(zoom_code_ranges[0]);
         ++index) {
        if (ranges_overlap(zoom_code_ranges[index].start,
                           zoom_code_ranges[index].size, address, size))
            return true;
    }
    return false;
}

void xg_field_zoom_register_code_watches(
        void (*set_range)(uint32_t physical_address, uint32_t size)) {
    if (set_range == NULL) return;
    for (uint32_t index = 0u;
         index < sizeof(zoom_code_ranges) / sizeof(zoom_code_ranges[0]);
         ++index)
        set_range(zoom_code_ranges[index].start & UINT32_C(0x1fffffff),
                  zoom_code_ranges[index].size);
}

void xg_field_zoom_invalidate_overlapping(uint32_t address, uint32_t size) {
    for (uint32_t quad = 0u; quad < XG_RENDER_ZOOM_QUAD_COUNT; ++quad) {
        for (uint32_t buffer = 0u; buffer < XG_RENDER_ZOOM_BUFFER_COUNT;
             ++buffer) {
            if (ranges_overlap(
                    UINT32_C(0x800b1274) + quad * 0x50u + buffer * 0x28u,
                    0x28u, address, size)) {
                xg_field_zoom_reset();
                return;
            }
        }
    }
}

static void materialize_zoom_source(uint8_t semi_transparent, uint8_t abr,
                                    uint64_t artifact_generation) {
    uint32_t buffer;
    uint32_t quad;

    zoom_source = (XgRenderZoomSource){
        .generation = zoom_next_generation++,
        .artifact_generation = artifact_generation,
        .producer_store_pc = XG_RENDER_ZOOM_TEMPLATE_STORE_PC,
        .command = semi_transparent ? 0x2eu : 0x2cu,
        .semi_transparent = semi_transparent != 0u,
        .authenticated = artifact_generation != 0u,
        .valid = true,
    };
    for (buffer = 0u; buffer < XG_RENDER_ZOOM_BUFFER_COUNT; ++buffer) {
        for (quad = 0u; quad < XG_RENDER_ZOOM_QUAD_COUNT; ++quad) {
            XgRenderZoomQuadSource *source = &zoom_source.quads[buffer][quad];
            const int16_t left = (int16_t)(quad * 64u);
            const int16_t right = (int16_t)((quad + 1u) * 64u);

            *source = (XgRenderZoomQuadSource){
                .projection_x = {
                    (int16_t)(-80 + (int32_t)quad * 32),
                    (int16_t)(-48 + (int32_t)quad * 32),
                    (int16_t)(-80 + (int32_t)quad * 32),
                    (int16_t)(-48 + (int32_t)quad * 32),
                },
                .projection_y = { -56, -56, 56, 56 },
                .projection_z = { 0, 0, 0, 0 },
                .x = { left, right, left, right },
                .y = { 0, 0, 223, 223 },
                .red = 0x80u,
                .green = 0x80u,
                .blue = 0x80u,
                .tpage = (uint16_t)(0x11bu + quad + ((uint32_t)abr << 5u)),
            };
        }
    }
}

void xg_field_zoom_reset(void) {
    if (!zoom_source.valid && !zoom_initializer_pending.valid &&
        !zoom_rgb_pending.valid && !zoom_invocation.valid)
        return;
    zoom_source = (XgRenderZoomSource){ 0 };
    zoom_initializer_pending = (XgRenderZoomInitializerPending){ 0 };
    zoom_rgb_pending = (XgRenderZoomRgbPending){ 0 };
    zoom_invocation = (XgRenderZoomInvocation){ 0 };
}

void xg_field_zoom_classify_code_write(
        uint32_t address, uint32_t size,
        XgRenderMutationClassification *out_classification) {
    const bool overlaps = xg_field_zoom_code_write_overlaps(address, size);

    if (out_classification == NULL) return;
    *out_classification = (XgRenderMutationClassification){
        .properties = {
            .watched_range_mutation = overlaps,
            .runtime_variant_mutation = overlaps,
            .executable_mutation = overlaps,
            .authentication_mutation = overlaps,
            .authority_loss = overlaps,
            .interpolation_reset = overlaps,
            .reset_runtime_variant = overlaps,
        },
        .code_write_mask = overlaps
            ? UINT32_C(1) << PSX_XG_RENDER_CODE_WRITE_ZOOM : 0u,
    };
}

void xg_field_zoom_handle_invalidation(
        const XgRenderInvalidationEvent *event,
        const XgRenderInvalidationServices *services) {
    (void)services;
    if (event->kind == XG_RENDER_INVALIDATION_CODE_WRITE) {
        if (event->mutation.resource_mutation)
            xg_field_zoom_invalidate_overlapping(event->address, event->size);
        if (event->mutation.semantic_authority_loss ||
            xg_render_invalidation_has_code_class(
                event, PSX_XG_RENDER_CODE_WRITE_ZOOM))
            xg_field_zoom_reset();
    } else if (event->kind == XG_RENDER_INVALIDATION_DISABLE ||
               event->kind == XG_RENDER_INVALIDATION_SCENE_BOUNDARY ||
               event->kind == XG_RENDER_INVALIDATION_LOADER_MISMATCH ||
               event->kind == XG_RENDER_INVALIDATION_RESOURCE_OVERLAP) {
        xg_field_zoom_reset();
    } else if (event->kind == XG_RENDER_INVALIDATION_RESET) {
        xg_field_zoom_reset();
    }
}

void xg_field_zoom_reset_pending(void) {
    zoom_initializer_pending = (XgRenderZoomInitializerPending){ 0 };
    zoom_rgb_pending = (XgRenderZoomRgbPending){ 0 };
    zoom_invocation = (XgRenderZoomInvocation){ 0 };
}

void xg_field_zoom_reset_source_and_invocation(void) {
    zoom_source = (XgRenderZoomSource){0};
    zoom_invocation = (XgRenderZoomInvocation){0};
}

bool xg_field_zoom_source_valid(void) { return zoom_source.valid; }

void xg_field_zoom_template_contract_snapshot(
        PsxXgRenderZoomTemplateContractSnapshot *out_snapshot) {
    if (out_snapshot == NULL) return;
    *out_snapshot = (PsxXgRenderZoomTemplateContractSnapshot){
        .generation = zoom_source.generation,
        .producer_store_pc = zoom_source.producer_store_pc,
        .template_count = XG_RENDER_ZOOM_QUAD_COUNT,
        .buffer_count = XG_RENDER_ZOOM_BUFFER_COUNT,
        .opcode = zoom_source.command,
        .authenticated = zoom_source.valid && zoom_source.command == 0x2eu &&
            zoom_source.authenticated &&
            zoom_source.producer_store_pc == XG_RENDER_ZOOM_TEMPLATE_STORE_PC,
        .initializer_begin_count = zoom_counters.initializer_begin_count,
        .initializer_commit_count = zoom_counters.initializer_commit_count,
        .initializer_2e_count = zoom_counters.initializer_2e_count,
        .rgb_update_count = zoom_counters.rgb_update_count,
        .invocation_count = zoom_counters.invocation_count,
        .cutover_attempt_count = zoom_counters.cutover_attempt_count,
        .native_invocation_count = zoom_counters.native_invocation_count,
        .native_primitive_count = zoom_counters.native_primitive_count,
        .replay_invocation_count = zoom_counters.replay_invocation_count,
        .replay_primitive_count = zoom_counters.replay_primitive_count,
        .rejection_count = zoom_counters.rejection_count,
        .last_rejection_blocker = zoom_counters.last_rejection_blocker,
    };
}

void xg_field_zoom_register_resource_watches(
        void (*watch_resource)(uint32_t address, uint32_t size)) {
    if (!zoom_source.valid || watch_resource == NULL) return;
    for (uint32_t quad = 0u; quad < XG_RENDER_ZOOM_QUAD_COUNT; ++quad) {
        for (uint32_t buffer = 0u; buffer < XG_RENDER_ZOOM_BUFFER_COUNT;
             ++buffer) {
            watch_resource(
                UINT32_C(0x800b1274) + quad * 0x50u + buffer * 0x28u,
                0x28u);
        }
    }
}

void xg_field_zoom_counters_reset(void) {
    zoom_counters = (XgRenderZoomCounters){ 0 };
}

void xg_field_zoom_note_cutover_attempt(void) {
    ++zoom_counters.cutover_attempt_count;
}

void xg_field_zoom_note_native_invocation(uint32_t primitive_count) {
    ++zoom_counters.native_invocation_count;
    zoom_counters.native_primitive_count += primitive_count;
}

void xg_field_zoom_note_replay_invocation(uint32_t primitive_count) {
    ++zoom_counters.replay_invocation_count;
    zoom_counters.replay_primitive_count += primitive_count;
}

void xg_field_zoom_note_rejection(uint32_t blocker) {
    ++zoom_counters.rejection_count;
    zoom_counters.last_rejection_blocker = blocker;
}

bool xg_field_zoom_caller_is_authorized(uint32_t return_address) {
    static const uint32_t callers[] = {
        UINT32_C(0x80078f00), UINT32_C(0x80078fc0),
        UINT32_C(0x80079170), UINT32_C(0x80079204),
        UINT32_C(0x800a58ac), UINT32_C(0x800a58f8),
        UINT32_C(0x800a5ac8), UINT32_C(0x800a5b94),
        UINT32_C(0x800a5bf4), UINT32_C(0x800a5e68),
        UINT32_C(0x800a5f74), UINT32_C(0x800a6090),
        UINT32_C(0x800a6158), UINT32_C(0x800a61f4),
        UINT32_C(0x800a6280), UINT32_C(0x800a62c8),
        UINT32_C(0x800a6384),
    };
    uint32_t index;

    for (index = 0u; index < sizeof(callers) / sizeof(callers[0]); ++index)
        if ((return_address & 0x1fffffffu) == (callers[index] & 0x1fffffffu))
            return true;
    return false;
}

static bool initializer_caller_is_authorized(uint32_t return_address) {
    static const uint32_t callers[] = {
        UINT32_C(0x800a5898), UINT32_C(0x800a5b2c),
        UINT32_C(0x800a5e1c), UINT32_C(0x800a5f28),
        UINT32_C(0x800a61dc), UINT32_C(0x800a62b0),
    };
    uint32_t index;

    for (index = 0u; index < sizeof(callers) / sizeof(callers[0]); ++index)
        if ((return_address & 0x1fffffffu) == (callers[index] & 0x1fffffffu))
            return true;
    return false;
}

void xg_field_zoom_observe_initializer_begin(
    CPUState *cpu, GuestRenderRenderMode render_mode,
    uint64_t artifact_generation) {
    ++zoom_counters.initializer_begin_count;
    zoom_source = (XgRenderZoomSource){ 0 };
    zoom_initializer_pending = (XgRenderZoomInitializerPending){ 0 };
    zoom_rgb_pending = (XgRenderZoomRgbPending){ 0 };
    if (render_mode == GUEST_RENDER_RENDER_ORIGINAL || cpu == NULL ||
        !xg_render_runtime_stack_address_is_valid(cpu->gpr[29]) ||
        !((cpu->gpr[4] == 0u && cpu->gpr[5] == 0u) ||
          (cpu->gpr[4] == 1u && cpu->gpr[5] == 1u)) ||
        artifact_generation == 0u ||
        !initializer_caller_is_authorized(cpu->gpr[31]))
        return;
    zoom_initializer_pending = (XgRenderZoomInitializerPending){
        .entry_sp = cpu->gpr[29],
        .return_address = cpu->gpr[31],
        .semi_transparent = (uint8_t)cpu->gpr[4],
        .abr = (uint8_t)cpu->gpr[5],
        .artifact_generation = artifact_generation,
        .artifact_authorized = true,
        .valid = true,
    };
}

void xg_field_zoom_observe_initializer_commit(
    CPUState *cpu, uint64_t artifact_generation) {
    if (cpu == NULL || cpu->read_word == NULL ||
        !zoom_initializer_pending.valid || zoom_next_generation == 0u ||
        zoom_initializer_pending.entry_sp < 0x68u ||
        cpu->gpr[29] != zoom_initializer_pending.entry_sp - 0x68u ||
        artifact_generation == 0u ||
        artifact_generation != zoom_initializer_pending.artifact_generation ||
        !xg_render_runtime_stack_address_is_valid(cpu->gpr[29]) ||
        cpu->read_word(cpu->gpr[29] + 0x64u) !=
            zoom_initializer_pending.return_address) {
        zoom_source = (XgRenderZoomSource){ 0 };
        zoom_initializer_pending = (XgRenderZoomInitializerPending){ 0 };
        return;
    }
    materialize_zoom_source(
        zoom_initializer_pending.semi_transparent, zoom_initializer_pending.abr,
        artifact_generation);
    ++zoom_counters.initializer_commit_count;
    if (zoom_source.command == 0x2eu) ++zoom_counters.initializer_2e_count;
    zoom_initializer_pending = (XgRenderZoomInitializerPending){ 0 };
}

bool xg_field_zoom_local_producer_begin(
        CPUState *cpu, GuestRenderRenderMode render_mode,
        uint64_t artifact_generation) {
    if (cpu == NULL || render_mode != GUEST_RENDER_RENDER_NATIVE ||
        !xg_render_runtime_stack_address_is_valid(cpu->gpr[29]) ||
        artifact_generation == 0u)
        return false;
    xg_field_zoom_observe_initializer_begin(
        cpu, render_mode, artifact_generation);
    return zoom_initializer_pending.valid;
}

bool xg_field_zoom_local_producer_writer(CPUState *cpu,
                                         uint32_t writer_index) {
    const uint32_t expected_address = UINT32_C(0x800b129c) +
        (writer_index / 2u) * 0x50u + (writer_index & 1u) * 0x10u;

    return cpu != NULL && zoom_initializer_pending.valid &&
        writer_index < 10u && zoom_initializer_pending.entry_sp >= 0x68u &&
        cpu->gpr[29] == zoom_initializer_pending.entry_sp - 0x68u &&
        (cpu->gpr[18] & UINT32_C(0x1fffffff)) ==
            (expected_address & UINT32_C(0x1fffffff));
}

bool xg_field_zoom_local_producer_commit(
        CPUState *cpu, uint64_t artifact_generation,
        const XgFieldZoomLocalProducerServices *services) {
    xg_field_zoom_observe_initializer_commit(cpu, artifact_generation);
    if (!zoom_source.valid || !zoom_source.authenticated) return false;
    xg_field_zoom_register_resource_watches(
        services != NULL ? services->watch_resource : NULL);
    return true;
}

void xg_field_zoom_observe_rgb_begin(
    CPUState *cpu, GuestRenderRenderMode render_mode,
    uint64_t artifact_generation) {
    uint32_t current_buffer;

    zoom_rgb_pending = (XgRenderZoomRgbPending){ 0 };
    if (render_mode == GUEST_RENDER_RENDER_ORIGINAL || cpu == NULL ||
        cpu->read_word == NULL || artifact_generation == 0u ||
        artifact_generation != zoom_source.artifact_generation ||
        !zoom_source.valid ||
        !zoom_source.authenticated)
        return;
    current_buffer = cpu->read_word(UINT32_C(0x800adb08));
    if (current_buffer >= XG_RENDER_ZOOM_BUFFER_COUNT) return;
    zoom_rgb_pending = (XgRenderZoomRgbPending){
        .generation = zoom_source.generation,
        .intensity = (uint8_t)cpu->gpr[4],
        .buffer_index = (uint8_t)((current_buffer + 1u) & 1u),
        .valid = true,
    };
}

void xg_field_zoom_observe_rgb_commit(CPUState *cpu,
                                      uint64_t artifact_generation) {
    uint32_t current_buffer;
    uint32_t quad;

    if (cpu != NULL && cpu->gpr[6] < XG_RENDER_ZOOM_QUAD_COUNT) return;
    if (cpu == NULL || cpu->read_word == NULL || !zoom_rgb_pending.valid ||
        artifact_generation == 0u ||
        artifact_generation != zoom_source.artifact_generation ||
        !zoom_source.valid || zoom_rgb_pending.generation != zoom_source.generation ||
        cpu->gpr[6] != XG_RENDER_ZOOM_QUAD_COUNT) {
        zoom_source = (XgRenderZoomSource){ 0 };
        zoom_rgb_pending = (XgRenderZoomRgbPending){ 0 };
        return;
    }
    current_buffer = cpu->read_word(UINT32_C(0x800adb08));
    if (current_buffer >= XG_RENDER_ZOOM_BUFFER_COUNT ||
        ((current_buffer + 1u) & 1u) != zoom_rgb_pending.buffer_index) {
        zoom_source = (XgRenderZoomSource){ 0 };
        zoom_rgb_pending = (XgRenderZoomRgbPending){ 0 };
        return;
    }
    for (quad = 0u; quad < XG_RENDER_ZOOM_QUAD_COUNT; ++quad) {
        XgRenderZoomQuadSource *source =
            &zoom_source.quads[zoom_rgb_pending.buffer_index][quad];
        source->red = zoom_rgb_pending.intensity;
        source->green = zoom_rgb_pending.intensity;
        source->blue = zoom_rgb_pending.intensity;
    }
    ++zoom_counters.rgb_update_count;
    zoom_rgb_pending = (XgRenderZoomRgbPending){ 0 };
}

void xg_field_zoom_observe_entry(
    CPUState *cpu, GuestRenderRenderMode render_mode,
    uint64_t artifact_generation) {
    zoom_invocation = (XgRenderZoomInvocation){ 0 };
    if (render_mode == GUEST_RENDER_RENDER_ORIGINAL || cpu == NULL ||
        artifact_generation == 0u ||
        artifact_generation != zoom_source.artifact_generation ||
        !zoom_source.valid ||
        !zoom_source.authenticated ||
        !xg_render_runtime_stack_address_is_valid(cpu->gpr[29]) ||
        !xg_field_zoom_caller_is_authorized(cpu->gpr[31]))
        return;
    zoom_invocation = (XgRenderZoomInvocation){
        .source_generation = zoom_source.generation,
        .entry_sp = cpu->gpr[29],
        .return_address = cpu->gpr[31],
        .valid = true,
    };
    ++zoom_counters.invocation_count;
}

bool xg_field_zoom_reject(
        uint32_t blocker, const XgFieldZoomPipelineServices *services) {
    xg_field_zoom_note_rejection(blocker);
    zoom_source = (XgRenderZoomSource){ 0 };
    zoom_rgb_pending = (XgRenderZoomRgbPending){ 0 };
    zoom_invocation = (XgRenderZoomInvocation){ 0 };
    if (services != NULL && services->proof_active != NULL &&
        services->proof_active()) {
        if (services->reject_policy != NULL) services->reject_policy(blocker);
    } else {
        xg_render_submission_pre_scene_block(blocker, true);
    }
    return false;
}

static bool stage_zoom_records(
        const XgRenderZoomNativeRecord records[XG_RENDER_ZOOM_QUAD_COUNT],
        uint32_t *failure_blocker,
        const XgFieldZoomPipelineServices *services) {
    uint32_t index;

    if (failure_blocker != NULL) *failure_blocker = 0u;
    if (services->proof_active != NULL && services->proof_active()) {
        if (services->stage_active == NULL) return false;
        for (index = 0u; index < XG_RENDER_ZOOM_QUAD_COUNT; ++index) {
            if (!services->stage_active(
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
    if (services->pre_scene_available == NULL ||
        services->stage_pre_scene == NULL ||
        !services->pre_scene_available(XG_RENDER_ZOOM_QUAD_COUNT))
        return false;
    for (index = 0u; index < XG_RENDER_ZOOM_QUAD_COUNT; ++index) {
        const XgRenderPreScenePrimitive staged = {
            .primitive = records[index].primitive,
            .packet_address = records[index].packet_address,
            .source_primitive_index = UINT32_C(0x30000000) |
                (records[index].packet_address & UINT32_C(0x001ffffc)),
            .ot_bucket = XG_RENDER_ZOOM_OT_BUCKET,
            .interpolation_producer_id = XG_RENDER_ZOOM_TEMPLATE_STORE_PC,
            .interpolation_primitive_id = index,
            .payload_word_count = XG_FIELD_CHARACTER_PACKET_WORD_COUNT,
            .interpolation_identity_valid = true,
        };
        if (!services->stage_pre_scene(&staged))
            return false;
    }
    return true;
}

static bool physical_address_equals(uint32_t left, uint32_t right) {
    return (left & UINT32_C(0x1fffffff)) ==
           (right & UINT32_C(0x1fffffff));
}

bool xg_field_zoom_cutover(
        CPUState *cpu, uint32_t continuation,
        const XgFieldZoomPipelineServices *services) {
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
    const bool proof_active = services != NULL &&
        services->proof_active != NULL && services->proof_active();

    if (cpu == NULL || services == NULL || cpu->read_word == NULL ||
        cpu->write_word == NULL)
        return xg_field_zoom_reject(71u, services);
    if (!zoom_source.valid || !zoom_source.authenticated) {
        if (proof_active) return xg_field_zoom_reject(73u, services);
        zoom_invocation = (XgRenderZoomInvocation){ 0 };
        return false;
    }
    if (!zoom_invocation.valid) {
        if (proof_active) return xg_field_zoom_reject(74u, services);
        zoom_source = (XgRenderZoomSource){ 0 };
        return false;
    }
    if (zoom_invocation.source_generation != zoom_source.generation)
        return xg_field_zoom_reject(75u, services);
    if (zoom_invocation.entry_sp < 0x88u ||
        cpu->gpr[29] != zoom_invocation.entry_sp - 0x88u)
        return xg_field_zoom_reject(76u, services);
    if (!xg_render_runtime_stack_address_is_valid(cpu->gpr[29]))
        return xg_field_zoom_reject(77u, services);
    if (!physical_address_equals(cpu->gpr[31], UINT32_C(0x800a6444)))
        return xg_field_zoom_reject(78u, services);
    if (cpu->read_word(cpu->gpr[29] + 0x84u) !=
        zoom_invocation.return_address)
        return xg_field_zoom_reject(79u, services);
    if (!xg_field_zoom_caller_is_authorized(zoom_invocation.return_address))
        return xg_field_zoom_reject(80u, services);
    stack_pointer = cpu->gpr[29];
    scale = cpu->gpr[2];
    buffer_index = cpu->read_word(UINT32_C(0x800adb08));
    context_address = cpu->read_word(UINT32_C(0x800c426c));
    if (buffer_index >= XG_RENDER_ZOOM_BUFFER_COUNT ||
        context_address != UINT32_C(0x800b249c) + buffer_index * 0x80f4u ||
        context_address > UINT32_MAX - 0x80d4u ||
        !xg_render_runtime_word_address_is_valid(context_address + 0x80d4u))
        return xg_field_zoom_reject(53u, services);
    ot_address = context_address + 0x80d4u;
    project = scale != 0x1000u;
    gpu_get_draw_state(&draw);
    xg_render_runtime_capture_shadow_projection(cpu, &projection);
    if (project) {
        XgHost3dMatrix matrix;
        const XgHost3dLongVector scale_vector = {
            (int32_t)scale, (int32_t)scale, (int32_t)scale,
        };
        if (!xg_render_runtime_capture_matrix(
                cpu, stack_pointer + 0x28u, &matrix) ||
            !xg_host_3d_scale_matrix(&matrix, &scale_vector))
            return xg_field_zoom_reject(54u, services);
        memcpy(projection.rotation, matrix.rotation,
               sizeof(projection.rotation));
        memcpy(projection.translation, matrix.translation,
               sizeof(projection.translation));
    }
    memset(records, 0, sizeof(records));
    for (index = 0u; index < XG_RENDER_ZOOM_QUAD_COUNT; ++index) {
        XgRenderZoomQuadSource *source = &zoom_source.quads[buffer_index][index];
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
                return xg_field_zoom_reject(54u, services);
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
        capture.semi_transparent = zoom_source.semi_transparent;
        if (xg_field_character_adapter_build(&capture, &candidate) !=
                XG_FIELD_CHARACTER_ADAPTER_OK ||
            xg_field_character_adapter_build_primitive(
                &candidate, &records[index].primitive) !=
                XG_FIELD_CHARACTER_ADAPTER_OK)
            return xg_field_zoom_reject(55u, services);
        if (project)
            xg_render_primitive_apply_projected_quad_positions(
                &records[index].primitive, output.vertices);
        records[index].packet_address = UINT32_C(0x800b1274) +
            index * 0x50u + buffer_index * 0x28u;
        records[index].draw_mode_address = UINT32_C(0x800b11ac) +
            index * 0x18u + buffer_index * 0x0cu;
        if (!xg_render_runtime_word_address_is_valid(
                records[index].packet_address) ||
            !xg_render_runtime_word_address_is_valid(
                records[index].packet_address + 0x24u) ||
            !xg_render_runtime_word_address_is_valid(
                records[index].draw_mode_address) ||
            !xg_render_runtime_word_address_is_valid(
                records[index].draw_mode_address + 8u))
            return xg_field_zoom_reject(53u, services);
        records[index].packet_tag = UINT32_C(0x09000000);
        records[index].draw_mode_tag = UINT32_C(0x02000000);
    }
    {
        uint32_t stage_blocker = 0u;
        if (!stage_zoom_records(records, &stage_blocker, services))
            return xg_field_zoom_reject(
                stage_blocker != 0u ? stage_blocker : 56u, services);
    }
    psx_store_cycle_barrier();
    cpu->write_word(stack_pointer + 0x4cu, scale);
    psx_store_cycle_barrier();
    cpu->write_word(stack_pointer + 0x50u, scale);
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
            memcpy(zoom_source.quads[buffer_index][index].x,
                   records[index].x, sizeof(records[index].x));
            memcpy(zoom_source.quads[buffer_index][index].y,
                   records[index].y, sizeof(records[index].y));
        }
    }
    zoom_invocation = (XgRenderZoomInvocation){ 0 };
    xg_field_zoom_note_native_invocation(XG_RENDER_ZOOM_QUAD_COUNT);
    cpu->gpr[2] = 0u;
    cpu->pc = continuation;
    return true;
}

bool xg_field_zoom_resolve(
        const GuestRenderNativeStreamMissContext *context,
        GpuRenderSemantic *out_semantic, GuestRenderRenderMode render_mode,
        const XgFieldZoomPipelineServices *services) {
    const uint64_t command_id = context != NULL ? context->command_id : 0u;
    GpuDrawState draw = { 0 };
    XgFieldCharacterCapture capture = { 0 };
    XgFieldCharacterCandidate candidate;
    XgRenderIrNativePrimitive primitive;
    uint32_t buffer;
    uint32_t quad;
    uint32_t component;

    if (out_semantic == NULL || context == NULL || services == NULL ||
        services->authorize_replay == NULL ||
        services->interpolation_scene == NULL || command_id > UINT32_MAX ||
        context->opcode != 0x2eu ||
        context->word_count != XG_FIELD_CHARACTER_PACKET_WORD_COUNT ||
        render_mode != GUEST_RENDER_RENDER_NATIVE)
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
    if (!zoom_source.valid || !zoom_source.authenticated ||
        zoom_source.command != 0x2eu ||
        !services->authorize_replay(&zoom_source, context))
        return false;
    gpu_get_draw_state(&draw);
    for (component = 0u; component < 4u; ++component) {
        static const uint8_t u[4] = { 0u, 64u, 0u, 64u };
        static const uint8_t v[4] = { 0u, 0u, 223u, 223u };
        const XgRenderZoomQuadSource *source =
            &zoom_source.quads[buffer][quad];
        capture.vertices[component] = (XgFieldCharacterCaptureVertex){
            source->x[component], source->y[component],
            u[component], v[component],
        };
    }
    capture.red = zoom_source.quads[buffer][quad].red;
    capture.green = zoom_source.quads[buffer][quad].green;
    capture.blue = zoom_source.quads[buffer][quad].blue;
    capture.tpage = zoom_source.quads[buffer][quad].tpage;
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
    xg_render_semantic_set_interpolation_identity(
        out_semantic, services->interpolation_scene(),
        zoom_source.producer_store_pc, quad);
    if (quad == XG_RENDER_ZOOM_QUAD_COUNT - 1u)
        xg_field_zoom_note_replay_invocation(XG_RENDER_ZOOM_QUAD_COUNT);
    return true;
}
