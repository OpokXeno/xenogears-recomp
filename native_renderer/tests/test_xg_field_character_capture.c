#include "xg_field_character_capture.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#define CHECK(expression) do { if (!(expression)) return 0; } while (0)

typedef struct ReaderFixture {
    uint32_t packet_start;
    uint32_t packet_end;
    uint32_t addresses[32];
    uint32_t count;
    uint32_t fail_address;
    int touched_packet;
} ReaderFixture;

static bool read_semantic_u16(void *context, uint32_t address,
                              uint16_t *out_value) {
    ReaderFixture *fixture = (ReaderFixture *)context;

    if (address >= fixture->packet_start && address < fixture->packet_end) {
        fixture->touched_packet = 1;
        return false;
    }
    if (fixture->count < 32u) fixture->addresses[fixture->count++] = address;
    if (address == fixture->fail_address) return false;
    if (address >= 0x800add70u && address < 0x800addb8u) {
        static const uint16_t u[4] = { (uint16_t)-1, 12u, 240u, 300u };
        *out_value = u[(address & 7u) / 2u];
        return true;
    }
    if (address >= 0x800addb8u && address < 0x800ade00u) {
        static const uint16_t v[4] = { 3u, 17u, 200u, 255u };
        *out_value = v[(address & 7u) / 2u];
        return true;
    }
    if (address >= 0x800ade00u && address < 0x800ade18u) {
        static const uint16_t material[6] = {
            2u, 1u, 320u, 256u, 32u, 7u,
        };
        *out_value = material[((address - 0x800ade00u) % 12u) / 2u];
        return true;
    }
    return false;
}

static XgFieldCharacterGeometryInput geometry(uint32_t packet_address) {
    XgFieldCharacterGeometryInput value = { 0 };
    size_t index;

    value.packet_guest_address = packet_address;
    for (index = 0u; index < XG_FIELD_CHARACTER_VERTEX_COUNT; ++index) {
        value.x[index] = (int16_t)(10 + (int)index * 20);
        value.y[index] = (int16_t)(-5 + (int)index * 15);
    }
    return value;
}

static XgFieldCharacterDrawStateInput draw_state(void) {
    XgFieldCharacterDrawStateInput state = { 0 };

    state.draw_area_right = 319u;
    state.draw_area_bottom = 239u;
    state.draw_offset_x = -12;
    state.draw_offset_y = 8;
    state.texture_window_mask_x = 1u;
    state.texture_window_mask_y = 2u;
    state.texture_window_offset_x = 3u;
    state.texture_window_offset_y = 4u;
    state.dither = 1u;
    state.mask_check = 1u;
    return state;
}

static int test_normal_template_uses_only_semantic_tables(void) {
    const uint32_t packet = 0x800b06bcu + 0x70u * 6u + 0x20u;
    XgFieldCharacterGeometryInput input = geometry(packet);
    XgFieldCharacterDrawStateInput state = draw_state();
    ReaderFixture fixture = { .packet_start = packet, .packet_end = packet + 40u };
    XgFieldCharacterSemanticReader reader = { &fixture, read_semantic_u16 };
    XgFieldCharacterCapture capture;
    XgFieldCharacterCandidate candidate;

    CHECK(xg_field_character_capture_build(&input, &state, &reader, &capture) ==
          XG_FIELD_CHARACTER_CAPTURE_OK);
    CHECK(!fixture.touched_packet);
    CHECK(fixture.count == 14u);
    CHECK(fixture.addresses[0] == 0x800add70u + 2u * 8u);
    CHECK(fixture.addresses[4] == 0x800addb8u + 1u * 8u);
    CHECK(fixture.addresses[8] == 0x800ade00u);
    CHECK(capture.vertices[0].u == 0u);
    CHECK(capture.vertices[1].u == 12u);
    CHECK(capture.vertices[2].u == 240u);
    CHECK(capture.vertices[3].u == 255u);
    CHECK(capture.vertices[2].v == 200u);
    CHECK(capture.red == 0x80u && capture.green == 0x80u &&
          capture.blue == 0x80u);
    CHECK(capture.tpage == 0x135u);
    CHECK(capture.clut_x == 32u && capture.clut_y == 7u);
    CHECK(xg_field_character_adapter_build(&capture, &candidate) ==
          XG_FIELD_CHARACTER_ADAPTER_OK);
    return 1;
}

static int test_special_template_selects_second_material_record(void) {
    const uint32_t packet = 0x800b0dbcu + 0x70u * 3u + 0x48u;
    XgFieldCharacterGeometryInput input = geometry(packet);
    XgFieldCharacterDrawStateInput state = draw_state();
    ReaderFixture fixture = { .packet_start = packet, .packet_end = packet + 40u };
    XgFieldCharacterSemanticReader reader = { &fixture, read_semantic_u16 };
    XgFieldCharacterCapture capture;

    CHECK(xg_field_character_capture_build(&input, &state, &reader, &capture) ==
          XG_FIELD_CHARACTER_CAPTURE_OK);
    CHECK(!fixture.touched_packet);
    CHECK(fixture.addresses[0] == 0x800add70u + 7u * 8u);
    CHECK(fixture.addresses[4] == 0x800addb8u + 7u * 8u);
    CHECK(fixture.addresses[8] == 0x800ade0cu);
    return 1;
}

static int test_unmapped_or_failed_semantic_input_fails_closed(void) {
    XgFieldCharacterGeometryInput input = geometry(0x80100000u);
    XgFieldCharacterDrawStateInput state = draw_state();
    ReaderFixture fixture = { 0 };
    XgFieldCharacterSemanticReader reader = { &fixture, read_semantic_u16 };
    XgFieldCharacterCapture capture;
    XgFieldCharacterCapture zero = { 0 };

    memset(&capture, 0xa5, sizeof(capture));
    CHECK(xg_field_character_capture_build(&input, &state, &reader, &capture) ==
          XG_FIELD_CHARACTER_CAPTURE_UNMAPPED_TEMPLATE);
    CHECK(memcmp(&capture, &zero, sizeof(capture)) == 0);
    input = geometry(0x800b06bcu + 0x20u);
    fixture.fail_address = 0x800add70u;
    CHECK(xg_field_character_capture_build(&input, &state, &reader, &capture) ==
          XG_FIELD_CHARACTER_CAPTURE_SEMANTIC_READ_FAILED);
    return 1;
}

int main(void) {
    return !(test_normal_template_uses_only_semantic_tables() &&
             test_special_template_selects_second_material_record() &&
             test_unmapped_or_failed_semantic_input_fails_closed());
}
