#include "mod_plugins.h"

#include <stdint.h>
#include <stdlib.h>

enum {
    XG_CARD_PORT = 0x80007264u,
    XG_CARD_PORT_STATE = 0x80007568u,
    XG_CARD_SERVICE = 0x80005128u,
    XG_CARD_SERVICE_WORDS = 17u
};

/* Retail BIOS kernel: the per-VBlank card/pad service toggles the current
 * port and skips a port whose state has bit 0 set (idle). */
static const uint32_t xg_card_service_code[XG_CARD_SERVICE_WORDS] = {
    0x3C0E0000u, 0x91CE7264u, 0x240F0001u, 0x3C010000u, 0x01EEC023u,
    0xA0387264u, 0x3C080000u, 0x91087264u, 0x3C070000u, 0x24E77568u,
    0x00E8C821u, 0x93290000u, 0x00000000u, 0x312A0001u, 0x11400003u,
    0x00000000u, 0x1000001Eu
};

static int xg_card_service_matches(void) {
    for (uint32_t i = 0; i < XG_CARD_SERVICE_WORDS; ++i)
        if (psx_mod_read_word(XG_CARD_SERVICE + i * 4u) != xg_card_service_code[i])
            return 0;
    return 1;
}

/* A card transfer owns one port but only gets a frame when the BIOS toggles
 * onto it; the other port's frame is an idle skip. Keeping the busy port
 * removes the idle frame and starts the next sector a VBlank sooner. */
static void xg_card_service_entry(void) {
    if (!psx_mod_game_started()) return;
    const uint32_t port = psx_mod_read_byte(XG_CARD_PORT);
    if (port > 1u) return;
    const uint32_t other = port ^ 1u;
    if (!(psx_mod_read_byte(XG_CARD_PORT_STATE + other) & 1u)) return;
    if (psx_mod_read_byte(XG_CARD_PORT_STATE + port) & 1u) return;
    if (!xg_card_service_matches()) return;
    psx_mod_write_byte(XG_CARD_PORT, (uint8_t)other);
}

PSX_MOD_CONSTRUCTOR(xg_register_card_service) {
    const char *disabled = getenv("XG_FAST_CARD_SERVICE");
    if (!disabled || disabled[0] != '0')
        psx_mod_set_vblank_entry_hook(xg_card_service_entry);
}
