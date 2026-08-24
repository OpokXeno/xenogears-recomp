#include "xg_render_f4_sources.h"

#include "cpu_state.h"
#include "gpu.h"
#include "xg_render_address_lookup.h"
#include "xg_render_backend.h"
#include "xg_render_ir.h"
#include "xg_render_primitive_utils.h"
#include "xg_render_quad_builder.h"

#include <limits.h>
#include <stddef.h>
#include <string.h>

#define XG_RENDER_F4_SOURCE_CAPACITY 128u

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
    XgRenderAddressLookupSlot lookup[XG_RENDER_LOOKUP_WORD_CAPACITY];
    uint16_t lookup_epoch;
    uint32_t count;
} XgRenderF4SourceState;

static XgRenderF4SourceState sources = {
    .lookup_epoch = 1u,
};

static const GuestRenderNativeSourceWriter f4_28_writers[] = {
    { UINT32_C(0x8003f984), 0u, UINT32_C(0x800714dc) },
    { UINT32_C(0x8002d0d8), 0u, UINT32_C(0x8002ca9c) },
    { UINT32_C(0x80027370), 0u, UINT32_C(0x80027338) },
    { UINT32_C(0x801caf74), 0u, UINT32_C(0x801caef4) },
    { UINT32_C(0x801cb14c), 0u, UINT32_C(0x801cb738) },
};

bool xg_render_f4_sources_writer_is_authorized(
        const GuestRenderNativeStreamMissContext *context) {
    if (context == NULL) return false;
    if (context->opcode != 0x28u) return true;
    if (!context->command_writer_valid) return false;
    for (size_t index = 0u;
         index < sizeof(f4_28_writers) / sizeof(f4_28_writers[0]); ++index) {
        if (context->command_writer.pc == f4_28_writers[index].pc &&
            context->command_writer.function ==
                f4_28_writers[index].function &&
            context->command_writer.return_address ==
                f4_28_writers[index].return_address)
            return true;
    }
    return false;
}

static bool services_are_valid(const XgRenderF4SourceServices *services) {
    return services != NULL && services->lifecycle != NULL &&
        services->lifecycle->begin != NULL &&
        services->lifecycle->matches != NULL &&
        services->lifecycle->matches_replay != NULL &&
        services->lifecycle->replay_container_matches_command != NULL &&
        services->guest_memory.data_range_is_valid != NULL &&
        services->guest_memory.word_address_is_valid != NULL &&
        services->guest_memory.stack_address_is_valid != NULL &&
        services->guest_memory.vector_address_is_valid != NULL &&
        services->guest_memory.capture_projection != NULL &&
        services->interpolation.scene_generation != NULL &&
        services->resources.watch != NULL;
}

static bool physical_address_equals(uint32_t left, uint32_t right) {
    return (left & UINT32_C(0x1fffffff)) ==
        (right & UINT32_C(0x1fffffff));
}

static bool normalized_ranges_overlap(
        uint32_t left_start, uint32_t left_size,
        uint32_t right_start, uint32_t right_size) {
    const uint64_t left_begin = left_start & UINT32_C(0x1fffffff);
    const uint64_t left_end = left_begin + left_size;
    const uint64_t right_begin = right_start & UINT32_C(0x1fffffff);
    const uint64_t right_end = right_begin + right_size;

    return left_size != 0u && right_size != 0u &&
        left_begin < right_end && right_begin < left_end;
}

static XgRenderF4SourceRecord *source_upsert(uint32_t packet_address) {
    const uint32_t source_id =
        (packet_address & UINT32_C(0x1fffffff)) + 4u;
    const uint32_t indexed = xg_render_lookup_find(
        sources.lookup, sources.lookup_epoch, source_id, sources.count);

    if (indexed != UINT32_MAX &&
        sources.records[indexed].source_id == source_id) {
        sources.records[indexed].semantic_ready = false;
        return &sources.records[indexed];
    }
    for (uint32_t index = 0u; index < sources.count; ++index) {
        if (sources.records[index].source_id == source_id) {
            sources.records[index].semantic_ready = false;
            xg_render_lookup_put(
                sources.lookup, sources.lookup_epoch, source_id, index);
            return &sources.records[index];
        }
    }
    if (sources.count == XG_RENDER_F4_SOURCE_CAPACITY) return NULL;
    sources.records[sources.count] = (XgRenderF4SourceRecord){
        .source_id = source_id,
    };
    xg_render_lookup_put(
        sources.lookup, sources.lookup_epoch, source_id, sources.count);
    return &sources.records[sources.count++];
}

