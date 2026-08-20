#include "xenogears_indexed_disc.h"

#include "disc_path.h"
#include "iso_reader.h"
#include "psx_sha256.h"

#include <array>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
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

} // namespace

int main(int argc, char** argv) {
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
    constexpr size_t table_size = 5 * 7;

    std::vector<uint8_t> table(16 * kUserSize);
    entry(table.data() + 0, 40, -1);
    entry(table.data() + 7, 41, 100);
    entry(table.data() + 14, embedded_lba, 4096);
    entry(table.data() + 21, 60, 3000);
    entry(table.data() + 28, 0xFFFFFF, 0);

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
    std::array<uint8_t, kUserSize> stock_tail{};
    std::memcpy(stock_tail.data(), stock.data() + kUserSize,
                stock.size() - kUserSize);
    sectors[60] = raw_sector(60, stock.data());
    sectors[61] = raw_sector(61, stock_tail.data());

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

    replacement.expected_sha256.assign(64, '0');
    check(!XenogearsRecomp::build_indexed_disc(
              cue, {replacement}, base_sectors, repeated, &error),
          "incorrect stock hash rejected");
    replacement.expected_sha256 = hash(stock);
    replacement.index = 1;
    check(!XenogearsRecomp::build_indexed_disc(
              cue, {replacement}, base_sectors, repeated, &error),
          "initial XA child index rejected");
    replacement.index = 4;
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
