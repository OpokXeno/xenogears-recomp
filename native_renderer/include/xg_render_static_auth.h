#ifndef XG_RENDER_STATIC_AUTH_H
#define XG_RENDER_STATIC_AUTH_H

#include "guest_render_bridge.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifndef XG_RENDER_STATIC_AUTH_TRACE_CAPACITY
#define XG_RENDER_STATIC_AUTH_TRACE_CAPACITY 8u
#endif

#ifdef __cplusplus
extern "C" {
#endif

typedef enum XgRenderStaticAuthResult {
    XG_RENDER_STATIC_AUTH_OK = 0,
    XG_RENDER_STATIC_AUTH_INVALID_ARGUMENT = 1,
} XgRenderStaticAuthResult;

typedef enum XgRenderStaticAuthStatus {
    XG_RENDER_STATIC_AUTH_UNINITIALIZED = 0,
    XG_RENDER_STATIC_AUTH_IDENTITY_MISMATCH = 1,
    XG_RENDER_STATIC_AUTH_TRACE_UNPROVEN = 2,
    XG_RENDER_STATIC_AUTH_SITE_MISMATCH = 3,
    XG_RENDER_STATIC_AUTH_DELAY_SLOT_MISMATCH = 4,
    XG_RENDER_STATIC_AUTH_AUTH_REJECTED = 5,
    XG_RENDER_STATIC_AUTH_SELECTED = 6,
} XgRenderStaticAuthStatus;

typedef enum XgRenderStaticAuthHook {
    XG_RENDER_STATIC_AUTH_HOOK_ENTRY = 0,
    XG_RENDER_STATIC_AUTH_HOOK_CAPTURE = 1,
    XG_RENDER_STATIC_AUTH_HOOK_RETURN = 2,
} XgRenderStaticAuthHook;

typedef struct XgRenderStaticAuthTraceEvent {
    uint64_t sequence;
    XgRenderStaticAuthHook hook;
    uint32_t producer_entry;
    uint32_t caller_site;
    uint32_t callee_entry;
    uint32_t return_site;
    bool accepted;
} XgRenderStaticAuthTraceEvent;

typedef struct XgRenderStaticAuthTraceSnapshot {
    uint64_t next_sequence;
    size_t count;
    XgRenderStaticAuthTraceEvent events[XG_RENDER_STATIC_AUTH_TRACE_CAPACITY];
} XgRenderStaticAuthTraceSnapshot;

typedef struct XgRenderStaticAuthSnapshot {
    XgRenderStaticAuthStatus status;
    GuestRenderRenderMode effective_render_mode;
    bool identity_bound;
    bool identity_gate_passed;
    bool trace_proven;
    bool auth_scene_selected;
    size_t trace_count;
} XgRenderStaticAuthSnapshot;

void psx_xg_render_static_auth_entry(uint32_t producer_entry);
void psx_xg_render_static_auth_capture(uint32_t caller_site,
                                       uint32_t callee_entry,
                                       uint32_t return_site,
                                       uint32_t instruction_word,
                                       uint32_t delay_slot_word);
void psx_xg_render_static_auth_return(uint32_t return_site,
                                      uint32_t return_address);
XgRenderStaticAuthResult xg_render_static_auth_snapshot(
    XgRenderStaticAuthSnapshot *out_snapshot);
XgRenderStaticAuthResult xg_render_static_auth_trace_snapshot(
    XgRenderStaticAuthTraceSnapshot *out_snapshot);
const char *xg_render_static_auth_status_name(XgRenderStaticAuthStatus status);

#ifdef __cplusplus
}
#endif

#endif
