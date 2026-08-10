#include "xg_world_minimap_shadow.h"

#include "xg_world_minimap.h"
#include "xg_world_minimap_source_capture.h"

#include <limits.h>
#include <stddef.h>
#include <string.h>

enum {
    MINIMAP_STACK_FRAME_SIZE = 0x38u,
    MINIMAP_CONTEXT_OT_OFFSET = 0x70u,
    MINIMAP_MARKER_WORDS = 4u,
};

#define MINIMAP_CONTEXT_GLOBAL UINT32_C(0x8009be3c)

typedef struct XgWorldMinimapShadowState {
    XgWorldMinimapCapture capture;
    XgWorldMinimapBuildOutput build;
    XgWorldMinimapShadowSnapshot snapshot;
    uint32_t initial_marker_words[XG_WORLD_MINIMAP_MARKER_CAPACITY]
                                 [MINIMAP_MARKER_WORDS];
    uint32_t initial_scratch_angle_z_word;
    uint32_t initial_scratch_rotation_tail_word;
    uint32_t entry_stack_pointer;
    uint32_t entry_return_address;
    uint32_t context;
    uint32_t ot_address;
    uint32_t initial_ot_word;
} XgWorldMinimapShadowState;

static XgWorldMinimapShadowState minimap_shadow;

static bool add_counter(uint64_t *counter, uint64_t amount) {
    if (*counter > UINT64_MAX - amount) {
        minimap_shadow.snapshot.counter_overflowed = true;
        return false;
    }
    *counter += amount;
    return true;
}

static bool word_address_is_valid(uint32_t address) {
    const uint32_t segment = address & UINT32_C(0xe0000000);
    const uint32_t physical = address & UINT32_C(0x1fffffff);
    const bool valid_segment = segment == 0u ||
        segment == UINT32_C(0x80000000) ||
        segment == UINT32_C(0xa0000000);

    return valid_segment && (physical & 3u) == 0u &&
        (physical <= UINT32_C(0x001ffffc) ||
         (physical >= UINT32_C(0x1f800000) &&
          physical <= UINT32_C(0x1f8003fc)));
}

static bool stack_address_is_valid(uint32_t address) {
    const uint32_t physical = address & UINT32_C(0x1fffffff);

    return word_address_is_valid(address) &&
        (physical <= UINT32_C(0x001fffd8) ||
         (physical >= UINT32_C(0x1f800000) &&
          physical <= UINT32_C(0x1f8003d8)));
}

static void clear_pending(void) {
    XgWorldMinimapShadowSnapshot snapshot = minimap_shadow.snapshot;

    if (!snapshot.pending) return;
    snapshot.pending = false;
    memset(&minimap_shadow, 0, sizeof(minimap_shadow));
    minimap_shadow.snapshot = snapshot;
}

static void block_observer(XgWorldMinimapShadowBlocker blocker,
                           XgWorldMinimapShadowResult result) {
    XgWorldMinimapShadowSnapshot *snapshot = &minimap_shadow.snapshot;

    if (snapshot->blocked) {
        snapshot->last_result = XG_WORLD_MINIMAP_SHADOW_BLOCKED;
        clear_pending();
        return;
    }
    (void)add_counter(&snapshot->block_count, 1u);
    if (snapshot->blocker == XG_WORLD_MINIMAP_SHADOW_BLOCK_NONE)
        snapshot->blocker = blocker;
    snapshot->blocked = true;
    snapshot->last_result = result;
    clear_pending();
}

static bool read_u16(void *context, uint32_t address, uint16_t *out_value) {
    CPUState *cpu = context;

    if (cpu == NULL || cpu->read_half == NULL || out_value == NULL)
        return false;
    *out_value = cpu->read_half(address);
    return true;
}

static bool read_u32(void *context, uint32_t address, uint32_t *out_value) {
    CPUState *cpu = context;

    if (cpu == NULL || cpu->read_word == NULL || out_value == NULL)
        return false;
    *out_value = cpu->read_word(address);
    return true;
}

static uint32_t pack_xy(int16_t x, int16_t y) {
    return (uint16_t)x | ((uint32_t)(uint16_t)y << 16u);
}

static uint32_t pack_color(uint8_t command, const XgRenderIrVertex *vertex) {
    return (uint32_t)vertex->r | ((uint32_t)vertex->g << 8u) |
        ((uint32_t)vertex->b << 16u) | ((uint32_t)command << 24u);
}

static uint32_t pack_uv(uint8_t u, uint8_t v, uint16_t upper) {
    return u | ((uint32_t)v << 8u) | ((uint32_t)upper << 16u);
}

static uint16_t encode_clut(const XgRenderIrMaterialState *material) {
    return (uint16_t)(((uint32_t)material->clut_y << 6u) |
                      ((uint32_t)material->clut_x >> 4u));
}

