#include "xenogears_indexed_disc.h"

#include "disc_path.h"
#include "iso_reader.h"
#include "mod_packages.h"
#include "psx_sha256.h"

#include <array>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <tuple>
#include <vector>

namespace fs = std::filesystem;
using PSXRecompV4::ModResolution;
using PSXRecompV4::ModVirtualDisc;

namespace {

constexpr size_t kRawSize = 2352;
constexpr size_t kUserSize = 2048;
constexpr size_t kUserOffset = 24;
int failures = 0;

void check(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        ++failures;
    }
}

uint8_t bcd(uint32_t value) {
    return static_cast<uint8_t>((value / 10 << 4) | value % 10);
}

uint32_t edc(const uint8_t* bytes, size_t size) {
    uint32_t value = 0;
    for (size_t i = 0; i < size; ++i) {
        value ^= bytes[i];
        for (unsigned bit = 0; bit < 8; ++bit)
            value = (value >> 1) ^ ((value & 1) ? 0xD8018001u : 0u);
    }
    return value;
}

void put_u32(uint8_t* p, uint32_t value) {
    p[0] = static_cast<uint8_t>(value);
    p[1] = static_cast<uint8_t>(value >> 8);
    p[2] = static_cast<uint8_t>(value >> 16);
    p[3] = static_cast<uint8_t>(value >> 24);
}

std::vector<uint8_t> packet(
    const std::vector<std::vector<uint8_t>>& files) {
    std::vector<uint8_t> result(4 + (files.size() + 1) * 4, 0);
    put_u32(result.data(), static_cast<uint32_t>(files.size()));
    uint32_t offset = static_cast<uint32_t>(result.size());
    for (size_t index = 0; index < files.size(); ++index) {
        put_u32(result.data() + 4 + index * 4, offset);
        result.insert(result.end(), files[index].begin(), files[index].end());
        offset += static_cast<uint32_t>(files[index].size());
    }
    put_u32(result.data() + 4 + files.size() * 4, offset);
    return result;
}

std::vector<uint8_t> lzss_literals(const std::vector<uint8_t>& bytes) {
    const size_t stored_size = (bytes.size() + 7) & ~size_t(7);
    std::vector<uint8_t> result(4, 0);
    put_u32(result.data(), static_cast<uint32_t>(stored_size));
    for (size_t offset = 0; offset < stored_size; offset += 8) {
        result.push_back(0);
        const size_t amount = std::min<size_t>(8, bytes.size() - offset);
        result.insert(result.end(), bytes.begin() + offset,
                      bytes.begin() + offset + amount);
        result.insert(result.end(), 8 - amount, 0);
    }
    return result;
}

std::vector<uint8_t> lzss_truncated_literals(const std::vector<uint8_t>& bytes) {
    std::vector<uint8_t> result(4, 0);
    put_u32(result.data(), static_cast<uint32_t>(bytes.size()));
    for (size_t offset = 0; offset < bytes.size(); offset += 8) {
        result.push_back(0);
        const size_t amount = std::min<size_t>(8, bytes.size() - offset);
        result.insert(result.end(), bytes.begin() + offset,
                      bytes.begin() + offset + amount);
    }
    return result;
}

void put_733(uint8_t* p, uint32_t value) {
    put_u32(p, value);
    p[4] = static_cast<uint8_t>(value >> 24);
    p[5] = static_cast<uint8_t>(value >> 16);
    p[6] = static_cast<uint8_t>(value >> 8);
    p[7] = static_cast<uint8_t>(value);
}

void ecc(const uint8_t* source, uint32_t major_count, uint32_t minor_count,
         uint32_t major_mult, uint32_t minor_inc, uint8_t* destination) {
    std::array<uint8_t, 256> forward{};
    std::array<uint8_t, 256> backward{};
    for (uint32_t i = 0; i < 256; ++i) {
        const uint32_t x = (i << 1) ^ ((i & 0x80) ? 0x11Du : 0u);
        forward[i] = static_cast<uint8_t>(x);
        backward[i ^ x] = static_cast<uint8_t>(i);
    }
    const uint32_t source_size = major_count * minor_count;
    for (uint32_t major = 0; major < major_count; ++major) {
        uint32_t index = (major >> 1) * major_mult + (major & 1);
        uint8_t a = 0;
        uint8_t b = 0;
        for (uint32_t minor = 0; minor < minor_count; ++minor) {
            const uint8_t value = source[index];
            index = (index + minor_inc) % source_size;
            a ^= value;
            b ^= value;
            a = forward[a];
        }
        a = backward[forward[a] ^ b];
        destination[major] = a;
        destination[major + major_count] = a ^ b;
    }
}

void finish_sector(std::array<uint8_t, kRawSize>& sector) {
    put_u32(sector.data() + 0x818, edc(sector.data() + 0x10, 0x808));
    const std::array<uint8_t, 4> address = {
        sector[12], sector[13], sector[14], sector[15]};
    std::fill(sector.begin() + 12, sector.begin() + 16, 0);
    ecc(sector.data() + 12, 86, 24, 2, 86, sector.data() + 0x81C);
    ecc(sector.data() + 12, 52, 43, 86, 88, sector.data() + 0x8C8);
    std::copy(address.begin(), address.end(), sector.begin() + 12);
}

