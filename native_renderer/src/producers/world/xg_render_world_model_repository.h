#ifndef XG_RENDER_WORLD_MODEL_REPOSITORY_H
#define XG_RENDER_WORLD_MODEL_REPOSITORY_H

#include "cpu_state.h"
#include "guest_render_types.h"
#include "xg_render_snapshot_types.h"
#include "xg_world_models_native.h"
#include "xg_render_invalidation_event.h"

#include <stdbool.h>
#include <stdint.h>

enum {
    XG_RENDER_WORLD_MODEL_TEMPLATE_CAPACITY = 16384u,
};

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

typedef struct XgRenderWorldModelRepositoryServices {
    bool (*authentication_generation)(uint64_t *out_generation);
    bool (*authorize_guest_range)(uint32_t address, uint32_t size,
                                  uint32_t alignment, bool allow_scratchpad);
    bool (*stack_address_is_valid)(uint32_t address);
    bool (*coordinator_in_progress)(void);
    void (*coordinator_fail)(void);
} XgRenderWorldModelRepositoryServices;

typedef struct XgRenderWorldModelRepositoryReaderContext {
    CPUState *cpu;
    const XgRenderWorldModelRepositoryServices *services;
} XgRenderWorldModelRepositoryReaderContext;

void xg_render_world_model_repository_initializer_begin(
    CPUState *cpu, GuestRenderRenderMode render_mode,
    const XgRenderWorldModelRepositoryServices *services);
void xg_render_world_model_repository_initializer_finish(
    CPUState *cpu, const XgRenderWorldModelRepositoryServices *services);
void xg_render_world_model_repository_observe_initializer_success(CPUState *cpu);
void xg_render_world_model_repository_observe_color_writes(
    CPUState *cpu, uint32_t pc,
    const XgRenderWorldModelRepositoryServices *services);
void xg_render_world_model_repository_begin_packet_copy(
    CPUState *cpu, GuestRenderRenderMode render_mode,
    const XgRenderWorldModelRepositoryServices *services);
void xg_render_world_model_repository_finish_packet_copy(
    CPUState *cpu, GuestRenderRenderMode render_mode,
    const XgRenderWorldModelRepositoryServices *services);

bool xg_render_world_model_repository_read_packet_template(
    void *context, uint32_t model_header_address,
    uint32_t packet_base_address, uint32_t packet_address,
    uint32_t attribute_address, uint8_t primitive_family,
    uint32_t *out_words, uint8_t word_count, uint64_t *out_resource_epoch);
uint32_t xg_render_world_model_repository_template_read_failure(void);
void xg_render_world_model_repository_clear_template_read_failure(void);
const XgRenderWorldModelTemplate *
xg_render_world_model_repository_find_template(
    uint32_t packet_address);
bool xg_render_world_model_repository_update_template_words(
    uint32_t packet_address, CPUState *owner_cpu, uint64_t resource_epoch,
    const uint32_t *words, uint8_t word_count, uint32_t word_write_mask);

void xg_render_world_model_repository_invalidate(void);
void xg_render_world_model_repository_invalidate_initializer(void);
void xg_render_world_model_repository_snapshot(
    PsxXgRenderWorldNativeSnapshot *out_snapshot);
void xg_render_world_model_repository_reset(void);
void xg_render_world_model_repository_handle_invalidation(
    const XgRenderInvalidationEvent *event,
    const XgRenderInvalidationServices *services);

#endif
