#include "mod_packages.h"
#include "mod_native_runtime.h"
#include "mod_native_api.hpp"
#include "mod_plugins.h"
#include "cpu_state.h"
#include "crc32.h"
#include "psx_sha256.h"
#include <array>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <vector>
namespace fs = std::filesystem;
using namespace PSXRecompV4;
static int failures, originals;
static uint32_t cycles;
static bool recurse;
static bool reset_in_original;
static uint32_t original_pc;
static bool game_started = true;
extern "C" { int g_psx_call_bail = 0; }
extern "C" int psx_mod_game_started(void) { return game_started; }
static std::array<uint8_t, 0x200000> ram{};
struct TestIdentity { uint32_t address, expected; uint32_t scope_address, scope_expected; };
static const TestIdentity hook_identity{0x80010000, 0x04030201, 0, 0};
static const TestIdentity sum_identity{0x80015000, 0x11223344, 0x80015008, 0xaabbccdd};
static const TestIdentity zero_identity{0x80015010, 0x55667788, 0, 0};
static const TestIdentity nested_identity{0x80015020, 0x04030201, 0, 0};
static const TestIdentity escape_identity{0x80015030, 0x12345678, 0, 0};
static const ModNativeSymbol fixture_symbols[] = {
    {"Escape", 0x80015030, &escape_identity},
    {"FixtureHook", 0x80010000, &hook_identity},
    {"Nested", 0x80015020, &nested_identity},
    {"Partial", 0x80012000, &hook_identity},
    {"Shared", 0x80015000, &sum_identity},
    {"Shared", 0x80015010, &zero_identity},
    {"Zero", 0x80015010, &zero_identity},
    {"resident/Nested", 0x80015020, &nested_identity},
    {"scene/SumSix", 0x80015000, &sum_identity},
};
static void check(bool value, const std::string& why) {
    if (!value) { ++failures; std::cerr << "FAIL: " << why << '\n'; }
}
extern "C" uint8_t psx_mod_read_byte(uint32_t a) { return ram[a & 0x1fffff]; }
extern "C" uint16_t psx_mod_read_half(uint32_t a) { return psx_mod_read_byte(a) | (psx_mod_read_byte(a+1) << 8); }
extern "C" uint32_t psx_mod_read_word(uint32_t a) { return psx_mod_read_half(a) | (uint32_t(psx_mod_read_half(a+2)) << 16); }
static int fixture_available(const void* value) {
    const auto& identity = *static_cast<const TestIdentity*>(value);
    return psx_mod_read_word(identity.address) == identity.expected &&
        (!identity.scope_address || psx_mod_read_word(identity.scope_address) == identity.scope_expected);
}
extern "C" void psx_mod_write_byte(uint32_t a, uint8_t v) { ram[a & 0x1fffff] = v; }
extern "C" void psx_mod_write_half(uint32_t a, uint16_t v) { psx_mod_write_byte(a,v); psx_mod_write_byte(a+1,v>>8); }
extern "C" void psx_mod_write_word(uint32_t a, uint32_t v) { psx_mod_write_half(a,v); psx_mod_write_half(a+2,v>>16); }
extern "C" void psx_native_mod_advance_cycles(uint32_t v) { cycles += v; }
extern "C" void psx_dispatch_call(CPUState* cpu, uint32_t a, uint32_t ra) {
    if (mod_native_on_dispatch(cpu, a, cpu->gpr[31])) return;
    ++originals;
    if ((a & 0x1fffffff) >= 0x15000 && (a & 0x1fffffff) <= 0x15030) {
        const uint32_t sp = cpu->gpr[29];
        check((sp & 7) == 0 && cpu->gpr[31] == ra, "guest-call stack alignment and return boundary");
        if ((a & 0x1fffffff) == 0x15030) {
            cpu->pc = 0x80019900; cpu->gpr[29] -= 32; g_psx_call_bail = 1;
            return;
        }
        for (unsigned i = 0; i < 4; ++i)
            check(psx_mod_read_word(sp + 4 * i) == cpu->gpr[4 + i], "o32 home area contains register arguments");
        uint32_t sum = 0;
        if ((a & 0x1fffffff) == 0x15000) {
            for (unsigned i = 0; i < 6; ++i) sum += psx_mod_read_word(sp + 4 * i);
            check(sum == 231, "fifth and sixth arguments are passed on the guest stack");
        } else if ((a & 0x1fffffff) == 0x15010) {
            check(cpu->gpr[4] == 0 && cpu->gpr[5] == 0 && cpu->gpr[6] == 0 && cpu->gpr[7] == 0,
                  "omitted arguments are zeroed rather than inherited from the hook");
            sum = 123;
        } else sum = cpu->gpr[4] + cpu->gpr[5];
        for (unsigned i = 1; i < 29; ++i) cpu->gpr[i] = 0xc0000000u + i;
        cpu->hi = 99; cpu->lo = 88;
        cpu->gte_data[3] += 1; cpu->cop0[4] += 1;
        cpu->gpr[2] = sum; cpu->gpr[3] = 66;
        psx_mod_write_word(0x80015100, psx_mod_read_word(0x80015100) + 1);
        cycles += 17;
        cpu->pc = 0;
        return;
    }
    if (reset_in_original) {
        reset_in_original = false;
        const auto stops = psx_mod_read_word(0x8001010c);
        mod_native_reset();
        check(psx_mod_read_word(0x8001010c) == stops,
              "reset defers stop/unload while an original call has a native continuation");
    }
    if (recurse) {
        recurse = false;
        psx_dispatch_call(cpu, a, ra);
    }
    cpu->gpr[2] = cpu->gpr[4] * 2;
    // Real psx_dispatch_call uses zero for a completed host-call return.
    cpu->pc = original_pc;
}
static std::vector<uint8_t> read(const fs::path& file) {
    std::ifstream in(file, std::ios::binary);
    return {std::istreambuf_iterator<char>(in), {}};
}
static void write(const fs::path& file, const std::vector<uint8_t>& bytes) {
    fs::create_directories(file.parent_path());
    std::ofstream out(file, std::ios::binary);
    out.write(reinterpret_cast<const char*>(bytes.data()), bytes.size());
}
static std::string hash(const std::vector<uint8_t>& bytes) {
    uint8_t digest[32]; psx_sha256_compute(bytes.data(), bytes.size(), digest);
    const char* hex = "0123456789abcdef";
    std::string s;
    for (auto b : digest) { s += hex[b>>4]; s += hex[b&15]; }
    return s;
}
static void write_stored_package(
    const fs::path& path,
    const std::vector<std::pair<std::string, std::vector<uint8_t>>>& entries) {
    struct CentralEntry {
        std::string name;
        uint32_t crc;
        uint32_t size;
        uint32_t offset;
    };
    std::vector<uint8_t> zip;
    std::vector<CentralEntry> central;
    auto le16 = [&](uint16_t v) {
        zip.push_back((uint8_t)v); zip.push_back((uint8_t)(v >> 8));
    };
    auto le32 = [&](uint32_t v) {
        le16((uint16_t)v); le16((uint16_t)(v >> 16));
    };
    for (const auto& [name, data] : entries) {
        const uint32_t crc = crc32_compute(data.data(), data.size());
        central.push_back({name, crc, (uint32_t)data.size(), (uint32_t)zip.size()});
        le32(0x04034b50); le16(20); le16(0); le16(0); le16(0); le16(0);
        le32(crc); le32((uint32_t)data.size()); le32((uint32_t)data.size());
        le16((uint16_t)name.size()); le16(0);
        zip.insert(zip.end(), name.begin(), name.end());
        zip.insert(zip.end(), data.begin(), data.end());
    }
    const uint32_t central_offset = (uint32_t)zip.size();
    for (const CentralEntry& entry : central) {
        le32(0x02014b50); le16(20); le16(20); le16(0); le16(0); le16(0); le16(0);
        le32(entry.crc); le32(entry.size); le32(entry.size);
        le16((uint16_t)entry.name.size()); le16(0); le16(0); le16(0); le16(0);
        le32(0); le32(entry.offset);
        zip.insert(zip.end(), entry.name.begin(), entry.name.end());
    }
    const uint32_t central_size = (uint32_t)zip.size() - central_offset;
    le32(0x06054b50); le16(0); le16(0);
    le16((uint16_t)central.size()); le16((uint16_t)central.size());
    le32(central_size); le32(central_offset); le16(0);
    fs::create_directories(path.parent_path());
    std::ofstream out(path, std::ios::binary);
    out.write((const char*)zip.data(), (std::streamsize)zip.size());
}

