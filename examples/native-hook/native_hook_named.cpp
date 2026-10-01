// A complete mod using annotation names, with the standard packer's settings.
// Package with --hook ClearCallbackChain --hook WaitForVerticalRetrace.
#include "mod_native_api.hpp"
#include <cstdio>

using namespace psx::mod;
static Host host;
static bool replace;
static uint32_t replacement, logged;

static int PSX_NATIVE_MOD_CALL start(void*, const PSXNativeHost* api) {
    host = Host(*api);
    if (!host.compatible() || !host.supports_named_functions()) return 0;
    if (!host.find_function("ClearCallbackChain") || !host.find_function("WaitForVerticalRetrace") ||
        !host.find_function("InspectPendingCallback")) return 0;
    replace = host.options().get_bool("replace");
    replacement = static_cast<uint32_t>(host.options().get_int("return_value"));
    logged = 0;
    host.log("Named hooks ready");
    return 1;
}

static void PSX_NATIVE_MOD_CALL hook(void*, PSXNativeCPU* registers, const PSXNativeCall* native) {
    Call call(host, *registers, *native);
    const char* name = call.matches("ClearCallbackChain") ? "ClearCallbackChain" :
                       call.matches("WaitForVerticalRetrace") ? "WaitForVerticalRetrace" : "OtherHook";
    if (logged < 5) {
        // Verified resident zero-argument getter; its return value is a halfword.
        const auto pending = call.call_guest("InspectPendingCallback");
        if (pending.nonlocal()) return;
        char text[160];
        std::snprintf(text, sizeof text, "%s: InspectPendingCallback status=%d value=%u",
                      name, pending.status, pending.v0);
        host.log(text);
        ++logged;
    }
    if (replace) call.return_value(replacement);
    else call.next();
}

static const PSXNativeMod mod = [] {
    auto value = descriptor();
    value.start = start;
    value.hook = hook;
    return value;
}();

extern "C" PSX_NATIVE_MOD_EXPORT const PSXNativeMod* PSX_NATIVE_MOD_CALL psx_native_mod_v1() {
    return &mod;
}
