#include "cpu_state.h"
#include "mod_plugins.h"

#include <stdint.h>

enum {
    XG_VSYNC = 0x8004B54C,
    XG_FIELD_PRESENT_VSYNC_RETURN = 0x800756D4,
    XG_VBLANK_COUNTER = 0x80058960
};

static void xg_field_pacing_vsync_entry(CPUState *cpu, uint32_t address) {
    (void)address;

    /* Field keeps the iteration's initial VBlank count in s1. Preserve the
     * original wait unless scene work has already crossed a VBlank boundary. */
    if (cpu->gpr[31] == XG_FIELD_PRESENT_VSYNC_RETURN && cpu->gpr[4] == 0 &&
        (int32_t)(psx_mod_read_word(XG_VBLANK_COUNTER) - cpu->gpr[17]) > 0)
        cpu->gpr[4] = UINT32_MAX;
}

PSX_MOD_CONSTRUCTOR(xg_register_field_pacing_hook) {
    (void)psx_mod_register_function_entry_plugin(
        "xenogears.field-pacing", XG_VSYNC, xg_field_pacing_vsync_entry);
}