static void capture_material(XgRenderF4SourceRecord *record,
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
    xg_render_material_apply_draw_state(
        &record->material, &environment.draw);
}

void xg_render_f4_sources_clear(void) {
    sources.count = 0u;
    xg_render_lookup_reset(sources.lookup, &sources.lookup_epoch);
}

void xg_render_f4_sources_reset(void) {
    xg_render_f4_sources_clear();
}

uint32_t xg_render_f4_sources_count(void) {
    return sources.count;
}

void xg_render_f4_sources_invalidate_overlapping(
        uint32_t address, uint32_t size) {
    if (size == 0u) return;
    for (uint32_t index = 0u; index < sources.count; ++index) {
        XgRenderF4SourceRecord *record = &sources.records[index];

        if (record->source_id >= 4u &&
            normalized_ranges_overlap(
                record->source_id - 4u, 0x18u, address, size)) {
            record->valid = false;
            xg_render_lookup_remove(
                sources.lookup, sources.lookup_epoch,
                record->source_id, index);
        }
    }
}

void xg_render_f4_sources_handle_invalidation(
        const XgRenderInvalidationEvent *event,
        const XgRenderInvalidationServices *services) {
    (void)services;
    if (event->kind == XG_RENDER_INVALIDATION_CODE_WRITE) {
        if (event->mutation.resource_mutation)
            xg_render_f4_sources_invalidate_overlapping(
                event->address, event->size);
        if (event->mutation.semantic_authority_loss)
            xg_render_f4_sources_clear();
    } else if (event->kind == XG_RENDER_INVALIDATION_DISABLE ||
               event->kind == XG_RENDER_INVALIDATION_SCENE_BOUNDARY ||
               event->kind == XG_RENDER_INVALIDATION_LOADER_MISMATCH ||
               event->kind == XG_RENDER_INVALIDATION_RESOURCE_OVERLAP) {
        xg_render_f4_sources_clear();
    } else if (event->kind == XG_RENDER_INVALIDATION_RESET) {
        xg_render_f4_sources_reset();
    }
}

/* This overscanned POLY_F4 is complete immediately before addPrim. */
bool xg_render_f4_sources_capture_battle_fader(
        CPUState *cpu, GuestRenderRenderMode render_mode,
        const XgRenderF4SourceServices *services) {
    XgRenderF4SourceRecord *record;
    XgRenderProducerLifecycle lifecycle;
    const uint32_t packet_address = cpu != NULL ? cpu->gpr[18] : 0u;
    uint32_t command;
    uint32_t ot_base;

    if (!services_are_valid(services) || cpu == NULL ||
        cpu->read_word == NULL || cpu->read_half == NULL ||
        cpu->read_byte == NULL ||
        render_mode != GUEST_RENDER_RENDER_NATIVE ||
        !services->lifecycle->begin(UINT32_C(0x800b3878), &lifecycle) ||
        !services->guest_memory.data_range_is_valid(
            packet_address, 0x18u, 4u, false))
        return false;
    command = cpu->read_word(packet_address + 4u);
    if ((command >> 24u) != 0x2au) return false;
    ot_base = cpu->read_word(UINT32_C(0x8005956c));
    if (ot_base > UINT32_MAX - 8u ||
        !services->guest_memory.word_address_is_valid(ot_base + 8u))
        return false;
    record = source_upsert(packet_address);
    if (record == NULL) return false;
    for (uint32_t vertex = 0u;
         vertex < XG_RENDER_QUAD_VERTEX_COUNT; ++vertex) {
        const uint32_t xy_address = packet_address + 8u + vertex * 4u;

        record->vertices[vertex] = (XgRenderQuadSourceVertex){
            .x = (int16_t)cpu->read_half(xy_address),
            .y = (int16_t)cpu->read_half(xy_address + 2u),
            .red = cpu->read_byte(packet_address + 4u),
            .green = cpu->read_byte(packet_address + 5u),
            .blue = cpu->read_byte(packet_address + 6u),
        };
    }
    capture_material(record, true);
    record->lifecycle = lifecycle;
    record->opcode = 0x2au;
    record->ot_address = ot_base + 8u;
    record->valid = true;
    services->resources.watch(packet_address, 0x18u);
    return true;
}