std::array<uint8_t, kRawSize> raw_sector(uint32_t lba,
                                         const uint8_t* user = nullptr) {
    std::array<uint8_t, kRawSize> sector{};
    std::fill(sector.begin() + 1, sector.begin() + 11, 0xFF);
    const uint32_t frame = lba + 150;
    sector[12] = bcd(frame / 4500);
    sector[13] = bcd((frame / 75) % 60);
    sector[14] = bcd(frame % 75);
    sector[15] = 2;
    sector[16] = sector[20] = static_cast<uint8_t>(lba);
    sector[17] = sector[21] = 7;
    sector[18] = sector[22] = 8;
    sector[19] = sector[23] = 0;
    if (user) std::memcpy(sector.data() + kUserOffset, user, kUserSize);
    finish_sector(sector);
    return sector;
}

void entry(uint8_t* p, uint32_t lba, int32_t size) {
    p[0] = static_cast<uint8_t>(lba);
    p[1] = static_cast<uint8_t>(lba >> 8);
    p[2] = static_cast<uint8_t>(lba >> 16);
    put_u32(p + 3, static_cast<uint32_t>(size));
}

std::string hash(const std::vector<uint8_t>& bytes) {
    uint8_t digest[32];
    psx_sha256_compute(bytes.data(), bytes.size(), digest);
    static constexpr char digits[] = "0123456789abcdef";
    std::string result(64, '0');
    for (size_t i = 0; i < 32; ++i) {
        result[i * 2] = digits[digest[i] >> 4];
        result[i * 2 + 1] = digits[digest[i] & 15];
    }
    return result;
}

uint32_t u24(const uint8_t* p) {
    return uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16;
}

uint32_t u32(const uint8_t* p) {
    return uint32_t(p[0]) | uint32_t(p[1]) << 8 |
           uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24;
}

const std::array<uint8_t, kRawSize>* virtual_sector(
    const ModVirtualDisc& disc, uint32_t lba) {
    const auto overridden = disc.raw_sectors.find(lba);
    if (overridden != disc.raw_sectors.end()) return &overridden->second;
    if (lba < disc.appended_start_lba) return nullptr;
    const uint64_t index = uint64_t(lba) - disc.appended_start_lba;
    return index < disc.appended_raw_sectors.size()
        ? &disc.appended_raw_sectors[static_cast<size_t>(index)] : nullptr;
}

bool build_real_identity_plan(const fs::path& path, ModVirtualDisc& output,
                              std::string& error) {
    const auto resolved = PSXRecompV4::resolve_disc_path(path);
    PS1::ISOReader disc;
    if (resolved.mount.empty() || !disc.Open(resolved.mount.string())) {
        error = "cannot open real-disc integration fixture";
        return false;
    }
    const uint32_t base_sectors = disc.GetSectorCount();
    std::vector<uint8_t> table(16 * kUserSize);
    for (uint32_t i = 0; i < 16; ++i) {
        if (!disc.ReadSector(0x18 + i, table.data() + i * kUserSize)) {
            error = "cannot read real Xenogears table";
            return false;
        }
    }
    const int32_t root_size = static_cast<int32_t>(u32(table.data() + 3));
    if (root_size >= 0) {
        error = "real Xenogears table has an invalid XA root";
        return false;
    }
    uint32_t index = static_cast<uint32_t>(-int64_t(root_size)) + 1;
    const auto iso_files = disc.ListFiles("");
    const uint8_t* selected = nullptr;
    uint32_t lba = 0;
    int32_t signed_size = 0;
    for (; size_t(index + 1) * 7 <= table.size(); ++index) {
        selected = table.data() + size_t(index) * 7;
        lba = u24(selected);
        signed_size = static_cast<int32_t>(u32(selected + 3));
        if (lba == 0xFFFFFF) break;
        if (signed_size > 0) {
            const bool iso_visible = std::any_of(
                iso_files.begin(), iso_files.end(),
                [&](const PS1::ISOFileEntry& item) {
                    return !item.is_directory && item.lba == lba &&
                        item.size == static_cast<uint32_t>(signed_size);
                });
            if (!iso_visible) break;
        }
    }
    if (!selected || lba == 0xFFFFFF || signed_size <= 0) {
        error = "real Xenogears table has no ordinary integration fixture";
        return false;
    }
    std::vector<uint8_t> payload(static_cast<uint32_t>(signed_size));
    std::array<uint8_t, kUserSize> sector{};
    size_t copied = 0;
    for (uint32_t i = 0; copied < payload.size(); ++i) {
        if (!disc.ReadSector(lba + i, sector.data())) {
            error = "cannot read real Xenogears indexed file";
            return false;
        }
        const size_t amount = std::min(sector.size(), payload.size() - copied);
        std::memcpy(payload.data() + copied, sector.data(), amount);
        copied += amount;
    }

    ModResolution::IndexedFile replacement;
    replacement.format = XenogearsRecomp::kIndexedDiscFormat;
    replacement.index = index;
    replacement.payload = payload;
    replacement.expected_sha256 = hash(payload);
    replacement.package_id = "integration.identity";
    replacement.feature_id = "real-disc";
    if (!XenogearsRecomp::build_indexed_disc(
            resolved.mount, {replacement}, base_sectors, output, &error))
        return false;

    const uint32_t payload_sectors = static_cast<uint32_t>(
        payload.size() / kUserSize + (payload.size() % kUserSize != 0));
    if (output.sector_count != base_sectors + payload_sectors) {
        error = "real-disc virtual leadout is incorrect";
        return false;
    }
    const auto primary = output.raw_sectors.find(0x18 + index * 7 / kUserSize);
    if (primary == output.raw_sectors.end()) {
        error = "real-disc primary table sector was not patched";
        return false;
    }
    const size_t entry_offset = index * 7 % kUserSize;
    const uint8_t* patched_entry =
        primary->second.data() + kUserOffset + entry_offset;
    if (u24(patched_entry) != base_sectors ||
        u32(patched_entry + 3) != payload.size()) {
        error = "real-disc primary table entry is incorrect";
        return false;
    }
    for (uint32_t i = 0; i < payload_sectors; ++i) {
        const auto* sector_bytes = virtual_sector(output, base_sectors + i);
        if (!sector_bytes) {
            error = "real-disc replacement sector is missing";
            return false;
        }
        const size_t offset = size_t(i) * kUserSize;
        const size_t amount = std::min(kUserSize, payload.size() - offset);
        if (std::memcmp(sector_bytes->data() + kUserOffset,
                        payload.data() + offset, amount) != 0) {
            error = "real-disc replacement payload changed";
            return false;
        }
    }
    return true;
}