static void record_first_mismatch(XgWorldMinimapShadowMismatchKind kind,
                                  uint32_t address, uint32_t word_index,
                                  uint32_t source_index, uint32_t expected,
                                  uint32_t actual) {
    XgWorldMinimapShadowFirstMismatch *first =
        &minimap_shadow.snapshot.first_mismatch;

    if (first->kind != XG_WORLD_MINIMAP_SHADOW_MISMATCH_NONE) return;
    *first = (XgWorldMinimapShadowFirstMismatch){
        .generation = minimap_shadow.snapshot.active_generation,
        .kind = kind,
        .address = address,
        .word_index = word_index,
        .source_index = source_index,
        .expected = expected,
        .actual = actual,
    };
}

static bool compare_word(CPUState *cpu, uint32_t address, uint32_t word_index,
                         uint32_t source_index, uint32_t expected,
                         XgWorldMinimapShadowMismatchKind kind) {
    const uint32_t actual = cpu->read_word(address);

    (void)add_counter(&minimap_shadow.snapshot.compared_word_count, 1u);
    if (actual == expected) return true;
    (void)add_counter(&minimap_shadow.snapshot.mismatched_word_count, 1u);
    record_first_mismatch(kind, address, word_index, source_index, expected,
                          actual);
    return false;
}

static bool compare_g3_payload(CPUState *cpu,
                               const XgWorldMinimapTriangleRecord *record,
                               uint32_t source_index) {
    const XgRenderIrNativePrimitive *primitive = &record->primitive;
    const uint32_t expected[6] = {
        pack_color(0x32u, &primitive->triangles[0].vertices[0]),
        pack_xy(record->screen_xy[0][0], record->screen_xy[0][1]),
        pack_color(0u, &primitive->triangles[0].vertices[1]),
        pack_xy(record->screen_xy[1][0], record->screen_xy[1][1]),
        pack_color(0u, &primitive->triangles[0].vertices[2]),
        pack_xy(record->screen_xy[2][0], record->screen_xy[2][1]),
    };
    bool matches = true;
    uint32_t word;

    for (word = 0u; word < 6u; ++word)
        matches &= compare_word(cpu, record->packet_address + 4u + word * 4u,
                                word + 1u, source_index, expected[word],
                                XG_WORLD_MINIMAP_SHADOW_MISMATCH_G3_PAYLOAD);
    return matches;
}

static bool compare_marker_payload(CPUState *cpu,
                                   const XgWorldMinimapMarkerRecord *record,
                                   uint32_t source_index) {
    const XgRenderIrVertex *vertex =
        &record->primitive.triangles[0].vertices[0];
    const uint32_t expected[3] = {
        pack_color(0x60u, vertex),
        pack_xy(record->x, record->y),
        (uint32_t)record->width | ((uint32_t)record->height << 16u),
    };
    bool matches = true;
    uint32_t word;

    for (word = 0u; word < 3u; ++word)
        matches &= compare_word(
            cpu, record->packet_address + 4u + word * 4u, word + 1u,
            source_index, expected[word],
            XG_WORLD_MINIMAP_SHADOW_MISMATCH_MARKER_PAYLOAD);
    return matches;
}

static bool compare_panel_payload(CPUState *cpu) {
    const XgWorldMinimapPanelRecord *record = &minimap_shadow.build.panel;
    const XgRenderIrMaterialState *material = &record->primitive.material;
    const XgRenderIrVertex *vertex =
        &record->primitive.triangles[0].vertices[0];
    const uint32_t expected[9] = {
        pack_color(0x2eu, vertex),
        pack_xy(record->screen_xy[0][0], record->screen_xy[0][1]),
        pack_uv(record->uv[0][0], record->uv[0][1], encode_clut(material)),
        pack_xy(record->screen_xy[1][0], record->screen_xy[1][1]),
        pack_uv(record->uv[1][0], record->uv[1][1], material->tpage),
        pack_xy(record->screen_xy[2][0], record->screen_xy[2][1]),
        pack_uv(record->uv[2][0], record->uv[2][1], 0u),
        pack_xy(record->screen_xy[3][0], record->screen_xy[3][1]),
        pack_uv(record->uv[3][0], record->uv[3][1], 0u),
    };
    bool matches = true;
    uint32_t word;

    for (word = 0u; word < 9u; ++word)
        matches &= compare_word(
            cpu, record->packet_address + 4u + word * 4u, word + 1u,
            XG_WORLD_MINIMAP_SHADOW_NO_SOURCE, expected[word],
            XG_WORLD_MINIMAP_SHADOW_MISMATCH_PANEL_PAYLOAD);
    return matches;
}

