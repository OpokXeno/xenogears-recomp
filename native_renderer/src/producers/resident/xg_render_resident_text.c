#include "xg_render_resident_text.h"

#include "cpu_state.h"
#include "gpu.h"
#include "xg_render_resident_capture.h"
#include "xg_render_primitive_utils.h"
#include "xg_render_submission.h"
#include "xg_render_resource_repository.h"
#include "xg_render_route_descriptor.h"

#include <stddef.h>
#include <string.h>

enum {
    RESIDENT_FONT_SIZE = 6594u,
    RESIDENT_TEXT_CONTEXT_SIZE = 0x90u,
};

typedef struct XgRenderResidentTextRoute {
    uint32_t action;
    uint32_t pc;
    uint32_t instruction_word;
    uint32_t owner_entry;
    uint32_t counter_index;
} XgRenderResidentTextRoute;

typedef struct XgRenderResidentTextState {
    PsxXgRenderResidentTextSnapshot snapshot;
    uint8_t font_bytes[RESIDENT_FONT_SIZE];
} XgRenderResidentTextState;

typedef struct XgRenderResidentTextCodeRange {
    uint32_t address;
    uint32_t size;
} XgRenderResidentTextCodeRange;

static const XgRenderResidentTextRoute routes[] = {
    { XG_CUTOVER_RESIDENT_TEXT_CONTEXT_INITIALIZE,
      UINT32_C(0x80032f54), UINT32_C(0x27bdffc8),
      UINT32_C(0x80032f54), 0u },
    { XG_CUTOVER_RESIDENT_TEXT_FONT_FREE,
      UINT32_C(0x800334d8), UINT32_C(0x3c048006),
      UINT32_C(0x800334d8), 1u },
    { XG_CUTOVER_RESIDENT_TEXT_FONT_INITIALIZE,
      UINT32_C(0x80033558), UINT32_C(0x27bdffe8),
      UINT32_C(0x80033558), 2u },
    { XG_CUTOVER_RESIDENT_TEXT_ADVANCE_BEGIN,
      UINT32_C(0x80033df0), UINT32_C(0x27bdffb8),
      UINT32_C(0x80033df0), 3u },
    { XG_CUTOVER_RESIDENT_TEXT_ROW_RECYCLE,
      UINT32_C(0x80033e9c), UINT32_C(0xa4400058),
      UINT32_C(0x80033df0), 4u },
    { XG_CUTOVER_RESIDENT_TEXT_STREAM_DESTROY,
      UINT32_C(0x800346d4), UINT32_C(0x27bdffe8),
      UINT32_C(0x800346d4), 5u },
    { XG_CUTOVER_RESIDENT_TEXT_ROW_COLORS,
      UINT32_C(0x80034800), UINT32_C(0x27bdfff8),
      UINT32_C(0x80034800), 6u },
    { XG_CUTOVER_RESIDENT_TEXT_ROW_SELECT,
      UINT32_C(0x80034874), UINT32_C(0x03e00008),
      UINT32_C(0x80034874), 7u },
    { XG_CUTOVER_RESIDENT_TEXT_ROW_SELECT_CLEAR,
      UINT32_C(0x8003487c), UINT32_C(0x340200ff),
      UINT32_C(0x8003487c), 8u },
    { XG_CUTOVER_RESIDENT_TEXT_STRING_ENTRY_RENDER,
      UINT32_C(0x80034888), UINT32_C(0x27bdff80),
      UINT32_C(0x80034888), 9u },
    { XG_CUTOVER_RESIDENT_TEXT_PAGE_RESET,
      UINT32_C(0x800349a8), UINT32_C(0x8e030028),
      UINT32_C(0x80034888), 10u },
    { XG_CUTOVER_RESIDENT_TEXT_FRAME_CHECKPOINT,
      UINT32_C(0x80034a3c), UINT32_C(0x86130016),
      UINT32_C(0x80034888), 11u },
    { XG_CUTOVER_RESIDENT_TEXT_STRING_ENTRY_RETURN,
      UINT32_C(0x80034ea4), UINT32_C(0x03e00008),
      UINT32_C(0x80034888), 12u },
    { XG_CUTOVER_RESIDENT_TEXT_GLYPH_RASTERIZE,
      UINT32_C(0x80034ffc), UINT32_C(0x00804021),
      UINT32_C(0x80034ffc), 13u },
};

