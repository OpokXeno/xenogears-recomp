// Complete C++17 native-mod template, not a ready-to-install Xenogears cheat.
// Every Game address below must be identified and verified before activation.
// The calling conventions and Player layout below are illustrative contracts.
#include "mod_native_api.hpp"
#include <algorithm>
#include <cstdio>
#include <string>

using namespace psx::mod;

struct Game {
    // calculate_damage(attacker_ptr, defender_ptr, base_damage, element) -> v0.
    static constexpr uint32_t damage_function = 0;
    // award_xp(player_ptr, amount): adds amount to Player.xp, returns amount.
    static constexpr uint32_t reward_function = 0;
    // Separate critical calculation: selected instructions originally do v0=t0+t1.
    static constexpr uint32_t critical_begin = 0, critical_end = 0;
    // Optional independent formula accepting six words and returning damage in v0.
    // Inputs: attacker_ptr, defender_ptr, base_damage, element, percent, flat_bonus.
    static constexpr uint32_t formula_function = 0;
    // Address of a u32 guest pointer to the current Player, not the Player itself.
    static constexpr uint32_t player_pointer = 0;

    // Illustrative Player layout; replace these offsets and flag meanings too.
    static constexpr uint32_t flags = 0, defense = 2, hp = 4, max_hp = 6, xp = 8;
    static constexpr uint32_t player_size = 12;
    static constexpr uint8_t dead = 1, low_health = 2;
};

struct Settings {
    enum class DamageMode { Observe, Extend, Replace } mode = DamageMode::Extend;
    bool log = true, game_formula = false, replace_reward = false, regenerate = false;
    uint32_t percent = 100, bonus = 0, critical_bonus = 0, xp_bonus = 0;
    uint32_t regen_frames = 120, regen_amount = 1;
};

struct State {
    Host host;
    Settings settings;
    uint64_t damage_calls = 0, reward_calls = 0, block_calls = 0, heals = 0;
    uint32_t regen_clock = 0;
};
static State state;

static uint32_t physical(uint32_t address) noexcept { return address & 0x1fffffff; }
static bool player_in_ram(uint32_t address) noexcept {
    return !(address & 3u) && physical(address) >= 0x10000 &&
        physical(address) <= 0x200000 - Game::player_size;
}
static uint32_t bounded(int64_t value, uint32_t low, uint32_t high) noexcept {
    return static_cast<uint32_t>(std::clamp<int64_t>(value, low, high));
}
static uint32_t damage_limit(uint64_t value) noexcept {
    return static_cast<uint32_t>(std::min<uint64_t>(value, 65535));
}
static uint32_t add_xp(uint32_t old, uint32_t amount) noexcept {
    return static_cast<uint32_t>(std::min<uint64_t>(uint64_t(old) + amount, UINT32_MAX));
}
static bool returned(const Call& call) noexcept {
    return physical(call.cpu().pc()) == physical(call.return_address());
}

static int PSX_NATIVE_MOD_CALL start(void* userdata, const PSXNativeHost* api) noexcept {
    auto& mod = *static_cast<State*>(userdata);
    mod = State{};
    mod.host = Host(*api);
    if (!mod.host.compatible()) return 0;
    if (!Game::damage_function || !Game::reward_function || !Game::critical_begin ||
        Game::critical_end <= Game::critical_begin || !Game::player_pointer) {
        mod.host.log("Complete the verified Game addresses and Player layout first");
        return 0;
    }
    try {
        auto options = mod.host.options();
        auto& cfg = mod.settings;
        const auto mode = options.get_string("damage_mode", "extend");
        cfg.mode = mode == "replace" ? Settings::DamageMode::Replace :
            mode == "observe" ? Settings::DamageMode::Observe : Settings::DamageMode::Extend;
        cfg.log = options.get_bool("log_calls", true);
        cfg.game_formula = options.get_bool("use_game_formula");
        cfg.replace_reward = options.get_bool("replace_reward");
        cfg.regenerate = options.get_bool("regenerate");
        cfg.percent = bounded(options.get_int("damage_percent", 100), 0, 300);
        cfg.bonus = bounded(options.get_int("flat_bonus"), 0, 9999);
        cfg.critical_bonus = bounded(options.get_int("critical_bonus"), 0, 9999);
        cfg.xp_bonus = bounded(options.get_int("xp_bonus"), 0, 9999);
        cfg.regen_frames = bounded(options.get_int("regen_frames", 120), 1, 3600);
        cfg.regen_amount = bounded(options.get_int("regen_amount", 1), 1, 999);
        if (cfg.game_formula && (!Game::formula_function || !mod.host.supports_guest_calls())) {
            mod.host.log("Game formula requires a verified function and guest-call support");
            return 0;
        }
    } catch (...) {
        mod.host.log("Could not read Combat Lab settings");
        return 0;
    }
    // start runs before gameplay: do not read Player memory here.
    mod.host.log("Combat Lab initialized");
    return 1;
}

