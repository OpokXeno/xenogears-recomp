#include "xg_world_decorations_shadow.h"

#include <limits.h>
#include <stddef.h>
#include <string.h>

enum {
    DECORATIONS_CONTEXT_OT_OFFSET = 0x70,
    DECORATIONS_OUTER_LOOP_COUNT = 5,
};

static XgWorldDecorationsShadow global_shadow;
static GpuDrawState global_draw_state;
static uint64_t global_authentication_generation;
static int32_t global_screen_x_cull_margin;

static bool address_equals(uint32_t left, uint32_t right) {
    return (left & UINT32_C(0x1fffffff)) ==
           (right & UINT32_C(0x1fffffff));
}

static bool word_range_is_valid(uint32_t address, uint32_t size) {
    const uint32_t segment = address & UINT32_C(0xe0000000);
    const uint32_t physical = address & UINT32_C(0x1fffffff);
    const uint64_t end = (uint64_t)physical + size;
    const bool valid_segment =
        segment == 0u || segment == UINT32_C(0x80000000) ||
        segment == UINT32_C(0xa0000000);
    const bool main_ram = physical <= UINT32_C(0x001fffff) &&
                          end <= UINT64_C(0x00200000);
    const bool scratch = physical >= UINT32_C(0x1f800000) &&
                         end <= UINT64_C(0x1f800400);

    return valid_segment && size >= 4u && (size & 3u) == 0u &&
           (address & 3u) == 0u && (main_ram || scratch);
}

static void clear_active(XgWorldDecorationsShadow *shadow) {
    shadow->snapshot.outer_active = false;
    shadow->snapshot.helper_active = false;
    shadow->read_context = NULL;
    shadow->read_u16 = NULL;
    shadow->read_u32 = NULL;
    shadow->authentication_generation = 0u;
    shadow->outer_entry_stack_pointer = 0u;
    shadow->helper_entry_stack_pointer = 0u;
    shadow->packet_base = 0u;
    shadow->ot_base = 0u;
    shadow->expected_count = 0u;
    shadow->helper_first_count = 0u;
    shadow->touched_ot_count = 0u;
    shadow->helper_matches = false;
    shadow->outer_matches = false;
}

void xg_world_decorations_shadow_state_reset(
    XgWorldDecorationsShadow *shadow) {
    if (shadow == NULL)
        return;
    memset(shadow, 0, sizeof(*shadow));
    shadow->snapshot.phase = XG_WORLD_DECORATIONS_SHADOW_PHASE_IDLE;
    shadow->snapshot.first_mismatch_word = UINT32_MAX;
    shadow->snapshot.first_mismatch_source = UINT32_MAX;
}

XgWorldDecorationsShadowResult xg_world_decorations_shadow_state_block(
    XgWorldDecorationsShadow *shadow,
    XgWorldDecorationsShadowBlocker blocker, uint32_t detail) {
    if (shadow == NULL)
        return XG_WORLD_DECORATIONS_SHADOW_INVALID_ARGUMENT;
    if (shadow->snapshot.blocked)
        return XG_WORLD_DECORATIONS_SHADOW_BLOCKED;
    clear_active(shadow);
    shadow->snapshot.blocked = true;
    shadow->snapshot.phase = XG_WORLD_DECORATIONS_SHADOW_PHASE_BLOCKED;
    shadow->snapshot.blocker =
        blocker == XG_WORLD_DECORATIONS_SHADOW_BLOCKER_NONE
            ? XG_WORLD_DECORATIONS_SHADOW_BLOCKER_EXPLICIT
            : blocker;
    shadow->snapshot.blocker_detail = detail;
    if (shadow->snapshot.block_count != UINT64_MAX)
        ++shadow->snapshot.block_count;
    return XG_WORLD_DECORATIONS_SHADOW_BLOCKED;
}

static XgWorldDecorationsShadowResult block_at(
    XgWorldDecorationsShadow *shadow,
    XgWorldDecorationsShadowBlocker blocker, uint32_t detail) {
    return xg_world_decorations_shadow_state_block(shadow, blocker, detail);
}

static bool add_counter(XgWorldDecorationsShadow *shadow, uint64_t *counter,
                        uint64_t amount) {
    if (*counter > UINT64_MAX - amount) {
        (void)block_at(shadow,
                       XG_WORLD_DECORATIONS_SHADOW_BLOCKER_COUNTER_SATURATED,
                       0u);
        return false;
    }
    *counter += amount;
    return true;
}

static bool observation_has_reader(
    const XgWorldDecorationsShadowObservation *observation, bool need_u16) {
    return observation != NULL && observation->read_u32 != NULL &&
           (!need_u16 || observation->read_u16 != NULL);
}

static bool observation_is_authenticated(
    const XgWorldDecorationsShadow *shadow,
    const XgWorldDecorationsShadowObservation *observation,
    bool require_identity) {
    if (observation == NULL || !observation->authenticated ||
        observation->authentication_generation == 0u)
        return false;
    if (!require_identity)
        return true;
    return observation->authentication_generation ==
               shadow->authentication_generation &&
           observation->read_context == shadow->read_context &&
           observation->read_u16 == shadow->read_u16 &&
           observation->read_u32 == shadow->read_u32;
}

static bool read_word(XgWorldDecorationsShadow *shadow,
                      const XgWorldDecorationsShadowObservation *observation,
                      uint32_t address, uint32_t *out_value) {
    if (out_value == NULL || observation == NULL ||
        observation->read_u32 == NULL ||
        !observation->read_u32(observation->read_context, address, out_value)) {
        (void)block_at(shadow,
                       XG_WORLD_DECORATIONS_SHADOW_BLOCKER_READ_FAILED,
                       address);
        return false;
    }
    return true;
}