static XgWorldMinimapShadowMismatchKind tag_mismatch_kind(
    XgWorldMinimapOrderKind kind) {
    switch (kind) {
    case XG_WORLD_MINIMAP_ORDER_TRIANGLE:
        return XG_WORLD_MINIMAP_SHADOW_MISMATCH_G3_TAG;
    case XG_WORLD_MINIMAP_ORDER_DRAW_MODE:
        return XG_WORLD_MINIMAP_SHADOW_MISMATCH_DRAW_MODE_TAG;
    case XG_WORLD_MINIMAP_ORDER_MARKER:
        return XG_WORLD_MINIMAP_SHADOW_MISMATCH_MARKER_TAG;
    case XG_WORLD_MINIMAP_ORDER_PANEL:
        return XG_WORLD_MINIMAP_SHADOW_MISMATCH_PANEL_TAG;
    }
    return XG_WORLD_MINIMAP_SHADOW_MISMATCH_OT_HEAD;
}

static bool compare_tags_and_ot(CPUState *cpu, bool tag_matches[],
                                bool *out_ot_matches) {
    const XgWorldMinimapBuildOutput *build = &minimap_shadow.build;
    uint32_t predecessor = minimap_shadow.initial_ot_word;
    uint32_t event;
    bool all_tags_match = true;

    for (event = 0u; event < build->ordering_count; ++event) {
        const XgWorldMinimapOrderEvent *order = &build->ordering[event];
        const uint32_t expected =
            ((uint32_t)order->payload_word_count << 24u) |
            (predecessor & UINT32_C(0x00ffffff));

        tag_matches[event] = compare_word(
            cpu, order->packet_address, 0u, order->source_index, expected,
            tag_mismatch_kind(order->kind));
        if (!tag_matches[event])
            (void)add_counter(&minimap_shadow.snapshot.tag_mismatch_count, 1u);
        all_tags_match &= tag_matches[event];
        predecessor = order->packet_address;
    }
    *out_ot_matches = compare_word(
        cpu, minimap_shadow.ot_address, 0u,
        XG_WORLD_MINIMAP_SHADOW_NO_SOURCE,
        (minimap_shadow.initial_ot_word & UINT32_C(0xff000000)) |
            (predecessor & UINT32_C(0x00ffffff)),
        XG_WORLD_MINIMAP_SHADOW_MISMATCH_OT_HEAD);
    if (!*out_ot_matches)
        (void)add_counter(&minimap_shadow.snapshot.ot_mismatch_count, 1u);
    return all_tags_match;
}

static bool compare_scratch(CPUState *cpu) {
    const XgWorldMinimapScratchUpdates *scratch = &minimap_shadow.build.scratch;
    const uint32_t expected[10] = {
        (uint16_t)scratch->angle_x |
            ((uint32_t)(uint16_t)scratch->angle_y << 16u),
        (minimap_shadow.initial_scratch_angle_z_word &
         UINT32_C(0xffff0000)) |
            scratch->angle_z,
        (uint16_t)scratch->rotation[0][0] |
            ((uint32_t)(uint16_t)scratch->rotation[0][1] << 16u),
        (uint16_t)scratch->rotation[0][2] |
            ((uint32_t)(uint16_t)scratch->rotation[1][0] << 16u),
        (uint16_t)scratch->rotation[1][1] |
            ((uint32_t)(uint16_t)scratch->rotation[1][2] << 16u),
        (uint16_t)scratch->rotation[2][0] |
            ((uint32_t)(uint16_t)scratch->rotation[2][1] << 16u),
        (minimap_shadow.initial_scratch_rotation_tail_word &
         UINT32_C(0xffff0000)) |
            (uint16_t)scratch->rotation[2][2],
        (uint32_t)scratch->translation[0],
        (uint32_t)scratch->translation[1],
        (uint32_t)scratch->translation[2],
    };
    const uint32_t address[10] = {
        XG_WORLD_MINIMAP_SCRATCH_ANGLE_ADDRESS,
        XG_WORLD_MINIMAP_SCRATCH_ANGLE_ADDRESS + 4u,
        XG_WORLD_MINIMAP_SCRATCH_ROTATION_ADDRESS,
        XG_WORLD_MINIMAP_SCRATCH_ROTATION_ADDRESS + 4u,
        XG_WORLD_MINIMAP_SCRATCH_ROTATION_ADDRESS + 8u,
        XG_WORLD_MINIMAP_SCRATCH_ROTATION_ADDRESS + 12u,
        XG_WORLD_MINIMAP_SCRATCH_ROTATION_ADDRESS + 16u,
        XG_WORLD_MINIMAP_SCRATCH_TRANSLATION_ADDRESS,
        XG_WORLD_MINIMAP_SCRATCH_TRANSLATION_ADDRESS + 4u,
        XG_WORLD_MINIMAP_SCRATCH_TRANSLATION_ADDRESS + 8u,
    };
    bool matches = true;
    uint32_t word;

    for (word = 0u; word < 10u; ++word) {
        const bool word_matches = compare_word(
            cpu, address[word], word,
            XG_WORLD_MINIMAP_SHADOW_NO_SOURCE, expected[word],
            XG_WORLD_MINIMAP_SHADOW_MISMATCH_SCRATCH);

        if (!word_matches)
            (void)add_counter(&minimap_shadow.snapshot.scratch_mismatch_count,
                              1u);
        matches &= word_matches;
    }
    return matches;
}