static const XgRenderResidentTextCodeRange code_ranges[] = {
    { UINT32_C(0x80032f54), 0x4d8u },
    { UINT32_C(0x800334d8), 0x40u },
    { UINT32_C(0x80033558), 0x9cu },
    { UINT32_C(0x80033df0), 0x7f0u },
    { UINT32_C(0x800346d4), 0x40u },
    { UINT32_C(0x80034800), 0x74u },
    { UINT32_C(0x80034874), 0x8u },
    { UINT32_C(0x8003487c), 0xcu },
    { UINT32_C(0x80034888), 0x624u },
    { UINT32_C(0x80034ffc), 0x6a0u },
    { UINT32_C(0x800370dc), 0x1f0u },
};

static const uint16_t canonical_font_header[] = {
    0x013au, 0x000eu, 0x00feu, 0x1474u,
    0x0051u, 0x0000u, 0x0010u,
};

static XgRenderResidentTextState resident_text;
static XgRenderResidentResourceTemplate debug_font_pending;
static uint32_t debug_font_entry_sp;
static uint32_t debug_font_return_address;
static bool debug_font_ready;

static bool debug_font_authorize(void *context, uint32_t pc, uint32_t instruction) {
    const XgRenderResidentTextServices *services = context;
    return (pc & UINT32_C(0x1fffffff)) == 0x370dcu && instruction == 0x3c068006u &&
        services->native_text_authorizes_pc(UINT32_C(0x800370dc));
}

static bool debug_font_range(void *context, uint32_t address, uint32_t size,
                             uint32_t alignment) {
    const XgRenderResidentTextServices *services = context;
    return services->guest_data_range_is_valid(address, size, alignment, false);
}

static bool debug_font_draw_state(void *context, XgRenderIrMaterialState *material) {
    GpuDrawState draw = {0};
    (void)context;
    *material = (XgRenderIrMaterialState){0};
    gpu_get_draw_state(&draw);
    xg_render_material_apply_draw_state(material, &draw);
    return true;
}

static bool debug_font_capture(void *context, uint32_t base, uint32_t parity,
        const XgRenderResidentResourceTemplate *records, uint32_t count) {
    (void)context; (void)base; (void)parity;
    if (count != 1u || records == NULL) return false;
    debug_font_pending = records[0];
    debug_font_ready = true;
    return true;
}

static bool debug_font_observe(CPUState *cpu, uint32_t pc, uint32_t instruction,
        GuestRenderRenderMode mode, const XgRenderResidentTextServices *services) {
    if (cpu == NULL || mode != GUEST_RENDER_RENDER_NATIVE ||
        !xg_render_submission_native_work_mode() || services == NULL ||
        services->native_text_authorizes_pc == NULL ||
        services->guest_data_range_is_valid == NULL) {
        debug_font_ready = false;
        return false;
    }
    if ((pc & UINT32_C(0x1fffffff)) == 0x370dcu && instruction == 0x3c068006u) {
        const XgRenderResidentResourceTemplateServices capture = {
            .context = (void *)services,
            .authorize = debug_font_authorize,
            .source_range_valid = debug_font_range,
            .capture_draw_state = debug_font_draw_state,
            .publish_templates = debug_font_capture,
        };
        debug_font_ready = false;
        debug_font_entry_sp = cpu->gpr[29];
        debug_font_return_address = cpu->gpr[31];
        return xg_render_resident_capture_font_character(cpu, pc, instruction, &capture) ==
            XG_RENDER_RESIDENT_CAPTURE_OK;
    }
    if ((pc & UINT32_C(0x1fffffff)) == 0x372c4u && instruction == 0x03e00008u) {
        const bool valid = debug_font_ready && cpu->gpr[29] == debug_font_entry_sp &&
            cpu->gpr[31] == debug_font_return_address &&
            services->native_text_authorizes_pc(UINT32_C(0x800370dc));
        debug_font_ready = false;
        if (valid) {
            const XgRenderPreScenePrimitive record = {
                .primitive = debug_font_pending.primitive,
                .packet_address = debug_font_pending.destination_address,
                .source_primitive_index = debug_font_pending.destination_address,
                .payload_word_count = 3u,
            };
            return xg_render_submission_pre_scene_stage(&record);
        }
    }
    return false;
}

