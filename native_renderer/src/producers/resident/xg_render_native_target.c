#include "xg_render_native_target.h"

#include "cpu_state.h"

#include <stddef.h>
#include <stdlib.h>
#include <string.h>

enum {
    NATIVE_TARGET_INITIAL_CAPACITY = 128u,
    /* At most one declaration per aligned command address in main RAM. */
    NATIVE_TARGET_MAX_CAPACITY = 0x80000u,
};

typedef struct XgRenderNativeTargetEntry {
    XgRenderNativeTargetRect rect;
    int16_t offset_x;
    int16_t offset_y;
    uint32_t command_id;
    uint32_t command_address;
    uint32_t producer_entry;
    bool occupied;
} XgRenderNativeTargetEntry;

static XgRenderNativeTargetEntry *targets;
static uint32_t target_capacity;
static XgRenderNativeTargetServices target_services;
/* Guest-owner diagnostics remain inspectable across template resets. */
static volatile struct {
    uint64_t declarations, takes, accepted, missing, authority_rejected, payload_rejected, resets;
    uint32_t last_declared_command, last_taken_command, last_accepted_command, last_rejected_command;
    XgRenderNativeTargetRect last_declared_rect, last_accepted_rect;
} target_diagnostics;

void xg_render_native_target_reset(void) {
    free(targets);
    targets = NULL;
    target_capacity = 0u;
    target_services = (XgRenderNativeTargetServices){0};
    target_diagnostics.resets++;
}

bool xg_render_native_target_observe(
        CPUState *cpu, uint32_t pc, uint32_t instruction_word,
        const XgRenderNativeTargetServices *services) {
    XgRenderNativeTargetEntry *entry = NULL;
    XgRenderNativeTargetEntry *available = NULL;
    uint32_t destination, source, command_id, producer;
    int32_t x, y, width, height;

    if (cpu == NULL || services == NULL ||
        cpu->read_half == NULL || services->read_word == NULL ||
        services->native_text_authorizes_pc == NULL ||
        services->guest_data_range_is_valid == NULL || services->generation == NULL)
        return false;
    producer = (pc & UINT32_C(0x1fffffff)) | UINT32_C(0x80000000);
    if ((producer != UINT32_C(0x80045534) && producer != UINT32_C(0x8004574c)) ||
        instruction_word != UINT32_C(0x27bdffc0))
        return false;
    if (services->generation() == 0u) {
        xg_render_native_target_reset();
        return false;
    }
    if (target_services.read_word != services->read_word ||
        target_services.native_text_authorizes_pc != services->native_text_authorizes_pc ||
        target_services.guest_data_range_is_valid != services->guest_data_range_is_valid ||
        target_services.generation != services->generation) {
        xg_render_native_target_reset();
        target_services = *services;
    }
    destination = cpu->gpr[4];
    source = cpu->gpr[5];
    if (destination > UINT32_MAX - 16u ||
        !services->guest_data_range_is_valid(destination, 16u, 4u, false)) return false;
    command_id = (destination + 4u) & UINT32_C(0x001ffffc);
    for (uint32_t index = 0u; index < target_capacity; ++index) {
        if (targets[index].occupied && targets[index].command_id == command_id) {
            entry = &targets[index];
            break;
        }
        if (!targets[index].occupied && available == NULL) available = &targets[index];
    }
    if (entry == NULL) entry = available;
    if (entry == NULL) {
        if (target_capacity == NATIVE_TARGET_MAX_CAPACITY) return false;
        const uint32_t capacity = target_capacity != 0u
            ? target_capacity * 2u : NATIVE_TARGET_INITIAL_CAPACITY;
        XgRenderNativeTargetEntry *grown = realloc(
            targets, (size_t)capacity * sizeof(*targets));
        if (grown == NULL) return false;
        targets = grown;
        memset(targets + target_capacity, 0,
            (size_t)(capacity - target_capacity) * sizeof(*targets));
        entry = &targets[target_capacity];
        target_capacity = capacity;
    }
    /* Re-execution supersedes the old declaration even if the new one fails. */
    entry->occupied = false;
    if (source > UINT32_MAX - 12u || !services->native_text_authorizes_pc(producer) ||
        !services->guest_data_range_is_valid(source, 12u, 2u, false))
        return false;
    x = (int16_t)cpu->read_half(source);
    y = (int16_t)cpu->read_half(source + 2u);
    width = (int16_t)cpu->read_half(source + 4u);
    height = (int16_t)cpu->read_half(source + 6u);
    if (x < 0 || y < 0 || width <= 0 || height <= 0 ||
        x >= 1024 || y >= 512 || width > 1024 - x || height > 512 - y)
        return false;
    *entry = (XgRenderNativeTargetEntry){
        .rect = {(uint16_t)x, (uint16_t)y, (uint16_t)width, (uint16_t)height},
        .offset_x = (int16_t)cpu->read_half(source + 8u),
        .offset_y = (int16_t)cpu->read_half(source + 10u),
        .command_id = command_id,
        .command_address = destination + 4u,
        .producer_entry = producer,
        .occupied = true,
    };
    target_diagnostics.declarations++;
    target_diagnostics.last_declared_command = command_id;
    target_diagnostics.last_declared_rect = entry->rect;
    return true;
}

