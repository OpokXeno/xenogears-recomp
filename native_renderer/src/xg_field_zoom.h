#ifndef XG_FIELD_ZOOM_H
#define XG_FIELD_ZOOM_H

#include "guest_render_bridge.h"
#include "xg_render_ir.h"

#include <stdbool.h>
#include <stdint.h>

typedef struct CPUState CPUState;

enum {
    XG_RENDER_ZOOM_QUAD_COUNT = 5u,
    XG_RENDER_ZOOM_BUFFER_COUNT = 2u,
    XG_RENDER_ZOOM_OT_BUCKET = 0x2002u,
    XG_RENDER_ZOOM_TEMPLATE_STORE_PC = 0x800a6884u,
};

typedef struct XgRenderZoomQuadSource {
    int16_t projection_x[4];
    int16_t projection_y[4];
    int16_t projection_z[4];
    int16_t x[4];
    int16_t y[4];
    uint8_t red;
    uint8_t green;
    uint8_t blue;
    uint16_t tpage;
} XgRenderZoomQuadSource;

typedef struct XgRenderZoomSource {
    XgRenderZoomQuadSource quads[XG_RENDER_ZOOM_BUFFER_COUNT]
                              [XG_RENDER_ZOOM_QUAD_COUNT];
    uint64_t generation;
    uint64_t artifact_generation;
    uint32_t producer_store_pc;
    uint8_t command;
    bool semi_transparent;
    bool authenticated;
    bool valid;
} XgRenderZoomSource;

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

typedef struct XgRenderZoomNativeRecord {
    XgRenderIrNativePrimitive primitive;
    int16_t x[4];
    int16_t y[4];
    uint32_t packet_address;
    uint32_t draw_mode_address;
    uint32_t packet_tag;
    uint32_t draw_mode_tag;
} XgRenderZoomNativeRecord;

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

void xg_field_zoom_reset(void);
void xg_field_zoom_reset_pending(void);
XgRenderZoomSource *xg_field_zoom_source(void);
XgRenderZoomInitializerPending *xg_field_zoom_initializer_pending(void);
XgRenderZoomRgbPending *xg_field_zoom_rgb_pending(void);
XgRenderZoomInvocation *xg_field_zoom_invocation(void);
void xg_field_zoom_counters_reset(void);
void xg_field_zoom_counters_snapshot(XgRenderZoomCounters *out_counters);
void xg_field_zoom_note_cutover_attempt(void);
void xg_field_zoom_note_native_invocation(uint32_t primitive_count);
void xg_field_zoom_note_replay_invocation(uint32_t primitive_count);
void xg_field_zoom_note_rejection(uint32_t blocker);
bool xg_field_zoom_caller_is_authorized(uint32_t return_address);
void xg_field_zoom_observe_initializer_begin(
    CPUState *cpu, GuestRenderRenderMode render_mode, uint64_t artifact_generation);
void xg_field_zoom_observe_initializer_commit(
    CPUState *cpu, uint64_t artifact_generation);
void xg_field_zoom_observe_rgb_begin(
    CPUState *cpu, GuestRenderRenderMode render_mode, uint64_t artifact_generation);
void xg_field_zoom_observe_rgb_commit(CPUState *cpu, uint64_t artifact_generation);
void xg_field_zoom_observe_entry(
    CPUState *cpu, GuestRenderRenderMode render_mode, uint64_t artifact_generation);

#endif
