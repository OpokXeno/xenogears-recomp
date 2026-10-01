// Standalone C++ example. The standard packer settings work with this library.
// Optional custom setting "guest_function": a verified one-word-in/word-out
// guest function that transforms a0 before the original hook runs. Zero disables it.
#include "mod_native_api.hpp"

using namespace psx::mod;
static Host host;
static bool replace;
static uint32_t replacement, guest_function;

static int PSX_NATIVE_MOD_CALL start(void*, const PSXNativeHost* api) {
    host = Host(*api);
    if (!host.compatible()) return 0;
    auto options = host.options();
    replace = options.get_bool("replace");
    replacement = static_cast<uint32_t>(options.get_int("return_value"));
    const int64_t address = options.get_int("guest_function");
    if (address < 0 || address > UINT32_MAX) return 0;
    guest_function = static_cast<uint32_t>(address);
    if (guest_function && !host.supports_guest_calls()) return 0;
    host.log("C++ hook ready");
    return 1;
}

static void PSX_NATIVE_MOD_CALL hook(void*, PSXNativeCPU* cpu, const PSXNativeCall* native) {
    Call call(host, *cpu, *native);
    if (guest_function) {
        const auto result = call.call_guest(guest_function, {call.cpu().a0()});
        if (result.nonlocal()) return;
        if (!result) {
            host.log("Guest call rejected; continuing with the original argument");
        } else {
            call.cpu().a0() = result.v0;
        }
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