bool xg_render_native_target_take(uint32_t command_id,
                                XgRenderNativeOperation *out_operation) {
    target_diagnostics.takes++;
    target_diagnostics.last_taken_command = command_id;
    if (out_operation == NULL || command_id < 4u ||
        (command_id & ~UINT32_C(0x001ffffc)) != 0u ||
        target_services.generation == NULL ||
        target_services.native_text_authorizes_pc == NULL ||
        target_services.read_word == NULL)
        return false;
    if (target_services.generation() == 0u) {
        xg_render_native_target_reset();
        return false;
    }
    for (uint32_t index = 0u; index < target_capacity; ++index) {
        XgRenderNativeTargetEntry *entry = &targets[index];
        if (!entry->occupied || entry->command_id != command_id) continue;
        /* The RAM allocation can be recycled without calling SetDrawEnv again.
         * Validate the SDK's E3/E4/E5 together at acceptance, never at its entry
         * hook (where the destination still contains the previous packet). */
        const uint32_t e3 = UINT32_C(0xe3000000) | entry->rect.x |
            ((uint32_t)entry->rect.y << 10u);
        const uint32_t e4 = UINT32_C(0xe4000000) |
            (entry->rect.x + entry->rect.width - 1u) |
            ((uint32_t)(entry->rect.y + entry->rect.height - 1u) << 10u);
        const uint32_t e5 = UINT32_C(0xe5000000) |
            ((uint32_t)entry->offset_x & 0x7ffu) |
            (((uint32_t)entry->offset_y & 0x7ffu) << 11u);
        if (!target_services.native_text_authorizes_pc(entry->producer_entry)) {
            target_diagnostics.authority_rejected++;
            target_diagnostics.last_rejected_command = command_id;
            entry->occupied = false;
            return false;
        }
        if (!target_services.guest_data_range_is_valid(
                entry->command_address, 12u, 4u, false) ||
            target_services.read_word(entry->command_address) != e3 ||
            target_services.read_word(entry->command_address + 4u) != e4 ||
            target_services.read_word(entry->command_address + 8u) != e5) {
            target_diagnostics.payload_rejected++;
            target_diagnostics.last_rejected_command = command_id;
            entry->occupied = false;
            return false;
        }
        /* Offsets are not the target origin. The accepted E5 supplies material
         * offsets; widening policy must use only this explicit source extent. */
        *out_operation = (XgRenderNativeOperation){
            .kind = XG_RENDER_NATIVE_OPERATION_TARGET,
            .dst_x = entry->rect.x,
            .dst_y = entry->rect.y,
            .width = entry->rect.width,
            .height = entry->rect.height,
        };
        target_diagnostics.accepted++;
        target_diagnostics.last_accepted_command = command_id;
        target_diagnostics.last_accepted_rect = entry->rect;
        return true;
    }
    target_diagnostics.missing++;
    return false;
}