bool build_real_fmv_plan(const fs::path& path, const fs::path& bundle_path,
                         ModVirtualDisc& output, std::string& error) {
    const auto resolved = PSXRecompV4::resolve_disc_path(path);
    PS1::ISOReader disc;
    if (resolved.mount.empty() || !disc.Open(resolved.mount.string())) {
        error = "cannot open real FMV integration disc";
        return false;
    }
    std::ifstream bundle_stream(bundle_path, std::ios::binary);
    std::vector<uint8_t> bundle(
        (std::istreambuf_iterator<char>(bundle_stream)),
        std::istreambuf_iterator<char>());
    if (bundle.size() < 16 ||
        std::memcmp(bundle.data(), "XGFMV112", 8) != 0 ||
        u32(bundle.data() + 8) != 1) {
        error = "cannot read real FMV integration bundle";
        return false;
    }
    const uint32_t disc_number = u32(bundle.data() + 12);
    const uint32_t raw_count = u32(bundle.data() + 16);
    const uint32_t subtitle_count = u32(bundle.data() + 20);
    const uint32_t soft_executable_size = u32(bundle.data() + 24);
    size_t bundle_offset = 28 + size_t(raw_count) * (4 + 32 + kRawSize);
    for (uint32_t i = 0; i < subtitle_count; ++i) {
        if (bundle_offset + 8 > bundle.size()) {
            error = "real FMV integration subtitle header is truncated";
            return false;
        }
        const uint32_t size = u32(bundle.data() + bundle_offset + 4);
        bundle_offset += 8 + size;
    }
    if (bundle_offset + soft_executable_size != bundle.size()) {
        error = "real FMV integration executable is truncated";
        return false;
    }
    const std::vector<uint8_t> soft_executable(
        bundle.begin() + bundle_offset, bundle.end());
    const uint32_t executable_index = disc_number == 1 ? 22 : 17;
    const uint32_t stream_index = disc_number == 1 ? 4150 : 4145;
    const uint32_t final_stream_index = disc_number == 1 ? 4161 : 4156;
    const uint32_t sentinel_index = disc_number == 1 ? 4162 : 4157;
    if (disc_number < 1 || disc_number > 2) {
        error = "real FMV integration bundle has an invalid disc number";
        return false;
    }

    std::vector<uint8_t> table(16 * kUserSize);
    for (uint32_t i = 0; i < 16; ++i) {
        if (!disc.ReadSector(0x18 + i, table.data() + i * kUserSize)) {
            error = "cannot read real FMV integration table";
            return false;
        }
    }
    const uint8_t* executable_entry =
        table.data() + size_t(executable_index) * 7;
    const uint32_t executable_lba = u24(executable_entry);
    const uint32_t executable_size = u32(executable_entry + 3);
    std::vector<uint8_t> stock_executable(executable_size);
    size_t copied = 0;
    std::array<uint8_t, kUserSize> sector{};
    for (uint32_t i = 0; copied < stock_executable.size(); ++i) {
        if (!disc.ReadSector(executable_lba + i, sector.data())) {
            error = "cannot read real FMV integration executable";
            return false;
        }
        const size_t amount =
            std::min(sector.size(), stock_executable.size() - copied);
        std::memcpy(stock_executable.data() + copied, sector.data(), amount);
        copied += amount;
    }
    const uint32_t stock_stream_lba =
        u24(table.data() + size_t(stream_index) * 7);
    const uint32_t stock_stream_size =
        u32(table.data() + size_t(stream_index) * 7 + 3);

    ModResolution::IndexedFile replacement;
    replacement.format = XenogearsRecomp::kIndexedDiscFormat;
    replacement.index = executable_index;
    replacement.payload = std::move(bundle);
    replacement.expected_sha256 = hash(stock_executable);
    replacement.package_id = "integration.fmv";
    replacement.feature_id = "perfect-works";
    replacement.compose = "xenogears-pwb-fmv-0.11.2";
    const uint32_t base_sectors = disc.GetSectorCount();
    if (!XenogearsRecomp::build_indexed_disc(
            resolved.mount, {replacement}, base_sectors, output, &error))
        return false;

    const auto read_virtual_entry = [&](uint32_t index, uint32_t& lba,
                                        uint32_t& size) {
        const size_t offset = size_t(index) * 7;
        const uint32_t sector_lba = 0x18 + static_cast<uint32_t>(offset / kUserSize);
        const auto found = output.raw_sectors.find(sector_lba);
        if (found == output.raw_sectors.end()) return false;
        const uint8_t* p = found->second.data() + kUserOffset + offset % kUserSize;
        lba = u24(p);
        size = u32(p + 3);
        return true;
    };
    uint32_t final_lba = 0;
    uint32_t final_size = 0;
    uint32_t sentinel_lba = 0;
    uint32_t sentinel_size = 1;
    if (!read_virtual_entry(final_stream_index, final_lba, final_size) ||
        !read_virtual_entry(sentinel_index, sentinel_lba, sentinel_size) ||
        final_lba != stock_stream_lba || final_size != stock_stream_size ||
        sentinel_lba != 0xFFFFFF || sentinel_size != 0 ||
        output.raw_sectors.find(executable_lba) == output.raw_sectors.end() ||
        output.appended_raw_sectors.empty()) {
        error = "real FMV virtual plan did not rebuild the expected layout";
        return false;
    }
    constexpr size_t injected_code_offset = 0xFB0;
    const auto injected_sector = output.raw_sectors.find(
        executable_lba + injected_code_offset / kUserSize);
    if (injected_sector == output.raw_sectors.end() ||
        std::memcmp(
            injected_sector->second.data() + kUserOffset +
                injected_code_offset % kUserSize,
            soft_executable.data() + injected_code_offset, 64) != 0) {
        error = "real FMV virtual plan overwrote the soft-sub executable code";
        return false;
    }
    return true;
}