static bool compare_inactive_markers(CPUState *cpu) {
    bool all_match = true;
    uint32_t marker;

    for (marker = 0u; marker < XG_WORLD_MINIMAP_MARKER_CAPACITY; ++marker) {
        const XgWorldMinimapMarkerRecord *record =
            &minimap_shadow.build.markers[marker];
        bool marker_matches = true;
        uint32_t word;

        if (record->active) continue;
        (void)add_counter(&minimap_shadow.snapshot.inactive_marker_count, 1u);
        for (word = 0u; word < MINIMAP_MARKER_WORDS; ++word)
            marker_matches &= compare_word(
                cpu, record->packet_address + word * 4u, word, marker,
                minimap_shadow.initial_marker_words[marker][word],
                XG_WORLD_MINIMAP_SHADOW_MISMATCH_INACTIVE_MARKER_MUTATION);
        if (marker_matches)
            (void)add_counter(
                &minimap_shadow.snapshot.inactive_marker_match_count, 1u);
        else
            (void)add_counter(
                &minimap_shadow.snapshot.inactive_marker_mutation_count, 1u);
        all_match &= marker_matches;
    }
    return all_match;
}

static bool capture_context_and_immutables(CPUState *cpu) {
    uint32_t marker;

    minimap_shadow.context = cpu->read_word(MINIMAP_CONTEXT_GLOBAL);
    if (minimap_shadow.context > UINT32_MAX - MINIMAP_CONTEXT_OT_OFFSET ||
        !word_address_is_valid(minimap_shadow.context +
                               MINIMAP_CONTEXT_OT_OFFSET))
        return false;
    minimap_shadow.ot_address = cpu->read_word(
        minimap_shadow.context + MINIMAP_CONTEXT_OT_OFFSET);
    if (!word_address_is_valid(minimap_shadow.ot_address)) return false;
    minimap_shadow.initial_ot_word = cpu->read_word(minimap_shadow.ot_address);

    for (marker = 0u; marker < XG_WORLD_MINIMAP_MARKER_CAPACITY; ++marker) {
        const uint32_t packet = minimap_shadow.build.markers[marker].packet_address;
        uint32_t word;

        if (!word_address_is_valid(packet) ||
            !word_address_is_valid(packet + 12u))
            return false;
        for (word = 0u; word < MINIMAP_MARKER_WORDS; ++word)
            minimap_shadow.initial_marker_words[marker][word] =
                cpu->read_word(packet + word * 4u);
    }
    minimap_shadow.initial_scratch_angle_z_word = cpu->read_word(
        XG_WORLD_MINIMAP_SCRATCH_ANGLE_ADDRESS + 4u);
    minimap_shadow.initial_scratch_rotation_tail_word = cpu->read_word(
        XG_WORLD_MINIMAP_SCRATCH_ROTATION_ADDRESS + 16u);
    return true;
}

static bool build_layout_is_valid(void) {
    const XgWorldMinimapBuildOutput *build = &minimap_shadow.build;
    uint32_t event;

    if (!build->requires_external_ot_tail ||
        build->ordering_count != 6u + build->active_marker_count ||
        build->ordering_count > XG_WORLD_MINIMAP_ORDER_EVENT_CAPACITY ||
        build->draw_mode.command_word != UINT32_C(0xe100043e) ||
        build->draw_mode.payload_word_count != 1u ||
        build->scratch.angle_address !=
            XG_WORLD_MINIMAP_SCRATCH_ANGLE_ADDRESS ||
        build->scratch.rotation_address !=
            XG_WORLD_MINIMAP_SCRATCH_ROTATION_ADDRESS ||
        build->scratch.translation_address !=
            XG_WORLD_MINIMAP_SCRATCH_TRANSLATION_ADDRESS)
        return false;
    for (event = 0u; event < build->ordering_count; ++event) {
        const XgWorldMinimapOrderEvent *order = &build->ordering[event];

        if (!word_address_is_valid(order->packet_address) ||
            order->payload_word_count == 0u ||
            !word_address_is_valid(order->packet_address +
                (uint32_t)order->payload_word_count * 4u) ||
            order->insertion_ordinal != event ||
            order->final_chain_ordinal != build->ordering_count - event - 1u ||
            order->successor_event_index !=
                (event == 0u ? XG_WORLD_MINIMAP_NO_ORDER_EVENT : event - 1u))
            return false;
    }
    return true;
}

void xg_world_minimap_shadow_reset(void) {
    memset(&minimap_shadow, 0, sizeof(minimap_shadow));
    minimap_shadow.snapshot.first_mismatch.source_index =
        XG_WORLD_MINIMAP_SHADOW_NO_SOURCE;
}