static bool physical_address_equals(uint32_t left, uint32_t right) {
    return (left & UINT32_C(0x1fffffff)) ==
        (right & UINT32_C(0x1fffffff));
}

static bool ranges_overlap(uint32_t left_address, uint32_t left_size,
                           uint32_t right_address, uint32_t right_size) {
    const uint64_t left = left_address & UINT32_C(0x1fffffff);
    const uint64_t right = right_address & UINT32_C(0x1fffffff);

    return left_size != 0u && right_size != 0u &&
        left < right + right_size && right < left + left_size;
}

static const XgRenderResidentTextRoute *route_lookup(
        uint32_t action, uint32_t pc, uint32_t instruction_word) {
    for (uint32_t index = 0u;
         index < sizeof(routes) / sizeof(routes[0]); ++index) {
        const XgRenderResidentTextRoute *route = &routes[index];

        if (route->action == action &&
            physical_address_equals(route->pc, pc) &&
            route->instruction_word == instruction_word)
            return route;
    }
    return NULL;
}

static bool context_is_valid(
        const XgRenderResidentTextServices *services, uint32_t address) {
    return services->guest_data_range_is_valid(
        address, RESIDENT_TEXT_CONTEXT_SIZE, 2u, false);
}

static bool decode_glyph_id(
        const PsxXgRenderResidentTextSnapshot *snapshot,
        uint16_t lead, uint16_t trail,
        uint16_t *out_glyph_id, uint16_t *out_width) {
    uint32_t slot;

    if (snapshot == NULL || out_glyph_id == NULL || out_width == NULL ||
        !snapshot->font_valid)
        return false;
    if (lead == 0u) {
        if (trail < snapshot->font_header[6]) return false;
        slot = trail - snapshot->font_header[6];
        if (slot >= 299u) return false;
        *out_width = slot < snapshot->font_header[4] ? 8u : 12u;
    } else if (lead == UINT16_C(0x00ff) &&
               trail == UINT16_C(0x00ff)) {
        *out_glyph_id = 300u;
        *out_width = 12u;
        return true;
    } else {
        if (lead < snapshot->font_header[2]) return false;
        slot = snapshot->font_header[3] / 0x16u +
            (uint32_t)(lead - snapshot->font_header[2]) * 256u + trail;
        if (slot >= 299u) return false;
        *out_width = trail < snapshot->font_header[5] ? 8u : 12u;
    }
    *out_glyph_id = (uint16_t)(slot + 1u);
    return true;
}

static void clear_captured_state(bool clear_font, bool reset_counters) {
    PsxXgRenderResidentTextSnapshot previous = resident_text.snapshot;
    PsxXgRenderResidentTextSnapshot next = {0};
    debug_font_ready = false;

    if (reset_counters) {
        memset(&resident_text, 0, sizeof(resident_text));
        return;
    }
    next.observation_count = previous.observation_count;
    memcpy(next.route_counts, previous.route_counts,
           sizeof(next.route_counts));
    next.invalidation_count = previous.invalidation_count + 1u;
    if (!clear_font && previous.font_valid) {
        next.font_content_digest = previous.font_content_digest;
        next.font_address = previous.font_address;
        memcpy(next.font_header, previous.font_header,
               sizeof(next.font_header));
        next.font_valid = true;
    } else {
        memset(resident_text.font_bytes, 0,
               sizeof(resident_text.font_bytes));
    }
    resident_text.snapshot = next;
}

