#include "xg_render_field_polyline.h"

#include "gpu.h"
#include "xg_field_render_services.h"
#include "xg_render_primitive_utils.h"

#include <limits.h>
#include <stddef.h>
#include <string.h>

enum {
    CAPACITY = 64u,
    PRODUCER_ID = UINT32_C(0x801cfb48),
    RED_FIRST_WRITER_OFFSET = 0x17cu,
    GREEN_FIRST_WRITER_OFFSET = 0x248u,
    CORE_OFFSET = 0x2c0u,
    FINISH_OFFSET = 0x3f4u,
};

typedef struct XgRenderFieldPolylineRecord {
    GpuRenderSemantic semantic;
    uint32_t packet_address;
    uint32_t command_word;
    uint32_t command_writer_pc;
    uint32_t xy[3];
    uint32_t interpolation_primitive_id;
} XgRenderFieldPolylineRecord;

typedef struct XgRenderFieldPolylineState {
    XgRenderFieldPolylineRecord records[CAPACITY];
    PsxXgRenderFieldPolylineSnapshot snapshot;
    uint32_t producer_entry;
    uint32_t count;
} XgRenderFieldPolylineState;

static XgRenderFieldPolylineState pending;
static XgRenderFieldPolylineRecord templates[CAPACITY];
static uint32_t template_count;

static const uint32_t entry_instructions[] = {
    UINT32_C(0x3c038006), UINT32_C(0x8c6325a0),
    UINT32_C(0x27bdffb8), UINT32_C(0xafbf0040),
    UINT32_C(0xafb5003c), UINT32_C(0xafb40038),
    UINT32_C(0xafb30034), UINT32_C(0xafb20030),
    UINT32_C(0xafb1002c), UINT32_C(0xafb00028),
    UINT32_C(0x906204d8),
};

static const uint32_t core_instructions[] = {
    UINT32_C(0xa0400086), UINT32_C(0x26040100),
    UINT32_C(0x3c038006), UINT32_C(0x8c6325a0),
    UINT32_C(0x26050108), UINT32_C(0x8c620308),
    UINT32_C(0x26060118), UINT32_C(0x00023840),
    UINT32_C(0x00e23821), UINT32_C(0x000738c0),
    UINT32_C(0x24e70050), UINT32_C(0x02073821),
    UINT32_C(0x24e2000c), UINT32_C(0xafa20010),
    UINT32_C(0x8c630308), UINT32_C(0x24e70008),
    UINT32_C(0xafb50018), UINT32_C(0xafb4001c),
    UINT32_C(0x00031040), UINT32_C(0x00431021),
    UINT32_C(0x000210c0), UINT32_C(0x00501021),
    UINT32_C(0x24420060), UINT32_C(0x0c01299f),
    UINT32_C(0xafa20014),
};

static const uint32_t finish_instructions[] = {
    UINT32_C(0x8fbf0040), UINT32_C(0x8fb5003c),
    UINT32_C(0x8fb40038), UINT32_C(0x8fb30034),
    UINT32_C(0x8fb20030), UINT32_C(0x8fb1002c),
    UINT32_C(0x8fb00028), UINT32_C(0x27bd0048),
    UINT32_C(0x03e00008), UINT32_C(0x00000000),
};

static bool physical_address_equals(uint32_t left, uint32_t right) {
    return (left & UINT32_C(0x1fffffff)) ==
        (right & UINT32_C(0x1fffffff));
}

static bool instruction_window_matches(
        CPUState *cpu, uint32_t start,
        const uint32_t *instructions, uint32_t instruction_count) {
    if (cpu == NULL || cpu->read_word == NULL || instructions == NULL ||
        instruction_count == 0u)
        return false;
    for (uint32_t index = 0u; index < instruction_count; ++index)
        if (cpu->read_word(start + index * 4u) != instructions[index])
            return false;
    return true;
}

static bool producer_contract_matches(CPUState *cpu, uint32_t entry) {
    if (entry > UINT32_MAX - FINISH_OFFSET - sizeof(finish_instructions))
        return false;
    return cpu != NULL && cpu->read_word != NULL &&
        cpu->read_word(entry + RED_FIRST_WRITER_OFFSET) ==
            UINT32_C(0xa0400056) &&
        cpu->read_word(entry + GREEN_FIRST_WRITER_OFFSET) ==
            UINT32_C(0xa0400056) &&
        instruction_window_matches(
            cpu, entry, entry_instructions,
            sizeof(entry_instructions) / sizeof(entry_instructions[0])) &&
        instruction_window_matches(
            cpu, entry + CORE_OFFSET, core_instructions,
            sizeof(core_instructions) / sizeof(core_instructions[0])) &&
        instruction_window_matches(
            cpu, entry + FINISH_OFFSET, finish_instructions,
            sizeof(finish_instructions) / sizeof(finish_instructions[0]));
}

