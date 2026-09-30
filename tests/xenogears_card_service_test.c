#include "../src/mods/xenogears_card_service.c"

#include <stdio.h>
#include <string.h>

static uint8_t ram[0x200000];
static int game_started = 1;

int psx_mod_game_started(void) { return game_started; }
uint8_t psx_mod_read_byte(uint32_t a) { return ram[a - 0x80000000u]; }
uint32_t psx_mod_read_word(uint32_t a) {
    uint32_t v = 0;
    for (uint32_t i = 0; i < 4u; ++i)
        v |= (uint32_t)ram[a - 0x80000000u + i] << (8u * i);
    return v;
}
void psx_mod_write_byte(uint32_t a, uint8_t v) { ram[a - 0x80000000u] = v; }
void psx_mod_set_vblank_entry_hook(PSXModVBlankCallback hook) { (void)hook; }

static void fixture(uint8_t port, uint8_t state0, uint8_t state1) {
    memset(ram, 0, sizeof(ram));
    for (uint32_t i = 0; i < XG_CARD_SERVICE_WORDS; ++i)
        for (uint32_t b = 0; b < 4u; ++b)
            ram[XG_CARD_SERVICE - 0x80000000u + i * 4u + b] =
                (uint8_t)(xg_card_service_code[i] >> (8u * b));
    ram[XG_CARD_PORT - 0x80000000u] = port;
    ram[XG_CARD_PORT_STATE - 0x80000000u] = state0;
    ram[XG_CARD_PORT_STATE - 0x80000000u + 1u] = state1;
    game_started = 1;
}
static uint8_t port(void) { return ram[XG_CARD_PORT - 0x80000000u]; }
#define CHECK(condition) do { if (!(condition)) { \
    fprintf(stderr, "failed at line %d: %s\n", __LINE__, #condition); return 1; \
} } while (0)

int main(void) {
    fixture(0, 0x02, 0x01); /* read in flight on port 0, port 1 idle */
    xg_card_service_entry();
    CHECK(port() == 1); /* BIOS toggles back onto port 0 */
    fixture(1, 0x01, 0x04); /* write in flight on port 1 */
    xg_card_service_entry();
    CHECK(port() == 0);
    fixture(1, 0x02, 0x01); /* BIOS is about to service the busy port */
    xg_card_service_entry();
    CHECK(port() == 1);
    fixture(0, 0x01, 0x01); /* nothing in flight */
    xg_card_service_entry();
    CHECK(port() == 0);
    fixture(0, 0x02, 0x04); /* both busy: authentic alternation */
    xg_card_service_entry();
    CHECK(port() == 0);
    fixture(0, 0x08, 0x11); /* reset value counts as idle */
    xg_card_service_entry();
    CHECK(port() == 1);
    fixture(0, 0x02, 0x01);
    ram[XG_CARD_SERVICE - 0x80000000u + 16u] ^= 1u; /* other kernel */
    xg_card_service_entry();
    CHECK(port() == 0);
    fixture(0, 0x02, 0x01);
    game_started = 0;
    xg_card_service_entry();
    CHECK(port() == 0);
    fixture(2, 0x02, 0x01); /* corrupt port value */
    xg_card_service_entry();
    CHECK(port() == 2);
    puts("Card port service hold: PASS");
    return 0;
}