bool xg_render_f4_sources_capture_fixed_2a(
        CPUState *cpu, GuestRenderRenderMode render_mode,
        const XgRenderF4SourceServices *services) {
    uint32_t global;
    uint32_t packet_base;
    XgRenderProducerLifecycle lifecycle;

    if (!services_are_valid(services) || cpu == NULL ||
        cpu->read_word == NULL ||
        render_mode != GUEST_RENDER_RENDER_NATIVE ||
        !services->lifecycle->begin(UINT32_C(0x801c6f70), &lifecycle))
        return false;
    global = cpu->read_word(UINT32_C(0x800625a0));
    if (!services->guest_memory.data_range_is_valid(
            global, 0x34cu, 4u, false))
        return false;
    packet_base = cpu->read_word(global + 0x348u);
    if (packet_base > UINT32_MAX - 0xc8u ||
        !services->guest_memory.data_range_is_valid(
            packet_base + 0x98u, 0x30u, 4u, false))
        return false;
    for (uint32_t buffer = 0u; buffer < 2u; ++buffer) {
        XgRenderF4SourceRecord *record =
            source_upsert(packet_base + 0x98u + buffer * 0x18u);
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
        services->resources.watch(
            packet_base + 0x98u + buffer * 0x18u, 0x18u);
    }
    return true;
}

bool xg_render_f4_sources_capture_projected_2a(
        CPUState *cpu, GuestRenderRenderMode render_mode,
        const XgRenderF4SourceServices *services) {
    XgHost3dProject4Input input = {0};
    XgHost3dRotTransPers4Output output;
    XgRenderF4SourceRecord *record;
    uint32_t global;
    uint32_t first_xy;
    uint8_t color;
    XgRenderProducerLifecycle lifecycle;

    if (!services_are_valid(services) || cpu == NULL ||
        cpu->read_word == NULL || cpu->read_half == NULL ||
        cpu->read_byte == NULL ||
        render_mode != GUEST_RENDER_RENDER_NATIVE ||
        !services->lifecycle->begin(UINT32_C(0x801cf550), &lifecycle) ||
        !services->guest_memory.stack_address_is_valid(cpu->gpr[29]))
        return false;
    first_xy = cpu->read_word(cpu->gpr[29] + 0x10u);
    if (first_xy < 8u ||
        !services->guest_memory.word_address_is_valid(first_xy))
        return false;
    global = cpu->read_word(UINT32_C(0x800625a0));
    if (!services->guest_memory.data_range_is_valid(
            global, 0x4d5u, 1u, false))
        return false;
    color = cpu->read_byte(global + 0x4d4u);
    services->guest_memory.capture_projection(cpu, &input.projection);
    for (uint32_t vertex = 0u; vertex < 4u; ++vertex) {
        const uint32_t address = cpu->gpr[4u + vertex];

        if (!services->guest_memory.vector_address_is_valid(address))
            return false;
        input.vertices[vertex] = (XgHost3dVector){
            (int16_t)cpu->read_half(address),
            (int16_t)cpu->read_half(address + 2u),
            (int16_t)cpu->read_half(address + 4u),
            cpu->read_half(address + 6u),
        };
    }
    if (!xg_host_3d_rot_trans_pers4(&input, &output)) return false;
    record = source_upsert(first_xy - 8u);
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
    services->resources.watch(first_xy - 8u, 0x18u);
    return true;
}

void xg_render_f4_sources_observe_2a_ot(
        CPUState *cpu, GuestRenderRenderMode render_mode,
        const XgRenderF4SourceServices *services) {
    uint32_t source_id;

    if (!services_are_valid(services) || cpu == NULL ||
        render_mode != GUEST_RENDER_RENDER_NATIVE)
        return;
    source_id = (cpu->gpr[5] & UINT32_C(0x1fffffff)) + 4u;
    for (uint32_t index = 0u; index < sources.count; ++index) {
        XgRenderF4SourceRecord *record = &sources.records[index];

        if (!record->valid ||
            !services->lifecycle->matches(&record->lifecycle) ||
            record->opcode != 0x2au || record->source_id != source_id)
            continue;
        record->ot_address = cpu->gpr[4];
        capture_material(record, true);
        return;
    }
}