XgWorldMinimapShadowResult xg_world_minimap_shadow_begin(
    CPUState *cpu, uint64_t authentication_generation,
    const GpuDrawState *draw_state) {
    XgWorldMinimapCaptureRequest request;
    XgWorldMinimapAuthenticatedReader reader;
    XgWorldMinimapCaptureResult capture_result;
    XgWorldMinimapResult build_result;
    uint32_t marker;

    if (minimap_shadow.snapshot.blocked) {
        minimap_shadow.snapshot.last_result = XG_WORLD_MINIMAP_SHADOW_BLOCKED;
        return XG_WORLD_MINIMAP_SHADOW_BLOCKED;
    }
    if (!add_counter(&minimap_shadow.snapshot.entry_count, 1u)) {
        block_observer(XG_WORLD_MINIMAP_SHADOW_BLOCK_COUNTER_OVERFLOW,
                       XG_WORLD_MINIMAP_SHADOW_BLOCKED);
        return XG_WORLD_MINIMAP_SHADOW_BLOCKED;
    }
    if (minimap_shadow.snapshot.pending) {
        block_observer(XG_WORLD_MINIMAP_SHADOW_BLOCK_UNEXPECTED_BEGIN,
                       XG_WORLD_MINIMAP_SHADOW_LIFECYCLE_ERROR);
        return XG_WORLD_MINIMAP_SHADOW_LIFECYCLE_ERROR;
    }
    if (cpu == NULL || draw_state == NULL || authentication_generation == 0u ||
        cpu->read_word == NULL || cpu->read_half == NULL) {
        block_observer(XG_WORLD_MINIMAP_SHADOW_BLOCK_INVALID_ARGUMENT,
                       XG_WORLD_MINIMAP_SHADOW_INVALID_ARGUMENT);
        return XG_WORLD_MINIMAP_SHADOW_INVALID_ARGUMENT;
    }
    if (cpu->gpr[31] != XG_WORLD_MINIMAP_SHADOW_CALLER_RETURN) {
        if (!add_counter(&minimap_shadow.snapshot.rejected_caller_count, 1u)) {
            block_observer(XG_WORLD_MINIMAP_SHADOW_BLOCK_COUNTER_OVERFLOW,
                           XG_WORLD_MINIMAP_SHADOW_BLOCKED);
            return XG_WORLD_MINIMAP_SHADOW_BLOCKED;
        }
        minimap_shadow.snapshot.last_result =
            XG_WORLD_MINIMAP_SHADOW_NOT_APPLICABLE;
        return XG_WORLD_MINIMAP_SHADOW_NOT_APPLICABLE;
    }
    if (!stack_address_is_valid(cpu->gpr[29]) ||
        cpu->gpr[29] < MINIMAP_STACK_FRAME_SIZE ||
        !stack_address_is_valid(cpu->gpr[29] - MINIMAP_STACK_FRAME_SIZE)) {
        block_observer(XG_WORLD_MINIMAP_SHADOW_BLOCK_INVALID_STACK,
                       XG_WORLD_MINIMAP_SHADOW_LIFECYCLE_ERROR);
        return XG_WORLD_MINIMAP_SHADOW_LIFECYCLE_ERROR;
    }
    if (!add_counter(&minimap_shadow.snapshot.begin_count, 1u)) {
        block_observer(XG_WORLD_MINIMAP_SHADOW_BLOCK_COUNTER_OVERFLOW,
                       XG_WORLD_MINIMAP_SHADOW_BLOCKED);
        return XG_WORLD_MINIMAP_SHADOW_BLOCKED;
    }
    minimap_shadow.snapshot.active_generation = authentication_generation;

    request = (XgWorldMinimapCaptureRequest){
        .authentication_generation = authentication_generation,
        .caller_return = cpu->gpr[31],
        .screen_offset_x = (int32_t)cpu->gte_ctrl[24],
        .screen_offset_y = (int32_t)cpu->gte_ctrl[25],
        .projection_distance = (uint16_t)cpu->gte_ctrl[26],
        .raster = {
            .draw_area_left = draw_state->left,
            .draw_area_top = draw_state->top,
            .draw_area_right = draw_state->right,
            .draw_area_bottom = draw_state->bottom,
            .draw_offset_x = draw_state->offset_x,
            .draw_offset_y = draw_state->offset_y,
            .dither = draw_state->dither != 0u,
            .mask_set = draw_state->mask_set != 0u,
            .mask_check = draw_state->mask_check != 0u,
        },
        .projection_state_authenticated = true,
    };
    reader = (XgWorldMinimapAuthenticatedReader){
        .context = cpu,
        .read_u16 = read_u16,
        .read_u32 = read_u32,
        .authentication_generation = authentication_generation,
        .authenticated = true,
    };
    capture_result = xg_world_minimap_source_capture(
        &request, &reader, &minimap_shadow.capture);
    minimap_shadow.snapshot.last_capture_result = (uint32_t)capture_result;
    if (capture_result != XG_WORLD_MINIMAP_CAPTURE_OK) {
        (void)add_counter(
            &minimap_shadow.snapshot.source_capture_failure_count, 1u);
        block_observer(XG_WORLD_MINIMAP_SHADOW_BLOCK_SOURCE_CAPTURE,
                       XG_WORLD_MINIMAP_SHADOW_SOURCE_CAPTURE_FAILED);
        return XG_WORLD_MINIMAP_SHADOW_SOURCE_CAPTURE_FAILED;
    }
    if (!add_counter(&minimap_shadow.snapshot.source_capture_count, 1u) ||
        !add_counter(&minimap_shadow.snapshot.source_read_count,
                     minimap_shadow.capture.authenticated_read_count) ||
        !add_counter(&minimap_shadow.snapshot.source_read_bytes,
                     minimap_shadow.capture.authenticated_read_bytes)) {
        block_observer(XG_WORLD_MINIMAP_SHADOW_BLOCK_COUNTER_OVERFLOW,
                       XG_WORLD_MINIMAP_SHADOW_BLOCKED);
        return XG_WORLD_MINIMAP_SHADOW_BLOCKED;
    }

    build_result = xg_world_minimap_build(&minimap_shadow.capture.source,
                                          &minimap_shadow.build);
    minimap_shadow.snapshot.last_build_result = (uint32_t)build_result;
    if (build_result != XG_WORLD_MINIMAP_OK || !build_layout_is_valid()) {
        block_observer(XG_WORLD_MINIMAP_SHADOW_BLOCK_SOURCE_BUILD,
                       XG_WORLD_MINIMAP_SHADOW_BUILD_FAILED);
        return XG_WORLD_MINIMAP_SHADOW_BUILD_FAILED;
    }
    if (!capture_context_and_immutables(cpu)) {
        block_observer(XG_WORLD_MINIMAP_SHADOW_BLOCK_INVALID_CONTEXT,
                       XG_WORLD_MINIMAP_SHADOW_LIFECYCLE_ERROR);
        return XG_WORLD_MINIMAP_SHADOW_LIFECYCLE_ERROR;
    }

    minimap_shadow.entry_stack_pointer = cpu->gpr[29];
    minimap_shadow.entry_return_address = cpu->gpr[31];
    minimap_shadow.snapshot.last_marker_mask =
        minimap_shadow.capture.source.marker_mask;
    minimap_shadow.snapshot.last_active_marker_count =
        minimap_shadow.build.active_marker_count;
    minimap_shadow.snapshot.last_ordering_count =
        minimap_shadow.build.ordering_count;
    minimap_shadow.snapshot.last_context = minimap_shadow.context;
    minimap_shadow.snapshot.last_ot_address = minimap_shadow.ot_address;
    minimap_shadow.snapshot.last_initial_ot_word = minimap_shadow.initial_ot_word;
    for (marker = 0u; marker < XG_WORLD_MINIMAP_MARKER_CAPACITY; ++marker) {
        if (minimap_shadow.build.markers[marker].active)
            (void)add_counter(&minimap_shadow.snapshot.active_marker_count, 1u);
    }
    if (minimap_shadow.snapshot.counter_overflowed) {
        block_observer(XG_WORLD_MINIMAP_SHADOW_BLOCK_COUNTER_OVERFLOW,
                       XG_WORLD_MINIMAP_SHADOW_BLOCKED);
        return XG_WORLD_MINIMAP_SHADOW_BLOCKED;
    }
    minimap_shadow.snapshot.pending = true;
    minimap_shadow.snapshot.last_result = XG_WORLD_MINIMAP_SHADOW_OK;
    return XG_WORLD_MINIMAP_SHADOW_OK;
}

