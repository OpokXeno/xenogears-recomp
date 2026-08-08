#include "xg_field_character_runtime.h"

#include "gpu.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#define CHECK(expression) do { if (!(expression)) return 0; } while (0)

static uint32_t packet_address;
static uint32_t packet_words[9];
static uint32_t packet_read_count;
static uint32_t semantic_read_count;

static uint16_t read_half(uint32_t address) {
    ++semantic_read_count;
    if (address >= 0x800add70u && address < 0x800addb8u) {
        static const uint16_t u[4] = { 0u, 16u, 0u, 16u };
        return u[(address & 7u) / 2u];
    }
    if (address >= 0x800addb8u && address < 0x800ade00u) {
        static const uint16_t v[4] = { 0u, 0u, 32u, 32u };
        return v[(address & 7u) / 2u];
    }
    if (address >= 0x800ade00u && address < 0x800ade18u) {
        static const uint16_t material[6] = { 0u, 0u, 0u, 0u, 32u, 7u };
        return material[((address - 0x800ade00u) % 12u) / 2u];
    }
    return 0xffffu;
}

static uint32_t read_word(uint32_t address) {
    ++packet_read_count;
    if (address < packet_address + 4u || address >= packet_address + 40u ||
        (address & 3u) != 0u)
        return 0u;
    return packet_words[(address - packet_address - 4u) / 4u];
}

void gpu_get_draw_state(GpuDrawState *out) {
    memset(out, 0, sizeof(*out));
    out->right = 319u;
    out->bottom = 239u;
    out->offset_x = -12;
    out->offset_y = 8;
    out->dither = 1u;
    out->mask_check = 1u;
}

static uint32_t xy(int16_t x, int16_t y) {
    return (uint16_t)x | ((uint32_t)(uint16_t)y << 16u);
}

static int test_candidate_finalizes_before_original_is_read(void) {
    CPUState cpu = {0};
    PsxXgRenderFt4Geometry geometry = {0};
    XgFieldCharacterRuntimeCandidate candidate;
    XgFieldCharacterRuntimeCandidate unchanged;
    XgRenderIrNativePrimitive primitive;
    size_t index;

    packet_address = 0x800b06bcu + 0x20u;
    geometry.packet_guest_address = packet_address;
    geometry.host_transformed = true;
    geometry.x[0] = -10; geometry.y[0] = 20;
    geometry.x[1] = 6; geometry.y[1] = 20;
    geometry.x[2] = -10; geometry.y[2] = 52;
    geometry.x[3] = 6; geometry.y[3] = 52;
    cpu.read_half = read_half;
    cpu.read_word = read_word;
    semantic_read_count = 0u;
    packet_read_count = 0u;
    CHECK(xg_field_character_runtime_build_shadow_candidate(&cpu, &geometry,
                                                            &candidate) ==
          XG_FIELD_CHARACTER_RUNTIME_OK);
    CHECK(candidate.valid == 1u);
    CHECK(candidate.candidate.finalized == 1u);
    CHECK(semantic_read_count == 14u);
    CHECK(packet_read_count == 0u);
    unchanged = candidate;

    packet_words[0] = 0x2c808080u;
    packet_words[1] = xy(-10, 20);
    packet_words[2] = 0x01c20000u;
    packet_words[3] = xy(6, 20);
    packet_words[4] = 0x00000010u;
    packet_words[5] = xy(-10, 52);
    packet_words[6] = 0x00002000u;
    packet_words[7] = xy(6, 52);
    packet_words[8] = 0x00002010u;
    CHECK(xg_field_character_runtime_compare_original(&cpu, &candidate, 77u,
                                                       77u, &primitive, NULL,
                                                       NULL, NULL) ==
          XG_FIELD_CHARACTER_RUNTIME_OK);
    CHECK(packet_read_count == 9u);
    CHECK(primitive.triangle_count == 2u);
    CHECK(memcmp(&candidate, &unchanged, sizeof(candidate)) == 0);

    packet_words[8] ^= 1u;
    CHECK(xg_field_character_runtime_compare_original(&cpu, &candidate, 77u,
                                                       77u, &primitive, NULL,
                                                       NULL, NULL) ==
          XG_FIELD_CHARACTER_RUNTIME_COMPARE_FAILED);
    CHECK(memcmp(&candidate, &unchanged, sizeof(candidate)) == 0);
    for (index = 0u; index < sizeof(primitive); ++index)
        CHECK(((const uint8_t *)&primitive)[index] == 0u);
    return 1;
}

int main(void) {
    return !test_candidate_finalizes_before_original_is_read();
}