bool validate_real_catalog(const fs::path& catalog, const fs::path& disc1,
                           const fs::path& disc2, std::string& error) {
    const fs::path root =
        fs::temp_directory_path() / "xenogears-pw-catalog-validation";
    std::error_code cleanup_error;
    fs::remove_all(root, cleanup_error);
    PSXRecompV4::ModPackageManager manager(root);
    size_t installed = 0;
    std::vector<fs::path> archives;
    for (const fs::directory_entry& item : fs::directory_iterator(catalog))
        if (item.path().extension() == ".psxmod")
            archives.push_back(item.path());
    std::sort(archives.begin(), archives.end());
    for (const fs::path& archive : archives) {
        if (!manager.install_archive(archive, nullptr, nullptr, &error)) {
            fs::remove_all(root, cleanup_error);
            return false;
        }
        ++installed;
    }
    if (installed != 21 ||
        (!PSXRecompV4::mod_indexed_file_format_register("xenogears") &&
         !PSXRecompV4::mod_indexed_file_format_registered("xenogears"))) {
        fs::remove_all(root, cleanup_error);
        error = "real catalog package count or format registration failed";
        return false;
    }
    size_t enabled = 0;
    for (const auto& [package_id, versions] : manager.packages()) {
        (void)versions;
        if (package_id ==
            "org.perfectworksbuild.individual.story-mode")
            continue;
        if (!manager.set_feature_enabled(
                package_id, "perfect-works", true, &error)) {
            fs::remove_all(root, cleanup_error);
            return false;
        }
        ++enabled;
    }
    if (enabled != 20) {
        fs::remove_all(root, cleanup_error);
        error = "real catalog did not enable all compatible packages";
        return false;
    }

    const std::array<std::tuple<fs::path, std::string>, 2> discs = {{
        {disc1, "74265236654985f8d5d76f79767ca62a9b2b6ba299c995211ff94588928a6235"},
        {disc2, "b5fce68b407e9f4ae7474b3487a3d9a35ccd2c98e8b377374dd1fc1060450e30"},
    }};
    for (const auto& [path, digest] : discs) {
        const ModResolution resolution =
            manager.resolve("SLUS-00664", {}, digest);
        if (!resolution.ok) {
            error = resolution.errors.empty()
                ? "real catalog resolution failed"
                : resolution.errors.front();
            fs::remove_all(root, cleanup_error);
            return false;
        }
        const auto resolved = PSXRecompV4::resolve_disc_path(path);
        PS1::ISOReader reader;
        if (resolved.mount.empty() || !reader.Open(resolved.mount.string())) {
            fs::remove_all(root, cleanup_error);
            error = "cannot open real catalog disc";
            return false;
        }
        ModVirtualDisc output;
        if (!XenogearsRecomp::build_indexed_disc(
                resolved.mount, resolution.indexed_files,
                reader.GetSectorCount(), output, &error) ||
            resolution.writes.empty() || output.raw_sectors.size() < 1000 ||
            output.appended_raw_sectors.empty()) {
            fs::remove_all(root, cleanup_error);
            if (error.empty()) error = "real catalog virtual plan is incomplete";
            return false;
        }
        if (digest == std::get<1>(discs[0])) {
            const auto primary = output.raw_sectors.find(0x18);
            if (primary == output.raw_sectors.end()) {
                fs::remove_all(root, cleanup_error);
                error = "real catalog primary table is missing";
                return false;
            }
            const uint32_t executable_lba =
                u24(primary->second.data() + kUserOffset + 22 * 7);
            constexpr size_t injected_code_offset = 0xFB0;
            const auto code = output.raw_sectors.find(
                executable_lba + injected_code_offset / kUserSize);
            static constexpr std::array<uint8_t, 4> prologue = {
                0xF8, 0xFF, 0xBD, 0x27};
            if (code == output.raw_sectors.end() ||
                !std::equal(
                    prologue.begin(), prologue.end(),
                    code->second.begin() + kUserOffset +
                        injected_code_offset % kUserSize)) {
                fs::remove_all(root, cleanup_error);
                error = "real catalog overwrote the soft-sub executable code";
                return false;
            }
        }
    }
    fs::remove_all(root, cleanup_error);
    return true;
}

} // namespace

