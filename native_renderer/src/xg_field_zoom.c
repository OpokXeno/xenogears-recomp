#include "xg_field_zoom.h"

#include "cpu_state.h"
#include "xg_field_render_services.h"

#include <stddef.h>

static XgRenderZoomSource zoom_source;
static XgRenderZoomInitializerPending zoom_initializer_pending;
static XgRenderZoomRgbPending zoom_rgb_pending;
static XgRenderZoomInvocation zoom_invocation;
static XgRenderZoomCounters zoom_counters;
static uint64_t zoom_next_generation = 1u;

static void materialize_zoom_source(uint8_t semi_transparent, uint8_t abr,
                                    uint64_t artifact_generation) {
    uint32_t buffer;
    uint32_t quad;

    zoom_source = (XgRenderZoomSource){
        .generation = zoom_next_generation++,
        .artifact_generation = artifact_generation,
        .producer_store_pc = XG_RENDER_ZOOM_TEMPLATE_STORE_PC,
        .command = semi_transparent ? 0x2eu : 0x2cu,
        .semi_transparent = semi_transparent != 0u,
        .authenticated = artifact_generation != 0u,
        .valid = true,
    };
    for (buffer = 0u; buffer < XG_RENDER_ZOOM_BUFFER_COUNT; ++buffer) {
        for (quad = 0u; quad < XG_RENDER_ZOOM_QUAD_COUNT; ++quad) {
            XgRenderZoomQuadSource *source = &zoom_source.quads[buffer][quad];
            const int16_t left = (int16_t)(quad * 64u);
            const int16_t right = (int16_t)((quad + 1u) * 64u);

            *source = (XgRenderZoomQuadSource){
                .projection_x = {
                    (int16_t)(-80 + (int32_t)quad * 32),
                    (int16_t)(-48 + (int32_t)quad * 32),
                    (int16_t)(-80 + (int32_t)quad * 32),
                    (int16_t)(-48 + (int32_t)quad * 32),
                },
                .projection_y = { -56, -56, 56, 56 },
                .projection_z = { 0, 0, 0, 0 },
                .x = { left, right, left, right },
                .y = { 0, 0, 223, 223 },
                .red = 0x80u,
                .green = 0x80u,
                .blue = 0x80u,
                .tpage = (uint16_t)(0x11bu + quad + ((uint32_t)abr << 5u)),
            };
        }
    }
}

void xg_field_zoom_reset(void) {
    zoom_source = (XgRenderZoomSource){ 0 };
    zoom_initializer_pending = (XgRenderZoomInitializerPending){ 0 };
    zoom_rgb_pending = (XgRenderZoomRgbPending){ 0 };
    zoom_invocation = (XgRenderZoomInvocation){ 0 };
}

void xg_field_zoom_reset_pending(void) {
    zoom_initializer_pending = (XgRenderZoomInitializerPending){ 0 };
    zoom_rgb_pending = (XgRenderZoomRgbPending){ 0 };
    zoom_invocation = (XgRenderZoomInvocation){ 0 };
}

XgRenderZoomSource *xg_field_zoom_source(void) { return &zoom_source; }
XgRenderZoomInitializerPending *xg_field_zoom_initializer_pending(void) {
    return &zoom_initializer_pending;
}
XgRenderZoomRgbPending *xg_field_zoom_rgb_pending(void) {
    return &zoom_rgb_pending;
}
XgRenderZoomInvocation *xg_field_zoom_invocation(void) {
    return &zoom_invocation;
}

void xg_field_zoom_counters_reset(void) {
    zoom_counters = (XgRenderZoomCounters){ 0 };
}

void xg_field_zoom_counters_snapshot(XgRenderZoomCounters *out_counters) {
    if (out_counters != NULL) *out_counters = zoom_counters;
}

void xg_field_zoom_note_cutover_attempt(void) {
    ++zoom_counters.cutover_attempt_count;
}

void xg_field_zoom_note_native_invocation(uint32_t primitive_count) {
    ++zoom_counters.native_invocation_count;
    zoom_counters.native_primitive_count += primitive_count;
}