void xg_render_field_polyline_clear_pending(void) {
    PsxXgRenderFieldPolylineSnapshot snapshot = pending.snapshot;

    if (pending.count == 0u && !snapshot.pending) return;
    snapshot.pending = false;
    pending = (XgRenderFieldPolylineState){ .snapshot = snapshot };
}

void xg_render_field_polyline_clear_templates(void) {
    template_count = 0u;
}

void xg_render_field_polyline_block(uint32_t blocker) {
    xg_render_field_polyline_clear_pending();
    pending.snapshot.blocked = true;
    if (pending.snapshot.blocker == 0u) pending.snapshot.blocker = blocker;
}

static XgHost3dVector read_vector(CPUState *cpu, uint32_t address) {
    const uint32_t xy = cpu->read_word(address);
    const uint32_t zp = cpu->read_word(address + 4u);

    return (XgHost3dVector){
        xg_render_runtime_low_s16(xy),
        xg_render_runtime_low_s16(xy >> 16u),
        xg_render_runtime_low_s16(zp),
        (uint16_t)(zp >> 16u),
    };
}

static void apply_draw_state(
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

static bool add_record(
        CPUState *cpu, uint32_t object, uint32_t packet,
        const uint32_t vector_offsets[3], uint8_t red, uint8_t green,
        const XgHost3dProjection *projection, const GpuDrawState *draw,
        uint32_t interpolation_primitive_id,
        const XgRenderFieldPolylineServices *services) {
    XgHost3dProject4Input input = {0};
    XgHost3dRotTransPers4Output output;
    XgRenderFieldPolylineRecord *record;

    if (pending.count >= CAPACITY || services == NULL ||
        services->guest_data_range_is_valid == NULL)
        return false;
    for (uint32_t vertex = 0u; vertex < 3u; ++vertex) {
        const uint32_t address = object + vector_offsets[vertex];

        if (!services->guest_data_range_is_valid(address, 8u, 2u, false))
            return false;
        input.vertices[vertex] = read_vector(cpu, address);
    }
    input.vertices[3] = input.vertices[2];
    input.projection = *projection;
    if (!xg_host_3d_rot_trans_pers4(&input, &output)) return false;

    record = &pending.records[pending.count++];
    record->packet_address = packet;
    record->interpolation_primitive_id = interpolation_primitive_id;
    record->command_word = UINT32_C(0x48000000) |
        (uint32_t)red | ((uint32_t)green << 8u);
    record->command_writer_pc = pending.producer_entry +
        ((interpolation_primitive_id & 1u) != 0u
             ? CORE_OFFSET
             : (red != 0u ? RED_FIRST_WRITER_OFFSET :
                             GREEN_FIRST_WRITER_OFFSET));
    record->semantic.topology = GPU_RENDER_SEMANTIC_LINES;
    record->semantic.line_count = 2u;
    apply_draw_state(&record->semantic.material, draw);
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
            .native_view_position = output.vertices[vertex].native_view_position,
            .projective_view_x = output.vertices[vertex].projective_view_x,
            .projective_view_y = output.vertices[vertex].projective_view_y,
            .projective_view_z = output.vertices[vertex].projective_view_z,
            .projective_offset_x =
                output.vertices[vertex].projective_offset_x_16_16,
            .projective_offset_y =
                output.vertices[vertex].projective_offset_y_16_16,
            .projective_native_offset_x =
                output.vertices[vertex].projective_native_offset_x_16_16,
            .projective_native_offset_y =
                output.vertices[vertex].projective_native_offset_y_16_16,
            .projective_distance = output.vertices[vertex].projective_distance,
            .projective_position = output.vertices[vertex].projective_position,
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

static bool begin(
        CPUState *cpu, GuestRenderRenderMode render_mode,
        const XgRenderFieldPolylineServices *services) {
    static const uint32_t first_offsets[3] = {0x100u, 0x108u, 0x118u};
    static const uint32_t second_offsets[3] = {0x120u, 0x130u, 0x138u};
    XgHost3dProjection projection;
    GpuDrawState draw = {0};
    uint32_t global;
    uint32_t field;
    uint32_t selected_index;
    uint32_t buffer;
    uint8_t selected_group;
    uint8_t mode;

    ++pending.snapshot.begin_count;
    if (render_mode != GUEST_RENDER_RENDER_SHADOW &&
        render_mode != GUEST_RENDER_RENDER_NATIVE)
        return false;
    if (pending.snapshot.blocked) return false;
    if (pending.snapshot.pending) {
        xg_render_field_polyline_block(1u);
        return false;
    }
    if (cpu == NULL || cpu->read_word == NULL || cpu->read_half == NULL ||
        cpu->read_byte == NULL || services == NULL ||
        services->guest_data_range_is_valid == NULL)
        return false;
    global = cpu->read_word(UINT32_C(0x800625a0));
    if (!services->guest_data_range_is_valid(global, 0x4d9u, 4u, false)) {
        xg_render_field_polyline_block(2u);
        return false;
    }
    mode = cpu->read_byte(global + 0x4d8u);
    if (mode == 0u) return false;
    field = cpu->read_word(global + 0x32cu);
    if (!services->guest_data_range_is_valid(field, 0x4fe6u, 4u, false)) {
        xg_render_field_polyline_block(3u);
        return false;
    }
    selected_index = cpu->read_word(
        UINT32_C(0x801e981c) + cpu->read_word(field + 0x4f7cu) * 4u);
    if (selected_index >= 32u) {
        xg_render_field_polyline_block(4u);
        return false;
    }
    selected_group = cpu->read_byte(field + 0x4faeu + selected_index);
    buffer = cpu->read_word(global + 0x308u);
    if (buffer > 1u) {
        xg_render_field_polyline_block(5u);
        return false;
    }
    xg_render_runtime_capture_shadow_projection(cpu, &projection);
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
        if (!services->guest_data_range_is_valid(object, 0x140u, 4u, false) ||
            !add_record(
                cpu, object, object + 0x50u + buffer * 0x18u,
                first_offsets, red, green, &projection, &draw,
                index * 2u, services) ||
            !add_record(
                cpu, object, object + 0x80u + buffer * 0x18u,
                second_offsets, red, green, &projection, &draw,
                index * 2u + 1u, services)) {
            xg_render_field_polyline_block(6u);
            return false;
        }
    }
    ++pending.snapshot.invocation_count;
    pending.snapshot.pending = true;
    return true;
}

static void finish(
        CPUState *cpu, GuestRenderRenderMode render_mode,
        const XgRenderFieldPolylineServices *services) {
    bool all_match = true;

    if (!pending.snapshot.pending) return;
    if (cpu == NULL || cpu->read_word == NULL) {
        xg_render_field_polyline_block(7u);
        return;
    }
    pending.snapshot.primitive_count += pending.count;
    for (uint32_t index = 0u; index < pending.count; ++index) {
        const XgRenderFieldPolylineRecord *record = &pending.records[index];
        bool matches = (cpu->read_word(record->packet_address) >> 24u) == 5u &&
            cpu->read_word(record->packet_address + 4u) ==
                record->command_word &&
            (cpu->read_word(record->packet_address + 0x14u) &
             UINT32_C(0xf000f000)) == UINT32_C(0x50005000);

        for (uint32_t vertex = 0u; vertex < 3u; ++vertex)
            matches &= cpu->read_word(
                record->packet_address + 8u + vertex * 4u) ==
                record->xy[vertex];
        if (matches) {
            ++pending.snapshot.match_count;
        } else {
            all_match = false;
            if (pending.snapshot.mismatch_count == 0u)
                pending.snapshot.first_mismatch_packet = record->packet_address;
            ++pending.snapshot.mismatch_count;
        }
    }
    if (render_mode == GUEST_RENDER_RENDER_NATIVE) {
        if (!all_match) {
            xg_render_field_polyline_block(8u);
            return;
        }
        if (services == NULL || services->stage_semantic == NULL ||
            services->register_replay_command == NULL) {
            xg_render_field_polyline_block(10u);
            return;
        }
        for (uint32_t index = 0u; index < pending.count; ++index) {
            const XgRenderFieldPolylineRecord *record = &pending.records[index];

            if (!services->stage_semantic(
                    &record->semantic, record->packet_address,
                    UINT32_C(0x48000000) |
                        (record->packet_address & UINT32_C(0x001ffffc)),
                    PRODUCER_ID, record->interpolation_primitive_id)) {
                xg_render_field_polyline_block(10u);
                return;
            }
        }
        memcpy(templates, pending.records,
               pending.count * sizeof(pending.records[0]));
        template_count = pending.count;
        for (uint32_t index = 0u; index < pending.count; ++index)
            services->register_replay_command(
                (pending.records[index].packet_address &
                 UINT32_C(0x1fffffff)) + 4u);
        ++pending.snapshot.native_cutover_count;
        pending.snapshot.native_primitive_count += pending.count;
    }
    xg_render_field_polyline_clear_pending();
}

XgRenderFieldPolylineObservation xg_render_field_polyline_observe(
        CPUState *cpu, uint32_t pc, uint32_t instruction_word,
        GuestRenderRenderMode render_mode,
        const XgRenderFieldPolylineServices *services) {
    if (instruction_word == entry_instructions[0] &&
        producer_contract_matches(cpu, pc)) {
        pending.producer_entry = pc;
        (void)begin(cpu, render_mode, services);
        return XG_RENDER_FIELD_POLYLINE_OBSERVATION_BEGIN;
    }
    if (instruction_word == finish_instructions[0] && pc >= FINISH_OFFSET &&
        producer_contract_matches(cpu, pc - FINISH_OFFSET)) {
        finish(cpu, render_mode, services);
        return XG_RENDER_FIELD_POLYLINE_OBSERVATION_FINISH;
    }
    return XG_RENDER_FIELD_POLYLINE_OBSERVATION_NONE;
}

bool xg_render_field_polyline_resolve(
        const GuestRenderNativeStreamMissContext *context,
        GpuRenderSemantic *out_semantic, GuestRenderRenderMode render_mode,
        const XgRenderFieldPolylineServices *services) {
    if (context == NULL || out_semantic == NULL ||
        context->command_id > UINT32_MAX || context->opcode != 0x48u ||
        context->word_count != 5u ||
        render_mode != GUEST_RENDER_RENDER_NATIVE ||
        !context->command_writer_valid || services == NULL ||
        services->interpolation_scene == NULL ||
        services->replay_container_matches_command == NULL ||
        !services->replay_container_matches_command(context))
        return false;
    for (uint32_t index = 0u; index < template_count; ++index) {
        const XgRenderFieldPolylineRecord *record = &templates[index];

        if (!physical_address_equals(
                record->packet_address + 4u,
                (uint32_t)context->command_id) ||
            !physical_address_equals(
                context->command_writer.pc, record->command_writer_pc))
            continue;
        *out_semantic = record->semantic;
        xg_render_semantic_set_interpolation_identity(
            out_semantic, services->interpolation_scene(), PRODUCER_ID,
            record->interpolation_primitive_id);
        return true;
    }
    return false;
}

void xg_render_field_polyline_scene_boundary(void) {
    if (pending.snapshot.pending)
        xg_render_field_polyline_block(9u);
    else
        xg_render_field_polyline_clear_pending();
}

void xg_render_field_polyline_invalidate_authority(void) {
    if (pending.snapshot.pending)
        xg_render_field_polyline_block(11u);
    else
        xg_render_field_polyline_clear_pending();
}

void xg_render_field_polyline_reset(void) {
    pending = (XgRenderFieldPolylineState){0};
    template_count = 0u;
}

void xg_render_field_polyline_handle_invalidation(
        const XgRenderInvalidationEvent *event,
        const XgRenderInvalidationServices *services) {
    (void)services;
    if (event->kind == XG_RENDER_INVALIDATION_DISABLE) {
        xg_render_field_polyline_clear_pending();
        xg_render_field_polyline_clear_templates();
    } else if (event->kind == XG_RENDER_INVALIDATION_SCENE_BOUNDARY) {
        xg_render_field_polyline_scene_boundary();
    } else if (event->kind == XG_RENDER_INVALIDATION_CODE_WRITE &&
               event->mutation.semantic_authority_loss) {
        xg_render_field_polyline_invalidate_authority();
    } else if (event->kind == XG_RENDER_INVALIDATION_RESET) {
        xg_render_field_polyline_reset();
    }
}

void xg_render_field_polyline_snapshot(
        PsxXgRenderFieldPolylineSnapshot *out_snapshot) {
    if (out_snapshot != NULL) *out_snapshot = pending.snapshot;
}
