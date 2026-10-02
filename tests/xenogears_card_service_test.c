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
void psx_mod_write_word(uint32_t a, uint32_t v) {
    for (uint32_t i = 0; i < 4u; ++i)
        ram[a - 0x80000000u + i] = (uint8_t)(v >> (8u * i));
}
void psx_mod_set_vblank_entry_hook(PSXModVBlankCallback hook) { (void)hook; }

static const XgCardKernel *kernel;

static void fixture(const XgCardKernel *k, uint32_t port, uint8_t state0, uint8_t state1) {
    memset(ram, 0, sizeof(ram));
    kernel = k;
    for (uint32_t i = 0; i < k->code_words; ++i)
        psx_mod_write_word(k->code + i * 4u, k->expected[i]);
    if (k->port_is_word) psx_mod_write_word(k->port, port);
    else psx_mod_write_byte(k->port, (uint8_t)port);
    psx_mod_write_byte(k->state, state0);
    psx_mod_write_byte(k->state + 1u, state1);
    game_started = 1;
}
static uint32_t port(void) {
    return kernel->port_is_word ? psx_mod_read_word(kernel->port)
                                : psx_mod_read_byte(kernel->port);
}
#define CHECK(condition) do { if (!(condition)) { \
    fprintf(stderr, "failed at line %d (kernel %u): %s\n", __LINE__, k, #condition); return 1; \
} } while (0)

int main(void) {
    for (unsigned k = 0; k < sizeof(xg_kernels) / sizeof(xg_kernels[0]); ++k) {
        const XgCardKernel *kn = &xg_kernels[k];
        fixture(kn, 0, 0x02, 0x01); /* read in flight on port 0, port 1 idle */
        xg_card_service_entry();
        CHECK(port() == 1); /* BIOS toggles back onto port 0 */
        fixture(kn, 1, 0x01, 0x04); /* write in flight on port 1 */
        xg_card_service_entry();
        CHECK(port() == 0);
        fixture(kn, 1, 0x02, 0x01); /* BIOS is about to service the busy port */
        xg_card_service_entry();
        CHECK(port() == 1);
        fixture(kn, 0, 0x01, 0x01); /* nothing in flight */
        xg_card_service_entry();
        CHECK(port() == 0);
        fixture(kn, 0, 0x02, 0x04); /* both busy: authentic alternation */
        xg_card_service_entry();
        CHECK(port() == 0);
        fixture(kn, 0, 0x08, 0x11); /* reset value counts as idle */
        xg_card_service_entry();
        CHECK(port() == 1);
        fixture(kn, 0, 0x02, 0x01);
        psx_mod_write_byte(kn->code + 8u, psx_mod_read_byte(kn->code + 8u) ^ 1u); /* other kernel */
        xg_card_service_entry();
        CHECK(port() == 0);
        fixture(kn, 0, 0x02, 0x01);
        game_started = 0;
        xg_card_service_entry();
        CHECK(port() == 0);
        fixture(kn, 2, 0x02, 0x01); /* corrupt port value */
        xg_card_service_entry();
        CHECK(port() == 2);
    }
    /* A kernel never matches the other kernel's layout. */
    fixture(&xg_kernels[0], 0, 0x02, 0x01);
    if (xg_kernel_matches(&xg_kernels[1])) {
        fputs("retail layout must not match OpenBIOS\n", stderr);
        return 1;
    }
    puts("Card port service hold (retail and OpenBIOS): PASS");
    return 0;
}