static void note_first_mismatch(XgWorldDecorationsShadow *shadow,
                                uint32_t kind, uint32_t packet,
                                uint32_t source, uint32_t word,
                                uint32_t address, uint32_t expected,
                                uint32_t actual) {
    XgWorldDecorationsShadowSnapshot *snapshot = &shadow->snapshot;

    if (snapshot->has_first_mismatch)
        return;
    snapshot->has_first_mismatch = true;
    snapshot->first_mismatch_helper = snapshot->helper_begin_count;
    snapshot->first_mismatch_kind = kind;
    snapshot->first_mismatch_packet = packet;
    snapshot->first_mismatch_source = source;
    snapshot->first_mismatch_word = word;
    snapshot->first_mismatch_address = address;
    snapshot->first_expected_word = expected;
    snapshot->first_actual_word = actual;
}

static bool note_component_mismatch(XgWorldDecorationsShadow *shadow,
                                    uint64_t *counter, uint32_t kind,
                                    uint32_t address, uint32_t expected,
                                    uint32_t actual) {
    if (!add_counter(shadow, counter, 1u))
        return false;
    note_first_mismatch(shadow, kind, 0u, UINT32_MAX, UINT32_MAX, address,
                        expected, actual);
    shadow->helper_matches = false;
    shadow->outer_matches = false;
    return true;
}

static bool capture_packet_base(
    XgWorldDecorationsShadow *shadow,
    const XgWorldDecorationsShadowObservation *observation,
    uint32_t *out_buffer_index, uint32_t *out_packet_base) {
    uint32_t buffer_index;
    uint32_t packet_base;

    if (!read_word(shadow, observation,
                   XG_WORLD_DECORATIONS_SHADOW_BUFFER_ADDRESS,
                   &buffer_index))
        return false;
    if (buffer_index >= 2u) {
        (void)block_at(shadow,
                       XG_WORLD_DECORATIONS_SHADOW_BLOCKER_BUFFER_INDEX,
                       buffer_index);
        return false;
    }
    if (!read_word(shadow, observation,
                   XG_WORLD_DECORATIONS_SHADOW_PACKET_BASES +
                       buffer_index * 4u,
                   &packet_base))
        return false;
    if (!word_range_is_valid(
            packet_base, XG_WORLD_DECORATIONS_PACKET_CAPACITY *
                             XG_WORLD_DECORATIONS_SHADOW_PACKET_STRIDE)) {
        (void)block_at(shadow,
                       XG_WORLD_DECORATIONS_SHADOW_BLOCKER_PACKET_BASE,
                       packet_base);
        return false;
    }
    *out_buffer_index = buffer_index;
    *out_packet_base = packet_base;
    return true;
}

static bool capture_ot_base(
    XgWorldDecorationsShadow *shadow,
    const XgWorldDecorationsShadowObservation *observation,
    uint32_t *out_ot_base) {
    uint32_t context;
    uint32_t ot_base;

    if (!read_word(shadow, observation,
                   XG_WORLD_DECORATIONS_SHADOW_CONTEXT, &context))
        return false;
    if (!word_range_is_valid(context, DECORATIONS_CONTEXT_OT_OFFSET + 4u)) {
        (void)block_at(shadow,
                       XG_WORLD_DECORATIONS_SHADOW_BLOCKER_CONTEXT, context);
        return false;
    }
    if (!read_word(shadow, observation,
                   context + DECORATIONS_CONTEXT_OT_OFFSET, &ot_base))
        return false;
    if (!word_range_is_valid(
            ot_base, XG_WORLD_DECORATIONS_SHADOW_OT_CAPACITY * 4u)) {
        (void)block_at(shadow,
                       XG_WORLD_DECORATIONS_SHADOW_BLOCKER_OT_BASE, ot_base);
        return false;
    }
    *out_ot_base = ot_base;
    return true;
}

XgWorldDecorationsShadowResult xg_world_decorations_shadow_state_outer_begin(
    XgWorldDecorationsShadow *shadow,
    const XgWorldDecorationsShadowObservation *observation) {
    uint32_t buffer_index;
    uint32_t packet_base;

    if (shadow == NULL || observation == NULL)
        return XG_WORLD_DECORATIONS_SHADOW_INVALID_ARGUMENT;
    if (shadow->snapshot.blocked)
        return XG_WORLD_DECORATIONS_SHADOW_BLOCKED;
    if (shadow->snapshot.phase != XG_WORLD_DECORATIONS_SHADOW_PHASE_IDLE)
        return block_at(shadow,
                        XG_WORLD_DECORATIONS_SHADOW_BLOCKER_NESTED_OUTER,
                        observation->pc);
    if (!observation_has_reader(observation, true))
        return block_at(
            shadow, XG_WORLD_DECORATIONS_SHADOW_BLOCKER_INVALID_OBSERVATION,
            observation->pc);
    if (!observation_is_authenticated(shadow, observation, false))
        return block_at(shadow,
                        XG_WORLD_DECORATIONS_SHADOW_BLOCKER_AUTHENTICATION,
                        observation->pc);
    if (!address_equals(observation->pc,
                        XG_WORLD_DECORATIONS_SHADOW_OUTER_BEGIN_PC))
        return block_at(shadow,
                        XG_WORLD_DECORATIONS_SHADOW_BLOCKER_OUTER_SITE,
                        observation->pc);
    if (!address_equals(observation->return_address,
                        XG_WORLD_DECORATIONS_SHADOW_OUTER_RETURN))
        return block_at(shadow,
                        XG_WORLD_DECORATIONS_SHADOW_BLOCKER_OUTER_RETURN,
                        observation->return_address);
    if (observation->stack_pointer <
            XG_WORLD_DECORATIONS_SHADOW_OUTER_FRAME_SIZE ||
        !word_range_is_valid(
            observation->stack_pointer -
                XG_WORLD_DECORATIONS_SHADOW_OUTER_FRAME_SIZE,
            XG_WORLD_DECORATIONS_SHADOW_OUTER_FRAME_SIZE))
        return block_at(shadow,
                        XG_WORLD_DECORATIONS_SHADOW_BLOCKER_OUTER_STACK,
                        observation->stack_pointer);
    if (!capture_packet_base(shadow, observation, &buffer_index,
                             &packet_base))
        return XG_WORLD_DECORATIONS_SHADOW_BLOCKED;
    if (!add_counter(shadow, &shadow->snapshot.outer_begin_count, 1u))
        return XG_WORLD_DECORATIONS_SHADOW_BLOCKED;

    shadow->read_context = observation->read_context;
    shadow->read_u16 = observation->read_u16;
    shadow->read_u32 = observation->read_u32;
    shadow->authentication_generation =
        observation->authentication_generation;
    shadow->outer_entry_stack_pointer = observation->stack_pointer;
    shadow->packet_base = packet_base;
    shadow->expected_count = 0u;
    shadow->outer_matches = true;
    shadow->snapshot.last_buffer_index = buffer_index;
    shadow->snapshot.last_packet_base = packet_base;
    shadow->snapshot.last_initial_count = 0u;
    shadow->snapshot.last_final_count = 0u;
    shadow->snapshot.outer_active = true;
    shadow->snapshot.phase = XG_WORLD_DECORATIONS_SHADOW_PHASE_OUTER;
    return XG_WORLD_DECORATIONS_SHADOW_OK;
}