XgWorldMinimapShadowResult xg_world_minimap_shadow_finish(CPUState *cpu) {
    bool tag_matches[XG_WORLD_MINIMAP_ORDER_EVENT_CAPACITY] = { false };
    bool invocation_matches = true;
    bool ot_matches = true;
    bool context_matches;
    bool ot_pointer_matches;
    uint32_t event = 0u;
    uint32_t triangle;
    uint32_t marker;

    if (!add_counter(&minimap_shadow.snapshot.finish_count, 1u)) {
        block_observer(XG_WORLD_MINIMAP_SHADOW_BLOCK_COUNTER_OVERFLOW,
                       XG_WORLD_MINIMAP_SHADOW_BLOCKED);
        return XG_WORLD_MINIMAP_SHADOW_BLOCKED;
    }
    if (!minimap_shadow.snapshot.pending) {
        minimap_shadow.snapshot.last_result = minimap_shadow.snapshot.blocked
            ? XG_WORLD_MINIMAP_SHADOW_BLOCKED
            : XG_WORLD_MINIMAP_SHADOW_NOT_PENDING;
        return minimap_shadow.snapshot.last_result;
    }
    if (cpu == NULL || cpu->read_word == NULL || cpu->read_half == NULL) {
        block_observer(XG_WORLD_MINIMAP_SHADOW_BLOCK_INVALID_ARGUMENT,
                       XG_WORLD_MINIMAP_SHADOW_INVALID_ARGUMENT);
        return XG_WORLD_MINIMAP_SHADOW_INVALID_ARGUMENT;
    }
    if (cpu->gpr[29] !=
        minimap_shadow.entry_stack_pointer - MINIMAP_STACK_FRAME_SIZE) {
        (void)add_counter(&minimap_shadow.snapshot.stack_mismatch_count, 1u);
        record_first_mismatch(
            XG_WORLD_MINIMAP_SHADOW_MISMATCH_STACK_POINTER, 0u, 0u,
            XG_WORLD_MINIMAP_SHADOW_NO_SOURCE,
            minimap_shadow.entry_stack_pointer - MINIMAP_STACK_FRAME_SIZE,
            cpu->gpr[29]);
        block_observer(XG_WORLD_MINIMAP_SHADOW_BLOCK_INVALID_STACK,
                       XG_WORLD_MINIMAP_SHADOW_LIFECYCLE_ERROR);
        return XG_WORLD_MINIMAP_SHADOW_LIFECYCLE_ERROR;
    }
    {
        const uint32_t saved_ra_address = cpu->gpr[29] + 0x30u;
        const uint32_t saved_ra = cpu->read_word(saved_ra_address);

        if (saved_ra != minimap_shadow.entry_return_address) {
            (void)add_counter(
                &minimap_shadow.snapshot.saved_return_mismatch_count, 1u);
            record_first_mismatch(
                XG_WORLD_MINIMAP_SHADOW_MISMATCH_SAVED_RETURN_ADDRESS,
                saved_ra_address, 0u, XG_WORLD_MINIMAP_SHADOW_NO_SOURCE,
                minimap_shadow.entry_return_address, saved_ra);
            block_observer(
                XG_WORLD_MINIMAP_SHADOW_BLOCK_SAVED_RETURN_ADDRESS,
                XG_WORLD_MINIMAP_SHADOW_LIFECYCLE_ERROR);
            return XG_WORLD_MINIMAP_SHADOW_LIFECYCLE_ERROR;
        }
    }

    context_matches = compare_word(
        cpu, MINIMAP_CONTEXT_GLOBAL, 0u,
        XG_WORLD_MINIMAP_SHADOW_NO_SOURCE, minimap_shadow.context,
        XG_WORLD_MINIMAP_SHADOW_MISMATCH_CONTEXT);
    if (!context_matches)
        (void)add_counter(&minimap_shadow.snapshot.context_mismatch_count, 1u);
    ot_pointer_matches = compare_word(
        cpu, minimap_shadow.context + MINIMAP_CONTEXT_OT_OFFSET, 0u,
        XG_WORLD_MINIMAP_SHADOW_NO_SOURCE, minimap_shadow.ot_address,
        XG_WORLD_MINIMAP_SHADOW_MISMATCH_OT_POINTER);
    if (!ot_pointer_matches)
        (void)add_counter(&minimap_shadow.snapshot.ot_pointer_mismatch_count,
                          1u);
    invocation_matches &= context_matches && ot_pointer_matches;

    invocation_matches &= compare_tags_and_ot(cpu, tag_matches, &ot_matches);
    invocation_matches &= ot_matches;
    for (triangle = 0u; triangle < XG_WORLD_MINIMAP_TRIANGLE_COUNT;
         ++triangle, ++event) {
        const bool payload_matches = compare_g3_payload(
            cpu, &minimap_shadow.build.triangles[triangle], triangle);
        const bool packet_matches = payload_matches && tag_matches[event];

        if (!payload_matches)
            (void)add_counter(&minimap_shadow.snapshot.payload_mismatch_count,
                              1u);
        if (packet_matches)
            (void)add_counter(&minimap_shadow.snapshot.g3_match_count, 1u);
        else
            (void)add_counter(&minimap_shadow.snapshot.g3_mismatch_count, 1u);
        invocation_matches &= packet_matches;
    }
    {
        const bool payload_matches = compare_word(
            cpu, minimap_shadow.build.draw_mode.packet_address + 4u, 1u,
            XG_WORLD_MINIMAP_SHADOW_NO_SOURCE,
            minimap_shadow.build.draw_mode.command_word,
            XG_WORLD_MINIMAP_SHADOW_MISMATCH_DRAW_MODE_PAYLOAD);
        const bool packet_matches = payload_matches && tag_matches[event++];

        if (!payload_matches)
            (void)add_counter(&minimap_shadow.snapshot.payload_mismatch_count,
                              1u);
        if (packet_matches)
            (void)add_counter(&minimap_shadow.snapshot.draw_mode_match_count,
                              1u);
        else
            (void)add_counter(
                &minimap_shadow.snapshot.draw_mode_mismatch_count, 1u);
        invocation_matches &= packet_matches;
    }
    for (marker = 0u; marker < XG_WORLD_MINIMAP_MARKER_CAPACITY; ++marker) {
        const XgWorldMinimapMarkerRecord *record =
            &minimap_shadow.build.markers[marker];
        bool payload_matches;
        bool packet_matches;

        if (!record->active) continue;
        payload_matches = compare_marker_payload(cpu, record, marker);
        packet_matches = payload_matches && tag_matches[event++];
        if (!payload_matches)
            (void)add_counter(&minimap_shadow.snapshot.payload_mismatch_count,
                              1u);
        if (packet_matches)
            (void)add_counter(&minimap_shadow.snapshot.marker_match_count, 1u);
        else
            (void)add_counter(&minimap_shadow.snapshot.marker_mismatch_count,
                              1u);
        invocation_matches &= packet_matches;
    }
    {
        const bool payload_matches = compare_panel_payload(cpu);
        const bool packet_matches = payload_matches && tag_matches[event++];

        if (!payload_matches)
            (void)add_counter(&minimap_shadow.snapshot.payload_mismatch_count,
                              1u);
        if (packet_matches)
            (void)add_counter(&minimap_shadow.snapshot.panel_match_count, 1u);
        else
            (void)add_counter(&minimap_shadow.snapshot.panel_mismatch_count,
                              1u);
        invocation_matches &= packet_matches;
    }
    if (event != minimap_shadow.build.ordering_count) invocation_matches = false;
    invocation_matches &= compare_inactive_markers(cpu);
    invocation_matches &= compare_scratch(cpu);

    (void)add_counter(&minimap_shadow.snapshot.primitive_count,
                      5u + minimap_shadow.build.active_marker_count);
    (void)add_counter(&minimap_shadow.snapshot.completion_count, 1u);
    if (invocation_matches)
        (void)add_counter(&minimap_shadow.snapshot.invocation_match_count, 1u);
    else
        (void)add_counter(&minimap_shadow.snapshot.invocation_mismatch_count,
                          1u);
    if (minimap_shadow.snapshot.counter_overflowed) {
        block_observer(XG_WORLD_MINIMAP_SHADOW_BLOCK_COUNTER_OVERFLOW,
                       XG_WORLD_MINIMAP_SHADOW_BLOCKED);
        return XG_WORLD_MINIMAP_SHADOW_BLOCKED;
    }
    minimap_shadow.snapshot.last_result = invocation_matches
        ? XG_WORLD_MINIMAP_SHADOW_OK
        : XG_WORLD_MINIMAP_SHADOW_MISMATCH;
    {
        const XgWorldMinimapShadowResult result =
            minimap_shadow.snapshot.last_result;

        clear_pending();
        return result;
    }
}

