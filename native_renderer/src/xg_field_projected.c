#include "xg_field_projected.h"

#include "cpu_state.h"
#include "guest_render_bridge.h"
#include "xg_field_render_services.h"

#include <stddef.h>
#include <string.h>

static XgRenderProjectedSourceState projected_sources = {
    .next_generation = 1u,
};
static XgRenderProjectedInitializerPending projected_initializer_pending;

void xg_field_projected_reset(void) {
    projected_sources = (XgRenderProjectedSourceState){
        .next_generation = 1u,
    };
    xg_field_projected_reset_pending();
}

void xg_field_projected_reset_pending(void) {
    projected_initializer_pending =
        (XgRenderProjectedInitializerPending){ 0 };
}

XgRenderProjectedSourceState *xg_field_projected_state(void) {
    return &projected_sources;
}

XgRenderProjectedInitializerPending *xg_field_projected_initializer_pending(void) {
    return &projected_initializer_pending;
}

XgRenderProjectedSource *xg_field_projected_find(uint32_t object_address) {
    uint32_t index;

    for (index = 0u; index < XG_RENDER_PROJECTED_SOURCE_CAPACITY; ++index) {
        XgRenderProjectedSource *source = &projected_sources.records[index];
        if (source->valid && source->object_address == object_address)
            return source;
    }
    return NULL;
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
    *source = (XgRenderProjectedSource){
        .generation = projected_sources.next_generation++,
        .object_address = object_address,
        .clut_x = projected_initializer_pending.clut_x,
        .clut_y = projected_initializer_pending.clut_y,
        .valid = true,
    };
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