static XgWorldDecorationsShadowOtExpectation *find_touched_ot(
    XgWorldDecorationsShadow *shadow, uint32_t bucket) {
    uint32_t index;

    for (index = 0u; index < shadow->touched_ot_count; ++index) {
        if (shadow->touched_ot[index].bucket == bucket)
            return &shadow->touched_ot[index];
    }
    return NULL;
}

static bool prepare_expected_links(
    XgWorldDecorationsShadow *shadow,
    const XgWorldDecorationsShadowObservation *observation) {
    uint32_t index;

    shadow->touched_ot_count = 0u;
    for (index = shadow->helper_first_count; index < shadow->expected_count;
         ++index) {
        const XgWorldDecorationsRecord *record = &shadow->records[index];
        XgWorldDecorationsShadowOtExpectation *expectation;
        uint32_t packet;
        uint32_t head;

        if (record->ordering_bucket >=
            XG_WORLD_DECORATIONS_SHADOW_OT_CAPACITY) {
            (void)block_at(shadow,
                           XG_WORLD_DECORATIONS_SHADOW_BLOCKER_OT_BASE,
                           record->ordering_bucket);
            return false;
        }
        packet = shadow->packet_base +
                  index * XG_WORLD_DECORATIONS_SHADOW_PACKET_STRIDE;
        shadow->packet_addresses[index] = packet;
        for (uint32_t word = 0u;
             word < XG_WORLD_DECORATIONS_FT4_PAYLOAD_WORD_COUNT; ++word) {
            if (!read_word(shadow, observation, packet + 4u + word * 4u,
                           &shadow->initial_payload_words[index][word]))
                return false;
        }
        expectation = find_touched_ot(shadow, record->ordering_bucket);
        if (expectation == NULL) {
            uint32_t initial_word;
            const uint32_t ot_address =
                shadow->ot_base + record->ordering_bucket * 4u;

            if (shadow->touched_ot_count >=
                    XG_WORLD_DECORATIONS_PACKET_CAPACITY ||
                !read_word(shadow, observation, ot_address, &initial_word))
                return false;
            expectation = &shadow->touched_ot[shadow->touched_ot_count++];
            *expectation = (XgWorldDecorationsShadowOtExpectation){
                .bucket = record->ordering_bucket,
                .address = ot_address,
                .last_packet = initial_word,
            };
        }
        head = expectation->last_packet;
        shadow->expected_tags[index] = head | UINT32_C(0x09000000);
        expectation->last_packet = packet & UINT32_C(0x00ffffff);
    }
    return true;
}

