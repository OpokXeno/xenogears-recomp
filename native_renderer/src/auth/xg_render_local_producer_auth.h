#ifndef XG_RENDER_LOCAL_PRODUCER_AUTH_H
#define XG_RENDER_LOCAL_PRODUCER_AUTH_H

#include "xg_render_invalidation_event.h"

#include "cpu_state.h"
#include "guest_render_types.h"
#include "xg_render_runtime_variants_generated.h"

#include <stdbool.h>
#include <stdint.h>

typedef enum XgRenderLocalProducerKind {
    XG_RENDER_LOCAL_PRODUCER_NONE = 0,
    XG_RENDER_LOCAL_PRODUCER_FT4_2C,
    XG_RENDER_LOCAL_PRODUCER_FT4_2E,
    XG_RENDER_LOCAL_PRODUCER_ZOOM,
    XG_RENDER_LOCAL_PRODUCER_COUNT,
} XgRenderLocalProducerKind;

typedef struct XgRenderLocalProducerAuthContext {
    CPUState *cpu;
    uint64_t generation;
    uint64_t scene_generation;
    uint32_t pc;
    uint32_t handler_data;
    GuestRenderRenderMode render_mode;
} XgRenderLocalProducerAuthContext;

typedef struct XgRenderLocalProducerAuthHandler {
    bool (*preflight)(const XgRenderLocalProducerAuthContext *context);
    bool (*begin)(const XgRenderLocalProducerAuthContext *context);
    bool (*writer)(const XgRenderLocalProducerAuthContext *context,
                   uint32_t writer_index);
    bool (*commit)(const XgRenderLocalProducerAuthContext *context);
    void (*cancel)(void);
} XgRenderLocalProducerAuthHandler;

bool xg_render_local_producer_auth_register(
    XgRenderLocalProducerKind kind,
    const XgRenderLocalProducerAuthHandler *handler);
bool xg_render_local_producer_auth_unregister(
    XgRenderLocalProducerKind kind,
    const XgRenderLocalProducerAuthHandler *handler);
bool xg_render_local_producer_auth_begin(
    CPUState *cpu, const XgRenderRuntimeVariantCutover *cutover,
    uint64_t scene_generation, GuestRenderRenderMode render_mode);
bool xg_render_local_producer_auth_writer(
    CPUState *cpu, const XgRenderRuntimeVariantCutover *cutover,
    uint64_t scene_generation, GuestRenderRenderMode render_mode);
bool xg_render_local_producer_auth_commit(
    CPUState *cpu, const XgRenderRuntimeVariantCutover *cutover,
    uint64_t scene_generation, GuestRenderRenderMode render_mode);
uint64_t xg_render_local_producer_auth_generation_for_pc(
    uint32_t pc, uint64_t scene_generation);
bool xg_render_local_producer_auth_matches(
    uint32_t pc, uint64_t generation, uint64_t scene_generation);
bool xg_render_local_producer_auth_pending_authorizes_pc(uint32_t pc);
bool xg_render_local_producer_auth_pending(void);
void xg_render_local_producer_auth_clear_kind(XgRenderLocalProducerKind kind);
void xg_render_local_producer_auth_invalidate(uint32_t address, uint32_t size);
void xg_render_local_producer_auth_clear(void);
void xg_render_local_producer_auth_handle_invalidation(
    const XgRenderInvalidationEvent *event,
    const XgRenderInvalidationServices *services);

#endif