int main(int argc, char** argv) {
    if (argc == 5 && std::string(argv[1]) == "--catalog") {
        std::string error;
        if (!validate_real_catalog(argv[2], argv[3], argv[4], error)) {
            std::cerr << "FAIL: " << error << '\n';
            return 1;
        }
        std::cout << "xenogears real catalog integration passed\n";
        return 0;
    }
    if (argc == 4 && std::string(argv[1]) == "--fmv") {
        ModVirtualDisc output;
        std::string error;
        if (!build_real_fmv_plan(argv[2], argv[3], output, error)) {
            std::cerr << "FAIL: " << error << '\n';
            return 1;
        }
        std::cout << "xenogears real-disc FMV integration passed\n";
        return 0;
    }
    if (argc > 1) {
        ModVirtualDisc reference;
        std::string error;
        if (!build_real_identity_plan(argv[1], reference, error)) {
            std::cerr << "FAIL: " << error << '\n';
            return 1;
        }
        for (int i = 2; i < argc; ++i) {
            ModVirtualDisc compared;
            if (!build_real_identity_plan(argv[i], compared, error)) {
                std::cerr << "FAIL: " << error << '\n';
                return 1;
            }
            if (compared.sector_count != reference.sector_count ||
                compared.appended_start_lba != reference.appended_start_lba ||
                compared.raw_sectors != reference.raw_sectors ||
                compared.appended_raw_sectors !=
                    reference.appended_raw_sectors) {
                std::cerr << "FAIL: real-disc virtual plans differ by container\n";
                return 1;
            }
        }
        std::cout << "xenogears real-disc indexed integration passed\n";
        return 0;
    }

    constexpr uint32_t base_sectors = 80;
    constexpr uint32_t embedded_lba = 44;
    constexpr size_t embedded_offset = 2030;
    constexpr size_t table_size = 7 * 7;

    std::vector<uint8_t> table(16 * kUserSize);
    entry(table.data() + 0, 40, -1);
    entry(table.data() + 7, 41, 100);
    entry(table.data() + 14, embedded_lba, 4096);
    entry(table.data() + 21, 60, 3000);
    const std::vector<uint8_t> stock_packet = packet({
        {0x10, 0x11, 0x12, 0x13}, {0x20, 0x21, 0x22, 0x23}});
    std::vector<uint8_t> stock_lzss_data(18, 0);
    std::vector<uint8_t> stock_lzss = {0, 0, 0, 0, 1, 0, 0};
    put_u32(stock_lzss.data(), static_cast<uint32_t>(stock_lzss_data.size()));
    entry(table.data() + 28, 62, static_cast<int32_t>(stock_packet.size()));
    entry(table.data() + 35, 63, static_cast<int32_t>(stock_lzss.size()));
    entry(table.data() + 42, 0xFFFFFF, 0);

    std::vector<std::array<uint8_t, kRawSize>> sectors(base_sectors);
    for (uint32_t lba = 0; lba < base_sectors; ++lba)
        sectors[lba] = raw_sector(lba);
    for (uint32_t i = 0; i < 16; ++i)
        sectors[0x18 + i] = raw_sector(0x18 + i, table.data() + i * kUserSize);

    std::vector<uint8_t> embedded(4096, 0x5A);
    std::fill(embedded.begin() + embedded_offset - 4,
              embedded.begin() + embedded_offset, 0xFF);
    std::memcpy(embedded.data() + embedded_offset, table.data(), table_size);
    sectors[embedded_lba] = raw_sector(embedded_lba, embedded.data());
    sectors[embedded_lba + 1] = raw_sector(embedded_lba + 1,
                                           embedded.data() + kUserSize);

    std::vector<uint8_t> stock(3000);
    for (size_t i = 0; i < stock.size(); ++i)
        stock[i] = static_cast<uint8_t>(i * 13 + 9);
    stock[0] = 0xEE;
    stock[1] = 0x01;
    stock[126 + 0x100] = 2;
    stock[126 + 0x101] = 0;
    stock[126 + 0x102] = 2;
    stock[126 + 0x103] = 0;
    stock[126 + 0x10A] = 0x34;
    stock[126 + 0x10B] = 0x12;
    std::array<uint8_t, kUserSize> stock_tail{};
    std::memcpy(stock_tail.data(), stock.data() + kUserSize,
                stock.size() - kUserSize);
    sectors[60] = raw_sector(60, stock.data());
    sectors[61] = raw_sector(61, stock_tail.data());
    std::array<uint8_t, kUserSize> packet_sector{};
    std::copy(stock_packet.begin(), stock_packet.end(), packet_sector.begin());
    sectors[62] = raw_sector(62, packet_sector.data());
    std::array<uint8_t, kUserSize> lzss_sector{};
    std::copy(stock_lzss.begin(), stock_lzss.end(), lzss_sector.begin());
    sectors[63] = raw_sector(63, lzss_sector.data());

    const fs::path dir = fs::temp_directory_path() /
        ("xg-indexed-disc-test-" + std::to_string(
            std::chrono::high_resolution_clock::now()
                .time_since_epoch().count()));
    fs::remove_all(dir);
    fs::create_directories(dir);
    const fs::path bin = dir / "disc.bin";
    const fs::path cue = dir / "disc.cue";
    {
        std::ofstream stream(bin, std::ios::binary);
        for (const auto& sector : sectors)
            stream.write(reinterpret_cast<const char*>(sector.data()), sector.size());
    }
    {
        std::ofstream stream(cue);
        stream << "FILE \"disc.bin\" BINARY\n"
                  "  TRACK 01 MODE2/2352\n"
                  "    INDEX 01 00:00:00\n";
    }

    ModResolution::IndexedFile replacement;
    replacement.format = XenogearsRecomp::kIndexedDiscFormat;
    replacement.index = 3;
    replacement.payload.resize(2500);
    for (size_t i = 0; i < replacement.payload.size(); ++i)
        replacement.payload[i] = static_cast<uint8_t>(255 - i * 3);
    replacement.expected_sha256 = hash(stock);
    replacement.package_id = "test.package";
    replacement.feature_id = "replacement";

    ModVirtualDisc output;
    std::string error;
    check(XenogearsRecomp::build_indexed_disc(
              cue, {replacement}, base_sectors, output, &error),
          error.c_str());
    check(output.sector_count == 82, "replacement grows disc by two sectors");
    check(output.raw_sectors.count(10) == 0, "untouched sectors stay sparse");

    const auto& primary = output.raw_sectors.at(0x18);
    const uint8_t* primary_entry = primary.data() + kUserOffset + 21;
    check(u24(primary_entry) == base_sectors, "primary table LBA patched");
    check(u32(primary_entry + 3) == replacement.payload.size(),
          "primary table size patched");
    check(primary[16] == sectors[0x18][16] && primary[17] == sectors[0x18][17],
          "primary sector metadata preserved");

    const auto& embedded_first = output.raw_sectors.at(embedded_lba);
    const auto& embedded_second = output.raw_sectors.at(embedded_lba + 1);
    std::array<uint8_t, table_size> embedded_table{};
    const size_t first_amount = kUserSize - embedded_offset;
    std::memcpy(embedded_table.data(),
                embedded_first.data() + kUserOffset + embedded_offset,
                first_amount);
    std::memcpy(embedded_table.data() + first_amount,
                embedded_second.data() + kUserOffset,
                table_size - first_amount);
    check(u24(embedded_table.data() + 21) == base_sectors,
          "embedded table LBA patched across sectors");
    check(u32(embedded_table.data() + 24) == replacement.payload.size(),
          "embedded table size patched");

    check(std::memcmp(virtual_sector(output, 80)->data() + kUserOffset,
                      replacement.payload.data(), kUserSize) == 0,
          "first payload sector readable");
    check(std::memcmp(virtual_sector(output, 81)->data() + kUserOffset,
                      replacement.payload.data() + kUserSize,
                      replacement.payload.size() - kUserSize) == 0,
          "final payload sector readable");
    check((*virtual_sector(output, 80))[18] == 0x08 &&
              (*virtual_sector(output, 80))[22] == 0x08 &&
              (*virtual_sector(output, 81))[18] == 0x89 &&
              (*virtual_sector(output, 81))[22] == 0x89,
          "only the final replacement sector carries EOF/EOR");
    const auto& first_payload_sector = *virtual_sector(output, 80);
    check(hash(std::vector<uint8_t>(first_payload_sector.begin(),
                                    first_payload_sector.end())) ==
              "4a5c8f13fb9e2ba74433648e83ce0872a82da774255430aec5ea102c6f58b3c3",
          "synthetic sector matches the independent Mode 2 Form 1 fixture");

    ModVirtualDisc repeated;
    const fs::path bare_dir = dir / "bare";
    fs::create_directories(bare_dir);
    const fs::path bare_bin = bare_dir / "disc.bin";
    fs::copy_file(bin, bare_bin);
    check(XenogearsRecomp::build_indexed_disc(
              bare_bin, {replacement}, base_sectors, repeated, &error),
          "direct BIN resolves and builds");
    check(repeated.raw_sectors == output.raw_sectors &&
              repeated.appended_raw_sectors == output.appended_raw_sectors,
          "EDC/ECC output is stable across CUE and BIN picks");
    auto expected_sector = *virtual_sector(output, 80);
    finish_sector(expected_sector);
    check(expected_sector == *virtual_sector(output, 80),
          "synthetic payload sector has stable EDC/ECC");
    auto expected_primary = output.raw_sectors.at(0x18);
    finish_sector(expected_primary);
    check(expected_primary == output.raw_sectors.at(0x18),
          "patched stock sector has stable EDC/ECC");

    ModResolution::IndexedFile embedded_replacement;
    embedded_replacement.format = XenogearsRecomp::kIndexedDiscFormat;
    embedded_replacement.index = 2;
    embedded_replacement.payload = embedded;
    embedded_replacement.expected_sha256 = hash(embedded);
    embedded_replacement.package_id = "test.package";
    embedded_replacement.feature_id = "embedded-container";
    ModVirtualDisc replaced_embedded;
    const bool replaced_embedded_ok = XenogearsRecomp::build_indexed_disc(
        cue, {replacement, embedded_replacement}, base_sectors,
        replaced_embedded, &error);
    check(replaced_embedded_ok, error.c_str());
    if (replaced_embedded_ok) {
        check(replaced_embedded.sector_count == 84,
              "multiple replacements receive deterministic appended ranges");
        std::vector<uint8_t> virtual_embedded(
            embedded_replacement.payload.size());
        for (uint32_t i = 0; i < 2; ++i) {
            std::memcpy(
                virtual_embedded.data() + i * kUserSize,
                virtual_sector(replaced_embedded, 82 + i)->data() + kUserOffset,
                kUserSize);
        }
        check(u24(virtual_embedded.data() + embedded_offset + 14) == 82 &&
                  u24(virtual_embedded.data() + embedded_offset + 21) == 80,
              "replacement containing the embedded table receives the final table");
    }
    std::fill(embedded_replacement.payload.begin() + embedded_offset + 3,
              embedded_replacement.payload.begin() + embedded_offset + 7, 0);
    check(!XenogearsRecomp::build_indexed_disc(
              cue, {embedded_replacement}, base_sectors,
              replaced_embedded, &error),
          "replacement table containers must preserve one recognizable table");

    ModResolution::IndexedFile merge_left;
    merge_left.format = XenogearsRecomp::kIndexedDiscFormat;
    merge_left.index = 3;
    merge_left.payload = stock;
    merge_left.payload[10] ^= 0x55;
    merge_left.expected_sha256 = hash(stock);
    merge_left.package_id = "merge.left";
    merge_left.feature_id = "left";
    merge_left.compose = "three-way";
    ModResolution::IndexedFile merge_right = merge_left;
    merge_right.payload = stock;
    merge_right.payload[2500] ^= 0xAA;
    merge_right.package_id = "merge.right";
    merge_right.feature_id = "right";
    ModVirtualDisc merged;
    const bool merged_ok = XenogearsRecomp::build_indexed_disc(
        cue, {merge_left, merge_right}, base_sectors, merged, &error);
    check(merged_ok, error.c_str());
    if (merged_ok) {
        check((*virtual_sector(merged, 80))[kUserOffset + 10] ==
                  merge_left.payload[10] &&
              (*virtual_sector(merged, 81))[
                  kUserOffset + 2500 - kUserSize] == merge_right.payload[2500],
              "three-way composition preserves disjoint edits from both owners");
    }
    merge_right.payload[10] ^= 0x33;
    check(!XenogearsRecomp::build_indexed_disc(
              cue, {merge_left, merge_right}, base_sectors, merged, &error) &&
              error.find("overlaps") != std::string::npos,
          "three-way composition rejects differing edits to the same byte");
    merge_right.payload = stock;
    merge_right.payload.pop_back();
    check(!XenogearsRecomp::build_indexed_disc(
              cue, {merge_left, merge_right}, base_sectors, merged, &error) &&
              error.find("equal-size") != std::string::npos,
           "three-way composition rejects structural payload changes");

    ModResolution::IndexedFile items_packet;
    items_packet.format = XenogearsRecomp::kIndexedDiscFormat;
    items_packet.index = 4;
    items_packet.payload = packet({
        {0x10, 0x11, 0x12, 0x13}, {0x20, 0x21, 0xA2, 0x23}});
    items_packet.expected_sha256 = hash(stock_packet);
    items_packet.package_id =
        "org.perfectworksbuild.individual.rebalanced-items";
    items_packet.feature_id = "perfect-works";
    items_packet.compose = "xenogears-pwb-0.11.2";
    ModResolution::IndexedFile deathblow_packet = items_packet;
    deathblow_packet.payload = packet({
        {0x10, 0xB1, 0x12, 0x13}, {0x20, 0x21, 0x22, 0x23}});
    deathblow_packet.package_id =
        "org.perfectworksbuild.individual.no-deathblow-levels";
    ModVirtualDisc packet_output;
    const bool packet_ok = XenogearsRecomp::build_indexed_disc(
        cue, {items_packet, deathblow_packet}, base_sectors,
        packet_output, &error);
    check(packet_ok, error.c_str());
    if (packet_ok) {
        const std::vector<uint8_t> expected_packet = packet({
            {0x10, 0xB1, 0x12, 0x13}, {0x20, 0x21, 0xA2, 0x23}});
        const uint8_t* actual =
            virtual_sector(packet_output, base_sectors)->data() + kUserOffset;
        check(std::memcmp(actual, expected_packet.data(),
                          expected_packet.size()) == 0 &&
                  u32(actual + 12) == expected_packet.size(),
              "PWB packet composition preserves both edits and terminal offset");
    }
    std::vector<uint8_t> longer_battle(20);
    for (size_t i = 0; i < longer_battle.size(); ++i)
        longer_battle[i] = static_cast<uint8_t>(0x40 + i);
    std::vector<uint8_t> flash_edit = stock_lzss_data;
    flash_edit[5] = 0xEE;
    ModResolution::IndexedFile bug_lzss;
    bug_lzss.format = XenogearsRecomp::kIndexedDiscFormat;
    bug_lzss.index = 5;
    bug_lzss.payload = lzss_truncated_literals(longer_battle);
    bug_lzss.expected_sha256 = hash(stock_lzss);
    bug_lzss.package_id = "org.perfectworksbuild.individual.bug-fixes";
    bug_lzss.feature_id = "perfect-works";
    bug_lzss.compose = "xenogears-pwb-0.11.2";
    ModResolution::IndexedFile flash_lzss = bug_lzss;
    flash_lzss.payload = lzss_truncated_literals(flash_edit);
    flash_lzss.package_id =
        "org.perfectworksbuild.individual.no-battle-flashes";
    ModVirtualDisc lzss_output;
    const bool lzss_ok = XenogearsRecomp::build_indexed_disc(
        cue, {bug_lzss, flash_lzss}, base_sectors, lzss_output, &error);
    check(lzss_ok, error.c_str());
    if (lzss_ok) {
        longer_battle[5] = 0xEE;
        const std::vector<uint8_t> expected_lzss = lzss_literals(longer_battle);
        const uint8_t* actual =
            virtual_sector(lzss_output, base_sectors)->data() + kUserOffset;
        check(std::memcmp(actual, expected_lzss.data(), expected_lzss.size()) == 0 &&
                  u32(actual) == 24,
              "PWB LZSS composition pads the final guest token group");
    }

    ModResolution::IndexedFile exp_claim;
    exp_claim.format = XenogearsRecomp::kIndexedDiscFormat;
    exp_claim.index = 3;
    exp_claim.payload = stock;
    exp_claim.expected_sha256 = hash(stock);
    exp_claim.package_id = "org.perfectworksbuild.individual.exp";
    exp_claim.feature_id = "perfect-works";
    exp_claim.compose = "xenogears-pwb-0.11.2";
    exp_claim.options["multiplier"] = "1x";
    ModResolution::IndexedFile gold_claim = exp_claim;
    gold_claim.package_id = "org.perfectworksbuild.individual.gold";
    gold_claim.options["multiplier"] = "2x";
    ModVirtualDisc reward_output;
    const bool reward_ok = XenogearsRecomp::build_indexed_disc(
        cue, {exp_claim, gold_claim}, base_sectors, reward_output, &error);
    check(reward_ok, error.c_str());
    if (reward_ok) {
        const uint8_t* actual =
            virtual_sector(reward_output, base_sectors)->data() + kUserOffset;
        check(u32(actual + 126 + 0x100) == 2,
              "PWB EXP 1x zero-extends the original low word");
        check(actual[126 + 0x10A] == 0x68 &&
                  actual[126 + 0x10B] == 0x24,
              "PWB gold scaling composes with EXP");
    }

    ModResolution::IndexedFile item_copy = exp_claim;
    item_copy.options.clear();
    item_copy.package_id =
        "org.perfectworksbuild.individual.rebalanced-items";
    item_copy.payload[100] = 0xA1;
    ModResolution::IndexedFile script_copy = item_copy;
    script_copy.package_id =
        "org.perfectworksbuild.individual.retranslation";
    script_copy.payload[100] = 0xA2;
    ModResolution::IndexedFile jpn_copy = item_copy;
    jpn_copy.package_id = "org.perfectworksbuild.individual.jpn-controls";
    jpn_copy.payload[100] = 0xA3;
    ModResolution::IndexedFile encounter_copy = item_copy;
    encounter_copy.package_id =
        "org.perfectworksbuild.individual.half-encounters";
    encounter_copy.payload[100] = 0xA4;
    ModVirtualDisc copy_order_output;
    const bool copy_order_ok = XenogearsRecomp::build_indexed_disc(
        cue, {encounter_copy, jpn_copy, script_copy, item_copy},
        base_sectors, copy_order_output, &error);
    check(copy_order_ok, error.c_str());
    if (copy_order_ok) {
        const uint8_t* actual =
            virtual_sector(copy_order_output, base_sectors)->data() + kUserOffset;
        check(actual[100] == 0xA4,
              "PWB whole-file claims follow items, script, JPN, encounter order");
    }

    replacement.expected_sha256.assign(64, '0');
    check(!XenogearsRecomp::build_indexed_disc(
              cue, {replacement}, base_sectors, repeated, &error),
          "incorrect stock hash rejected");
    replacement.expected_sha256 = hash(stock);
    replacement.index = 1;
    check(!XenogearsRecomp::build_indexed_disc(
              cue, {replacement}, base_sectors, repeated, &error),
          "initial XA child index rejected");
    replacement.index = 6;
    check(!XenogearsRecomp::build_indexed_disc(
              cue, {replacement}, base_sectors, repeated, &error),
          "sentinel index rejected");
    replacement.index = 3;
    check(!XenogearsRecomp::build_indexed_disc(
              cue, {replacement}, base_sectors - 1, repeated, &error),
          "runtime sector-count mismatch rejected");
    check(!XenogearsRecomp::build_indexed_disc(
              cue, {replacement, replacement}, base_sectors, repeated, &error),
          "duplicate index rejected");

    std::vector<std::array<uint8_t, kRawSize>> iso_visible_sectors = sectors;
    std::array<uint8_t, kUserSize> pvd{};
    pvd[0] = 1;
    std::memcpy(pvd.data() + 1, "CD001", 5);
    pvd[6] = 1;
    uint8_t* root_record = pvd.data() + 156;
    root_record[0] = 34;
    put_733(root_record + 2, 12);
    put_733(root_record + 10, kUserSize);
    root_record[25] = 2;
    root_record[32] = 1;
    root_record[33] = 0;
    iso_visible_sectors[16] = raw_sector(16, pvd.data());
    std::array<uint8_t, kUserSize> root_directory{};
    static constexpr char directory_name[] = "DATA";
    uint8_t* directory_record = root_directory.data();
    directory_record[0] = static_cast<uint8_t>(33 + sizeof(directory_name) - 1);
    put_733(directory_record + 2, 13);
    put_733(directory_record + 10, kUserSize);
    directory_record[25] = 2;
    directory_record[32] = static_cast<uint8_t>(sizeof(directory_name) - 1);
    std::memcpy(directory_record + 33, directory_name, sizeof(directory_name) - 1);
    iso_visible_sectors[12] = raw_sector(12, root_directory.data());
    std::array<uint8_t, kUserSize> nested_directory{};
    static constexpr char visible_name[] = "VISIBLE.BIN;1";
    uint8_t* visible_record = nested_directory.data();
    visible_record[0] = static_cast<uint8_t>(33 + sizeof(visible_name) - 1);
    put_733(visible_record + 2, 60);
    put_733(visible_record + 10, 3000);
    visible_record[32] = static_cast<uint8_t>(sizeof(visible_name) - 1);
    std::memcpy(visible_record + 33, visible_name, sizeof(visible_name) - 1);
    iso_visible_sectors[13] = raw_sector(13, nested_directory.data());
    const fs::path iso_visible_bin = dir / "iso-visible.bin";
    {
        std::ofstream stream(iso_visible_bin, std::ios::binary);
        for (const auto& sector : iso_visible_sectors)
            stream.write(reinterpret_cast<const char*>(sector.data()), sector.size());
    }
    check(!XenogearsRecomp::build_indexed_disc(
              iso_visible_bin, {replacement}, base_sectors, repeated, &error) &&
              error.find("ISO9660-visible") != std::string::npos,
          "ISO9660-visible entries must not be redirected only in the hidden table");

    fs::remove_all(dir);
    if (failures == 0) std::cout << "xenogears indexed-disc tests passed\n";
    return failures == 0 ? 0 : 1;
}