bool xg_render_resident_text_observe(
        CPUState *cpu, uint32_t action, uint32_t pc,
        uint32_t instruction_word, GuestRenderRenderMode render_mode,
        const XgRenderResidentTextServices *services) {
    if ((pc & UINT32_C(0x1fffffff)) == 0x370dcu ||
        (pc & UINT32_C(0x1fffffff)) == 0x372c4u)
        return debug_font_observe(cpu, pc, instruction_word, render_mode, services);
    const XgRenderResidentTextRoute *route = route_lookup(
        action, pc, instruction_word);
    XgRenderResidentTextState next;
    uint32_t context;

    if (route == NULL || cpu == NULL || services == NULL ||
        services->native_text_authorizes_pc == NULL ||
        services->guest_data_range_is_valid == NULL ||
        render_mode != GUEST_RENDER_RENDER_NATIVE ||
        !services->native_text_authorizes_pc(route->owner_entry))
        return false;
    next = resident_text;

    switch ((XgRenderCutoverAction)action) {
    case XG_CUTOVER_RESIDENT_TEXT_CONTEXT_INITIALIZE: {
        const uint32_t stack = cpu->gpr[29];

        context = cpu->gpr[4];
        if (cpu->read_half == NULL || !context_is_valid(services, context) ||
            !services->guest_data_range_is_valid(
                stack + 0x10u, 0xau, 2u, false))
            return false;
        next.snapshot.context_address = context;
        next.snapshot.base_x = (int16_t)cpu->gpr[7];
        next.snapshot.base_y = (int16_t)cpu->read_half(stack + 0x10u);
        next.snapshot.width_units = cpu->read_half(stack + 0x14u);
        next.snapshot.row_count = cpu->read_half(stack + 0x18u);
        next.snapshot.line_height = 14u;
        next.snapshot.display_first_row = 0u;
        next.snapshot.selected_row = UINT8_C(0xff);
        next.snapshot.context_valid = true;
        next.snapshot.frame_frozen = false;
        break;
    }
    case XG_CUTOVER_RESIDENT_TEXT_FONT_FREE:
        memset(next.font_bytes, 0, sizeof(next.font_bytes));
        next.snapshot.font_address = 0u;
        next.snapshot.font_content_digest = 0u;
        memset(next.snapshot.font_header, 0,
               sizeof(next.snapshot.font_header));
        next.snapshot.font_valid = false;
        break;
    case XG_CUTOVER_RESIDENT_TEXT_FONT_INITIALIZE: {
        uint16_t header[7];

        if (cpu->gpr[4] == 0u) {
            memset(next.font_bytes, 0, sizeof(next.font_bytes));
            next.snapshot.font_address = 0u;
            next.snapshot.font_content_digest = 0u;
            memset(next.snapshot.font_header, 0,
                   sizeof(next.snapshot.font_header));
            next.snapshot.font_valid = false;
            break;
        }
        if (cpu->read_half == NULL || cpu->read_byte == NULL ||
            !services->guest_data_range_is_valid(
                cpu->gpr[4], RESIDENT_FONT_SIZE, 2u, false))
            return false;
        for (uint32_t index = 0u; index < 7u; ++index)
            header[index] = cpu->read_half(cpu->gpr[4] + index * 2u);
        if (memcmp(header, canonical_font_header, sizeof(header)) != 0)
            return false;
        for (uint32_t index = 0u; index < RESIDENT_FONT_SIZE; ++index)
            next.font_bytes[index] = cpu->read_byte(cpu->gpr[4] + index);
        next.snapshot.font_address = cpu->gpr[4];
        next.snapshot.font_content_digest = xg_render_resource_digest(
            next.font_bytes, sizeof(next.font_bytes));
        memcpy(next.snapshot.font_header, header, sizeof(header));
        next.snapshot.font_valid = true;
        break;
    }
    case XG_CUTOVER_RESIDENT_TEXT_ADVANCE_BEGIN:
        context = cpu->gpr[4];
        if (!context_is_valid(services, context)) return false;
        next.snapshot.active_advance_context = context;
        break;
    case XG_CUTOVER_RESIDENT_TEXT_ROW_RECYCLE:
        context = cpu->gpr[16];
        if (cpu->read_half == NULL ||
            !context_is_valid(services, context) ||
            !services->guest_data_range_is_valid(
                cpu->gpr[2] + 0x58u, 2u, 2u, false))
            return false;
        next.snapshot.active_advance_context = context;
        next.snapshot.last_row_record_address = cpu->gpr[2];
        next.snapshot.last_row_advance =
            cpu->read_half(cpu->gpr[2] + 0x58u);
        break;
    case XG_CUTOVER_RESIDENT_TEXT_STREAM_DESTROY:
        context = cpu->gpr[4];
        if (!context_is_valid(services, context)) return false;
        if (physical_address_equals(next.snapshot.context_address, context)) {
            next.snapshot.context_address = 0u;
            next.snapshot.context_valid = false;
        }
        if (physical_address_equals(
                next.snapshot.active_advance_context, context))
            next.snapshot.active_advance_context = 0u;
        if (physical_address_equals(
                next.snapshot.render_context_address, context)) {
            next.snapshot.render_context_address = 0u;
            next.snapshot.render_ot_address = 0u;
            next.snapshot.render_active = false;
            next.snapshot.frame_frozen = false;
        }
        break;
    case XG_CUTOVER_RESIDENT_TEXT_ROW_COLORS:
        context = cpu->gpr[4];
        if (!context_is_valid(services, context)) return false;
        next.snapshot.context_address = context;
        next.snapshot.red = (uint8_t)cpu->gpr[5];
        next.snapshot.green = (uint8_t)cpu->gpr[6];
        next.snapshot.blue = (uint8_t)cpu->gpr[7];
        next.snapshot.context_valid = true;
        break;
    case XG_CUTOVER_RESIDENT_TEXT_ROW_SELECT:
        context = cpu->gpr[4];
        if (!context_is_valid(services, context)) return false;
        next.snapshot.context_address = context;
        next.snapshot.selected_row = (uint8_t)cpu->gpr[5];
        next.snapshot.context_valid = true;
        break;
    case XG_CUTOVER_RESIDENT_TEXT_ROW_SELECT_CLEAR:
        context = cpu->gpr[4];
        if (!context_is_valid(services, context)) return false;
        next.snapshot.context_address = context;
        next.snapshot.selected_row = UINT8_C(0xff);
        next.snapshot.context_valid = true;
        break;
    case XG_CUTOVER_RESIDENT_TEXT_STRING_ENTRY_RENDER:
        context = cpu->gpr[4];
        if (!context_is_valid(services, context)) return false;
        next.snapshot.context_address = context;
        next.snapshot.render_context_address = context;
        next.snapshot.render_ot_address = cpu->gpr[5];
        next.snapshot.render_parity = (uint8_t)(cpu->gpr[6] & 1u);
        next.snapshot.context_valid = true;
        next.snapshot.render_active = true;
        next.snapshot.frame_frozen = false;
        break;
    case XG_CUTOVER_RESIDENT_TEXT_PAGE_RESET:
        context = cpu->gpr[16];
        if (!next.snapshot.render_active ||
            !physical_address_equals(
                next.snapshot.render_context_address, context) ||
            !context_is_valid(services, context))
            return false;
        next.snapshot.display_first_row = 0u;
        next.snapshot.frame_frozen = false;
        break;
    case XG_CUTOVER_RESIDENT_TEXT_FRAME_CHECKPOINT:
        context = cpu->gpr[16];
        if (cpu->read_half == NULL || !next.snapshot.render_active ||
            !physical_address_equals(
                next.snapshot.render_context_address, context) ||
            !context_is_valid(services, context))
            return false;
        next.snapshot.base_x = (int16_t)cpu->read_half(context + 4u);
        next.snapshot.base_y = (int16_t)cpu->read_half(context + 6u);
        next.snapshot.width_units = cpu->read_half(context + 0xau);
        next.snapshot.row_count = cpu->read_half(context + 0xcu);
        next.snapshot.line_height = cpu->read_half(context + 0x14u);
        next.snapshot.display_first_row = cpu->read_half(context + 0x16u);
        next.snapshot.selected_row = cpu->read_byte != NULL
            ? cpu->read_byte(context + 0x6eu) : UINT8_C(0xff);
        next.snapshot.frame_frozen = true;
        break;
    case XG_CUTOVER_RESIDENT_TEXT_STRING_ENTRY_RETURN:
        if (!next.snapshot.render_active) return false;
        next.snapshot.render_active = false;
        break;
    case XG_CUTOVER_RESIDENT_TEXT_GLYPH_RASTERIZE: {
        uint16_t glyph_id;
        uint16_t glyph_width;
        const uint32_t stack = cpu->gpr[29];

        context = cpu->gpr[16];
        if (cpu->read_half == NULL ||
            !physical_address_equals(
                next.snapshot.active_advance_context, context) ||
            !context_is_valid(services, context) ||
            !services->guest_data_range_is_valid(
                stack + 0x10u, 2u, 2u, false) ||
            !decode_glyph_id(&next.snapshot,
                (uint16_t)cpu->gpr[4], (uint16_t)cpu->gpr[5],
                &glyph_id, &glyph_width))
            return false;
        next.snapshot.last_lead = (uint16_t)cpu->gpr[4];
        next.snapshot.last_trail = (uint16_t)cpu->gpr[5];
        next.snapshot.last_destination_address = cpu->gpr[6];
        next.snapshot.last_stride = (uint16_t)cpu->gpr[7];
        next.snapshot.last_plane =
            (uint8_t)(cpu->read_half(stack + 0x10u) & 1u);
        next.snapshot.last_glyph_id = glyph_id;
        next.snapshot.last_glyph_x = cpu->read_half(context);
        next.snapshot.last_glyph_row = cpu->read_half(context + 2u);
        next.snapshot.last_glyph_width = glyph_width;
        break;
    }
    default:
        return false;
    }

    ++next.snapshot.observation_count;
    ++next.snapshot.route_counts[route->counter_index];
    next.snapshot.last_pc = pc;
    next.snapshot.last_owner_entry = route->owner_entry;
    resident_text = next;
    if (action == XG_CUTOVER_RESIDENT_TEXT_FONT_INITIALIZE &&
        resident_text.snapshot.font_valid && services->watch_resource != NULL)
        services->watch_resource(
            resident_text.snapshot.font_address, RESIDENT_FONT_SIZE);
    return true;
}

