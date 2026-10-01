// Validate the actual generated catalog against local authenticated images.
// Guest instructions and native mod libraries are never executed in this test.
#include "mod_native_runtime.h"
#include "psx_sha256.h"
#include <algorithm>
#include <array>
#include <cstring>
#include <fstream>
#include <iostream>
#include <vector>

static const ModNativeSymbol* symbols;
static uint32_t symbol_count;
static ModNativeSymbolAvailable available;
static std::array<uint8_t, 0x200000> ram;
static int failures;

extern "C" void mod_native_register_symbols(const ModNativeSymbol* table, uint32_t count,
                                             ModNativeSymbolAvailable predicate) {
    symbols = table; symbol_count = count; available = predicate;
}
extern "C" int psx_overlay_static_code_matches(const uint32_t* ranges, uint32_t count,
                                                const uint8_t* expected) {
    psx_sha256_ctx hash; psx_sha256_init(&hash);
    for (uint32_t i = 0; i < count; ++i) {
        const uint32_t address = ranges[i * 2] & 0x1fffffff, size = ranges[i * 2 + 1];
        if (!size || address >= ram.size() || size > ram.size() - address) return 0;
        psx_sha256_update(&hash, ram.data() + address, size);
    }
    uint8_t digest[32]; psx_sha256_final(&hash, digest);
    return std::memcmp(digest, expected, sizeof digest) == 0;
}
static void check(bool value, const char* why) {
    if (!value) { ++failures; std::cerr << "FAIL: " << why << '\n'; }
}
static const ModNativeSymbol* find(const char* name) {
    const auto* end = symbols + symbol_count;
    const auto* found = std::lower_bound(symbols, end, name,
        [](const ModNativeSymbol& symbol, const char* text) { return std::strcmp(symbol.name, text) < 0; });
    return found != end && std::strcmp(found->name, name) == 0 ? found : nullptr;
}
static std::vector<uint8_t> read(const char* path) {
    std::ifstream source(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(source), {}};
}
static bool load(const std::vector<uint8_t>& bytes, uint32_t address) {
    if (address >= ram.size() || bytes.size() > ram.size() - address || bytes.empty()) return false;
    ram.fill(0);
    std::copy(bytes.begin(), bytes.end(), ram.begin() + address);
    return true;
}
int main(int argc, char** argv) {
    if (argc != 4) return 2;
    check(symbols && symbol_count > 1000 && available, "catalog is registered before main");
    if (!symbols || !available) return 1;
    const auto* resident = find("InspectPendingCallback");
    const auto* first = find("battle-velocity-sprite-clone-strip-overlay/MeasureSpriteFrameBounds");
    const auto* second = find("battle-fixed-origin-sprite-marquee-overlay/MeasureSpriteFrameBounds");
    check(resident && first && second, "resident and both qualified overlay symbols exist");
    if (!resident || !first || !second) return 1;
    check(!available(resident->identity) && !available(first->identity), "absent code is never available");
    auto exe = read(argv[1]);
    check(exe.size() > 0x800 && std::memcmp(exe.data(), "PS-X EXE", 8) == 0, "local EXE exists");
    if (exe.size() <= 0x800) return 1;
    uint32_t address = 0, size = 0;
    for (unsigned i = 0; i < 4; ++i) { address |= uint32_t(exe[0x18 + i]) << (i * 8); size |= uint32_t(exe[0x1c + i]) << (i * 8); }
    if (size > exe.size() - 0x800) return 1;
    check(load({exe.begin() + 0x800, exe.begin() + 0x800 + size}, address & 0x1fffffff), "EXE maps to RAM");
    check(available(resident->identity), "resident name matches its original code");
    ram[resident->address & 0x1fffffff] ^= 1;
    check(!available(resident->identity), "changed resident entry rejects its name identity");
    const auto* shared = find("MeasureSpriteFrameBounds");
    check(shared && shared + 1 < symbols + symbol_count && !std::strcmp(shared[1].name, shared->name),
          "ambiguous unqualified name preserves both choices");
    check(load(read(argv[2]), 0x1fc000), "first overlay maps to reused address");
    check(available(first->identity) && !available(second->identity), "first image enables only its qualified function");
    check(load(read(argv[3]), 0x1fc000), "second overlay replaces first");
    check(!available(first->identity) && available(second->identity), "replacement enables only the second qualified function");
    ram[second->address & 0x1fffffff] ^= 1;
    check(!available(second->identity), "changed overlay code rejects named execution");
    std::cout << "Generated symbol catalog: " << failures << " failures\n";
    return failures ? 1 : 0;
}
