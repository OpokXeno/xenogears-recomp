#include "cdrom.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

static uint8_t ram[0x200000];
static int game_started = 1;
static CDROMDataReadPolicy policy;

int psx_mod_game_started(void) { return game_started; }
uint8_t psx_mod_read_byte(uint32_t a) { return ram[a - 0x80000000u]; }
uint16_t psx_mod_read_half(uint32_t a) {
    return psx_mod_read_byte(a) | ((uint16_t)psx_mod_read_byte(a + 1u) << 8);
}
uint32_t psx_mod_read_word(uint32_t a) {
    return psx_mod_read_half(a) | ((uint32_t)psx_mod_read_half(a + 2u) << 16);
}
void cdrom_set_data_read_policy(CDROMDataReadPolicy p) { policy = p; }
void cdrom_set_data_read_policy_enabled(int enabled) { (void)enabled; }

static void word(uint32_t a, uint32_t value) {
    for (uint32_t i = 0; i < 4u; ++i)
        ram[a - 0x80000000u + i] = (uint8_t)(value >> (8u * i));
}
static void entry(uint32_t index, uint32_t lba, int32_t bytes) {
    const uint32_t a = 0x80100000u + index * 7u;
    for (uint32_t i = 0; i < 3u; ++i)
        ram[a - 0x80000000u + i] = (uint8_t)(lba >> (8u * i));
    word(a + 3u, (uint32_t)bytes);
}
static void fixture(uint32_t offset, uint32_t lba) {
    memset(ram, 0, sizeof(ram));
    word(0x8004FDF0u, 0x80100000u);
    word(0x8004FDF4u, 0x80110000u);
    word(0x80110008u, offset + 1u);
    entry(offset + 0xB7u - 1u, lba, -1460);
    entry(offset + 0xB7u, lba, 4097); /* three sectors, including tail */
    entry(offset + 0xB7u + 1u, lba + 4u, 2048); /* one-sector gap */
    entry(offset + 0x6B8u - 1u, lba + 10000u, -145);
}
#define CHECK(condition) do { if (!(condition)) { \
    fprintf(stderr, "failed at line %d: %s\n", __LINE__, #condition); return 1; \
} } while (0)

int main(void) {
    CHECK(policy != NULL);
    for (uint32_t disc = 0; disc < 2u; ++disc) {
        const uint32_t offset = disc ? 418u : 423u;
        const int lba = disc ? 185010 : 120698;
        fixture(offset, (uint32_t)lba);
        CDROMDataReadRange range = policy(lba + 2);
        CHECK(range.first_lba == lba && range.end_lba == lba + 3);
        CHECK(range.sectors_per_frame == 32);
        CHECK(policy(lba - 1).sectors_per_frame == 0);
        CHECK(policy(lba + 3).sectors_per_frame == 0); /* gap, not a map */
        CHECK(policy(lba + 4).sectors_per_frame == 32);
        CHECK(policy(lba + 5).sectors_per_frame == 0);
        CHECK(policy(lba + 10000).sectors_per_frame == 0); /* models */
        CHECK(policy(239265).sectors_per_frame == 0); /* menu */
        CHECK(policy(111023).sectors_per_frame == 0); /* music */
        game_started = 0;
        CHECK(policy(lba).sectors_per_frame == 0); /* BIOS */
        game_started = 1;
        entry(offset + 0xB7u, (uint32_t)lba, INT32_MAX);
        CHECK(policy(lba).sectors_per_frame == 0); /* malformed span */
        entry(offset + 0xB7u, (uint32_t)lba, 0);
        CHECK(policy(lba).sectors_per_frame == 0); /* restored empty entry */
        entry(offset + 0xB7u - 1u, (uint32_t)lba, -12);
        CHECK(policy(lba + 4).sectors_per_frame == 0); /* unknown group */
    }
    fixture(423u, 120698u);
    word(0x8004FDF0u, 0x801FFFFFu);
    CHECK(policy(120698).sectors_per_frame == 0);
    fixture(423u, 120698u);
    word(0x80110008u, 65535u);
    CHECK(policy(120698).sectors_per_frame == 0);
    puts("map read selection: PASS");
    return 0;
}