void xg_render_resident_text_snapshot(
        PsxXgRenderResidentTextSnapshot *out_snapshot) {
    if (out_snapshot != NULL) *out_snapshot = resident_text.snapshot;
}

void xg_render_resident_text_handle_invalidation(
        const XgRenderInvalidationEvent *event,
        const XgRenderInvalidationServices *services) {
    bool font_overlap;
    bool code_overlap = false;

    (void)services;
    if (event == NULL) return;
    if (event->kind == XG_RENDER_INVALIDATION_RESET) {
        clear_captured_state(true, true);
        return;
    }
    font_overlap = resident_text.snapshot.font_valid &&
        event->kind == XG_RENDER_INVALIDATION_CODE_WRITE &&
        ranges_overlap(resident_text.snapshot.font_address,
                       RESIDENT_FONT_SIZE, event->address, event->size);
    if (event->kind == XG_RENDER_INVALIDATION_CODE_WRITE) {
        for (uint32_t index = 0u;
             index < sizeof(code_ranges) / sizeof(code_ranges[0]); ++index)
            code_overlap = code_overlap || ranges_overlap(
                code_ranges[index].address, code_ranges[index].size,
                event->address, event->size);
    }
    if (event->kind == XG_RENDER_INVALIDATION_DISABLE ||
        event->kind == XG_RENDER_INVALIDATION_LOADER_MISMATCH ||
        code_overlap || font_overlap) {
        clear_captured_state(true, false);
    } else if (event->kind == XG_RENDER_INVALIDATION_SCENE_BOUNDARY ||
               event->kind == XG_RENDER_INVALIDATION_AUTHORITY_LOST) {
        clear_captured_state(false, false);
    }
}

void xg_render_resident_text_classify_code_write(
        uint32_t address, uint32_t size,
        XgRenderMutationClassification *out_classification) {
    bool overlaps = false;

    if (out_classification == NULL) return;
    for (uint32_t index = 0u;
         index < sizeof(code_ranges) / sizeof(code_ranges[0]); ++index)
        overlaps = overlaps || ranges_overlap(
            code_ranges[index].address, code_ranges[index].size,
            address, size);
    *out_classification = (XgRenderMutationClassification){
        .properties = {
            .watched_range_mutation = overlaps,
            .executable_mutation = overlaps,
            .semantic_authority_loss = overlaps,
            .authentication_mutation = overlaps,
            .authority_loss = overlaps,
        },
    };
}

void xg_render_resident_text_register_code_watches(
        void (*set_range)(uint32_t physical_address, uint32_t size)) {
    if (set_range == NULL) return;
    for (uint32_t index = 0u;
         index < sizeof(code_ranges) / sizeof(code_ranges[0]); ++index)
        set_range(code_ranges[index].address & UINT32_C(0x1fffffff),
                  code_ranges[index].size);
}
