/* Build as a separate DLL/.so/.dylib. No linking against the game is needed. */
#include "mod_native_api.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const PSXNativeHost* host;
static unsigned calls;
static int replace_function;
static uint32_t replacement_result;
static int PSX_NATIVE_MOD_CALL start(void* userdata, const PSXNativeHost* api) {
    char text[32];
    (void)userdata;
    host = api;
    calls = 0;
    if (api->abi_version != PSX_NATIVE_MOD_ABI || api->size < sizeof(*api)) return 0;
    replace_function = api->option(api->context, "replace", text, sizeof text)
        && strcmp(text, "true") == 0;
    replacement_result = api->option(api->context, "return_value", text, sizeof text)
        ? (uint32_t)strtoul(text, NULL, 10) : 0;
    api->log(api->context, "Function hook ready");
    return 1;
}
static void PSX_NATIVE_MOD_CALL hook(void* userdata, PSXNativeCPU* cpu,
                                    const PSXNativeCall* call) {
    (void)userdata;
    if (++calls <= 5) {
        char message[96];
        snprintf(message, sizeof message, "Call %u to 0x%08x, a0 = 0x%08x",
                 calls, (unsigned)call->address, (unsigned)cpu->gpr[4]);
        host->log(host->context, message);
    }
    if (replace_function) {
        cpu->gpr[2] = replacement_result;
        cpu->pc = call->return_address;
        host->advance_cycles(1);
    } else {
        /* Change arguments before next(), or v0/v1 afterwards, to extend the
         * guest function. Only invoke next once with this exact CPU pointer. */
        call->next(call->context, cpu);
    }
}
static void PSX_NATIVE_MOD_CALL restored(void* userdata) {
    (void)userdata;
    calls = 0;
}
/* Use only for a range whose effect is producing a value in v0. The rest of
 * the function still runs, so preserve every other live guest register. */
static void PSX_NATIVE_MOD_CALL block(void* userdata, PSXNativeCPU* cpu,
                                     const PSXNativeBlock* span) {
    (void)userdata;
    cpu->gpr[2] = replacement_result;
    host->advance_cycles(1);
    if (++calls <= 5) {
        char message[96];
        snprintf(message, sizeof message, "Replaced 0x%08x..0x%08x",
                 (unsigned)span->address, (unsigned)span->resume_address);
        host->log(host->context, message);
    }
}
static void PSX_NATIVE_MOD_CALL stop(void* userdata) {
    (void)userdata;
    if (host) host->log(host->context, "Function hook stopped");
    host = NULL;
}
static const PSXNativeMod mod = {
    sizeof(PSXNativeMod), PSX_NATIVE_MOD_ABI, NULL,
    start, stop, hook, NULL, restored, block
};
PSX_NATIVE_MOD_EXPORT const PSXNativeMod* PSX_NATIVE_MOD_CALL psx_native_mod_v1(void) {
    return &mod;
}