XgWorldDecorationsShadowResult xg_world_decorations_shadow_state_helper_begin(
    XgWorldDecorationsShadow *shadow,
    const XgWorldDecorationsShadowObservation *observation) {
    XgWorldDecorationsCaptureRequest request = {0};
    XgWorldDecorationsAuthenticatedReader reader = {0};
    XgWorldDecorationsCapture capture;
    XgWorldDecorationsCaptureResult capture_result;
    XgWorldDecorationsResult build_result;
    uint32_t buffer_index;
    uint32_t packet_base;
    uint32_t ot_base;
    uint32_t shared_count;
    uint32_t expected_cursor;
    uint32_t previous_buffer_index;
    uint32_t previous_packet_base;

    if (shadow == NULL || observation == NULL)
        return XG_WORLD_DECORATIONS_SHADOW_INVALID_ARGUMENT;
    if (shadow->snapshot.blocked)
        return XG_WORLD_DECORATIONS_SHADOW_BLOCKED;
    if (shadow->snapshot.phase == XG_WORLD_DECORATIONS_SHADOW_PHASE_HELPER)
        return block_at(shadow,
                        XG_WORLD_DECORATIONS_SHADOW_BLOCKER_NESTED_HELPER,
                        observation->pc);
    if (shadow->snapshot.phase != XG_WORLD_DECORATIONS_SHADOW_PHASE_OUTER)
        return block_at(
            shadow,
            XG_WORLD_DECORATIONS_SHADOW_BLOCKER_UNMATCHED_HELPER_BEGIN,
            observation->pc);
    if (!observation_has_reader(observation, true))
        return block_at(
            shadow, XG_WORLD_DECORATIONS_SHADOW_BLOCKER_INVALID_OBSERVATION,
            observation->pc);
    if (!observation_is_authenticated(shadow, observation, true))
        return block_at(shadow,
                        XG_WORLD_DECORATIONS_SHADOW_BLOCKER_AUTHENTICATION,
                        observation->pc);
    if (!address_equals(observation->pc,
                        XG_WORLD_DECORATIONS_SHADOW_HELPER_BEGIN_PC))
        return block_at(shadow,
                        XG_WORLD_DECORATIONS_SHADOW_BLOCKER_HELPER_SITE,
                        observation->pc);
    if (!address_equals(observation->return_address,
                        XG_WORLD_DECORATIONS_SHADOW_HELPER_RETURN))
        return block_at(shadow,
                        XG_WORLD_DECORATIONS_SHADOW_BLOCKER_HELPER_RETURN,
                        observation->return_address);
    if (observation->stack_pointer !=
        shadow->outer_entry_stack_pointer -
            XG_WORLD_DECORATIONS_SHADOW_OUTER_FRAME_SIZE ||
        !word_range_is_valid(
            observation->stack_pointer,
            XG_WORLD_DECORATIONS_SHADOW_OUTER_FRAME_SIZE))
        return block_at(shadow,
                        XG_WORLD_DECORATIONS_SHADOW_BLOCKER_HELPER_STACK,
                        observation->stack_pointer);
    if (!capture_packet_base(shadow, observation, &buffer_index,
                             &packet_base) ||
        !capture_ot_base(shadow, observation, &ot_base) ||
        !read_word(shadow, observation,
                   XG_WORLD_DECORATIONS_SHADOW_SHARED_COUNT, &shared_count))
        return XG_WORLD_DECORATIONS_SHADOW_BLOCKED;
    if (shared_count > XG_WORLD_DECORATIONS_PACKET_CAPACITY)
        return block_at(shadow,
                        XG_WORLD_DECORATIONS_SHADOW_BLOCKER_SHARED_COUNT,
                        shared_count);
    if (observation->a1 == 0u ||
        observation->a1 > XG_WORLD_DECORATIONS_POSITION_CAPACITY ||
        !word_range_is_valid(observation->a0, observation->a1 * 8u))
        return block_at(
            shadow, XG_WORLD_DECORATIONS_SHADOW_BLOCKER_INVALID_OBSERVATION,
            observation->a0);
    if (!add_counter(shadow, &shadow->snapshot.helper_begin_count, 1u))
        return XG_WORLD_DECORATIONS_SHADOW_BLOCKED;

    shadow->helper_matches = true;
    shadow->helper_entry_stack_pointer = observation->stack_pointer;
    shadow->helper_first_count = shadow->expected_count;
    shadow->packet_base = packet_base;
    shadow->ot_base = ot_base;
    shadow->snapshot.last_ot_base = ot_base;
    shadow->snapshot.last_position_address = observation->a0;
    shadow->snapshot.last_position_count = observation->a1;
    shadow->snapshot.last_initial_count = shared_count;

    previous_buffer_index = shadow->snapshot.last_buffer_index;
    previous_packet_base = shadow->snapshot.last_packet_base;
    if (buffer_index != previous_buffer_index &&
        !note_component_mismatch(
            shadow, &shadow->snapshot.buffer_mismatch_count,
            XG_WORLD_DECORATIONS_SHADOW_MISMATCH_BUFFER,
            XG_WORLD_DECORATIONS_SHADOW_BUFFER_ADDRESS,
            previous_buffer_index, buffer_index))
        return XG_WORLD_DECORATIONS_SHADOW_BLOCKED;
    if (packet_base != previous_packet_base &&
        !note_component_mismatch(
            shadow, &shadow->snapshot.buffer_mismatch_count,
            XG_WORLD_DECORATIONS_SHADOW_MISMATCH_BUFFER,
            XG_WORLD_DECORATIONS_SHADOW_PACKET_BASES + buffer_index * 4u,
            previous_packet_base, packet_base))
        return XG_WORLD_DECORATIONS_SHADOW_BLOCKED;
    shadow->snapshot.last_buffer_index = buffer_index;
    shadow->snapshot.last_packet_base = packet_base;

    expected_cursor = packet_base +
                      shadow->expected_count *
                          XG_WORLD_DECORATIONS_SHADOW_PACKET_STRIDE;
    if (shared_count != shadow->expected_count &&
        !note_component_mismatch(
            shadow, &shadow->snapshot.count_mismatch_count,
            XG_WORLD_DECORATIONS_SHADOW_MISMATCH_COUNT,
            XG_WORLD_DECORATIONS_SHADOW_SHARED_COUNT, shadow->expected_count,
            shared_count))
        return XG_WORLD_DECORATIONS_SHADOW_BLOCKED;
    if (observation->a2 != ot_base &&
        !note_component_mismatch(
            shadow, &shadow->snapshot.argument_mismatch_count,
            XG_WORLD_DECORATIONS_SHADOW_MISMATCH_ARGUMENT, 2u, ot_base,
            observation->a2))
        return XG_WORLD_DECORATIONS_SHADOW_BLOCKED;
    if (observation->a3 != expected_cursor &&
        !note_component_mismatch(
            shadow, &shadow->snapshot.a3_mismatch_count,
            XG_WORLD_DECORATIONS_SHADOW_MISMATCH_A3, 3u, expected_cursor,
            observation->a3))
        return XG_WORLD_DECORATIONS_SHADOW_BLOCKED;

    request = (XgWorldDecorationsCaptureRequest){
        .authentication_generation = observation->authentication_generation,
        .caller_return = observation->return_address,
        .position_address = observation->a0,
        .position_count = observation->a1,
        .screen_offset_x = observation->screen_offset_x,
        .screen_offset_y = observation->screen_offset_y,
        .screen_x_cull_margin = observation->screen_x_cull_margin,
        .projection_distance = observation->projection_distance,
        .raster = observation->raster,
        .helper_arguments_authenticated =
            observation->helper_arguments_authenticated,
        .projection_state_authenticated =
            observation->projection_state_authenticated,
    };
    reader = (XgWorldDecorationsAuthenticatedReader){
        .context = observation->read_context,
        .read_u16 = observation->read_u16,
        .read_u32 = observation->read_u32,
        .authentication_generation = observation->authentication_generation,
        .authenticated = observation->authenticated,
    };
    capture_result =
        xg_world_decorations_source_capture(&request, &reader, &capture);
    shadow->snapshot.last_capture_result = (uint32_t)capture_result;
    if (capture_result != XG_WORLD_DECORATIONS_CAPTURE_OK) {
        if (!add_counter(shadow,
                         &shadow->snapshot.source_capture_failure_count, 1u))
            return XG_WORLD_DECORATIONS_SHADOW_BLOCKED;
        return block_at(shadow,
                        XG_WORLD_DECORATIONS_SHADOW_BLOCKER_SOURCE_CAPTURE,
                        (uint32_t)capture_result);
    }
    if (!add_counter(shadow, &shadow->snapshot.source_capture_count, 1u) ||
        !add_counter(shadow, &shadow->snapshot.source_read_count,
                     capture.authenticated_read_count) ||
        !add_counter(shadow, &shadow->snapshot.source_read_bytes,
                     capture.authenticated_read_bytes) ||
        !add_counter(shadow, &shadow->snapshot.candidate_count,
                     capture.source.position_count))
        return XG_WORLD_DECORATIONS_SHADOW_BLOCKED;

    build_result = xg_world_decorations_build(
        &capture.source, shadow->records,
        XG_WORLD_DECORATIONS_PACKET_CAPACITY, &shadow->expected_count);
    shadow->snapshot.last_build_result = (uint32_t)build_result;
    if (build_result != XG_WORLD_DECORATIONS_OK)
        return block_at(shadow,
                        XG_WORLD_DECORATIONS_SHADOW_BLOCKER_BUILD,
                        (uint32_t)build_result);
    shadow->snapshot.last_final_count = shadow->expected_count;
    if (!prepare_expected_links(shadow, observation))
        return XG_WORLD_DECORATIONS_SHADOW_BLOCKED;

    shadow->snapshot.helper_active = true;
    shadow->snapshot.phase = XG_WORLD_DECORATIONS_SHADOW_PHASE_HELPER;
    return XG_WORLD_DECORATIONS_SHADOW_OK;
}

