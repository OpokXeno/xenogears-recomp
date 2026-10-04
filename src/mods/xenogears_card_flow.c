#include "cpu_state.h"
#include "mod_plugins.h"

#include <stdint.h>
#include <stdlib.h>

enum {
    XG_INIT_BIOS_UTILITY = 0x80040464u,
    XG_STORE_FILE_BYTES = 0x80040554u,
    XG_VSYNC = 0x8004B54Cu,
    XG_VBLANK_COUNT = 0x80058960u, /* VSync's retrace counter */
    XG_MENU_CONTEXT = 0x800625A0u,
    XG_MENU_FRAME_OFFSET = 0x2D8u, /* MenuDraw frame counter */
    /* Return addresses inside the menu overlay. */
    XG_RESTART_CALLS_BU_INIT = 0x801D9B38u,
    XG_SCAN_RESTART_RETURN = 0x801C95F8u,
    XG_INIT_CARDS_SCAN_RETURN = 0x801D9DD4u,
    XG_MENU_DRAW_VSYNC_RETURN = 0x801C7CE8u,
    XG_SAVE_LOOP_DRAW_RETURN = 0x801CC460u,
    XG_LOAD_LOOP_DRAW_RETURN = 0x801CB678u,
    XG_SAVE_LOOP_WRITE_RETURN = 0x801CC470u,
    XG_SAVE_CHUNK_BYTES = 0x400u,
    XG_LOAD_STEP_VBLANKS = 4u
};

/* The card check restarts the card interface twice: once when the menu opens
 * and once from the scan, whose "presence changed" test always fires against
 * its 0xFF placeholder. Each restart makes the BIOS re-read the whole card, 74
 * sectors at one per VBlank. The scan's restart follows the first one with no
 * card change in between, so its _bu_init is skipped. Everything is read from
 * the stack: Restart's frame (0x18) holds its return address at +0x10, and the
 * scan's frame (0xE8) holds its caller at +0xE4. */
static int xg_skip_scan_bu_init(uint32_t ra, uint32_t sp) {
    if (ra != XG_RESTART_CALLS_BU_INIT) return 0;
    if (psx_mod_read_word(sp + 0x10u) != XG_SCAN_RESTART_RETURN) return 0;
    return psx_mod_read_word(sp + 0x18u + 0xE4u) == XG_INIT_CARDS_SCAN_RETURN;
}

static int xg_card_flow_bu_init_entry(CPUState *cpu, uint32_t address) {
    (void)address;
    return xg_skip_scan_bu_init(cpu->gpr[31], cpu->gpr[29]);
}

/* Save and load move one 0x100-byte chunk per MenuDraw, and MenuDraw ends in
 * VSync(0). Inside those two loops the chunk, not the display, sets the pace,
 * so the wait is dropped. MenuDraw keeps its return address at +0x14; the
 * loop it was called from is returned (0 outside them). */
static uint32_t xg_menu_draw_card_loop(uint32_t ra, uint32_t sp, uint32_t wait) {
    if (ra != XG_MENU_DRAW_VSYNC_RETURN || wait != 0u) return 0;
    const uint32_t caller = psx_mod_read_word(sp + 0x14u);
    return caller == XG_SAVE_LOOP_DRAW_RETURN || caller == XG_LOAD_LOOP_DRAW_RETURN
        ? caller : 0u;
}

/* MenuDraw counts its calls in the menu context (+0x2D8) and the loading
 * text and red arrow blink from it (shown while count % 6 < 4, 0x801D00BC).
 * At the original card speed the load loop takes one step per 4 VBlanks
 * (measured: 6, 3, 3 VBlanks per step), a 24-VBlank blink. Without the wait
 * the count would race, so a load-loop step that comes sooner takes its
 * increment back. */
static uint32_t xg_load_step_vblank;

static int xg_card_flow_vsync_entry(CPUState *cpu, uint32_t address) {
    (void)address;
    const uint32_t loop = xg_menu_draw_card_loop(cpu->gpr[31], cpu->gpr[29], cpu->gpr[4]);
    if (!loop) return 0;
    cpu->gpr[4] = UINT32_MAX;
    if (loop != XG_LOAD_LOOP_DRAW_RETURN) return 0;
    const uint32_t vblank = psx_mod_read_word(XG_VBLANK_COUNT);
    if (vblank - xg_load_step_vblank < XG_LOAD_STEP_VBLANKS) {
        const uint32_t frame = psx_mod_read_word(XG_MENU_CONTEXT) + XG_MENU_FRAME_OFFSET;
        psx_mod_write_word(frame, psx_mod_read_word(frame) - 1u);
    } else {
        xg_load_step_vblank = vblank;
    }
    return 0;
}

/* The save loop writes the payload in 0x100-byte BIOS calls, and every call
 * costs extra VBlanks beyond its two sectors. Larger writes keep the same
 * bytes and the same order and still step the progress bar. The loop keeps its
 * progress in s3 (bytes done), s2 (source pointer), a2 and s6 (size and the
 * count it expects back), so they are advanced together. */
static int xg_save_chunk_adjust(uint32_t ra, uint32_t *gpr) {
    if (ra != XG_SAVE_LOOP_WRITE_RETURN) return 0;
    const uint32_t done = gpr[19];
    if (done < 0x100u || (done & 0x7Fu)) return 0;
    const uint32_t context = psx_mod_read_word(XG_MENU_CONTEXT);
    const uint32_t workspace = psx_mod_read_word(context + 0x32Cu);
    const uint32_t limit = (uint32_t)psx_mod_read_byte(workspace + 0x4B97u) << 13;
    if (limit <= 0x200u || limit > 0x20000u || done >= limit) return 0;
    uint32_t chunk = limit - done;
    if (chunk > XG_SAVE_CHUNK_BYTES) chunk = XG_SAVE_CHUNK_BYTES;
    if (chunk == 0x100u) return 0;
    gpr[6] = chunk;
    gpr[22] = chunk;
    gpr[19] = done + chunk - 0x100u;
    gpr[18] += chunk - 0x100u;
    return 1;
}

static int xg_card_flow_store_entry(CPUState *cpu, uint32_t address) {
    (void)address;
    (void)xg_save_chunk_adjust(cpu->gpr[31], cpu->gpr);
    return 0;
}

PSX_MOD_CONSTRUCTOR(xg_register_card_flow) {
    const char *disabled = getenv("XG_FAST_CARD_FLOW");
    if (disabled && disabled[0] == '0') return;
    (void)psx_mod_register_function_entry_plugin(
        "xenogears.card-flow.bu-init", XG_INIT_BIOS_UTILITY, xg_card_flow_bu_init_entry);
    (void)psx_mod_register_function_entry_plugin(
        "xenogears.card-flow.vsync", XG_VSYNC, xg_card_flow_vsync_entry);
    (void)psx_mod_register_function_entry_plugin(
        "xenogears.card-flow.store", XG_STORE_FILE_BYTES, xg_card_flow_store_entry);
}
