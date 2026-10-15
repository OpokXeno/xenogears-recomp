#include "xg_render_world_model_repository.h"

#include <stdio.h>
#include <string.h>

#define CHECK(c) do { if (!(c)) { fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #c); return 0; } } while (0)

enum { MODEL = 0x800a0000u, TOPOLOGY = 0x800a1000u,
       ATTRIBUTES = 0x800a2000u, PACKETS = 0x800a3000u };
static unsigned char ram[0x200000];
static bool authenticated, deny_packets;
static CPUState cpu;

/* The repository's only external dependency: unrelated resident-model cache. */
void xg_render_model_repository_reset_templates(void) {}

static uint8_t read_byte(uint32_t a) { return ram[a & 0x1fffffu]; }
static uint16_t read_half(uint32_t a) { return read_byte(a) | (uint16_t)read_byte(a + 1u) << 8u; }
static uint32_t read_word(uint32_t a) { return read_half(a) | (uint32_t)read_half(a + 2u) << 16u; }
static void word(uint32_t a, uint32_t v) {
    for (unsigned i = 0u; i < 4u; ++i) ram[(a + i) & 0x1fffffu] = (uint8_t)(v >> (i * 8u));
}
static bool generation(uint64_t *out) { *out = 7u; return authenticated; }
static bool authorize(uint32_t a, uint32_t n, uint32_t alignment, bool scratch) {
    (void)scratch;
    const uint32_t p = a & 0x1fffffffu;
    return n && p < sizeof(ram) && n <= sizeof(ram) - p && !(a % alignment) &&
        !(deny_packets && p >= (PACKETS & 0x1fffffffu));
}
static bool stack_valid(uint32_t a) { (void)a; return true; }
static bool coordinating(void) { return false; }
static void fail(void) {}
static const XgRenderWorldModelRepositoryServices services = {
    generation, authorize, stack_valid, coordinating, fail,
};
static XgRenderWorldModelRepositoryReaderContext reader = {&cpu, &services};

static void fixture(void) {
    memset(ram, 0, sizeof(ram));
    cpu = (CPUState){0};
    cpu.read_byte = read_byte; cpu.read_half = read_half; cpu.read_word = read_word;
    authenticated = true; deny_packets = false;
    xg_render_world_model_repository_reset();
    word(MODEL + 4u, 3u | (2u << 16u));
    word(MODEL + 0x10u, TOPOLOGY); word(MODEL + 0x14u, ATTRIBUTES);
    word(MODEL + 0x34u, (5u + 6u + 6u) * 4u);
    word(TOPOLOGY, 1u << 16u);                 /* one F3 */
    word(TOPOLOGY + 12u, 8u | (2u << 16u));  /* two F4 */
    word(ATTRIBUTES, 0xc4000000u);
    word(ATTRIBUTES + 4u, 0x20808080u);
    word(ATTRIBUTES + 8u, 0xc8000000u);
    word(ATTRIBUTES + 12u, 0x28808080u);
    word(ATTRIBUTES + 16u, 0x28808080u);
    for (unsigned family = 0u; family <= 8u; family += 8u) {
        const uint32_t t = 0x8004fe50u + family * 0x28u;
        word(t + 0x18u, family ? 0x8002cf58u : 0x8002cdccu);
        word(t + 0x1cu, 8u); word(t + 0x20u, 4u);
        word(t + 0x24u, family ? 0x18u : 0x14u);
    }
    for (unsigned i = 0u; i < 17u; ++i) word(PACKETS + i * 4u, 0x100u + i);
    word(PACKETS, 4u << 24u);
    word(PACKETS + 20u, 4u << 24u);  /* initialized F4 */
    word(PACKETS + 44u, 5u << 24u);  /* previously rendered F4 */
}

static bool get(uint32_t *out, uint64_t *epoch) {
    return xg_render_world_model_repository_read_packet_template(
        &reader, MODEL, PACKETS, PACKETS, ATTRIBUTES + 4u, 0u, out, 5u, epoch);
}

static int test_cold_restore_and_address_reuse(void) {
    uint32_t words[13]; uint64_t epoch, first_epoch;
    fixture();
    CHECK(!get(words, &epoch));
    xg_render_world_model_repository_note_savestate_restore();
    CHECK(get(words, &epoch)); first_epoch = epoch;
    CHECK(epoch && words[1] == 0x101u);
    const XgRenderWorldModelTemplate *third =
        xg_render_world_model_repository_find_template(PACKETS + 44u);
    CHECK(third && third->primitive_ordinal == 2u && third->attribute_address ==
          ((ATTRIBUTES + 16u) & 0x1ffffcu));
    word(PACKETS + 4u, 0x12345678u);
    CHECK(get(words, &epoch) && words[1] == 0x101u);
    xg_render_world_model_repository_note_savestate_restore();
    CHECK(get(words, &epoch) && words[1] == 0x12345678u && epoch != first_epoch);
    xg_render_world_model_repository_invalidate();
    CHECK(!get(words, &epoch));
    return 1;
}

static int test_rejects_invalid_restored_sources(void) {
    uint32_t words[13]; uint64_t epoch;
    for (unsigned fault = 0u; fault < 6u; ++fault) {
        fixture();
        xg_render_world_model_repository_note_savestate_restore();
        switch (fault) {
        case 0u: authenticated = false; break;
        case 1u: deny_packets = true; break;
        case 2u: word(0x8004fe50u + 0x18u, 0u); break;
        case 3u: word(PACKETS + 44u, 0u); break;
        case 4u: word(MODEL + 4u, 2u | (2u << 16u)); break;
        case 5u: word(MODEL + 0x34u, 64u); break;
        }
        CHECK(!get(words, &epoch));
        CHECK(!xg_render_world_model_repository_find_template(PACKETS));
    }
    return 1;
}

int main(void) {
    return test_cold_restore_and_address_reuse() &&
        test_rejects_invalid_restored_sources() ? 0 : 1;
}