static bool payload_word_is_geometry(uint32_t word) {
    return word == 1u || word == 3u || word == 5u || word == 7u;
}

static uint32_t expected_payload_word(
    const XgWorldDecorationsShadow *shadow, uint32_t index, uint32_t word) {
    if (payload_word_is_geometry(word))
        return shadow->records[index].ft4_payload_words[word];
    if (word == 2u)
        return (shadow->initial_payload_words[index][word] & UINT32_C(0x0000ffff)) |
            ((uint32_t)shadow->records[index].clut << 16u);
    return shadow->initial_payload_words[index][word];
}

static bool compare_packets(
    XgWorldDecorationsShadow *shadow,
    const XgWorldDecorationsShadowObservation *observation) {
    uint32_t index;

    for (index = shadow->helper_first_count; index < shadow->expected_count;
         ++index) {
        const XgWorldDecorationsRecord *record = &shadow->records[index];
        const uint32_t packet = shadow->packet_addresses[index];
        bool payload_matches = true;
        bool geometry_matches = true;
        bool tag_matches = true;
        uint32_t actual;
        uint32_t word;

        if (!read_word(shadow, observation, packet, &actual))
            return false;
        if (actual != shadow->expected_tags[index]) {
            tag_matches = false;
            if (!add_counter(shadow, &shadow->snapshot.tag_mismatch_count, 1u))
                return false;
            note_first_mismatch(
                shadow, XG_WORLD_DECORATIONS_SHADOW_MISMATCH_TAG, packet,
                record->source_index, UINT32_MAX, packet,
                shadow->expected_tags[index], actual);
        }
        for (word = 0u;
             word < XG_WORLD_DECORATIONS_FT4_PAYLOAD_WORD_COUNT; ++word) {
            const uint32_t address = packet + 4u + word * 4u;
            const uint32_t expected = expected_payload_word(
                shadow, index, word);

            if (!read_word(shadow, observation, address, &actual))
                return false;
            if (actual == expected)
                continue;
            payload_matches = false;
            if (payload_word_is_geometry(word))
                geometry_matches = false;
            if (!add_counter(
                    shadow, &shadow->snapshot.payload_word_mismatch_count,
                    1u))
                return false;
            note_first_mismatch(
                shadow, XG_WORLD_DECORATIONS_SHADOW_MISMATCH_PAYLOAD |
                            (payload_word_is_geometry(word)
                                 ? XG_WORLD_DECORATIONS_SHADOW_MISMATCH_GEOMETRY
                                 : 0u),
                packet, record->source_index, word, address, expected, actual);
        }
        if (!payload_matches &&
            !add_counter(shadow, &shadow->snapshot.payload_mismatch_count, 1u))
            return false;
        if (!geometry_matches &&
            !add_counter(shadow, &shadow->snapshot.geometry_mismatch_count,
                         1u))
            return false;
        if (payload_matches && tag_matches) {
            if (!add_counter(shadow, &shadow->snapshot.match_count, 1u))
                return false;
        } else {
            if (!add_counter(shadow, &shadow->snapshot.mismatch_count, 1u))
                return false;
            shadow->helper_matches = false;
            shadow->outer_matches = false;
        }
    }
    return true;
}

static bool compare_ot(
    XgWorldDecorationsShadow *shadow,
    const XgWorldDecorationsShadowObservation *observation) {
    uint32_t index;

    for (index = 0u; index < shadow->touched_ot_count; ++index) {
        const XgWorldDecorationsShadowOtExpectation *expectation =
            &shadow->touched_ot[index];
        uint32_t actual;

        if (!read_word(shadow, observation, expectation->address, &actual))
            return false;
        if (actual == expectation->last_packet) {
            if (!add_counter(shadow, &shadow->snapshot.ot_match_count, 1u))
                return false;
            continue;
        }
        if (!add_counter(shadow, &shadow->snapshot.ot_mismatch_count, 1u))
            return false;
        note_first_mismatch(shadow,
                            XG_WORLD_DECORATIONS_SHADOW_MISMATCH_OT, 0u,
                            UINT32_MAX, UINT32_MAX, expectation->address,
                            expectation->last_packet, actual);
        shadow->helper_matches = false;
        shadow->outer_matches = false;
    }
    return true;
}

