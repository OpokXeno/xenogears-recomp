#include "cpu_state.h"
#include "mod_plugins.h"
#include "psx_cycles.h"
#include "interrupts.h"

#include <stdint.h>
#include <stdlib.h>

enum {
    XG_VSYNC = 0x8004B54C,
    XG_FIELD_PRESENT_VSYNC_RETURN = 0x800756D4,
    XG_VBLANK_COUNTER = 0x80058960,
    XG_FIELD_WAIT_VSYNC_RETURN = 0x800758D8,
    XG_FIELD_FRAME_MODE = 0x800B217C
};

static int xg_field_wait_code_matches(void) {
    static const uint32_t instructions[] = {
        0x0C012D53u, 0x2404FFFFu, 0x3C03800Bu, 0x8C63217Cu, 0u,
        0x24630002u, 0x02231821u, 0x0043102Au, 0x1440FFF7u, 0u
    };
    for (uint32_t i = 0; i < sizeof(instructions) / sizeof(instructions[0]); ++i)
        if (psx_mod_read_word(0x800758D0u + i * 4u) != instructions[i]) return 0;
    return 1;
}

static int xg_field_pacing_vsync_entry(CPUState *cpu, uint32_t address) {
    (void)address;

    /* Field's tail polls VSync(-1) until counter >= s1 + frame_mode + 2.
     * Its iterations only rewrite VSync's temporary stack frame. Let devices
     * run to the next observable event, then execute the original call and
     * branch normally; guest IRQ handlers still update the counter. */
    if (cpu->gpr[31] == XG_FIELD_WAIT_VSYNC_RETURN && cpu->gpr[4] == UINT32_MAX) {
        extern int g_precise_mode, g_ls_mode, g_ls_replay_active;
        extern int psx_netplay_active(void), psx_selfcheck_enabled(void);
        static int enabled = -1;
        static uint64_t last_cycle;
        static uint32_t last_target, last_counter, last_sp, quantum, stable;
        if (enabled < 0) {
            const char *e = getenv("XG_FIELD_IDLE_SKIP");
            enabled = !e || e[0] != '0';
        }
        if (enabled && !g_precise_mode && !g_ls_mode && !g_ls_replay_active &&
            !psx_get_in_exception() && !psx_netplay_active() && !psx_selfcheck_enabled() &&
            (cpu->cop0[12] & 0x401u) == 0x401u &&
            (int32_t)psx_mod_read_word(XG_VBLANK_COUNTER) <
                (int32_t)(cpu->gpr[17] + psx_mod_read_word(XG_FIELD_FRAME_MODE) + 2u)) {
            psx_cyc_batch_flush();
            if (!psx_interrupt_delivery_needed(cpu) &&
                !(cpu->cop0[13] & cpu->cop0[12] & 0x300u)) {
                const uint64_t now = psx_cycle_count;
                const uint32_t counter = psx_mod_read_word(XG_VBLANK_COUNTER);
                const uint32_t target = cpu->gpr[17] + psx_mod_read_word(XG_FIELD_FRAME_MODE) + 2u;
                const uint64_t elapsed = now - last_cycle;
                if (now > last_cycle && counter == last_counter && target == last_target &&
                    cpu->gpr[29] == last_sp && elapsed <= 32768u) {
                    if (elapsed == quantum) ++stable;
                    else { quantum = (uint32_t)elapsed; stable = 0; }
                } else stable = 0;
                last_cycle = now; last_counter = counter; last_target = target;
                last_sp = cpu->gpr[29];
                if (stable >= 2u && quantum) {
                    const uint32_t distance = psx_idle_cycles_to_next_observable_event();
                    /* Whole iterations preserve the instruction/IRQ phase.
                     * The iteration containing the event always runs normally. */
                    const uint32_t loops = (distance - 1u) / quantum;
                    if (loops && xg_field_wait_code_matches()) {
                        psx_advance_cycles(loops * quantum);
                        last_cycle = psx_cycle_count;
                        stable = 0;
                    }
                }
            }
        }
    }

    /* Field keeps the iteration's initial VBlank count in s1. Preserve the
     * original wait unless scene work has already crossed a VBlank boundary. */
    if (cpu->gpr[31] == XG_FIELD_PRESENT_VSYNC_RETURN && cpu->gpr[4] == 0 &&
        (int32_t)(psx_mod_read_word(XG_VBLANK_COUNTER) - cpu->gpr[17]) > 0)
        cpu->gpr[4] = UINT32_MAX;
    return 0;
}

PSX_MOD_CONSTRUCTOR(xg_register_field_pacing_hook) {
    (void)psx_mod_register_function_entry_plugin(
        "xenogears.field-pacing", XG_VSYNC, xg_field_pacing_vsync_entry);
}
