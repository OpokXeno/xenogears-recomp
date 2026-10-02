#include "mod_plugins.h"

#include <stdint.h>
#include <stdlib.h>

typedef struct XgCardKernel {
    uint32_t port;        /* current port: a byte (retail) or a word (OpenBIOS) */
    int port_is_word;
    uint32_t state;       /* per-port state bytes; bit 0 set means idle */
    uint32_t code;        /* per-VBlank service code that toggles and skips */
    uint32_t code_words;
    const uint32_t *expected;
} XgCardKernel;

/* The per-VBlank card/pad service toggles the current port and skips a port
 * whose state has bit 0 set (idle). */
static const uint32_t xg_retail_code[] = {
    0x3C0E0000u, 0x91CE7264u, 0x240F0001u, 0x3C010000u, 0x01EEC023u,
    0xA0387264u, 0x3C080000u, 0x91087264u, 0x3C070000u, 0x24E77568u,
    0x00E8C821u, 0x93290000u, 0x00000000u, 0x312A0001u, 0x11400003u,
    0x00000000u, 0x1000001Eu
};
static const uint32_t xg_openbios_code[] = {
    0x8C854C58u, 0x24030001u, 0x00651823u, 0x00431021u, 0xAC834C58u,
    0x90440000u, 0x00000000u, 0x30840001u, 0x1480FFF2u, 0x00031880u
};
static const XgCardKernel xg_kernels[] = {
    { 0x80007264u, 0, 0x80007568u, 0x80005128u, 17u, xg_retail_code },
    { 0x80004C58u, 1, 0x80004C4Cu, 0x80003FF0u, 10u, xg_openbios_code }
};

static int xg_kernel_matches(const XgCardKernel *k) {
    for (uint32_t i = 0; i < k->code_words; ++i)
        if (psx_mod_read_word(k->code + i * 4u) != k->expected[i]) return 0;
    return 1;
}

/* A card transfer owns one port but only gets a frame when the BIOS toggles
 * onto it; the other port's frame is an idle skip. Keeping the busy port
 * removes the idle frame and starts the next sector a VBlank sooner. */
static int xg_card_service_hold(const XgCardKernel *k) {
    const uint32_t port = k->port_is_word ? psx_mod_read_word(k->port)
                                          : psx_mod_read_byte(k->port);
    if (port > 1u) return 0;
    const uint32_t other = port ^ 1u;
    if (!(psx_mod_read_byte(k->state + other) & 1u)) return 0;
    if (psx_mod_read_byte(k->state + port) & 1u) return 0;
    if (!xg_kernel_matches(k)) return 0;
    if (k->port_is_word) psx_mod_write_word(k->port, other);
    else psx_mod_write_byte(k->port, (uint8_t)other);
    return 1;
}

static void xg_card_service_entry(void) {
    if (!psx_mod_game_started()) return;
    for (uint32_t i = 0; i < sizeof(xg_kernels) / sizeof(xg_kernels[0]); ++i)
        if (xg_card_service_hold(&xg_kernels[i])) return;
}

PSX_MOD_CONSTRUCTOR(xg_register_card_service) {
    const char *disabled = getenv("XG_FAST_CARD_SERVICE");
    if (!disabled || disabled[0] != '0')
        psx_mod_set_vblank_entry_hook(xg_card_service_entry);
}