XgWorldDecorationsShadowResult xg_world_decorations_shadow_state_helper_finish(
    XgWorldDecorationsShadow *shadow,
    const XgWorldDecorationsShadowObservation *observation) {
    uint32_t shared_count;
    uint32_t expected_cursor;
    uint32_t primitive_count;

    if (shadow == NULL || observation == NULL)
        return XG_WORLD_DECORATIONS_SHADOW_INVALID_ARGUMENT;
    if (shadow->snapshot.blocked)
        return XG_WORLD_DECORATIONS_SHADOW_BLOCKED;
    if (shadow->snapshot.phase != XG_WORLD_DECORATIONS_SHADOW_PHASE_HELPER)
        return block_at(
            shadow,
            XG_WORLD_DECORATIONS_SHADOW_BLOCKER_UNMATCHED_HELPER_FINISH,
            observation->pc);
    if (!observation_has_reader(observation, false))
        return block_at(
            shadow, XG_WORLD_DECORATIONS_SHADOW_BLOCKER_INVALID_OBSERVATION,
            observation->pc);
    if (!observation_is_authenticated(shadow, observation, true))
        return block_at(shadow,
                        XG_WORLD_DECORATIONS_SHADOW_BLOCKER_AUTHENTICATION,
                        observation->pc);
    if (!address_equals(observation->pc,
                        XG_WORLD_DECORATIONS_SHADOW_HELPER_FINISH_PC))
        return block_at(shadow,
                        XG_WORLD_DECORATIONS_SHADOW_BLOCKER_HELPER_SITE,
                        observation->pc);
    if (!address_equals(observation->return_address,
                        XG_WORLD_DECORATIONS_SHADOW_HELPER_RETURN))
        return block_at(shadow,
                        XG_WORLD_DECORATIONS_SHADOW_BLOCKER_HELPER_RETURN,
                        observation->return_address);
    if (observation->stack_pointer !=
        shadow->helper_entry_stack_pointer -
            XG_WORLD_DECORATIONS_SHADOW_HELPER_FRAME_SIZE)
        return block_at(shadow,
                        XG_WORLD_DECORATIONS_SHADOW_BLOCKER_HELPER_STACK,
                        observation->stack_pointer);
    if (!read_word(shadow, observation,
                   XG_WORLD_DECORATIONS_SHADOW_SHARED_COUNT, &shared_count))
        return XG_WORLD_DECORATIONS_SHADOW_BLOCKED;

    expected_cursor = shadow->packet_base +
                      shadow->expected_count *
                          XG_WORLD_DECORATIONS_SHADOW_PACKET_STRIDE;
    shadow->snapshot.last_a3 = observation->a3;
    shadow->snapshot.last_s0 = observation->s0;
    shadow->snapshot.last_final_count = shared_count;
    if (shared_count != shadow->expected_count &&
        !note_component_mismatch(
            shadow, &shadow->snapshot.count_mismatch_count,
            XG_WORLD_DECORATIONS_SHADOW_MISMATCH_COUNT,
            XG_WORLD_DECORATIONS_SHADOW_SHARED_COUNT, shadow->expected_count,
            shared_count))
        return XG_WORLD_DECORATIONS_SHADOW_BLOCKED;
    if (observation->a3 != expected_cursor &&
        !note_component_mismatch(
            shadow, &shadow->snapshot.a3_mismatch_count,
            XG_WORLD_DECORATIONS_SHADOW_MISMATCH_A3, 3u, expected_cursor,
            observation->a3))
        return XG_WORLD_DECORATIONS_SHADOW_BLOCKED;
    if (observation->s0 != shadow->expected_count &&
        !note_component_mismatch(
            shadow, &shadow->snapshot.s0_mismatch_count,
            XG_WORLD_DECORATIONS_SHADOW_MISMATCH_S0, 16u,
            shadow->expected_count, observation->s0))
        return XG_WORLD_DECORATIONS_SHADOW_BLOCKED;
    if (!compare_packets(shadow, observation) ||
        !compare_ot(shadow, observation))
        return XG_WORLD_DECORATIONS_SHADOW_BLOCKED;

    primitive_count = shadow->expected_count - shadow->helper_first_count;
    if (!add_counter(shadow, &shadow->snapshot.primitive_count,
                     primitive_count) ||
        !add_counter(shadow, &shadow->snapshot.helper_finish_count, 1u))
        return XG_WORLD_DECORATIONS_SHADOW_BLOCKED;
    if (shadow->helper_matches) {
        if (!add_counter(shadow, &shadow->snapshot.helper_match_count, 1u))
            return XG_WORLD_DECORATIONS_SHADOW_BLOCKED;
    } else if (!add_counter(shadow, &shadow->snapshot.helper_mismatch_count,
                            1u)) {
        return XG_WORLD_DECORATIONS_SHADOW_BLOCKED;
    }

    shadow->helper_first_count = shadow->expected_count;
    shadow->helper_entry_stack_pointer = 0u;
    shadow->touched_ot_count = 0u;
    shadow->helper_matches = false;
    shadow->snapshot.helper_active = false;
    shadow->snapshot.phase = XG_WORLD_DECORATIONS_SHADOW_PHASE_OUTER;
    return XG_WORLD_DECORATIONS_SHADOW_OK;
}