bool xg_render_f4_sources_capture_field_f4(
        CPUState *cpu, GuestRenderRenderMode render_mode,
        const XgRenderF4SourceServices *services) {
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

    if (!services_are_valid(services) || cpu == NULL ||
        cpu->read_word == NULL || cpu->read_half == NULL ||
        cpu->read_byte == NULL ||
        render_mode != GUEST_RENDER_RENDER_NATIVE ||
        !services->lifecycle->begin(UINT32_C(0x8001e874), &lifecycle))
        return false;
    global = cpu->read_word(UINT32_C(0x800625a0));
    if (!services->guest_memory.data_range_is_valid(
            global, 0x450u, 4u, false))
        return false;
    packet_base = cpu->read_word(global + 0x44cu);
    state_address = cpu->read_word(global + 0x33cu);
    ot_base = cpu->read_word(global + 0x1d4u);
    if (!services->guest_memory.data_range_is_valid(
            packet_base, 0x7b9u, 1u, false) ||
        !services->guest_memory.data_range_is_valid(
            state_address + 0x52u, 1u, 1u, false) ||
        ot_base > UINT32_MAX - 0x80u)
        return false;
    buffer = cpu->read_byte(packet_base + 0x7b8u);
    if (buffer > 1u || packet_base > UINT32_MAX - buffer * 0x18u)
        return false;
    record = source_upsert(packet_base + buffer * 0x18u);
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
    xg_render_material_apply_draw_state(
        &record->material, &environment.draw);
    record->ot_address = ot_base + 0x80u;
    for (uint32_t vertex = 0u; vertex < 4u; ++vertex) {
        static const int16_t x[4] = {0x20, 0, 0x20, 0};
        static const int16_t y[4] = {0x61, 0x61, 0x68, 0x68};

        record->vertices[vertex] = (XgRenderQuadSourceVertex){
            vertex == 1u || vertex == 3u ? right : x[vertex], y[vertex],
            0u, 0u, red, 0xa0u, 0u,
        };
    }
    record->opcode = 0x28u;
    record->valid = true;
    record->lifecycle = lifecycle;
    services->resources.watch(packet_base + buffer * 0x18u, 0x18u);
    return true;
}

bool xg_render_f4_sources_resolve(
        const GuestRenderNativeStreamMissContext *context,
        GpuRenderSemantic *out_semantic, GuestRenderRenderMode render_mode,
        const XgRenderF4SourceServices *services) {
    const uint64_t command_id = context != NULL ? context->command_id : 0u;
    XgRenderF4SourceRecord *record = NULL;
    const uint32_t indexed = command_id <= UINT32_MAX
        ? xg_render_lookup_find(
              sources.lookup, sources.lookup_epoch,
              (uint32_t)command_id, sources.count)
        : UINT32_MAX;

    if (!services_are_valid(services) || out_semantic == NULL ||
        context == NULL || command_id > UINT32_MAX ||
        render_mode != GUEST_RENDER_RENDER_NATIVE ||
        !xg_render_f4_sources_writer_is_authorized(context))
        return false;
    {
        uint32_t lookup_key;
        if (xg_render_lookup_key((uint32_t)command_id, &lookup_key)) {
            if (indexed == UINT32_MAX) return false;
            if (sources.records[indexed].valid &&
                physical_address_equals(
                    sources.records[indexed].source_id,
                    (uint32_t)command_id))
                record = &sources.records[indexed];
        } else {
            for (uint32_t index = 0u; index < sources.count; ++index) {
                if (sources.records[index].valid &&
                    physical_address_equals(
                        sources.records[index].source_id,
                        (uint32_t)command_id)) {
                    record = &sources.records[index];
                    xg_render_lookup_put(
                        sources.lookup, sources.lookup_epoch,
                        (uint32_t)command_id, index);
                    break;
                }
            }
        }
    }
    if (record == NULL || record->opcode != context->opcode ||
        !services->lifecycle->replay_container_matches_command(context) ||
        !services->lifecycle->matches_replay(&record->lifecycle, context) ||
        (record->opcode == 0x2au && record->ot_address == 0u))
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
