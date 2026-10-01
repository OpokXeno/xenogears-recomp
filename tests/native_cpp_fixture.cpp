#include "mod_native_api.hpp"
#include <cstdlib>
#include <cstring>
#include <string>

using namespace psx::mod;
static Host host;
static const PSXNativeHost* api;
static std::string mode;

static int PSX_NATIVE_MOD_CALL start(void*, const PSXNativeHost* value) {
    api = value;
    host = Host(*value);
    if (!host.compatible() || !host.supports_guest_calls()) return 0;
    mode = host.options().get_string("mode");
    if (!host.options().get_bool("cpp_enabled", true) ||
        host.options().get_int("delta", -1) != 3) return 0;
    return 1;
}

static GuestResult invoke(void* context, CPU cpu) {
    const PSXNativeCPU before = *cpu.raw();
    auto result = host.call_guest(context, cpu, 0x80015000, {11, 22, 33, 44, 55, 66});
    if (!result || std::memcmp(cpu.raw(), &before, sizeof before)) std::abort();
    auto zero = host.call_guest(context, cpu, 0x80015010);
    if (!zero || zero.v0 != 123 || std::memcmp(cpu.raw(), &before, sizeof before)) std::abort();
    return result;
}

static GuestResult invoke_named(void* context, CPU cpu) {
    const PSXNativeCPU before = *cpu.raw();
    if (!host.supports_named_functions()) std::abort();
    const auto sum = host.find_function("scene/SumSix");
    const auto zero = host.find_function("Zero");
    if (!sum || sum.address != 0x80015000 || !zero || zero.address != 0x80015010 ||
        !host.find_function("Shared").ambiguous() || host.find_function("Missing")) std::abort();
    auto result = host.call_guest(context, cpu, "scene/SumSix", {11, 22, 33, 44, 55, 66});
    auto empty = host.call_guest(context, cpu, "Zero");
    auto numeric = host.call_guest(context, cpu, "0x80015010");
    if (!result || !empty || empty.v0 != 123 || !numeric || numeric.v0 != 123 ||
        std::memcmp(cpu.raw(), &before, sizeof before)) std::abort();
    return result;
}