XgWorldDecorationsShadowResult xg_world_decorations_shadow_state_outer_finish(
    XgWorldDecorationsShadow *shadow,
    const XgWorldDecorationsShadowObservation *observation) {
    uint32_t saved_return;
    uint32_t shared_count;

    if (shadow == NULL || observation == NULL)
        return XG_WORLD_DECORATIONS_SHADOW_INVALID_ARGUMENT;
    if (shadow->snapshot.blocked)
        return XG_WORLD_DECORATIONS_SHADOW_BLOCKED;
    if (shadow->snapshot.phase == XG_WORLD_DECORATIONS_SHADOW_PHASE_HELPER)
        return block_at(
            shadow,
            XG_WORLD_DECORATIONS_SHADOW_BLOCKER_OUTER_FINISH_WITH_HELPER,
            observation->pc);
    if (shadow->snapshot.phase != XG_WORLD_DECORATIONS_SHADOW_PHASE_OUTER)
        return block_at(
            shadow,
            XG_WORLD_DECORATIONS_SHADOW_BLOCKER_UNMATCHED_OUTER_FINISH,
            observation->pc);
    if (!observation_has_reader(observation, false))
        return block_at(
            shadow, XG_WORLD_DECORATIONS_SHADOW_BLOCKER_INVALID_OBSERVATION,
            observation->pc);
    if (!observation_is_authenticated(shadow, observation, true))
        return block_at(shadow,
                        XG_WORLD_DECORATIONS_SHADOW_BLOCKER_AUTHENTICATION,
                        observation->pc);
    if (!address_equals(observation->pc,
                        XG_WORLD_DECORATIONS_SHADOW_OUTER_FINISH_PC))
        return block_at(shadow,
                        XG_WORLD_DECORATIONS_SHADOW_BLOCKER_OUTER_SITE,
                        observation->pc);
    if (observation->stack_pointer !=
        shadow->outer_entry_stack_pointer -
            XG_WORLD_DECORATIONS_SHADOW_OUTER_FRAME_SIZE)
        return block_at(shadow,
                        XG_WORLD_DECORATIONS_SHADOW_BLOCKER_OUTER_STACK,
                        observation->stack_pointer);
    if (!read_word(shadow, observation,
                   observation->stack_pointer + 0x24u, &saved_return))
        return XG_WORLD_DECORATIONS_SHADOW_BLOCKED;
    if (!address_equals(saved_return,
                        XG_WORLD_DECORATIONS_SHADOW_OUTER_RETURN))
        return block_at(shadow,
                        XG_WORLD_DECORATIONS_SHADOW_BLOCKER_OUTER_RETURN,
                        saved_return);
    if (!read_word(shadow, observation,
                   XG_WORLD_DECORATIONS_SHADOW_SHARED_COUNT, &shared_count))
        return XG_WORLD_DECORATIONS_SHADOW_BLOCKED;
    if (shared_count != shadow->expected_count) {
        if (!add_counter(shadow, &shadow->snapshot.count_mismatch_count, 1u))
            return XG_WORLD_DECORATIONS_SHADOW_BLOCKED;
        note_first_mismatch(
            shadow, XG_WORLD_DECORATIONS_SHADOW_MISMATCH_OUTER |
                        XG_WORLD_DECORATIONS_SHADOW_MISMATCH_COUNT,
            0u, UINT32_MAX, UINT32_MAX,
            XG_WORLD_DECORATIONS_SHADOW_SHARED_COUNT, shadow->expected_count,
            shared_count);
        shadow->outer_matches = false;
    }
    if (observation->s0 != DECORATIONS_OUTER_LOOP_COUNT) {
        if (!add_counter(shadow, &shadow->snapshot.s0_mismatch_count, 1u))
            return XG_WORLD_DECORATIONS_SHADOW_BLOCKED;
        note_first_mismatch(
            shadow, XG_WORLD_DECORATIONS_SHADOW_MISMATCH_OUTER |
                        XG_WORLD_DECORATIONS_SHADOW_MISMATCH_S0,
            0u, UINT32_MAX, UINT32_MAX, 16u,
            DECORATIONS_OUTER_LOOP_COUNT, observation->s0);
        shadow->outer_matches = false;
    }
    if (!add_counter(shadow, &shadow->snapshot.outer_finish_count, 1u))
        return XG_WORLD_DECORATIONS_SHADOW_BLOCKED;
    if (shadow->outer_matches) {
        if (!add_counter(shadow, &shadow->snapshot.outer_match_count, 1u))
            return XG_WORLD_DECORATIONS_SHADOW_BLOCKED;
    } else if (!add_counter(shadow, &shadow->snapshot.outer_mismatch_count,
                            1u)) {
        return XG_WORLD_DECORATIONS_SHADOW_BLOCKED;
    }

    clear_active(shadow);
    shadow->snapshot.phase = XG_WORLD_DECORATIONS_SHADOW_PHASE_IDLE;
    return XG_WORLD_DECORATIONS_SHADOW_OK;
}

XgWorldDecorationsShadowResult
xg_world_decorations_shadow_state_lifecycle_invalidate(
    XgWorldDecorationsShadow *shadow, uint32_t detail) {
    if (shadow == NULL)
        return XG_WORLD_DECORATIONS_SHADOW_INVALID_ARGUMENT;
    if (shadow->snapshot.blocked)
        return XG_WORLD_DECORATIONS_SHADOW_BLOCKED;
    if (!add_counter(shadow,
                     &shadow->snapshot.lifecycle_invalidation_count, 1u))
        return XG_WORLD_DECORATIONS_SHADOW_BLOCKED;
    if (shadow->snapshot.phase == XG_WORLD_DECORATIONS_SHADOW_PHASE_IDLE) {
        clear_active(shadow);
        return XG_WORLD_DECORATIONS_SHADOW_OK;
    }
    return block_at(
        shadow, XG_WORLD_DECORATIONS_SHADOW_BLOCKER_LIFECYCLE_INVALIDATED,
        detail);
}

XgWorldDecorationsShadowResult xg_world_decorations_shadow_state_snapshot(
    const XgWorldDecorationsShadow *shadow,
    XgWorldDecorationsShadowSnapshot *out_snapshot) {
    if (shadow == NULL || out_snapshot == NULL)
        return XG_WORLD_DECORATIONS_SHADOW_INVALID_ARGUMENT;
    *out_snapshot = shadow->snapshot;
    return XG_WORLD_DECORATIONS_SHADOW_OK;
}

static bool cpu_read_u16(void *context, uint32_t address,
                         uint16_t *out_value) {
    CPUState *cpu = context;

    if (cpu == NULL || cpu->read_half == NULL || out_value == NULL)
        return false;
    *out_value = cpu->read_half(address);
    return true;
}