static std::string manifest(const std::vector<uint8_t>& lib, const std::string& id="test.native", const std::string& platform=mod_native_platform()) {
    return "format_version = 9\nid = \"" + id + "\"\nversion = \"1.0.0\"\nname = \"Native fixture\"\nauthor = \"Test\"\n"
        "[[target]]\ngame_id = \"SLUS-00664\"\n"
        "[[feature]]\nid = \"hook\"\nname = \"Hook\"\n"
        "[[option]]\nfeature = \"hook\"\nid = \"mode\"\nlabel = \"Mode\"\ntype = \"choice\"\ndefault = \"extend\"\n"
        "choice = [{ value = \"extend\", label = \"Extend\" }, { value = \"replace\", label = \"Replace\" }, { value = \"fail\", label = \"Fail\" }]\n"
        "[[option]]\nfeature = \"hook\"\nid = \"delta\"\nlabel = \"Delta\"\ntype = \"integer\"\ndefault = 3\nmin = 0\nmax = 10\nstep = 1\n"
        "[[native_module]]\nfeature = \"hook\"\nid = \"fixture\"\nplatform = \"" + platform + "\"\nfile = \"fixture.bin\"\nsha256 = \"" + hash(lib) + "\"\n"
        "[[native_module.hook]]\naddress = 0x80010000\nexpected = \"01020304\"\n";
}
static void archive(const fs::path& path, const std::string& m, const std::vector<uint8_t>& bytes) {
    write_stored_package(path, {{"manifest.toml", {m.begin(),m.end()}}, {"fixture.bin", bytes}});
}
static int PSX_NATIVE_MOD_CALL helper_option(void*, const char* id, char* out, uint32_t capacity) {
    const std::map<std::string, std::string> values{
        {"on", "true"}, {"off", "false"}, {"bad", "12tail"}, {"negative", "-12"},
        {"overflow", "9223372036854775808"}, {"text", "my-choice"}};
    auto found = values.find(id);
    if (found == values.end() || capacity <= found->second.size()) return 0;
    std::memcpy(out, found->second.c_str(), found->second.size() + 1);
    return 1;
}
int main(int argc, char** argv) {
    if (argc != 5) return 2;
    const fs::path root = fs::temp_directory_path() / ("xg-native-test-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    fs::create_directories(root);
    const auto marker = (root / "marker").string();
#ifdef _WIN32
    _putenv_s("PSX_NATIVE_TEST_MARKER", marker.c_str());
#else
    setenv("PSX_NATIVE_TEST_MARKER", marker.c_str(), 1);
#endif
    const auto bytes = read(argv[1]), bad = read(argv[2]);
    check(!bytes.empty() && !bad.empty(), "fixture binaries exist");
    auto m = manifest(bytes);
    const fs::path pack = root / "test.psxmod";
    archive(pack, m, bytes);
    ModPackageManager manager(root / "mods");
    ModArchiveInspection info;
    std::string error;
    check(manager.inspect_archive(pack, info, &error), error);
    check(info.native_code && info.id == "test.native" && info.archive_sha256.size() == 64, "inspection reports native package and exact archive hash");
    check(!fs::exists(marker), "inspection does not load or execute code");
    check(!manager.install_archive(pack, nullptr, nullptr, &error), "native install needs explicit consent");
    check(!fs::exists(manager.installed_root()/"test.native"/"1.0.0"), "declined native package is not installed");
    check(!manager.install_archive(pack, nullptr, nullptr, &error, std::string(64,'0')), "wrong approval digest rejected");
    archive(pack, m + "\n# changed since prompt\n", bytes);
    check(!manager.install_archive(pack, nullptr, nullptr, &error, info.archive_sha256), "archive changed after prompt rejected");
    archive(pack, m, bytes);
    check(manager.install_archive(pack, nullptr, nullptr, &error, info.archive_sha256), error);
    check(!fs::exists(marker), "installation does not load code");
    check(manager.set_feature_enabled("test.native","hook",true,&error), error);
    auto plan = manager.resolve("SLUS-00664");
    check(plan.ok && plan.native_modules.size()==1, "trusted enabled native module resolves");
    check(!fs::exists(marker), "resolution does not load code");
    check(!manager.resolve("another-game").ok, "wrong game is rejected");
    check(manager.save_state(&error), error);
    ModPackageManager restart(root / "mods");
    check(restart.scan(&error) && restart.load_state(&error), error);
    check(restart.resolve("SLUS-00664").ok, "exact content consent persists across launches");
    check(mod_native_prepare(plan, &error), error);
    check(fs::exists(marker), "library only executes at launch preparation");
    CPUState cpu{}; cpu.gpr[4] = 5; cpu.gpr[31] = 0x80020000;
    psx_mod_write_word(0x80010000,0x04030201);
    check(!mod_native_on_dispatch(&cpu,0x80010000,cpu.gpr[31]), "prepared hooks remain inactive before activation");
    mod_native_activate();
    game_started = false;
    check(!mod_native_on_dispatch(&cpu,0x80010000,cpu.gpr[31]), "hooks never intercept BIOS boot RAM");
    game_started = true;
    check(mod_native_on_dispatch(&cpu,0x00010000,cpu.gpr[31]) && cpu.gpr[2]==23 && originals==1, "around hook changes args, calls original once and changes return value across RAM aliases");
    check(cpu.gpr[0]==0 && psx_mod_read_word(0x80010108)==0 && cycles==4, "r0 protected, next single-use, cycle API works");
    check(cpu.pc == cpu.gpr[31], "next translates the dispatcher's return sentinel into the guest continuation");
    cpu.gpr[4] = 5;
    psx_dispatch_call(&cpu, 0x80010000, 0x80021000);
    check(cpu.pc == cpu.gpr[31] && cpu.pc != 0x80021000,
          "around hook resumes the CPS callee's caller rather than the enclosing dispatcher stop");
    original_pc = 0x80030000;
    cpu.gpr[4] = 5;
    mod_native_on_dispatch(&cpu,0x80010000,cpu.gpr[31]);
    check(cpu.pc == original_pc, "next preserves a nonlocal guest continuation");
    original_pc = 0;
    mod_native_vblank(); mod_native_savestate_loaded();
    check(psx_mod_read_word(0x80010100)==1 && psx_mod_read_word(0x80010104)==1, "lifecycle callbacks run");
    cpu.gpr[4]=5; recurse=true;
    mod_native_on_dispatch(&cpu,0x80010000,cpu.gpr[31]);
    check(originals==5 && cycles==20, "original bypass does not suppress nested recursion");
    check(mod_native_prepare(plan, &error), error);
    cpu.gpr[4] = 5;
    check(mod_native_on_dispatch(&cpu,0x80010000,cpu.gpr[31]) && cpu.gpr[2]==23,
          "disc-swap recommit keeps hooks active without a second activation call");
    reset_in_original = true;
    const auto stops_during_call = psx_mod_read_word(0x8001010c);
    cpu.gpr[4] = 5;
    check(mod_native_on_dispatch(&cpu,0x80010000,cpu.gpr[31]) && cpu.gpr[2]==23 &&
          psx_mod_read_word(0x8001010c)==stops_during_call+1,
          "pending native code survives reset and unloads after its callback returns");
    check(mod_native_prepare(plan, &error), error);
    mod_native_activate();
    const auto stops_before_reset = psx_mod_read_word(0x8001010c);
    mod_native_reset();
    check(psx_mod_read_word(0x8001010c)==stops_before_reset+1, "reset stops native modules");
    check(manager.set_feature_option("test.native","hook","mode","replace",&error), error);
    auto replacement = manager.resolve("SLUS-00664");
    check(replacement.fingerprint != plan.fingerprint, "native options affect plan fingerprint");
    check(mod_native_prepare(replacement,&error),error); mod_native_activate();
    const int before=originals;
    cpu.gpr[2]=0; check(mod_native_on_dispatch(&cpu,0x80010000,0x80020000) && cpu.gpr[2]==99 && cpu.pc==0x80020000 && originals==before, "replacement skips original and publishes continuation");
    psx_dispatch_call(&cpu, 0x80010000, 0x80021000);
    check(cpu.pc == cpu.gpr[31] && cpu.pc != 0x80021000,
          "CPS callee hook returns to guest RA rather than the enclosing dispatcher stop");
    psx_mod_write_byte(0x80010000,0xff);
    check(!mod_native_on_dispatch(&cpu,0x80010000,cpu.gpr[31]), "changed overlay/code fails live guard");
    psx_mod_write_byte(0x80010000,1);
    check(manager.set_feature_enabled("test.native","hook",false,&error),error);
    check(manager.resolve("SLUS-00664").native_modules.empty(), "disabled module excluded");
    check(mod_native_prepare(manager.resolve("SLUS-00664"),&error),error); mod_native_activate();
    check(!mod_native_on_dispatch(&cpu,0x80010000,cpu.gpr[31]), "all-off unloads native hooks");
    check(manager.set_feature_enabled("test.native","hook",true,&error),error);
    // Manual copies never inherit trust; even bundled/native catalogs need consent.
    ModPackageManager manual(root/"manual");
    const auto manual_path=manual.installed_root()/"test.native"/"1.0.0";
    write(manual_path/"manifest.toml",{m.begin(),m.end()}); write(manual_path/"fixture.bin",bytes);
    check(manual.scan(&error) && manual.set_feature_enabled("test.native","hook",true,&error),error);
    check(!manual.resolve("SLUS-00664").ok, "manual native install has no trust");
    auto installed=manager.installed_root()/"test.native"/"1.0.0";
    auto altered=bytes; altered.back() ^= 1; write(installed/"fixture.bin",altered);
    check(!manager.resolve("SLUS-00664").ok, "payload tampering after scan rejected");
    check(!mod_native_prepare(plan,&error), "payload tampering between resolve and load rejected");
    write(installed/"fixture.bin",bytes);
    auto changed_manifest = m + "\n# changed\n";
    write(installed/"manifest.toml",{changed_manifest.begin(),changed_manifest.end()});
    check(!manager.resolve("SLUS-00664").ok, "manifest tampering after scan rejected");
    write(installed/"manifest.toml",{m.begin(),m.end()});
    auto broken = plan; broken.native_modules[0].module.file = argv[2]; broken.native_modules[0].module.sha256 = hash(bad);
    check(!mod_native_prepare(broken,&error) && error.find("ABI")!=std::string::npos, "incompatible ABI rejected before hooks activate");
    auto failing = plan; failing.native_modules[0].options["mode"]="fail";
    const auto stopped=psx_mod_read_word(0x8001010c);
    check(!mod_native_prepare(failing,&error) && psx_mod_read_word(0x8001010c)==stopped+1, "failed startup cleans up and stops library");
    auto chained=plan;
    auto second=plan.native_modules[0]; second.package_id="test.second"; second.options["delta"]="1";
    chained.native_modules.push_back(second);
    check(mod_native_prepare(chained,&error),error); mod_native_activate();
    cpu.gpr[4]=5; const auto orig=originals;
    check(mod_native_on_dispatch(&cpu,0x80010000,cpu.gpr[31]) && cpu.gpr[2]==32 && originals==orig+1, "two mods chain deterministically around one original call");
    mod_native_reset();
    check(!mod_native_on_dispatch(&cpu,0x80010000,cpu.gpr[31]), "netplay/reset path leaves no native hooks");
    auto guard_manifest = manifest(bytes, "test.guard");
    guard_manifest.replace(guard_manifest.find("01020304"), 8, "01020305");
    archive(root/"guard.psxmod",guard_manifest,bytes);
    ModArchiveInspection guard;
    check(manager.inspect_archive(root/"guard.psxmod",guard,&error),error);
    check(manager.install_archive(root/"guard.psxmod",nullptr,nullptr,&error,guard.archive_sha256),error);
    check(manager.set_feature_enabled("test.guard","hook",true,&error),error);
    check(!manager.resolve("SLUS-00664").ok,"incompatible native hook guards fail composition");
    check(manager.set_feature_enabled("test.guard","hook",false,&error),error);
    const auto other_platform=std::string(mod_native_platform()).find("windows")==0?"linux-x86_64":"windows-x86_64";
    archive(root/"unsupported.psxmod",manifest(bytes,"test.other",other_platform),bytes);
    ModArchiveInspection other;
    check(manager.inspect_archive(root/"unsupported.psxmod",other,&error),error);
    check(manager.install_archive(root/"unsupported.psxmod",nullptr,nullptr,&error,other.archive_sha256),error);
    check(manager.set_feature_enabled("test.other","hook",true,&error),error);
    check(!manager.resolve("SLUS-00664").ok,"unsupported host platform rejects enabled feature");
    check(manager.set_feature_enabled("test.other","hook",false,&error),error);
    auto legacy=m; legacy.replace(legacy.find("= 9"),3,"= 8"); archive(root/"legacy.psxmod",legacy,bytes);
    check(!manager.inspect_archive(root/"legacy.psxmod",other,&error), "format 8 cannot declare native modules");
    auto unguarded=m; unguarded.replace(unguarded.find("01020304"),8,"0102"); archive(root/"unguarded.psxmod",unguarded,bytes);
    check(!manager.inspect_archive(root/"unguarded.psxmod",other,&error),"short code guard rejected");
    auto escape=m; escape.replace(escape.find("fixture.bin"),11,"../outside.bin"); archive(root/"escape.psxmod",escape,bytes);
    check(!manager.inspect_archive(root/"escape.psxmod",other,&error),"native path traversal rejected");
#ifndef _WIN32
    fs::remove(manual_path/"fixture.bin");
    write(root/"outside.bin",bytes); fs::create_symlink(root/"outside.bin",manual_path/"fixture.bin");
    ModPackage parsed;
    check(!ModPackageManager::read_manifest(manual_path/"manifest.toml",parsed,&error),"manual native symlink escaping root rejected");
#endif
    check(manager.set_feature_enabled("test.native","hook",false,&error),error);
    check(manager.remove_version("test.native","1.0.0",&error),error);
    write(installed/"manifest.toml",{m.begin(),m.end()}); write(installed/"fixture.bin",bytes);
    check(manager.scan(&error) && manager.set_feature_enabled("test.native","hook",true,&error),error);
    check(!manager.resolve("SLUS-00664").ok,"uninstall revokes native trust");
    // A single library can own hundreds of independently dispatched functions.
    ModPackageManager expanded(root / "expanded");
    auto many = manifest(bytes, "test.many");
    for (unsigned i = 1; i <= 300; ++i) {
        many += "\n[[native_module.hook]]\naddress = " + std::to_string(0x80010000u + i * 4u) +
            "\nexpected = \"01020304\"\n";
    }
    archive(root/"many.psxmod", many, bytes);
    check(expanded.inspect_archive(root/"many.psxmod", other, &error), error);
    check(expanded.install_archive(root/"many.psxmod", nullptr, nullptr, &error, other.archive_sha256), error);
    check(expanded.set_feature_enabled("test.many", "hook", true, &error), error);
    auto many_plan = expanded.resolve("SLUS-00664");
    check(many_plan.ok && many_plan.native_modules[0].module.hooks.size() == 301, "more than 256 functions fit one native library/package");
    check(mod_native_prepare(many_plan, &error), error); mod_native_activate();
    for (unsigned i : {1u, 150u, 300u}) {
        const uint32_t addr = 0x80010000u + i * 4u;
        psx_mod_write_word(addr, 0x04030201);
        cpu.gpr[4] = 5;
        check(mod_native_on_dispatch(&cpu, addr, cpu.gpr[31]) && cpu.gpr[2] == 23, "each function dispatches through the same module callback");
    }
    mod_native_reset();
    // A guard describes guest identity, not the size of the replacement code.
    ModPackageManager long_guards(root / "long-guards");
    auto long_manifest = manifest(bytes, "test.long-guard");
    std::string long_expected = "01020304";
    for (unsigned i = 0; i < 76; ++i) long_expected += "00";
    long_manifest.replace(long_manifest.find("01020304"), 8, long_expected);
    archive(root/"long-guard.psxmod", long_manifest, bytes);
    check(long_guards.inspect_archive(root/"long-guard.psxmod", other, &error), error);
    check(long_guards.install_archive(root/"long-guard.psxmod", nullptr, nullptr, &error, other.archive_sha256), error);
    check(long_guards.set_feature_enabled("test.long-guard", "hook", true, &error), error);
    auto long_plan = long_guards.resolve("SLUS-00664");
    check(long_plan.ok && long_plan.native_modules[0].module.hooks[0].expected.size() == 80,
          "function guards may exceed 64 bytes");
    check(mod_native_prepare(long_plan, &error), error); mod_native_activate();
    for (uint32_t at = 0x80010004; at < 0x80010050; at += 4) psx_mod_write_word(at, 0);
    psx_mod_write_word(0x80010000, 0x04030201); cpu.gpr[4] = 5;
    check(mod_native_on_dispatch(&cpu, 0x80010000, cpu.gpr[31]) && cpu.gpr[2] == 23,
          "native dispatch validates a long function guard");
    psx_mod_write_byte(0x8001004f, 1);
    check(!mod_native_on_dispatch(&cpu, 0x80010000, cpu.gpr[31]), "long guard checks its final byte");
    mod_native_reset();
    // Partial hooks and whole-function hooks coexist in one module.
    ModPackageManager partial(root / "partial");
    auto span_manifest = manifest(bytes, "test.partial");
    span_manifest += "\n[[native_module.hook]]\naddress = 0x80012008\nresume_address = 0x80012010\nexpected = \"0102030405060708\"\n";
    span_manifest += "\n[[native_module.hook]]\naddress = 0x80013004\nresume_address = 0x80013008\nexpected = \"090a0b0c\"\n";
    span_manifest += "\n[[native_module.hook]]\naddress = 0x80014000\nresume_address = 0x80014050\nexpected = \"";
    for (unsigned i = 0; i < 20; ++i) span_manifest += "00000000";
    span_manifest += "\"\n";
    auto install_partial = [&](const std::string& text, const std::vector<uint8_t>& library) {
        if (fs::exists(partial.installed_root()/"test.partial"/"1.0.0")) {
            check(partial.set_feature_enabled("test.partial", "hook", false, &error), error);
            check(partial.remove_version("test.partial", "1.0.0", &error), error);
        }
        archive(root/"partial.psxmod", text, library);
        check(partial.inspect_archive(root/"partial.psxmod", other, &error), error);
        check(partial.install_archive(root/"partial.psxmod", nullptr, nullptr, &error, other.archive_sha256), error);
        check(partial.set_feature_enabled("test.partial", "hook", true, &error), error);
        return partial.resolve("SLUS-00664");
    };
    auto spans = install_partial(span_manifest, bytes);
    check(spans.ok && spans.native_modules[0].module.hooks.size() == 4, "format 9 resolves function and multiple partial hooks together");
    check(mod_native_prepare(spans, &error), error);
    check(!mod_native_blocks_intersect(0x80012000, 64), "partial hooks do not alter boot/pre-activation execution");
    mod_native_activate();
    check(mod_native_blocks_intersect(0x00012000, 64) && !mod_native_blocks_intersect(0x80012000, 8) &&
          !mod_native_blocks_intersect(0x80012010, 4) && mod_native_blocks_intersect(0x8001200c, 4),
          "compiled footprints intersect physical partial ranges with exact half-open boundaries");
    psx_mod_write_word(0x80012008, 0x04030201); psx_mod_write_word(0x8001200c, 0x08070605);
    for (uint32_t previous : {0x8fbf0010u, 0x0040f809u, 0x10850003u, 0x40026000u}) {
        // lw ra,16(sp); jalr v0; beq a0,a1,+3; mfc0 v0,Status.
        psx_mod_write_word(0x80012004, previous);
        check(!mod_native_on_block(&cpu, 0x80012008), "branch and load delay-slot points preserve original guest instructions");
    }
    psx_mod_write_word(0x80012004, 0x27bdffe8); // addiu sp,sp,-24 is a safe boundary.
    cpu.gpr[2] = 4; cpu.gpr[16] = 12; cpu.gpr[29] = 0x801ff000; cpu.gpr[31] = 0x80022000;
    const auto original_count = originals;
    check(!mod_native_on_dispatch(&cpu, 0x80012008, cpu.gpr[31]), "partial range is not a whole-function detour");
    check(mod_native_on_block(&cpu, 0x00012008) && cpu.gpr[2] == 54 && cpu.pc == 0x00012010 &&
          cpu.gpr[0] == 0 && cpu.gpr[16] == 12 && cpu.gpr[29] == 0x801ff000 &&
          psx_mod_read_word(0x80010110) == cpu.gpr[31] && originals == original_count,
          "partial callback replaces only the span, preserves live state, forces the suffix and respects RAM aliases");
    psx_mod_write_word(0x8001200c, 0);
    check(!mod_native_on_block(&cpu, 0x80012008), "every byte of the replaced span guards overlay reuse/self-modification");
    psx_mod_write_word(0x80013004, 0x0c0b0a09);
    check(mod_native_on_block(&cpu, 0x80013004) && cpu.pc == 0x80013008, "a second partial hook in the same library is independent");
    check(mod_native_on_block(&cpu, 0x80014000) && cpu.pc == 0x80014050, "partial ranges can guard and replace more than 64 instruction bytes");
    auto moved_spans = spans;
    moved_spans.native_modules[0].module.hooks[1].address += 0x100;
    moved_spans.native_modules[0].module.hooks[1].resume_address += 0x100;
    check(mod_native_prepare(moved_spans, &error) && !mod_native_has_block(0x80012008) && mod_native_has_block(0x80012108),
          "recommit updates partial range bindings while preserving activation");
    mod_native_reset();
    check(!mod_native_blocks_intersect(0x80012000, 64), "reset removes partial compilation barriers");
    auto invalid_span = span_manifest;
    invalid_span.replace(invalid_span.find("= 9"), 3, "= 8");
    archive(root/"invalid-span.psxmod", invalid_span, bytes);
    check(!partial.inspect_archive(root/"invalid-span.psxmod", other, &error), "native hooks still require format 9");
    invalid_span = span_manifest;
    invalid_span.replace(invalid_span.find("= 9"), 3, "= 10");
    archive(root/"invalid-span.psxmod", invalid_span, bytes);
    check(!partial.inspect_archive(root/"invalid-span.psxmod", other, &error), "unsupported format 10 is rejected");
    invalid_span = span_manifest;
    invalid_span.replace(invalid_span.find("resume_address"), 14, "resume_adress");
    archive(root/"invalid-span.psxmod", invalid_span, bytes);
    check(!partial.inspect_archive(root/"invalid-span.psxmod", other, &error) && error.find("unknown field") != std::string::npos,
          "unknown hook fields cannot silently turn a partial hook into a function hook");
    invalid_span = span_manifest;
    invalid_span.insert(invalid_span.find("[[native_module.hook]]"), "mode = \"block\"\n");
    archive(root/"invalid-span.psxmod", invalid_span, bytes);
    check(!partial.inspect_archive(root/"invalid-span.psxmod", other, &error) && error.find("unknown field") != std::string::npos,
          "unknown native module fields are rejected");
    invalid_span = span_manifest;
    invalid_span.replace(invalid_span.find("resume_address = 0x80012010"), 27, "resume_address = 0x80012014");
    archive(root/"invalid-span.psxmod", invalid_span, bytes);
    check(!partial.inspect_archive(root/"invalid-span.psxmod", other, &error), "partial guard must cover the whole replaced range");
    invalid_span = span_manifest;
    invalid_span.replace(invalid_span.find("resume_address = 0x80012010"), 27, "resume_address = 0x80012004");
    archive(root/"invalid-span.psxmod", invalid_span, bytes);
    check(!partial.inspect_archive(root/"invalid-span.psxmod", other, &error), "backward partial continuations are rejected");
    auto overlap = span_manifest + "\n[[native_module.hook]]\naddress = 0x8001200c\nresume_address = 0x80012014\nexpected = \"05060708090a0b0c\"\n";
    check(!install_partial(overlap, bytes).ok, "overlapping partial replacements are rejected");
    const auto old_library = read(argv[3]);
    auto old_manifest = manifest(old_library, "test.partial");
    auto old_plan = install_partial(old_manifest, old_library);
    check(mod_native_prepare(old_plan, &error), "original ABI v1 descriptors remain loadable");
    mod_native_reset();
    auto old_span = span_manifest;
    old_span.replace(old_span.find(hash(bytes)), 64, hash(old_library));
    auto old_span_plan = install_partial(old_span, old_library);
    check(!mod_native_prepare(old_span_plan, &error), "partial hooks require an extended descriptor and block callback");

    // A standalone C++ mod calls actual host dispatch through the public ABI.
    auto cpp = plan;
    auto& module = cpp.native_modules[0];
    module.module.file = fs::absolute(argv[4]);
    module.module.sha256 = hash(read(argv[4]));
    module.options["mode"] = "guest";
    auto nested_hook = module.module.hooks[0]; nested_hook.address = 0x80015020;
    module.module.hooks.push_back(nested_hook);
    module.module.hooks.push_back(spans.native_modules[0].module.hooks[1]);
    psx_mod_write_word(0x80010000, 0x04030201);
    psx_mod_write_word(0x80015020, 0x04030201);
    psx_mod_write_word(0x80012008, 0x04030201); psx_mod_write_word(0x8001200c, 0x08070605);
    auto setup_cpu = [&] {
        cpu = {};
        for (unsigned i = 1; i < 32; ++i) cpu.gpr[i] = 1000 + i;
        cpu.gpr[29] = 0x801ff004; cpu.gpr[31] = 0x80021000; cpu.hi = 71; cpu.lo = 72;
        for (uint32_t at = 0x1fefc0; at < 0x1ff004; ++at) ram[at] = uint8_t(at);
    };
    setup_cpu();
    auto caller = cpu;
    const auto stack_before = std::vector<uint8_t>(ram.begin() + 0x1fefc0, ram.begin() + 0x1ff004);
    check(mod_native_prepare(cpp, &error), error); mod_native_activate();
    int count_before = originals; uint32_t cycle_before = cycles;
    const auto writes_before = psx_mod_read_word(0x80015100);
    check(mod_native_on_dispatch(&cpu, 0x80010000, cpu.gpr[31]) && cpu.gpr[2] == 231 && cpu.gpr[3] == 66,
          "C++ helper invokes a six-argument function and publishes both return words");
    for (unsigned i = 1; i < 32; ++i) if (i != 2 && i != 3)
        check(cpu.gpr[i] == caller.gpr[i], "guest calls preserve all caller general registers");
    check(cpu.hi == caller.hi && cpu.lo == caller.lo && cpu.pc == caller.gpr[31], "guest calls preserve HI/LO and the hook continuation");
    check(cpu.gte_data[3] == 2 && cpu.cop0[4] == 2 && originals == count_before + 2 && cycles == cycle_before + 34 &&
          psx_mod_read_word(0x80015100) == writes_before + 2,
          "guest-call memory, coprocessor effects and cycles remain authoritative");
    check(std::equal(stack_before.begin(), stack_before.end(), ram.begin() + 0x1fefc0),
          "temporary argument frame, including alignment padding, is restored");
    setup_cpu();
    check(mod_native_on_block(&cpu, 0x80012008) && cpu.gpr[2] == 231 && cpu.gpr[3] == 66 && cpu.pc == 0x80012010,
          "partial-hook context supports guest calls and preserves its suffix");
    module.options["mode"] = "nested";
    check(mod_native_prepare(cpp, &error), error); mod_native_activate(); setup_cpu();
    check(mod_native_on_dispatch(&cpu, 0x80010000, cpu.gpr[31]) && cpu.gpr[2] == 19,
          "calls to another hooked function compose with next and modify its result");
    module.options["mode"] = "invalid";
    check(mod_native_prepare(cpp, &error), error); mod_native_activate(); setup_cpu(); count_before = originals;
    check(mod_native_on_dispatch(&cpu, 0x80010000, cpu.gpr[31]) && cpu.gpr[2] == 77 && originals == count_before,
          "invalid addresses, pointers, output sizes and oversized argument lists never execute guest code");
    module.options["mode"] = "invalid_stack";
    check(mod_native_prepare(cpp, &error), error); mod_native_activate(); setup_cpu(); cpu.gpr[29] = 0;
    check(mod_native_on_dispatch(&cpu, 0x80010000, cpu.gpr[31]) && cpu.gpr[2] == 78 && originals == count_before,
          "uninitialized guest stack rejects an injected call without side effects");
    module.options["mode"] = "escaped";
    check(mod_native_prepare(cpp, &error), error); mod_native_activate(); setup_cpu();
    check(mod_native_on_dispatch(&cpu, 0x80010000, cpu.gpr[31]) && g_psx_call_bail && cpu.pc == 0x80019900 && cpu.gpr[29] == 0x801fefd0,
          "nonlocal guest exit prevents further calls and cannot be overwritten by the mod");
    g_psx_call_bail = 0; setup_cpu();
    check(mod_native_on_block(&cpu, 0x80012008) && g_psx_call_bail && cpu.pc == 0x80019900,
          "partial hook propagates a guest escape instead of forcing the original suffix");
    g_psx_call_bail = 0;
    auto escape_chain = cpp;
    escape_chain.native_modules[0].module.hooks.resize(1);
    auto inner = escape_chain.native_modules[0]; inner.package_id = "test.cpp-inner";
    escape_chain.native_modules.push_back(inner);
    escape_chain.native_modules[0].options["mode"] = "chained_escape";
    check(mod_native_prepare(escape_chain, &error), error); mod_native_activate(); setup_cpu();
    check(mod_native_on_dispatch(&cpu, 0x80010000, cpu.gpr[31]) && g_psx_call_bail && cpu.pc == 0x80019900 && cpu.gpr[29] == 0x801fefd0,
          "guest escape propagates through every enclosing mod in a hook chain");
    g_psx_call_bail = 0; mod_native_reset();

    // Names are resolved anew on every call, so cached metadata cannot execute
    // an unloaded or reused overlay at its old address.
    mod_native_register_symbols(fixture_symbols, sizeof fixture_symbols / sizeof fixture_symbols[0], fixture_available);
    psx_mod_write_word(0x80015000, sum_identity.expected);
    psx_mod_write_word(0x80015008, sum_identity.scope_expected);
    psx_mod_write_word(0x80015010, zero_identity.expected);
    psx_mod_write_word(0x80015030, escape_identity.expected);
    module.options["mode"] = "named";
    check(mod_native_prepare(cpp, &error), error); mod_native_activate(); setup_cpu();
    check(mod_native_on_dispatch(&cpu, 0xa0010000, cpu.gpr[31]) && cpu.gpr[2] == 231 && cpu.gpr[3] == 66,
          "named calls pass six words and match hook entries through RAM aliases");
    setup_cpu();
    check(mod_native_on_block(&cpu, 0x80012008) && cpu.gpr[2] == 231 && cpu.pc == 0x80012010,
          "partial hooks match a name plus offset and call named guest functions");
    module.options["mode"] = "named_nested";
    check(mod_native_prepare(cpp, &error), error); mod_native_activate(); setup_cpu();
    check(mod_native_on_dispatch(&cpu, 0x80010000, cpu.gpr[31]) && cpu.gpr[2] == 19,
          "named calls use normal dispatch including another native hook");
    module.options["mode"] = "named_rejected";
    psx_mod_write_word(0x80015008, 0); // Keep the same prologue but replace its owner/body identity.
    check(mod_native_prepare(cpp, &error), error); mod_native_activate(); setup_cpu(); count_before = originals;
    check(mod_native_on_dispatch(&cpu, 0x80010000, cpu.gpr[31]) && cpu.gpr[2] == 79 && originals == count_before,
          "unknown, ambiguous and reused-overlay names reject without executing guest code");
    psx_mod_write_word(0x80015008, sum_identity.scope_expected);
    module.options["mode"] = "named";
    check(mod_native_prepare(cpp, &error), error); mod_native_activate(); setup_cpu();
    check(mod_native_on_dispatch(&cpu, 0x80010000, cpu.gpr[31]) && cpu.gpr[2] == 231,
          "named functions become callable again when their correct code is reloaded");
    module.options["mode"] = "named_stack_overlap";
    check(mod_native_prepare(cpp, &error), error); mod_native_activate(); setup_cpu(); count_before = originals;
    cpu.gpr[29] = 0x80015010;
    check(mod_native_on_dispatch(&cpu, 0x80010000, cpu.gpr[31]) && cpu.gpr[2] == 80 && originals == count_before &&
          psx_mod_read_word(0x80015000) == sum_identity.expected && psx_mod_read_word(0x80015008) == sum_identity.scope_expected,
          "a stack overlapping target code rejects execution and restores borrowed bytes and registers");
    module.options["mode"] = "named_escape";
    check(mod_native_prepare(cpp, &error), error); mod_native_activate(); setup_cpu();
    check(mod_native_on_dispatch(&cpu, 0x80010000, cpu.gpr[31]) && g_psx_call_bail && cpu.pc == 0x80019900,
          "named calls propagate nonlocal exits with the same semantics as hexadecimal calls");
    g_psx_call_bail = 0; mod_native_reset();
    mod_native_register_symbols(nullptr, 0, nullptr);

    PSXNativeHost helper_host{};
    helper_host.abi_version = PSX_NATIVE_MOD_ABI; helper_host.size = sizeof(helper_host);
    helper_host.option = helper_option;
    psx::mod::Host helper(helper_host);
    auto options = helper.options();
    check(options.get_bool("on") && !options.get_bool("off", true) && options.get_bool("missing", true),
          "C++ boolean options use validated values and explicit fallbacks");
    check(options.get_int("negative") == -12 && options.get_int("bad", 9) == 9 && options.get_int("overflow", 9) == 9,
          "C++ integer options reject malformed text and overflow");
    check(options.get_string("text") == "my-choice" && options.get_string("missing", "fallback") == "fallback" &&
          options.get_string("text", "short", 2) == "short", "C++ string options handle missing values and insufficient capacity");
    helper_host.size = offsetof(PSXNativeHost, call_guest);
    PSXNativeCPU helper_cpu{};
    check(helper.compatible() && !helper.supports_guest_calls() && !helper.call_guest(nullptr, psx::mod::CPU(helper_cpu), 0x80015000),
          "C++ helpers tolerate the original ABI v1 host without the appended service");
    helper_host.size = offsetof(PSXNativeHost, find_function);
    check(!helper.supports_named_functions() && !helper.find_function("Zero") &&
          !helper.call_guest(nullptr, psx::mod::CPU(helper_cpu), "Zero"),
          "C++ named helpers tolerate hosts with address calls but no name extension");
    fs::remove_all(root);
    std::cout << (failures ? "native mod tests failed: " : "native mod tests passed: ") << failures << '\n';
    return failures?1:0;
}