static void PSX_NATIVE_MOD_CALL hook(void*, PSXNativeCPU* value, const PSXNativeCall* native) {
    Call call(host, *value, *native);
    if ((call.address() & 0x1fffffff) == 0x15020) {
        if (!call.next()) return;
        call.cpu().return_value() += 7;
        return;
    }
    if (mode == "nested") {
        auto result = call.call_guest(0x80015020, {9, 3});
        if (result.nonlocal()) return;
        if (!result) std::abort();
        call.return_value(result.v0);
    } else if (mode == "named") {
        if (!call.matches("FixtureHook") || call.matches("Missing") || call.matches("Nested")) std::abort();
        const auto result = invoke_named(native->context, call.cpu());
        call.return_value(result.v0, result.v1);
    } else if (mode == "named_nested") {
        const auto result = call.call_guest("resident/Nested", {9, 3});
        if (!result || result.v0 != 19) std::abort();
        call.return_value(result.v0);
    } else if (mode == "named_rejected") {
        const PSXNativeCPU before = *value;
        if (call.call_guest("Missing").status != PSX_NATIVE_GUEST_UNKNOWN_FUNCTION ||
            call.call_guest("Shared").status != PSX_NATIVE_GUEST_AMBIGUOUS_FUNCTION ||
            call.call_guest("scene/SumSix").status != PSX_NATIVE_GUEST_FUNCTION_NOT_LOADED ||
            call.call_guest("Zero+4") || std::memcmp(value, &before, sizeof before)) std::abort();
        PSXNativeCPU copy = *value;
        PSXNativeGuestResult output{sizeof(output), 88, 99};
        if (api->call_guest_named(native->context, &copy, "Zero", nullptr, 0, &output) ||
            api->call_guest_named(nullptr, value, "Zero", nullptr, 0, &output) ||
            api->call_guest_named(native->context, value, "Zero", nullptr, 1, &output)) std::abort();
        output.size = 4;
        if (api->call_guest_named(native->context, value, "Zero", nullptr, 0, &output) ||
            output.v0 != 88 || output.v1 != 99) std::abort();
        uint32_t address = 0xdeadbeef;
        if (api->find_function(api->context, "Missing", &address) != PSX_NATIVE_FUNCTION_UNKNOWN ||
            api->find_function(api->context, "Shared", &address) != PSX_NATIVE_FUNCTION_AMBIGUOUS ||
            address != 0xdeadbeef) std::abort();
        call.return_value(79);
    } else if (mode == "named_stack_overlap") {
        const PSXNativeCPU before = *value;
        if (call.call_guest("scene/SumSix").status != PSX_NATIVE_GUEST_FUNCTION_NOT_LOADED ||
            std::memcmp(value, &before, sizeof before)) std::abort();
        call.return_value(80);
    } else if (mode == "named_escape") {
        if (!call.call_guest("Escape").nonlocal() || call.next() || call.call_guest("Zero")) std::abort();
        call.cpu().pc() = 0;
    } else if (mode == "escaped") {
        auto result = call.call_guest(0x80015030, {1});
        if (!result.nonlocal() || call.next() || call.call_guest(0x80015010)) std::abort();
        // Even a badly behaved callback must not erase a guest escape.
        call.cpu().pc() = 0;
        call.cpu().sp() = 0;
    } else if (mode == "invalid") {
        const PSXNativeCPU before = *value;
        if (call.call_guest(0x80015001) || call.call_guest(0xbfc00000) ||
            call.call_guest(0x80015000, nullptr, 1)) std::abort();
        PSXNativeGuestResult output{sizeof(output), 88, 99};
        PSXNativeCPU copy = *value;
        uint32_t word = 1;
        if (api->call_guest(native->context, &copy, 0x80015000, nullptr, 0, &output) ||
            api->call_guest(nullptr, value, 0x80015000, nullptr, 0, &output) ||
            api->call_guest(native->context, value, 0x80015000, &word, UINT32_MAX, &output)) std::abort();
        output.size = 4;
        if (api->call_guest(native->context, value, 0x80015000, nullptr, 0, &output) ||
            output.v0 != 88 || output.v1 != 99 || std::memcmp(value, &before, sizeof before)) std::abort();
        call.return_value(77);
    } else if (mode == "invalid_stack") {
        if (call.call_guest(0x80015000, {1})) std::abort();
        call.return_value(78);
    } else if (mode == "chained_escape") {
        if (call.next()) std::abort();
        call.cpu().pc() = 0;
        call.cpu().sp() = 0;
    } else {
        const auto result = invoke(native->context, call.cpu());
        call.return_value(result.v0, result.v1);
    }
}

static void PSX_NATIVE_MOD_CALL block(void*, PSXNativeCPU* value, const PSXNativeBlock* native) {
    Block span(host, *value, *native);
    if (mode == "named") {
        if (!span.matches("Partial+8")) std::abort();
        const auto result = invoke_named(native->context, span.cpu());
        span.cpu().v0() = result.v0; span.cpu().v1() = result.v1;
        return;
    }
    if (mode == "escaped") {
        if (!span.call_guest(0x80015030).nonlocal()) std::abort();
        span.cpu().pc() = 0;
        return;
    }
    const auto result = invoke(native->context, span.cpu());
    span.cpu().return_value() = result.v0;
    span.cpu().v1() = result.v1;
}

static const PSXNativeMod mod = [] {
    auto value = descriptor();
    value.start = start;
    value.hook = hook;
    value.block = block;
    return value;
}();

extern "C" PSX_NATIVE_MOD_EXPORT const PSXNativeMod* PSX_NATIVE_MOD_CALL psx_native_mod_v1() {
    return &mod;
}