static bool cpu_read_u32(void *context, uint32_t address,
                         uint32_t *out_value) {
    CPUState *cpu = context;

    if (cpu == NULL || cpu->read_word == NULL || out_value == NULL)
        return false;
    *out_value = cpu->read_word(address);
    return true;
}

static XgWorldDecorationsShadowObservation cpu_observation(
    CPUState *cpu, uint32_t pc) {
    XgWorldDecorationsShadowObservation observation = {0};

    if (cpu == NULL)
        return observation;
    observation = (XgWorldDecorationsShadowObservation){
        .read_context = cpu,
        .read_u16 = cpu_read_u16,
        .read_u32 = cpu_read_u32,
        .authentication_generation = global_authentication_generation,
        .pc = pc,
        .return_address = cpu->gpr[31],
        .stack_pointer = cpu->gpr[29],
        .a0 = cpu->gpr[4],
        .a1 = cpu->gpr[5],
        .a2 = cpu->gpr[6],
        .a3 = cpu->gpr[7],
        .s0 = cpu->gpr[16],
        .screen_offset_x = (int32_t)cpu->gte_ctrl[24],
        .screen_offset_y = (int32_t)cpu->gte_ctrl[25],
        .screen_x_cull_margin = global_screen_x_cull_margin,
        .projection_distance = (uint16_t)cpu->gte_ctrl[26],
        .raster = {
            .draw_area_left = global_draw_state.left,
            .draw_area_top = global_draw_state.top,
            .draw_area_right = global_draw_state.right,
            .draw_area_bottom = global_draw_state.bottom,
            .draw_offset_x = global_draw_state.offset_x,
            .draw_offset_y = global_draw_state.offset_y,
            .dither = global_draw_state.dither != 0u,
            .mask_set = global_draw_state.mask_set != 0u,
            .mask_check = global_draw_state.mask_check != 0u,
        },
        .authenticated = true,
        .helper_arguments_authenticated = true,
        .projection_state_authenticated = true,
    };
    return observation;
}

void xg_world_decorations_shadow_reset(void) {
    xg_world_decorations_shadow_state_reset(&global_shadow);
    global_draw_state = (GpuDrawState){0};
    global_authentication_generation = 0u;
    global_screen_x_cull_margin = 0;
}

XgWorldDecorationsShadowResult xg_world_decorations_shadow_outer_begin(
    CPUState *cpu, uint64_t authentication_generation,
    const GpuDrawState *draw_state, int32_t screen_x_cull_margin) {
    XgWorldDecorationsShadowObservation observation;

    if (cpu == NULL || draw_state == NULL ||
        authentication_generation == 0u || cpu->read_word == NULL ||
        cpu->read_half == NULL) {
        return xg_world_decorations_shadow_state_block(
            &global_shadow,
            XG_WORLD_DECORATIONS_SHADOW_BLOCKER_INVALID_OBSERVATION, 0u);
    }
    global_draw_state = *draw_state;
    global_authentication_generation = authentication_generation;
    global_screen_x_cull_margin = screen_x_cull_margin;
    observation = cpu_observation(
        cpu, XG_WORLD_DECORATIONS_SHADOW_OUTER_BEGIN_PC);
    return xg_world_decorations_shadow_state_outer_begin(&global_shadow,
                                                          &observation);
}

XgWorldDecorationsShadowResult xg_world_decorations_shadow_helper_begin(
    CPUState *cpu) {
    XgWorldDecorationsShadowObservation observation = cpu_observation(
        cpu, XG_WORLD_DECORATIONS_SHADOW_HELPER_BEGIN_PC);

    return xg_world_decorations_shadow_state_helper_begin(&global_shadow,
                                                           &observation);
}

XgWorldDecorationsShadowResult xg_world_decorations_shadow_helper_finish(
    CPUState *cpu) {
    XgWorldDecorationsShadowObservation observation = cpu_observation(
        cpu, XG_WORLD_DECORATIONS_SHADOW_HELPER_FINISH_PC);

    return xg_world_decorations_shadow_state_helper_finish(&global_shadow,
                                                            &observation);
}

XgWorldDecorationsShadowResult xg_world_decorations_shadow_outer_finish(
    CPUState *cpu) {
    XgWorldDecorationsShadowObservation observation = cpu_observation(
        cpu, XG_WORLD_DECORATIONS_SHADOW_OUTER_FINISH_PC);

    return xg_world_decorations_shadow_state_outer_finish(&global_shadow,
                                                           &observation);
}

void xg_world_decorations_shadow_lifecycle_invalidate(void) {
    (void)xg_world_decorations_shadow_state_lifecycle_invalidate(
        &global_shadow, 0u);
    if (!global_shadow.snapshot.outer_active) {
        global_authentication_generation = 0u;
        global_draw_state = (GpuDrawState){0};
    }
}

void xg_world_decorations_shadow_lifecycle_block(void) {
    (void)xg_world_decorations_shadow_state_block(
        &global_shadow, XG_WORLD_DECORATIONS_SHADOW_BLOCKER_EXPLICIT, 0u);
}

bool xg_world_decorations_shadow_record_native_cutover(
    uint32_t primitive_count) {
    XgWorldDecorationsShadowSnapshot *snapshot = &global_shadow.snapshot;

    if (snapshot->blocked ||
        snapshot->phase != XG_WORLD_DECORATIONS_SHADOW_PHASE_IDLE ||
        snapshot->outer_active || snapshot->helper_active ||
        snapshot->native_cutover_count == UINT64_MAX ||
        snapshot->native_primitive_count > UINT64_MAX - primitive_count)
        return false;
    ++snapshot->native_cutover_count;
    snapshot->native_primitive_count += primitive_count;
    return true;
}

XgWorldDecorationsShadowResult xg_world_decorations_shadow_snapshot(
    XgWorldDecorationsShadowSnapshot *out_snapshot) {
    return xg_world_decorations_shadow_state_snapshot(&global_shadow,
                                                       out_snapshot);
}
