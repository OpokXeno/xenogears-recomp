#include "xg_field_projected.h"

#include "cpu_state.h"
#include "guest_render_bridge.h"
#include "gpu.h"
#include "psx_cyc.h"
#include "xg_field_render_services.h"
#include "xg_render_primitive_utils.h"
#include "xg_render_quad_builder.h"

#include <limits.h>
#include <stddef.h>
#include <string.h>

static XgRenderProjectedSourceState projected_sources = {
    .next_generation = 1u,
};
static XgRenderProjectedInitializerPending projected_initializer_pending;
static uint32_t projected_source_count;
static PsxXgRenderProjectedLifecycleSnapshot projected_lifecycle;

typedef struct XgFieldProjectedRange {
    uint32_t start;
    uint32_t size;
} XgFieldProjectedRange;

static const XgFieldProjectedRange projected_code_ranges[] = {
    { UINT32_C(0x8002709c), 0x328u },
    { UINT32_C(0x800273c4), 0x534u },
    { UINT32_C(0x800278f8), 0x448u },
    { UINT32_C(0x80043a1c), 0x54u },
    { UINT32_C(0x80043c24), 0x20u },
    { UINT32_C(0x80043c9c), 0x3cu },
    { UINT32_C(0x80048c4c), 0x84u },
    { UINT32_C(0x80048d68), 0x12cu },
    { UINT32_C(0x8004b32c), 0x180u },
    { UINT32_C(0x80056a00), 0x180u },
    { UINT32_C(0x80056b94), 0x180u },
    { UINT32_C(0x80057030), 0x802u },
};

static bool code_ranges_overlap(uint32_t address, uint32_t size) {
    const uint64_t begin = address & UINT32_C(0x1fffffff);
    const uint64_t end = begin + size;

    if (size == 0u) return false;
    for (uint32_t index = 0u;
         index < sizeof(projected_code_ranges) /
                     sizeof(projected_code_ranges[0]); ++index) {
        const uint64_t range_begin =
            projected_code_ranges[index].start & UINT32_C(0x1fffffff);
        const uint64_t range_end =
            range_begin + projected_code_ranges[index].size;
        if (range_begin < end && begin < range_end) return true;
    }
    return false;
}

bool xg_field_projected_code_write_overlaps(uint32_t address, uint32_t size) {
    return code_ranges_overlap(address, size);
}

void xg_field_projected_register_code_watches(
        void (*set_range)(uint32_t physical_address, uint32_t size)) {
    if (set_range == NULL) return;
    for (uint32_t index = 0u;
         index < sizeof(projected_code_ranges) /
                     sizeof(projected_code_ranges[0]); ++index)
        set_range(projected_code_ranges[index].start & UINT32_C(0x1fffffff),
                  projected_code_ranges[index].size);
}

void xg_field_projected_reset(void) {
    if (projected_source_count == 0u && !projected_sources.blocked &&
        !projected_initializer_pending.valid)
        return;
    projected_sources = (XgRenderProjectedSourceState){
        .next_generation = 1u,
    };
    projected_source_count = 0u;
    xg_field_projected_reset_pending();
}

void xg_field_projected_classify_code_write(
        uint32_t address, uint32_t size,
        XgRenderMutationClassification *out_classification) {
    const bool overlaps = xg_field_projected_code_write_overlaps(address, size);

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
            ? UINT32_C(1) << PSX_XG_RENDER_CODE_WRITE_PROJECTED : 0u,
    };
}

void xg_field_projected_handle_invalidation(
        const XgRenderInvalidationEvent *event,
        const XgRenderInvalidationServices *services) {
    (void)services;
    if (event->kind == XG_RENDER_INVALIDATION_DISABLE) {
        xg_field_projected_note_disable_reset();
        xg_field_projected_reset();
    } else if (event->kind == XG_RENDER_INVALIDATION_SCENE_BOUNDARY) {
        xg_field_projected_reset_pending();
        xg_field_projected_note_pending_reset();
    } else if (event->kind == XG_RENDER_INVALIDATION_LOADER_MISMATCH) {
        xg_field_projected_note_loader_reset();
        xg_field_projected_reset();
    } else if (event->kind == XG_RENDER_INVALIDATION_CODE_WRITE) {
        if (!event->mutation.watched_range_mutation &&
            !event->mutation.artifact_mutation)
            return;
        xg_field_projected_note_code_write(
            event->address == 0u ? 0u :
                (event->address & UINT32_C(0x1fffffff)) |
                    UINT32_C(0x80000000),
            event->size, event->code_write_mask);
        if (event->mutation.semantic_authority_loss ||
            xg_render_invalidation_has_code_class(
                event, PSX_XG_RENDER_CODE_WRITE_PROJECTED))
            xg_field_projected_reset();
    } else if (event->kind == XG_RENDER_INVALIDATION_RESET) {
        xg_field_projected_reset();
        xg_field_projected_lifecycle_reset();
    }
}

void xg_field_projected_reset_pending(void) {
    projected_initializer_pending =
        (XgRenderProjectedInitializerPending){ 0 };
}