void xg_field_zoom_note_replay_invocation(uint32_t primitive_count) {
    ++zoom_counters.replay_invocation_count;
    zoom_counters.replay_primitive_count += primitive_count;
}

void xg_field_zoom_note_rejection(uint32_t blocker) {
    ++zoom_counters.rejection_count;
    zoom_counters.last_rejection_blocker = blocker;
}

bool xg_field_zoom_caller_is_authorized(uint32_t return_address) {
    static const uint32_t callers[] = {
        UINT32_C(0x80078f00), UINT32_C(0x80078fc0),
        UINT32_C(0x80079170), UINT32_C(0x80079204),
        UINT32_C(0x800a58ac), UINT32_C(0x800a58f8),
        UINT32_C(0x800a5ac8), UINT32_C(0x800a5b94),
        UINT32_C(0x800a5bf4), UINT32_C(0x800a5e68),
        UINT32_C(0x800a5f74), UINT32_C(0x800a6090),
        UINT32_C(0x800a6158), UINT32_C(0x800a61f4),
        UINT32_C(0x800a6280), UINT32_C(0x800a62c8),
        UINT32_C(0x800a6384),
    };
    uint32_t index;

    for (index = 0u; index < sizeof(callers) / sizeof(callers[0]); ++index)
        if ((return_address & 0x1fffffffu) == (callers[index] & 0x1fffffffu))
            return true;
    return false;
}

static bool initializer_caller_is_authorized(uint32_t return_address) {
    static const uint32_t callers[] = {
        UINT32_C(0x800a5898), UINT32_C(0x800a5b2c),
        UINT32_C(0x800a5e1c), UINT32_C(0x800a5f28),
        UINT32_C(0x800a61dc), UINT32_C(0x800a62b0),
    };
    uint32_t index;

    for (index = 0u; index < sizeof(callers) / sizeof(callers[0]); ++index)
        if ((return_address & 0x1fffffffu) == (callers[index] & 0x1fffffffu))
            return true;
    return false;
}

void xg_field_zoom_observe_initializer_begin(
    CPUState *cpu, GuestRenderRenderMode render_mode,
    uint64_t artifact_generation) {
    ++zoom_counters.initializer_begin_count;
    zoom_source = (XgRenderZoomSource){ 0 };
    zoom_initializer_pending = (XgRenderZoomInitializerPending){ 0 };
    zoom_rgb_pending = (XgRenderZoomRgbPending){ 0 };
    if (render_mode == GUEST_RENDER_RENDER_ORIGINAL || cpu == NULL ||
        !xg_render_runtime_stack_address_is_valid(cpu->gpr[29]) ||
        !((cpu->gpr[4] == 0u && cpu->gpr[5] == 0u) ||
          (cpu->gpr[4] == 1u && cpu->gpr[5] == 1u)) ||
        artifact_generation == 0u ||
        !initializer_caller_is_authorized(cpu->gpr[31]))
        return;
    zoom_initializer_pending = (XgRenderZoomInitializerPending){
        .entry_sp = cpu->gpr[29],
        .return_address = cpu->gpr[31],
        .semi_transparent = (uint8_t)cpu->gpr[4],
        .abr = (uint8_t)cpu->gpr[5],
        .artifact_generation = artifact_generation,
        .artifact_authorized = true,
        .valid = true,
    };
}

void xg_field_zoom_observe_initializer_commit(
    CPUState *cpu, uint64_t artifact_generation) {
    if (cpu == NULL || cpu->read_word == NULL ||
        !zoom_initializer_pending.valid || zoom_next_generation == 0u ||
        zoom_initializer_pending.entry_sp < 0x68u ||
        cpu->gpr[29] != zoom_initializer_pending.entry_sp - 0x68u ||
        artifact_generation == 0u ||
        artifact_generation != zoom_initializer_pending.artifact_generation ||
        !xg_render_runtime_stack_address_is_valid(cpu->gpr[29]) ||
        cpu->read_word(cpu->gpr[29] + 0x64u) !=
            zoom_initializer_pending.return_address) {
        zoom_source = (XgRenderZoomSource){ 0 };
        zoom_initializer_pending = (XgRenderZoomInitializerPending){ 0 };
        return;
    }
    materialize_zoom_source(
        zoom_initializer_pending.semi_transparent, zoom_initializer_pending.abr,
        artifact_generation);
    ++zoom_counters.initializer_commit_count;
    if (zoom_source.command == 0x2eu) ++zoom_counters.initializer_2e_count;
    zoom_initializer_pending = (XgRenderZoomInitializerPending){ 0 };
}

