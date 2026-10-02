#include "../src/mods/xenogears_card_flow.c"

#include <stdio.h>
#include <string.h>

static uint8_t ram[0x200000];

uint8_t psx_mod_read_byte(uint32_t a) { return ram[a - 0x80000000u]; }
uint32_t psx_mod_read_word(uint32_t a) {
    uint32_t v = 0;
    for (uint32_t i = 0; i < 4u; ++i)
        v |= (uint32_t)ram[a - 0x80000000u + i] << (8u * i);
    return v;
}
static void word(uint32_t a, uint32_t v) {
    for (uint32_t i = 0; i < 4u; ++i) ram[a - 0x80000000u + i] = (uint8_t)(v >> (8u * i));
}
int psx_mod_register_function_entry_plugin(const char *id, uint32_t address,
                                           PSXModFunctionEntryCallback callback) {
    (void)id; (void)address; (void)callback;
    return 1;
}

#define CHECK(condition) do { if (!(condition)) { \
    fprintf(stderr, "failed at line %d: %s\n", __LINE__, #condition); return 1; \
} } while (0)

int main(void) {
    const uint32_t sp = 0x80180000u;

    /* Scan restart after the menu's own restart: skip. */
    memset(ram, 0, sizeof(ram));
    word(sp + 0x10u, XG_SCAN_RESTART_RETURN);
    word(sp + 0x18u + 0xE4u, XG_INIT_CARDS_SCAN_RETURN);
    CHECK(xg_skip_scan_bu_init(XG_RESTART_CALLS_BU_INIT, sp));
    CHECK(!xg_skip_scan_bu_init(0x80040000u, sp));              /* another caller */
    word(sp + 0x18u + 0xE4u, 0x801CB350u);                        /* scan from an operation */
    CHECK(!xg_skip_scan_bu_init(XG_RESTART_CALLS_BU_INIT, sp));
    word(sp + 0x18u + 0xE4u, XG_INIT_CARDS_SCAN_RETURN);
    word(sp + 0x10u, 0x801D9CFCu);                                /* the menu's own restart */
    CHECK(!xg_skip_scan_bu_init(XG_RESTART_CALLS_BU_INIT, sp));

    /* Menu frame inside the save or load chunk loop: drop the VSync wait. */
    memset(ram, 0, sizeof(ram));
    word(sp + 0x14u, XG_SAVE_LOOP_DRAW_RETURN);
    CHECK(xg_menu_draw_in_card_loop(XG_MENU_DRAW_VSYNC_RETURN, sp, 0));
    word(sp + 0x14u, XG_LOAD_LOOP_DRAW_RETURN);
    CHECK(xg_menu_draw_in_card_loop(XG_MENU_DRAW_VSYNC_RETURN, sp, 0));
    word(sp + 0x14u, 0x801C55C4u);                                /* ordinary menu frame */
    CHECK(!xg_menu_draw_in_card_loop(XG_MENU_DRAW_VSYNC_RETURN, sp, 0));
    word(sp + 0x14u, XG_SAVE_LOOP_DRAW_RETURN);
    CHECK(!xg_menu_draw_in_card_loop(XG_MENU_DRAW_VSYNC_RETURN, sp, 1));
    CHECK(!xg_menu_draw_in_card_loop(0x8004B000u, sp, 0));

    /* Save payload written in 0x400-byte calls with the loop state advanced. */
    memset(ram, 0, sizeof(ram));
    word(XG_MENU_CONTEXT, 0x80100000u);
    word(0x80100000u + 0x32Cu, 0x80110000u);
    ram[0x80110000u + 0x4B97u - 0x80000000u] = 1;                 /* one block: 0x2000 bytes */
    uint32_t gpr[32] = {0};
    gpr[19] = 0x100u; gpr[18] = 0x80120000u; gpr[6] = 0x100u; gpr[22] = 0x100u;
    CHECK(xg_save_chunk_adjust(XG_SAVE_LOOP_WRITE_RETURN, gpr));
    CHECK(gpr[6] == 0x400u && gpr[22] == 0x400u);
    CHECK(gpr[19] == 0x100u + 0x400u - 0x100u);                   /* loop adds 0x100 afterwards */
    CHECK(gpr[18] == 0x80120000u + 0x300u);
    gpr[19] = 0x1D00u; /* near the end */
    gpr[6] = 0x100u;
    CHECK(xg_save_chunk_adjust(XG_SAVE_LOOP_WRITE_RETURN, gpr));
    CHECK(gpr[6] == 0x300u);                                      /* remainder only */
    gpr[19] = 0x1F00u; gpr[6] = 0x100u;
    CHECK(!xg_save_chunk_adjust(XG_SAVE_LOOP_WRITE_RETURN, gpr)); /* last 0x100: unchanged */
    gpr[19] = 0x100u;
    CHECK(!xg_save_chunk_adjust(0x801CC000u, gpr));               /* some other caller */
    gpr[19] = 0x180u + 1u;
    CHECK(!xg_save_chunk_adjust(XG_SAVE_LOOP_WRITE_RETURN, gpr)); /* unaligned progress */
    ram[0x80110000u + 0x4B97u - 0x80000000u] = 0;                 /* no block count */
    gpr[19] = 0x100u;
    CHECK(!xg_save_chunk_adjust(XG_SAVE_LOOP_WRITE_RETURN, gpr));

    puts("Card flow hooks: PASS");
    return 0;
}