static XgRenderProjectedSource *projected_source_find(
        uint32_t object_address) {
    uint32_t index;

    for (index = 0u; index < XG_RENDER_PROJECTED_SOURCE_CAPACITY; ++index) {
        XgRenderProjectedSource *source = &projected_sources.records[index];
        if (source->valid && source->object_address == object_address)
            return source;
    }
    return NULL;
}

bool xg_field_projected_lookup(
        uint32_t object_address, XgRenderProjectedSource *out_source) {
    const XgRenderProjectedSource *source =
        projected_source_find(object_address);

    if (source == NULL) return false;
    if (out_source != NULL) *out_source = *source;
    return true;
}

static XgRenderProjectedSource *allocate_source(uint32_t object_address) {
    XgRenderProjectedSource *free_record = NULL;
    XgRenderProjectedSource *oldest_record = NULL;
    uint32_t index;

    for (index = 0u; index < XG_RENDER_PROJECTED_SOURCE_CAPACITY; ++index) {
        XgRenderProjectedSource *source = &projected_sources.records[index];
        if (source->valid && source->object_address == object_address)
            return source;
        if (!source->valid && free_record == NULL) free_record = source;
        if (source->valid &&
            (oldest_record == NULL || source->generation < oldest_record->generation))
            oldest_record = source;
    }
    return free_record != NULL ? free_record : oldest_record;
}

void xg_field_projected_observe_initializer_begin(
    CPUState *cpu, GuestRenderRenderMode mode) {
    uint32_t color_address;
    int32_t clut_x;
    int32_t clut_y;

    ++projected_lifecycle.initializer_begin_count;
    projected_initializer_pending =
        (XgRenderProjectedInitializerPending){ 0 };
    if (mode == GUEST_RENDER_RENDER_ORIGINAL || cpu == NULL ||
        cpu->read_word == NULL || cpu->read_byte == NULL ||
        !xg_render_runtime_stack_address_is_valid(cpu->gpr[29]))
        return;
    clut_x = (int32_t)cpu->read_word(cpu->gpr[29] + 0x10u);
    clut_y = (int32_t)cpu->read_word(cpu->gpr[29] + 0x14u);
    color_address = cpu->read_word(cpu->gpr[29] + 0x24u);
    if (clut_x < 0 || clut_x > 0x3ff || clut_y < 0 || clut_y > 0x1ff ||
        color_address > UINT32_MAX - 10u ||
        !xg_render_runtime_vector_address_is_valid(color_address) ||
        !xg_render_runtime_vector_address_is_valid(color_address + 10u))
        return;
    projected_initializer_pending = (XgRenderProjectedInitializerPending){
        .entry_sp = cpu->gpr[29],
        .return_address = cpu->gpr[31],
        .clut_x = (uint16_t)clut_x & UINT16_C(0x03f0),
        .clut_y = (uint16_t)clut_y & UINT16_C(0x01ff),
        .upper_color = {
            cpu->read_byte(color_address), cpu->read_byte(color_address + 1u),
            cpu->read_byte(color_address + 2u),
        },
        .middle_top_color = {
            cpu->read_byte(color_address + 4u),
            cpu->read_byte(color_address + 5u),
            cpu->read_byte(color_address + 6u),
        },
        .lower_color = {
            cpu->read_byte(color_address + 8u),
            cpu->read_byte(color_address + 9u),
            cpu->read_byte(color_address + 10u),
        },
        .valid = true,
    };
}

void xg_field_projected_observe_initializer_commit(CPUState *cpu) {
    XgRenderProjectedSource *source;
    uint32_t object_address;

    if (cpu == NULL || cpu->read_word == NULL ||
        !projected_initializer_pending.valid || projected_sources.blocked ||
        projected_sources.next_generation == 0u ||
        projected_initializer_pending.entry_sp < 0xa0u ||
        cpu->gpr[29] != projected_initializer_pending.entry_sp - 0xa0u ||
        !xg_render_runtime_stack_address_is_valid(cpu->gpr[29]) ||
        cpu->read_word(cpu->gpr[29] + 0x9cu) !=
            projected_initializer_pending.return_address) {
        projected_initializer_pending =
            (XgRenderProjectedInitializerPending){ 0 };
        return;
    }
    object_address = cpu->gpr[2];
    if (object_address == 0u || object_address > UINT32_MAX - 0x34cu ||
        !xg_render_runtime_word_address_is_valid(object_address) ||
        !xg_render_runtime_word_address_is_valid(object_address + 0x348u)) {
        projected_initializer_pending =
            (XgRenderProjectedInitializerPending){ 0 };
        return;
    }
    source = allocate_source(object_address);
    if (source == NULL) {
        projected_initializer_pending =
            (XgRenderProjectedInitializerPending){ 0 };
        return;
    }
    const bool new_source = !source->valid;
    *source = (XgRenderProjectedSource){
        .generation = projected_sources.next_generation++,
        .object_address = object_address,
        .clut_x = projected_initializer_pending.clut_x,
        .clut_y = projected_initializer_pending.clut_y,
        .valid = true,
    };
    if (new_source) ++projected_source_count;
    memcpy(source->upper_color, projected_initializer_pending.upper_color,
           sizeof(source->upper_color));
    memcpy(source->middle_top_color,
           projected_initializer_pending.middle_top_color,
           sizeof(source->middle_top_color));
    memcpy(source->lower_color, projected_initializer_pending.lower_color,
           sizeof(source->lower_color));
    projected_initializer_pending =
        (XgRenderProjectedInitializerPending){ 0 };
}