void xg_field_zoom_observe_rgb_begin(
    CPUState *cpu, GuestRenderRenderMode render_mode,
    uint64_t artifact_generation) {
    uint32_t current_buffer;

    zoom_rgb_pending = (XgRenderZoomRgbPending){ 0 };
    if (render_mode == GUEST_RENDER_RENDER_ORIGINAL || cpu == NULL ||
        cpu->read_word == NULL || artifact_generation == 0u ||
        artifact_generation != zoom_source.artifact_generation ||
        !zoom_source.valid ||
        !zoom_source.authenticated)
        return;
    current_buffer = cpu->read_word(UINT32_C(0x800adb08));
    if (current_buffer >= XG_RENDER_ZOOM_BUFFER_COUNT) return;
    zoom_rgb_pending = (XgRenderZoomRgbPending){
        .generation = zoom_source.generation,
        .intensity = (uint8_t)cpu->gpr[4],
        .buffer_index = (uint8_t)((current_buffer + 1u) & 1u),
        .valid = true,
    };
}

void xg_field_zoom_observe_rgb_commit(CPUState *cpu,
                                      uint64_t artifact_generation) {
    uint32_t current_buffer;
    uint32_t quad;

    if (cpu != NULL && cpu->gpr[6] < XG_RENDER_ZOOM_QUAD_COUNT) return;
    if (cpu == NULL || cpu->read_word == NULL || !zoom_rgb_pending.valid ||
        artifact_generation == 0u ||
        artifact_generation != zoom_source.artifact_generation ||
        !zoom_source.valid || zoom_rgb_pending.generation != zoom_source.generation ||
        cpu->gpr[6] != XG_RENDER_ZOOM_QUAD_COUNT) {
        zoom_source = (XgRenderZoomSource){ 0 };
        zoom_rgb_pending = (XgRenderZoomRgbPending){ 0 };
        return;
    }
    current_buffer = cpu->read_word(UINT32_C(0x800adb08));
    if (current_buffer >= XG_RENDER_ZOOM_BUFFER_COUNT ||
        ((current_buffer + 1u) & 1u) != zoom_rgb_pending.buffer_index) {
        zoom_source = (XgRenderZoomSource){ 0 };
        zoom_rgb_pending = (XgRenderZoomRgbPending){ 0 };
        return;
    }
    for (quad = 0u; quad < XG_RENDER_ZOOM_QUAD_COUNT; ++quad) {
        XgRenderZoomQuadSource *source =
            &zoom_source.quads[zoom_rgb_pending.buffer_index][quad];
        source->red = zoom_rgb_pending.intensity;
        source->green = zoom_rgb_pending.intensity;
        source->blue = zoom_rgb_pending.intensity;
    }
    ++zoom_counters.rgb_update_count;
    zoom_rgb_pending = (XgRenderZoomRgbPending){ 0 };
}

void xg_field_zoom_observe_entry(
    CPUState *cpu, GuestRenderRenderMode render_mode,
    uint64_t artifact_generation) {
    zoom_invocation = (XgRenderZoomInvocation){ 0 };
    if (render_mode == GUEST_RENDER_RENDER_ORIGINAL || cpu == NULL ||
        artifact_generation == 0u ||
        artifact_generation != zoom_source.artifact_generation ||
        !zoom_source.valid ||
        !zoom_source.authenticated ||
        !xg_render_runtime_stack_address_is_valid(cpu->gpr[29]) ||
        !xg_field_zoom_caller_is_authorized(cpu->gpr[31]))
        return;
    zoom_invocation = (XgRenderZoomInvocation){
        .source_generation = zoom_source.generation,
        .entry_sp = cpu->gpr[29],
        .return_address = cpu->gpr[31],
        .valid = true,
    };
    ++zoom_counters.invocation_count;
}