void xg_world_minimap_shadow_invalidate(void) {
    if (minimap_shadow.snapshot.blocked) return;
    if (!add_counter(&minimap_shadow.snapshot.invalidation_count, 1u)) {
        block_observer(XG_WORLD_MINIMAP_SHADOW_BLOCK_COUNTER_OVERFLOW,
                       XG_WORLD_MINIMAP_SHADOW_BLOCKED);
        return;
    }
    if (minimap_shadow.snapshot.pending) {
        block_observer(XG_WORLD_MINIMAP_SHADOW_BLOCK_LIFECYCLE_INVALIDATED,
                       XG_WORLD_MINIMAP_SHADOW_LIFECYCLE_ERROR);
        return;
    }
    clear_pending();
    minimap_shadow.snapshot.active_generation = 0u;
}

void xg_world_minimap_shadow_block(XgWorldMinimapShadowBlocker blocker) {
    if (blocker == XG_WORLD_MINIMAP_SHADOW_BLOCK_NONE)
        blocker = XG_WORLD_MINIMAP_SHADOW_BLOCK_EXTERNAL;
    block_observer(blocker, XG_WORLD_MINIMAP_SHADOW_BLOCKED);
}

bool xg_world_minimap_shadow_record_native_cutover(uint32_t primitive_count) {
    XgWorldMinimapShadowSnapshot *snapshot = &minimap_shadow.snapshot;

    if (snapshot->native_cutover_count == UINT64_MAX ||
        snapshot->native_primitive_count > UINT64_MAX - primitive_count)
        return false;
    ++snapshot->native_cutover_count;
    snapshot->native_primitive_count += primitive_count;
    return true;
}

void xg_world_minimap_shadow_snapshot(
    XgWorldMinimapShadowSnapshot *out_snapshot) {
    if (out_snapshot != NULL) *out_snapshot = minimap_shadow.snapshot;
}