static void damage_hook(State& mod, const Call& call) {
    ++mod.damage_calls;
    auto cpu = call.cpu();
    const auto& cfg = mod.settings;
    if (cfg.log && mod.damage_calls <= 5) mod.host.log("Damage function intercepted");
    if (cfg.mode == Settings::DamageMode::Observe) { call.next(); return; }

    if (cfg.mode == Settings::DamageMode::Extend) {
        cpu.a2() = damage_limit(uint64_t(cpu.a2()) * cfg.percent / 100);
        if (!call.next() || !returned(call)) return;
        cpu.return_value() = damage_limit(uint64_t(cpu.return_value()) + cfg.bonus);
        return;
    }

    uint32_t value;
    if (cfg.game_formula) {
        // Fifth and sixth arguments are placed on the guest stack automatically.
        const auto result = call.call_guest(Game::formula_function,
            {cpu.a0(), cpu.a1(), cpu.a2(), cpu.a3(), cfg.percent, cfg.bonus});
        if (result.nonlocal()) return;
        if (!result) { mod.host.log("Formula unavailable; using original damage"); call.next(); return; }
        value = damage_limit(result.v0);
        if (cfg.log && mod.damage_calls <= 5) {
            char text[96];
            std::snprintf(text, sizeof text, "Game formula returned v0=%u, v1=%u",
                static_cast<unsigned>(result.v0), static_cast<unsigned>(result.v1));
            mod.host.log(text);
        }
    } else {
        if (!player_in_ram(cpu.a1())) { call.next(); return; }
        const auto defense = mod.host.read_half(cpu.a1() + Game::defense);
        const auto attack = uint64_t(cpu.a2()) * cfg.percent / 100;
        value = damage_limit((attack > defense ? attack - defense : 0) + cfg.bonus);
        mod.host.advance_cycles(4); // Illustrative cost: calibrate for your replacement.
    }
    call.return_value(value); // Skip the original and return to its caller.
}

static void reward_hook(State& mod, const Call& call) {
    ++mod.reward_calls;
    auto cpu = call.cpu();
    const uint32_t player = cpu.a0();
    if (!player_in_ram(player)) { call.next(); return; }
    if (mod.settings.replace_reward) {
        const auto amount = add_xp(cpu.a1(), mod.settings.xp_bonus);
        mod.host.write_word(player + Game::xp,
            add_xp(mod.host.read_word(player + Game::xp), amount));
        call.return_value(amount);
        mod.host.advance_cycles(4); // Demonstration timing, not a measured game cost.
    } else {
        if (!call.next() || !returned(call)) return;
        mod.host.write_word(player + Game::xp,
            add_xp(mod.host.read_word(player + Game::xp), mod.settings.xp_bonus));
        cpu.return_value() = add_xp(cpu.return_value(), mod.settings.xp_bonus);
    }
}

static void PSX_NATIVE_MOD_CALL hook(void* userdata, PSXNativeCPU* cpu, const PSXNativeCall* native) {
    auto& mod = *static_cast<State*>(userdata);
    Call call(mod.host, *cpu, *native);
    if (physical(call.address()) == physical(Game::damage_function)) damage_hook(mod, call);
    else if (physical(call.address()) == physical(Game::reward_function)) reward_hook(mod, call);
    else call.next();
}

static void PSX_NATIVE_MOD_CALL block(void* userdata, PSXNativeCPU* cpu, const PSXNativeBlock* native) {
    auto& mod = *static_cast<State*>(userdata);
    Block range(mod.host, *cpu, *native);
    ++mod.block_calls;
    // This contract requires a range whose only original effect is v0=t0+t1.
    range.cpu().v0() = range.cpu().t0() + range.cpu().t1() + mod.settings.critical_bonus;
    mod.host.advance_cycles(2); // Calibrate the guest cost before using real addresses.
    // Preserve sp, ra, t0/t1 and other live state. Host resumes at critical_end.
}

static void PSX_NATIVE_MOD_CALL vblank(void* userdata) noexcept {
    auto& mod = *static_cast<State*>(userdata);
    if (!mod.settings.regenerate || !mod.host.game_started()) return;
    if (++mod.regen_clock < mod.settings.regen_frames) return;
    mod.regen_clock = 0;
    const auto player = mod.host.read_word(Game::player_pointer);
    if (!player_in_ram(player)) return;
    const auto flags = mod.host.read_byte(player + Game::flags);
    const auto hp = mod.host.read_half(player + Game::hp);
    const auto max_hp = mod.host.read_half(player + Game::max_hp);
    if ((flags & Game::dead) || !hp || hp >= max_hp) return;
    const auto healed = std::min<uint32_t>(max_hp, hp + mod.settings.regen_amount);
    mod.host.write_half(player + Game::hp, static_cast<uint16_t>(healed));
    if (healed > max_hp / 4)
        mod.host.write_byte(player + Game::flags, static_cast<uint8_t>(flags & ~Game::low_health));
    ++mod.heals;
    // Guest calls need a hook context; do not use call_guest from VBlank.
}

static void PSX_NATIVE_MOD_CALL restored(void* userdata) noexcept {
    auto& mod = *static_cast<State*>(userdata);
    mod.regen_clock = 0;
    mod.damage_calls = mod.reward_calls = mod.block_calls = mod.heals = 0;
    mod.host.log("Savestate loaded: native counters and regeneration timer reset");
}

static void PSX_NATIVE_MOD_CALL stop(void* userdata) noexcept {
    auto& mod = *static_cast<State*>(userdata);
    char text[192];
    std::snprintf(text, sizeof text, "Combat Lab stopped: damage=%llu rewards=%llu ranges=%llu heals=%llu",
        static_cast<unsigned long long>(mod.damage_calls), static_cast<unsigned long long>(mod.reward_calls),
        static_cast<unsigned long long>(mod.block_calls), static_cast<unsigned long long>(mod.heals));
    mod.host.log(text);
    mod = State{};
}

static const PSXNativeMod mod = [] {
    auto value = descriptor(&state);
    value.start = start; value.stop = stop; value.hook = hook; value.block = block;
    value.vblank = vblank; value.savestate_loaded = restored;
    return value;
}();
extern "C" PSX_NATIVE_MOD_EXPORT const PSXNativeMod* PSX_NATIVE_MOD_CALL psx_native_mod_v1() {
    return &mod;
}