void xg_field_projected_note_initializer_result(CPUState *cpu) {
    if (cpu != NULL && projected_source_find(cpu->gpr[2]) != NULL) {
        projected_lifecycle.last_registered_object = cpu->gpr[2];
        ++projected_lifecycle.initializer_registration_count;
    }
}

void xg_field_projected_lifecycle_reset(void) {
    projected_lifecycle = (PsxXgRenderProjectedLifecycleSnapshot){ 0 };
}

void xg_field_projected_lifecycle_snapshot(
        PsxXgRenderProjectedLifecycleSnapshot *out_snapshot) {
    if (out_snapshot != NULL) *out_snapshot = projected_lifecycle;
}

void xg_field_projected_note_pending_reset(void) {
    ++projected_lifecycle.pending_reset_count;
}

void xg_field_projected_note_cutover_attempt(void) {
    ++projected_lifecycle.cutover_attempt_count;
}

void xg_field_projected_note_disable_reset(void) {
    ++projected_lifecycle.disable_reset_count;
}

void xg_field_projected_note_loader_reset(void) {
    ++projected_lifecycle.loader_reset_count;
}

void xg_field_projected_note_code_write(
        uint32_t address, uint32_t size, uint32_t code_write_mask) {
    uint32_t code_class;

    if (projected_lifecycle.code_write_reset_count == 0u) {
        projected_lifecycle.first_code_write_address = address;
        projected_lifecycle.first_code_write_size = size;
        projected_lifecycle.first_code_write_mask = code_write_mask;
    }
    projected_lifecycle.last_code_write_address = address;
    projected_lifecycle.last_code_write_size = size;
    projected_lifecycle.last_code_write_mask = code_write_mask;
    for (code_class = 0u;
         code_class < PSX_XG_RENDER_CODE_WRITE_CLASS_COUNT; ++code_class) {
        if ((code_write_mask & (UINT32_C(1) << code_class)) == 0u) continue;
        if (projected_lifecycle.code_write_class_counts[code_class] == 0u)
            projected_lifecycle.code_write_class_first_address[code_class] =
                address;
        projected_lifecycle.code_write_class_last_address[code_class] = address;
        ++projected_lifecycle.code_write_class_counts[code_class];
    }
    ++projected_lifecycle.code_write_reset_count;
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

static int32_t wrap_add(int32_t left, int32_t right) {
    return (int32_t)((uint32_t)left + (uint32_t)right);
}

static int32_t wrap_subtract(int32_t left, int32_t right) {
    return (int32_t)((uint32_t)left - (uint32_t)right);
}

static int32_t wrap_multiply(int32_t left, int32_t right) {
    return (int32_t)((uint32_t)left * (uint32_t)right);
}

static int32_t shift_right_floor(int32_t value, unsigned bits) {
    if (bits == 0u) return value;
    if (value >= 0) return value >> bits;
    return -(int32_t)(((uint64_t)(-(int64_t)value) +
                       ((UINT64_C(1) << bits) - 1u)) >> bits);
}

static int32_t rounded_shift(int32_t value, unsigned bits) {
    if (value < 0) value = wrap_add(value, (int32_t)((1u << bits) - 1u));
    return shift_right_floor(value, bits);
}

static bool projected_divide(int32_t numerator, int32_t denominator,
                             int32_t *quotient) {
    if (quotient == NULL || denominator == 0 ||
        (numerator == INT32_MIN && denominator == -1))
        return false;
    *quotient = numerator / denominator;
    return true;
}

static unsigned leading_sign_bits(uint32_t value) {
    uint32_t scan = (value & UINT32_C(0x80000000)) != 0u ? ~value : value;
    unsigned count = 0u;
    if (scan == 0u) return 32u;
    while ((scan & UINT32_C(0x80000000)) == 0u) {
        ++count;
        scan <<= 1u;
    }
    return count;
}

static bool square_root0(CPUState *cpu, uint32_t value, int32_t *result) {
    const unsigned leading = leading_sign_bits(value);
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

static bool ratan2_guest(CPUState *cpu, int32_t y, int32_t x,
                         int32_t *result) {
    bool x_negative;
    bool y_negative;
    int32_t ratio;
    int32_t divisor;
    int32_t angle;

    if (cpu == NULL || cpu->read_half == NULL || result == NULL) return false;
    x_negative = x < 0;
    y_negative = y < 0;
    if (x_negative) x = wrap_subtract(0, x);
    if (y_negative) y = wrap_subtract(0, y);
    if (x == 0 && y == 0) {
        *result = 0;
        return true;
    }
    if (y < x) {
        if (((uint32_t)y & UINT32_C(0x7fe00000)) != 0u) {
            divisor = shift_right_floor(x, 10u);
            if (!projected_divide(y, divisor, &ratio)) return false;
        } else if (!projected_divide(
                       (int32_t)((uint32_t)y << 10u), x, &ratio)) {
            return false;
        }
        if (ratio < 0 || ratio > 0x400) return false;
        angle = (int16_t)cpu->read_half(
            UINT32_C(0x80057030) + (uint32_t)ratio * 2u);
    } else {
        if (((uint32_t)x & UINT32_C(0x7fe00000)) != 0u) {
            divisor = shift_right_floor(y, 10u);
            if (!projected_divide(x, divisor, &ratio)) return false;
        } else if (!projected_divide(
                       (int32_t)((uint32_t)x << 10u), y, &ratio)) {
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

static uint16_t get_tpage(int16_t depth, int32_t page_x, int32_t page_y) {
    return (uint16_t)(((uint32_t)page_x >> 6u) & 0x0fu) |
        (uint16_t)((((uint32_t)page_y >> 8u) & 1u) << 4u) |
        (uint16_t)(((uint16_t)depth & 3u) << 7u) |
        (uint16_t)(((uint32_t)page_y & 0x200u) << 2u);
}

static bool capture_config(CPUState *cpu, uint32_t object_address,
                           XgRenderProjectedConfig *config) {
    if (cpu == NULL || config == NULL || cpu->read_word == NULL ||
        cpu->read_half == NULL || object_address > UINT32_MAX - 0x34au ||
        !xg_render_runtime_word_address_is_valid(object_address) ||
        !xg_render_runtime_word_address_is_valid(object_address + 0x348u))
        return false;
    *config = (XgRenderProjectedConfig){
        .strip_width = (int32_t)cpu->read_word(object_address + 0x328u),
        .strip_height = (int32_t)cpu->read_word(object_address + 0x32cu),
        .phase_multiplier = (int32_t)cpu->read_word(object_address + 0x330u),
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

static void set_native_view_position(
        XgRenderQuadSourceVertex *vertex,
        const XgFieldProjectedPipelineServices *services) {
    const XgNativeView *view = services != NULL && services->native_view != NULL
        ? services->native_view() : NULL;
    int32_t surface_width;
    int64_t native_x;
    int64_t native_y;

    if (vertex == NULL || view == NULL || !view->enabled ||
        view->canonical_width == 0u || view->canonical_height == 0u ||
        view->aspect_den == 0u)
        return;
    surface_width = (int32_t)(view->surface_width_16_16 >> 16u);
    if (surface_width <= (int32_t)view->canonical_width) return;
    if (vertex->x <= 0) {
        native_x = 0;
    } else if ((uint32_t)(uint16_t)vertex->x >= view->canonical_width) {
        native_x = (int64_t)surface_width * INT32_C(65536);
    } else {
        native_x = ((int64_t)vertex->x * surface_width * INT32_C(65536) +
                    (int32_t)view->canonical_width / 2) /
            (int32_t)view->canonical_width;
    }
    native_y = (int64_t)vertex->y * INT32_C(65536);
    if (native_x < INT32_MIN || native_x > INT32_MAX ||
        native_y < INT32_MIN || native_y > INT32_MAX)
        return;
    vertex->native_view_x_16_16 = (int32_t)native_x;
    vertex->native_view_y_16_16 = (int32_t)native_y;
    vertex->native_view_position = true;
}

static bool build_primitive(
        XgRenderProjectedNativeRecord *record,
        const XgRenderProjectedSource *source, const GpuDrawState *draw,
        const XgFieldProjectedPipelineServices *services) {
    XgRenderQuadSource quad = { 0 };
    uint32_t vertex;

    xg_render_material_apply_draw_state(&quad.material, draw);
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
        quad.material.shading = record->kind == XG_RENDER_PROJECTED_RECORD_G4
            ? XG_RENDER_IR_SHADING_GOURAUD : XG_RENDER_IR_SHADING_FLAT;
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
        set_native_view_position(&quad.vertices[vertex], services);
    }
    return xg_render_quad_build_primitive(&quad, &record->primitive) ==
        XG_RENDER_QUAD_BUILDER_OK;
}

static bool prepare_record(
        CPUState *cpu, XgRenderProjectedNativeRecord *record,
        const XgRenderProjectedSource *source, const GpuDrawState *draw,
        const XgFieldProjectedPipelineServices *services) {
    uint8_t expected_payload;
    if (cpu == NULL || record == NULL || source == NULL || draw == NULL ||
        cpu->read_word == NULL)
        return false;
    expected_payload = record->kind == XG_RENDER_PROJECTED_RECORD_FT4 ? 9u :
        (record->kind == XG_RENDER_PROJECTED_RECORD_G4 ? 8u : 5u);
    if (record->packet_address > UINT32_MAX - expected_payload * 4u ||
        !xg_render_runtime_word_address_is_valid(record->packet_address) ||
        !xg_render_runtime_word_address_is_valid(
            record->packet_address + expected_payload * 4u))
        return false;
    record->packet_tag = cpu->read_word(record->packet_address);
    if ((record->packet_tag >> 24u) != expected_payload) return false;
    record->payload_word_count = expected_payload;
    return build_primitive(record, source, draw, services);
}

static bool append_record(
        CPUState *cpu, XgRenderProjectedNativeRecord records[],
        uint32_t *count, const XgRenderProjectedSource *source,
        const GpuDrawState *draw, const XgRenderProjectedNativeRecord *record,
        const XgFieldProjectedPipelineServices *services) {
    if (count == NULL || *count >= XG_RENDER_PROJECTED_MAX_RECORDS) return false;
    records[*count] = *record;
    if (!prepare_record(cpu, &records[*count], source, draw, services))
        return false;
    ++*count;
    return true;
}

static bool build_strips(
        CPUState *cpu, const XgRenderProjectedSource *source,
        const XgRenderProjectedConfig *config, const GpuDrawState *draw,
        uint32_t buffer_index, int32_t phase, int16_t projected_y, int16_t fade,
        XgRenderProjectedNativeRecord records[], uint32_t *count, bool temporal,
        const XgFieldProjectedPipelineServices *services) {
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
    fade_adjustment = rounded_shift(
        wrap_multiply(centered_width, fade), 8u);
    phase_cursor = xg_render_runtime_low_s16((uint32_t)wrap_subtract(
        phase, wrap_add(centered_width, fade_adjustment)));
    phase_cursor %= config->strip_width;
    if (phase_cursor < 0)
        phase_cursor = (uint16_t)config->strip_width + phase_cursor;
    if (temporal || (projected_y >= 0 &&
        projected_y <= wrap_add(config->strip_height, 0xf0))) {
        if (!projected_divide(
                (int32_t)((uint32_t)config->strip_height << 8u), denominator,
                &strip_screen_height))
            return false;
    }
    if (xg_render_runtime_low_s16((uint32_t)strip_screen_height) <= 0)
        return true;

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

        if (texture_page < 0) texture_page = wrap_add(texture_page, 0x3f);
        page_remainder = config->texture_x -
            shift_right_floor(texture_page, 6u) * 0x40;
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
        if (0x140 < xg_render_runtime_low_s16((uint32_t)screen_x) +
                        xg_render_runtime_low_s16((uint32_t)screen_width)) {
            screen_width = 0x140 - screen_x;
            source_width = rounded_shift(
                wrap_multiply(
                    xg_render_runtime_low_s16((uint32_t)screen_width),
                    denominator), 8u);
        }
        next_phase = xg_render_runtime_low_s16(
            (uint32_t)(phase_cursor + source_width));
        next_phase %= config->strip_width;
        screen_width = xg_render_runtime_low_s16((uint32_t)screen_width);
        record.x[0] = record.x[2] =
            xg_render_runtime_low_s16((uint32_t)screen_x);
        screen_x = wrap_add(screen_x, screen_width);
        record.x[1] = record.x[3] =
            xg_render_runtime_low_s16((uint32_t)screen_x);
        record.y[0] = record.y[1] = (int16_t)
            ((uint16_t)projected_y - (uint16_t)strip_screen_height);
        record.y[2] = record.y[3] = projected_y;
        record.u[0] = record.u[2] = (uint8_t)u;
        record.u[1] = record.u[3] = (uint8_t)(u + source_width - 1);
        record.v[0] = record.v[1] = (uint8_t)config->texture_v;
        record.v[2] = record.v[3] =
            (uint8_t)(config->texture_v + (uint8_t)config->strip_height);
        page_x = xg_render_runtime_low_s16(
            (uint32_t)(uint16_t)config->texture_x +
            (uint32_t)shift_right_floor(
                xg_render_runtime_low_s16((uint32_t)phase_cursor),
                texture_shift));
        if (page_x < 0) page_x = wrap_add(page_x, 0x3f);
        if (page_y < 0) page_y = wrap_add(page_y, 0xff);
        record.tpage = get_tpage(
            config->texture_depth,
            (int32_t)((uint32_t)shift_right_floor(page_x, 6u) << 6u),
            (int32_t)((uint32_t)shift_right_floor(page_y, 8u) << 8u));
        if (!append_record(
                cpu, records, count, source, draw, &record, services))
            return false;
        phase_cursor = next_phase;
        if (xg_render_runtime_low_s16((uint32_t)screen_x) > 0x13f) break;
    }
    return true;
}

static bool resolve_ot_bucket(
        CPUState *cpu, uint32_t ot_address, uint32_t *bucket,
        XgFieldProjectedOrderingDomain *domain) {
    uint32_t context;
    uint32_t base;
    if (cpu == NULL || cpu->read_word == NULL || bucket == NULL ||
        domain == NULL)
        return false;
    *domain = XG_FIELD_PROJECTED_ORDERING_UNKNOWN;
    context = cpu->read_word(UINT32_C(0x800c426c));
    if (context <= UINT32_MAX - 0x80c8u) {
        base = context + 0x40ccu;
        if (ot_address >= base && ot_address <= base + 0x3ffcu &&
            ((ot_address - base) & 3u) == 0u) {
            *bucket = (ot_address - base) / 4u;
            *domain = XG_FIELD_PROJECTED_ORDERING_FIELD;
            return true;
        }
    }
    base = cpu->read_word(UINT32_C(0x800ccb04));
    if (base <= UINT32_MAX - 0x3ffcu && ot_address >= base &&
        ot_address <= base + 0x3ffcu && ((ot_address - base) & 3u) == 0u) {
        *bucket = (ot_address - base) / 4u;
        *domain = XG_FIELD_PROJECTED_ORDERING_BATTLE;
        return true;
    }
    context = cpu->read_word(UINT32_C(0x800ccb00));
    if (context <= UINT32_MAX - 0x406cu) {
        base = context + 0x70u;
        if (ot_address >= base && ot_address <= base + 0x3ffcu &&
            ((ot_address - base) & 3u) == 0u) {
            *bucket = (ot_address - base) / 4u;
            *domain = XG_FIELD_PROJECTED_ORDERING_BATTLE;
            return true;
        }
    }
    return false;
}

static bool stage_records(
        const XgRenderProjectedNativeRecord records[], uint32_t count,
        uint32_t ot_bucket, XgFieldProjectedOrderingDomain domain,
        uint32_t interpolation_producer_id,
        const XgFieldProjectedPipelineServices *services) {
    uint32_t index;
    const bool proof_active = services->proof_active != NULL &&
        services->proof_active();

    if (proof_active) {
        if (services->stage_active == NULL) return false;
        for (index = 0u; index < count; ++index) {
            if (!services->stage_active(
                    &records[index].primitive, records[index].packet_address,
                    UINT32_C(0x40000000) |
                        (records[index].packet_address & UINT32_C(0x001ffffc)),
                    ot_bucket, records[index].payload_word_count,
                    interpolation_producer_id, index, NULL))
                return false;
        }
        return true;
    }
    if (domain == XG_FIELD_PROJECTED_ORDERING_BATTLE) {
        if (services->stage_standalone == NULL) return false;
        for (index = 0u; index < count; ++index) {
            if (!services->stage_standalone(
                    &records[index].primitive, records[index].packet_address,
                    UINT32_C(0x40000000) |
                        (records[index].packet_address & UINT32_C(0x001ffffc)),
                    interpolation_producer_id, index))
                return false;
        }
        return true;
    }
    if (domain != XG_FIELD_PROJECTED_ORDERING_FIELD ||
        services->pre_scene_available == NULL ||
        services->stage_pre_scene == NULL ||
        !services->pre_scene_available(count))
        return false;
    for (index = 0u; index < count; ++index) {
        const XgRenderPreScenePrimitive staged = {
            .primitive = records[index].primitive,
            .packet_address = records[index].packet_address,
            .source_primitive_index = UINT32_C(0x40000000) |
                (records[index].packet_address & UINT32_C(0x001ffffc)),
            .ot_bucket = ot_bucket,
            .interpolation_producer_id = interpolation_producer_id,
            .interpolation_primitive_id = index,
            .payload_word_count = records[index].payload_word_count,
            .interpolation_identity_valid = interpolation_producer_id != 0u,
        };
        if (!services->stage_pre_scene(&staged))
            return false;
    }
    return true;
}

static bool stage_temporal_strips(
        const XgRenderProjectedNativeRecord records[], uint32_t count,
        XgFieldProjectedOrderingDomain domain,
        uint32_t interpolation_producer_id,
        const XgFieldProjectedPipelineServices *services) {
    const GpuRenderTemporalCullPolicy policy = {
        .flags = GPU_RENDER_TEMPORAL_CULL_SCREEN,
        .screen_left = 0,
        .screen_top = 0,
        .screen_right_exclusive = 320 * INT32_C(65536),
        .screen_bottom_exclusive = 240 * INT32_C(65536),
    };
    uint32_t index;
    const bool proof_active = services->proof_active != NULL &&
        services->proof_active();

    if (count == 0u) return true;
    if (proof_active || domain == XG_FIELD_PROJECTED_ORDERING_BATTLE) {
        if (services->stage_temporal == NULL) return false;
        for (index = 0u; index < count; ++index) {
            if (!services->stage_temporal(
                    &records[index].primitive, interpolation_producer_id,
                    index, &policy))
                return false;
        }
        return true;
    }
    if (domain != XG_FIELD_PROJECTED_ORDERING_FIELD ||
        services->pre_scene_available == NULL ||
        services->stage_pre_scene == NULL ||
        !services->pre_scene_available(count))
        return false;
    for (index = 0u; index < count; ++index) {
        const XgRenderPreScenePrimitive staged = {
            .primitive = records[index].primitive,
            .interpolation_producer_id = interpolation_producer_id,
            .interpolation_primitive_id = index,
            .interpolation_identity_valid = true,
            .temporal_only = true,
            .temporal_cull = policy,
        };
        if (!services->stage_pre_scene(&staged))
            return false;
    }
    return true;
}

static void write_half(CPUState *cpu, uint32_t address, int16_t value) {
    psx_store_cycle_barrier();
    cpu->write_half(address, (uint16_t)value);
}

static void write_byte(CPUState *cpu, uint32_t address, uint8_t value) {
    psx_store_cycle_barrier();
    cpu->write_byte(address, value);
}

static void write_record_payload(
        CPUState *cpu, const XgRenderProjectedNativeRecord *record) {
    uint32_t vertex;
    switch (record->kind) {
    case XG_RENDER_PROJECTED_RECORD_FT4:
        for (vertex = 0u; vertex < 4u; ++vertex) {
            write_half(cpu, record->packet_address + 8u + vertex * 8u,
                       record->x[vertex]);
            write_half(cpu, record->packet_address + 10u + vertex * 8u,
                       record->y[vertex]);
        }
        write_byte(cpu, record->packet_address + 12u, record->u[0]);
        write_byte(cpu, record->packet_address + 13u, record->v[0]);
        write_byte(cpu, record->packet_address + 20u, record->u[1]);
        write_byte(cpu, record->packet_address + 21u, record->v[1]);
        write_byte(cpu, record->packet_address + 28u, record->u[2]);
        write_byte(cpu, record->packet_address + 29u, record->v[2]);
        write_byte(cpu, record->packet_address + 36u, record->u[3]);
        write_byte(cpu, record->packet_address + 37u, record->v[3]);
        write_half(cpu, record->packet_address + 22u, (int16_t)record->tpage);
        break;
    case XG_RENDER_PROJECTED_RECORD_F4_UPPER:
        write_half(cpu, record->packet_address + 18u, record->y[2]);
        write_half(cpu, record->packet_address + 22u, record->y[3]);
        break;
    case XG_RENDER_PROJECTED_RECORD_G4:
        for (vertex = 0u; vertex < 4u; ++vertex)
            write_half(cpu, record->packet_address + 10u + vertex * 8u,
                       record->y[vertex]);
        break;
    case XG_RENDER_PROJECTED_RECORD_F4_LOWER:
        write_half(cpu, record->packet_address + 10u, record->y[0]);
        write_half(cpu, record->packet_address + 14u, record->y[1]);
        break;
    }
}

static bool reject_projected(
        uint32_t blocker, const XgFieldProjectedPipelineServices *services) {
    projected_initializer_pending = (XgRenderProjectedInitializerPending){ 0 };
    ++projected_lifecycle.cutover_rejection_count;
    projected_lifecycle.last_rejection_blocker = blocker;
    if (services != NULL && services->reject_policy != NULL)
        services->reject_policy(blocker);
    return false;
}

bool xg_field_projected_reject(
        uint32_t blocker, const XgFieldProjectedPipelineServices *services) {
    return reject_projected(blocker, services);
}

bool xg_field_projected_cutover(
        CPUState *cpu, uint32_t producer_pc,
        const XgFieldProjectedPipelineServices *services) {
    XgRenderProjectedConfig config;
    const XgRenderProjectedSource *source;
    XgRenderProjectedNativeRecord records[XG_RENDER_PROJECTED_MAX_RECORDS];
    XgRenderProjectedNativeRecord temporal_strips[XG_RENDER_PROJECTED_MAX_STRIPS];
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
    XgRenderProducerLifecycle lifecycle;
    XgFieldProjectedOrderingDomain ordering_domain;
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

    (void)producer_pc;
    if (cpu == NULL || services == NULL || cpu->read_word == NULL ||
        cpu->write_word == NULL || cpu->read_half == NULL ||
        cpu->write_half == NULL || cpu->read_byte == NULL ||
        cpu->write_byte == NULL ||
        !xg_render_runtime_stack_address_is_valid(cpu->gpr[29]))
        return reject_projected(61u, services);
    stack_pointer = cpu->gpr[29];
    object_address = cpu->gpr[4];
    eye_address = cpu->gpr[5];
    at_address = cpu->gpr[6];
    matrix_address = cpu->gpr[7];
    ot_address = cpu->read_word(stack_pointer + 0x10u);
    buffer_index = cpu->read_word(stack_pointer + 0x14u);
    source = projected_source_find(object_address);
    if (projected_sources.blocked) {
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
        !xg_render_runtime_vector_address_is_valid(eye_address) ||
        !xg_render_runtime_vector_address_is_valid(at_address) ||
        !xg_render_runtime_word_address_is_valid(ot_address) ||
        !resolve_ot_bucket(cpu, ot_address, &ot_bucket, &ordering_domain) ||
        !xg_render_runtime_capture_matrix(cpu, matrix_address, &matrix) ||
        !capture_config(cpu, object_address, &config))
        return reject_projected(62u, services);
    for (index = 0u; index < 3u; ++index) {
        eye[index] = (int16_t)cpu->read_half(eye_address + index * 2u);
        at[index] = (int16_t)cpu->read_half(at_address + index * 2u);
    }
    direction = (XgHost3dLongVector){
        wrap_subtract(at[0], eye[0]), 0, wrap_subtract(at[2], eye[2]),
    };
    if (!xg_host_3d_vector_normal(&direction, &normalized))
        return reject_projected(63u, services);
    product = wrap_multiply(normalized.x, config.point_radius);
    point.x = (int16_t)(rounded_shift(product, 12u) + at[0]);
    point.y = config.point_y;
    product = wrap_multiply(normalized.z, config.point_radius);
    point.z = (int16_t)(rounded_shift(product, 12u) + at[2]);
    xg_render_runtime_capture_shadow_projection(cpu, &projection);
    memcpy(projection.rotation, matrix.rotation, sizeof(projection.rotation));
    memcpy(projection.translation, matrix.translation,
           sizeof(projection.translation));
    if (!xg_host_3d_rtps(
            &projection, &point, &first_projection, &projection_flags))
        return reject_projected(63u, services);

    if (config.fade_divisor == 0) {
        fade = 0;
    } else {
        uint32_t length_squared = 0u;
        for (index = 0u; index < 3u; ++index) {
            const int32_t delta = wrap_subtract(at[index], eye[index]);
            length_squared += (uint32_t)wrap_multiply(delta, delta);
        }
        if (!square_root0(cpu, length_squared, &distance) ||
            !projected_divide(
                wrap_subtract(distance, config.fade_offset),
                config.fade_divisor, &quotient))
            return reject_projected(64u, services);
        fade = xg_render_runtime_low_s16((uint32_t)quotient);
        if (fade < 0) fade = 0;
        if (fade > 0x100) fade = 0x100;
    }
    if (!ratan2_guest(cpu, direction.x, direction.z, &angle))
        return reject_projected(64u, services);
    product = wrap_multiply(config.strip_width, config.phase_multiplier);
    product = wrap_multiply(product, angle & 0xfff);
    phase = rounded_shift(product, 12u);
    gpu_get_draw_state(&draw);
    memset(records, 0, sizeof(records));
    memset(temporal_strips, 0, sizeof(temporal_strips));
    if (!build_strips(
            cpu, source, &config, &draw, buffer_index, phase,
            first_projection.y, fade, records, &record_count, false, services))
        return reject_projected(65u, services);
    strip_record_count = record_count;
    if (strip_record_count == 0u &&
        !build_strips(
            cpu, source, &config, &draw, buffer_index, phase,
            first_projection.y, fade, temporal_strips,
            &temporal_strip_count, true, services))
        return reject_projected(65u, services);

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
            if (!append_record(
                    cpu, records, &record_count, source, &draw, &record,
                    services))
                return reject_projected(66u, services);
        }
        product = wrap_multiply(normalized.x, config.point_radius);
        product = wrap_multiply(rounded_shift(product, 12u), config.fixed_scale);
        point.x = (int16_t)(rounded_shift(product, 8u) + at[0]);
        product = wrap_multiply(config.point_y, config.fixed_scale);
        point.y = (int16_t)rounded_shift(product, 8u);
        product = wrap_multiply(normalized.z, config.point_radius);
        product = wrap_multiply(rounded_shift(product, 12u), config.fixed_scale);
        point.z = (int16_t)(rounded_shift(product, 8u) + at[2]);
        if (!xg_host_3d_rtps(
                &projection, &point, &second_projection, &projection_flags))
            return reject_projected(63u, services);
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
            if (!append_record(
                    cpu, records, &record_count, source, &draw, &record,
                    services))
                return reject_projected(66u, services);
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
                if (!append_record(
                        cpu, records, &record_count, source, &draw, &record,
                        services))
                    return reject_projected(66u, services);
            }
        }
    }
    {
        uint32_t new_template_count = 0u;
        if (services->has_template == NULL ||
            services->available_template_capacity == NULL ||
            services->begin_lifecycle == NULL ||
            services->capture_template == NULL)
            return reject_projected(67u, services);
        for (index = 0u; index < record_count; ++index) {
            if (records[index].kind == XG_RENDER_PROJECTED_RECORD_FT4 &&
                !services->has_template(records[index].packet_address))
                ++new_template_count;
        }
        if (new_template_count > services->available_template_capacity() ||
            !services->begin_lifecycle(UINT32_C(0x800273c4), &lifecycle))
            return reject_projected(67u, services);
    }
    if (!stage_records(
            records, record_count, ot_bucket, ordering_domain,
            object_address & UINT32_C(0x1fffffff), services) ||
        !stage_temporal_strips(
            temporal_strips, temporal_strip_count, ordering_domain,
            object_address & UINT32_C(0x1fffffff), services))
        return reject_projected(67u, services);
    previous_head = cpu->read_word(ot_address);
    for (index = 0u; index < record_count; ++index) {
        uint32_t linked_head;
        write_record_payload(cpu, &records[index]);
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
    for (index = 0u; index < record_count; ++index) {
        const XgRenderProjectedNativeRecord *record = &records[index];
        XgRenderFieldSpriteTemplateInput template_record = { 0 };
        uint32_t packet_offset;
        if (record->kind != XG_RENDER_PROJECTED_RECORD_FT4) continue;
        packet_offset = record->packet_address - object_address -
            buffer_index * 0x140u;
        template_record.primitive = record->primitive;
        template_record.lifecycle = lifecycle;
        template_record.packet_address = record->packet_address;
        template_record.interpolation_producer_id =
            object_address & UINT32_C(0x1fffffff);
        template_record.interpolation_primitive_id = packet_offset / 0x28u;
        template_record.interpolation_identity_valid = true;
        template_record.tpage = record->tpage;
        template_record.clut = (uint16_t)(
            ((source->clut_y & 0x1ffu) << 6u) |
            ((source->clut_x >> 4u) & 0x3fu));
        for (uint32_t vertex = 0u; vertex < 4u; ++vertex) {
            template_record.xy[vertex] = (uint16_t)record->x[vertex] |
                ((uint32_t)(uint16_t)record->y[vertex] << 16u);
            template_record.uv[vertex] = record->u[vertex] |
                ((uint16_t)record->v[vertex] << 8u);
        }
        if (!services->capture_template(&template_record))
            return reject_projected(67u, services);
    }
    cpu->gpr[2] = (uint32_t)(int32_t)first_projection.y;
    cpu->pc = cpu->gpr[31];
    ++projected_lifecycle.cutover_success_count;
    projected_lifecycle.primitive_count += record_count;
    return true;
}
