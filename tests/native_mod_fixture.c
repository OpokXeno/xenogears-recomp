#include "mod_native_api.h"
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const PSXNativeHost* host;
static void marker(void) {
    const char* path = getenv("PSX_NATIVE_TEST_MARKER");
    if (path) {
        FILE* f = fopen(path, "a");
        if (f) { fputs("loaded\n", f); fclose(f); }
    }
}
#if defined(__GNUC__) || defined(__clang__)
__attribute__((constructor)) static void on_load(void) { marker(); }
#endif
static int PSX_NATIVE_MOD_CALL start(void* userdata, const PSXNativeHost* api) {
    (void)userdata;
    host = api;
    char option[32];
    if (host->option(host->context, "mode", option, sizeof option) && !strcmp(option, "fail")) return 0;
    return 1;
}
static void PSX_NATIVE_MOD_CALL hook(void* userdata, PSXNativeCPU* cpu, const PSXNativeCall* call) {
    (void)userdata;
    char mode[32] = "extend", delta[32] = "3";
    host->option(host->context, "mode", mode, sizeof mode);
    host->option(host->context, "delta", delta, sizeof delta);
    if (!strcmp(mode, "replace")) {
        cpu->gpr[2] = 99;
        cpu->pc = call->return_address;
    } else {
        cpu->gpr[4] += (uint32_t)atoi(delta);
        if (!call->next(call->context, cpu)) abort();
        cpu->gpr[2] += 7;
        /* next may only be called once. */
        host->write_word(0x80010108, call->next(call->context, cpu));
    }
    cpu->gpr[0] = 123; /* host must preserve architectural r0 */
    host->advance_cycles(4);
}
static void PSX_NATIVE_MOD_CALL stop(void* userdata) {
    (void)userdata;
    host->write_word(0x8001010c, host->read_word(0x8001010c) + 1);
}
static void PSX_NATIVE_MOD_CALL vblank(void* userdata) {
    (void)userdata;
    host->write_word(0x80010100, host->read_word(0x80010100) + 1);
}
static void PSX_NATIVE_MOD_CALL restored(void* userdata) {
    (void)userdata;
    host->write_word(0x80010104, host->read_word(0x80010104) + 1);
}
#ifndef OLD_DESCRIPTOR
static void PSX_NATIVE_MOD_CALL block(void* userdata, PSXNativeCPU* cpu, const PSXNativeBlock* span) {
    (void)userdata;
    if (span->size < sizeof(*span) || span->byte_count != span->resume_address - span->address) abort();
    cpu->gpr[2] += 50;
    cpu->gpr[0] = 123;
    cpu->pc = 0; /* The host owns the continuation for partial hooks. */
    host->write_word(0x80010110, span->return_address);
    host->advance_cycles(2);
}
#endif
static const PSXNativeMod mod = {
#ifdef OLD_DESCRIPTOR
    offsetof(PSXNativeMod, block),
#else
    sizeof(PSXNativeMod),
#endif
#ifdef BAD_ABI
    999,
#else
    PSX_NATIVE_MOD_ABI,
#endif
    0, start, stop, hook, vblank, restored,
#ifdef OLD_DESCRIPTOR
    0
#else
    block
#endif
};
PSX_NATIVE_MOD_EXPORT const PSXNativeMod* PSX_NATIVE_MOD_CALL psx_native_mod_v1(void) {
    marker();
    return &mod;
}
