#include "xenogears_indexed_disc.h"

#include "disc_path.h"
#include "iso_reader.h"
#include "psx_sha256.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <exception>
#include <limits>
#include <set>
#include <utility>

namespace XenogearsRecomp {
namespace {

constexpr uint32_t kPrimaryTableLba = 0x18;
constexpr uint32_t kPrimaryTableSectors = 0x10;
constexpr size_t kUserSectorSize = 2048;
constexpr size_t kRawSectorSize = 2352;
constexpr size_t kUserOffset = 24;
constexpr size_t kEntrySize = 7;
constexpr uint32_t kSentinelLba = 0xFFFFFF;
constexpr uint32_t kMaxMsfLba = 99u * 60u * 75u + 59u * 75u + 74u - 150u;
constexpr uint32_t kMaxEmbeddedFiles = 64;
constexpr uint32_t kMaxScannedFileBytes = 64u * 1024u * 1024u;
constexpr uint32_t kMaxVirtualSectors = 64u * 1024u;

struct TableEntry {
    uint32_t lba;
    int32_t size;
};

struct EmbeddedTable {
    size_t entry_index = 0;
    uint32_t lba = 0;
    size_t offset = 0;
};

struct ReplacementAllocation {
    const PSXRecompV4::ModResolution::IndexedFile* file = nullptr;
    uint32_t lba = 0;
    uint32_t sectors = 0;
};

bool fail(std::string* error, const std::string& message) {
    if (error) *error = message;
    return false;
}

uint32_t read_u24(const uint8_t* p) {
    return uint32_t(p[0]) | (uint32_t(p[1]) << 8) | (uint32_t(p[2]) << 16);
}

uint32_t read_u32(const uint8_t* p) {
    return uint32_t(p[0]) | (uint32_t(p[1]) << 8) |
           (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24);
}

void write_entry(uint8_t* p, uint32_t lba, uint32_t size) {
    p[0] = static_cast<uint8_t>(lba);
    p[1] = static_cast<uint8_t>(lba >> 8);
    p[2] = static_cast<uint8_t>(lba >> 16);
    p[3] = static_cast<uint8_t>(size);
    p[4] = static_cast<uint8_t>(size >> 8);
    p[5] = static_cast<uint8_t>(size >> 16);
    p[6] = static_cast<uint8_t>(size >> 24);
}

uint32_t sector_count_for(uint32_t size) {
    return size / kUserSectorSize + (size % kUserSectorSize != 0);
}

bool hash_file(PS1::ISOReader& disc, const TableEntry& entry,
               std::string& result) {
    psx_sha256_ctx hash;
    psx_sha256_init(&hash);
    std::array<uint8_t, kUserSectorSize> sector{};
    uint32_t remaining = static_cast<uint32_t>(entry.size);
    for (uint32_t i = 0; remaining != 0; ++i) {
        if (!disc.ReadSector(entry.lba + i, sector.data())) return false;
        const size_t amount = std::min<size_t>(sector.size(), remaining);
        psx_sha256_update(&hash, sector.data(), amount);
        remaining -= static_cast<uint32_t>(amount);
    }
    uint8_t digest[32];
    psx_sha256_final(&hash, digest);
    static constexpr char hex[] = "0123456789abcdef";
    result.assign(64, '0');
    for (size_t i = 0; i < 32; ++i) {
        result[i * 2] = hex[digest[i] >> 4];
        result[i * 2 + 1] = hex[digest[i] & 15];
    }
    return true;
}

bool read_file(PS1::ISOReader& disc, const TableEntry& entry,
               std::vector<uint8_t>& bytes) {
    bytes.resize(static_cast<uint32_t>(entry.size));
    std::array<uint8_t, kUserSectorSize> sector{};
    size_t copied = 0;
    for (uint32_t i = 0; copied < bytes.size(); ++i) {
        if (!disc.ReadSector(entry.lba + i, sector.data())) return false;
        const size_t amount = std::min(sector.size(), bytes.size() - copied);
        std::memcpy(bytes.data() + copied, sector.data(), amount);
        copied += amount;
    }
    return true;
}

bool parse_table(const std::vector<uint8_t>& serialized,
                 uint32_t base_sector_count,
                 std::vector<TableEntry>& entries,
                 size_t& serialized_size,
                 std::string* error) {
    for (size_t offset = 0; offset + kEntrySize <= serialized.size();
         offset += kEntrySize) {
        const uint8_t* p = serialized.data() + offset;
        const TableEntry entry{
            read_u24(p), static_cast<int32_t>(read_u32(p + 3))};
        if (entry.lba == kSentinelLba && entry.size == 0) {
            if (entries.empty() || entries.front().size >= 0)
                return fail(error, "Xenogears table has no negative XA root entry");
            const int64_t xa_children = -int64_t(entries.front().size);
            if (xa_children > static_cast<int64_t>(entries.size() - 1))
                return fail(error, "Xenogears XA root exceeds the file table");
            serialized_size = offset + kEntrySize;
            return true;
        }
        if (entry.lba == kSentinelLba)
            return fail(error, "Xenogears table contains a malformed entry");
        if (entry.size > 0) {
            const uint64_t end = uint64_t(entry.lba) +
                                 sector_count_for(static_cast<uint32_t>(entry.size));
            if (entry.lba >= base_sector_count || end > base_sector_count)
                return fail(error, "Xenogears file entry is outside the stock disc");
        }
        entries.push_back(entry);
    }
    return fail(error, "Xenogears table sentinel was not found");
}

bool locate_embedded_table(PS1::ISOReader& disc,
                           const std::vector<TableEntry>& entries,
                           size_t first_ordinary,
                           const std::vector<uint8_t>& serialized,
                           size_t serialized_size,
                           EmbeddedTable& found,
                           std::string* error) {
    std::vector<uint8_t> needle(4 + serialized_size, 0xFF);
    std::memcpy(needle.data() + 4, serialized.data(), serialized_size);

    uint32_t ordinary_seen = 0;
    uint32_t matches = 0;
    for (size_t index = first_ordinary; index < entries.size(); ++index) {
        const TableEntry& entry = entries[index];
        if (entry.size <= 0) continue;
        if (ordinary_seen++ >= kMaxEmbeddedFiles) break;
        const uint32_t size = static_cast<uint32_t>(entry.size);
        if (size > kMaxScannedFileBytes || size < needle.size()) continue;

        std::vector<uint8_t> bytes;
        if (!read_file(disc, entry, bytes))
            return fail(error, "cannot read an early Xenogears indexed file");
        for (auto it = bytes.cbegin();;) {
            it = std::search(it, bytes.cend(), needle.cbegin(), needle.cend());
            if (it == bytes.cend()) break;
            const size_t table_offset = static_cast<size_t>(it - bytes.cbegin()) + 4;
            found.entry_index = index;
            found.lba = entry.lba + static_cast<uint32_t>(table_offset / kUserSectorSize);
            found.offset = table_offset % kUserSectorSize;
            ++matches;
            ++it;
        }
    }
    if (matches != 1)
        return fail(error, "expected exactly one embedded Xenogears table, found " +
                           std::to_string(matches));
    return true;
}

bool locate_replacement_table(const std::vector<uint8_t>& payload,
                              size_t entry_count, size_t table_size,
                              size_t& found_offset) {
    if (entry_count == 0 || table_size != entry_count * kEntrySize ||
        payload.size() < table_size + 4)
        return false;
    size_t matches = 0;
    for (size_t offset = 4; offset + table_size <= payload.size(); ++offset) {
        if (!std::all_of(payload.begin() + offset - 4,
                         payload.begin() + offset,
                         [](uint8_t value) { return value == 0xFF; }))
            continue;
        const int32_t root_size = static_cast<int32_t>(
            read_u32(payload.data() + offset + 3));
        if (root_size >= 0 || -int64_t(root_size) > int64_t(entry_count - 1))
            continue;
        bool valid = true;
        for (size_t i = 0; i + 1 < entry_count; ++i) {
            if (read_u24(payload.data() + offset + i * kEntrySize) ==
                kSentinelLba) {
                valid = false;
                break;
            }
        }
        const uint8_t* sentinel =
            payload.data() + offset + (entry_count - 1) * kEntrySize;
        if (!valid || read_u24(sentinel) != kSentinelLba ||
            read_u32(sentinel + 3) != 0)
            continue;
        found_offset = offset;
        ++matches;
    }
    return matches == 1;
}

uint8_t to_bcd(uint32_t value) {
    return static_cast<uint8_t>((value / 10u << 4) | (value % 10u));
}

struct CdCodeTables {
    std::array<uint32_t, 256> edc{};
    std::array<uint8_t, 256> forward{};
    std::array<uint8_t, 256> backward{};

    CdCodeTables() {
        for (uint32_t i = 0; i < 256; ++i) {
            uint32_t value = i;
            for (unsigned bit = 0; bit < 8; ++bit)
                value = (value >> 1) ^
                    ((value & 1) ? 0xD8018001u : 0u);
            edc[i] = value;
            const uint32_t doubled =
                (i << 1) ^ ((i & 0x80) ? 0x11Du : 0u);
            forward[i] = static_cast<uint8_t>(doubled);
            backward[i ^ doubled] = static_cast<uint8_t>(i);
        }
    }
};

const CdCodeTables& cd_code_tables() {
    static const CdCodeTables tables;
    return tables;
}

uint32_t edc(const uint8_t* bytes, size_t size) {
    const auto& table = cd_code_tables().edc;
    uint32_t value = 0;
    for (size_t i = 0; i < size; ++i)
        value = (value >> 8) ^ table[(value ^ bytes[i]) & 0xFFu];
    return value;
}

void write_u32(uint8_t* p, uint32_t value) {
    p[0] = static_cast<uint8_t>(value);
    p[1] = static_cast<uint8_t>(value >> 8);
    p[2] = static_cast<uint8_t>(value >> 16);
    p[3] = static_cast<uint8_t>(value >> 24);
}

void ecc_block(const uint8_t* source, uint32_t major_count,
               uint32_t minor_count, uint32_t major_mult,
               uint32_t minor_inc, uint8_t* destination) {
    const auto& forward = cd_code_tables().forward;
    const auto& backward = cd_code_tables().backward;

    // Reed-Solomon P/Q parity over GF(2^8), primitive polynomial 0x11D.
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

void regenerate_mode2_form1(std::array<uint8_t, kRawSectorSize>& sector) {
    write_u32(sector.data() + 0x818, edc(sector.data() + 0x10, 0x808));

    const std::array<uint8_t, 4> address = {
        sector[12], sector[13], sector[14], sector[15]};
    std::fill(sector.begin() + 12, sector.begin() + 16, 0);
    ecc_block(sector.data() + 12, 86, 24, 2, 86, sector.data() + 0x81C);
    ecc_block(sector.data() + 12, 52, 43, 86, 88, sector.data() + 0x8C8);
    std::copy(address.begin(), address.end(), sector.begin() + 12);
}

std::array<uint8_t, kRawSectorSize> make_sector(
    uint32_t lba, const uint8_t* payload, size_t size, bool final_sector) {
    std::array<uint8_t, kRawSectorSize> sector{};
    sector[0] = 0;
    std::fill(sector.begin() + 1, sector.begin() + 11, 0xFF);
    sector[11] = 0;
    const uint32_t frame = lba + 150;
    sector[12] = to_bcd(frame / (75 * 60));
    sector[13] = to_bcd((frame / 75) % 60);
    sector[14] = to_bcd(frame % 75);
    sector[15] = 2;
    sector[18] = sector[22] = final_sector ? 0x89 : 0x08;
    if (size != 0) std::memcpy(sector.data() + kUserOffset, payload, size);
    regenerate_mode2_form1(sector);
    return sector;
}

bool patch_serialized_table(PS1::ISOReader& disc, uint32_t start_lba,
                            size_t start_offset,
                            const std::vector<uint8_t>& table,
                            size_t table_size,
                            PSXRecompV4::ModVirtualDisc& output,
                            std::string* error) {
    size_t copied = 0;
    while (copied < table_size) {
        const uint32_t lba = start_lba +
            static_cast<uint32_t>((start_offset + copied) / kUserSectorSize);
        const size_t offset = (start_offset + copied) % kUserSectorSize;
        auto [it, inserted] = output.raw_sectors.try_emplace(lba);
        if (inserted && !disc.ReadRawSector(lba, it->second.data()))
            return fail(error, "cannot read a raw Xenogears table sector");
        if (it->second[15] != 2 ||
            std::memcmp(it->second.data() + 16, it->second.data() + 20, 4) != 0 ||
            (it->second[18] & 0x20) != 0)
            return fail(error, "Xenogears table sector is not Mode 2 Form 1");
        const size_t amount = std::min(kUserSectorSize - offset, table_size - copied);
        std::memcpy(it->second.data() + kUserOffset + offset,
                    table.data() + copied, amount);
        copied += amount;
    }
    return true;
}

} // namespace

bool build_indexed_disc(
    PS1::ISOReader& disc,
    const std::vector<PSXRecompV4::ModResolution::IndexedFile>& files,
    uint32_t base_sector_count,
    PSXRecompV4::ModVirtualDisc& output,
    std::string* error) try {
    output = {};
    if (files.empty()) return fail(error, "Xenogears indexed-file plan is empty");

    if (disc.GetSectorCount() != base_sector_count)
        return fail(error, "Xenogears base sector count changed during resolution");
    if (base_sector_count < kPrimaryTableLba + kPrimaryTableSectors)
        return fail(error, "Xenogears disc is too small for its primary table");

    std::vector<uint8_t> table(kPrimaryTableSectors * kUserSectorSize);
    for (uint32_t i = 0; i < kPrimaryTableSectors; ++i) {
        if (!disc.ReadSector(kPrimaryTableLba + i,
                             table.data() + i * kUserSectorSize))
            return fail(error, "cannot read the primary Xenogears table");
    }

    std::vector<TableEntry> entries;
    size_t table_size = 0;
    if (!parse_table(table, base_sector_count, entries, table_size, error)) return false;

    EmbeddedTable embedded;
    const size_t first_ordinary =
        static_cast<size_t>(-int64_t(entries.front().size)) + 1;
    if (!locate_embedded_table(disc, entries, first_ordinary, table, table_size,
                               embedded, error))
        return false;
    const std::vector<PS1::ISOFileEntry> iso_files =
        disc.ListFilesRecursive();

    std::set<uint32_t> claimed;
    uint64_t next_lba = base_sector_count;
    const int64_t xa_children = -int64_t(entries.front().size);
    PSXRecompV4::ModVirtualDisc built;
    std::vector<ReplacementAllocation> allocations;
    allocations.reserve(files.size());
    for (const auto& file : files) {
        if (file.format != kIndexedDiscFormat)
            return fail(error, "unexpected indexed-file format in Xenogears handler");
        if (!claimed.insert(file.index).second)
            return fail(error, "duplicate Xenogears indexed-file index " +
                               std::to_string(file.index));
        if (file.index >= entries.size())
            return fail(error, "Xenogears indexed-file index is beyond the sentinel");
        if (file.index <= static_cast<uint64_t>(xa_children) ||
            entries[file.index].size <= 0)
            return fail(error, "Xenogears indexed-file index is not an ordinary file");
        const bool iso_visible = std::any_of(
            iso_files.begin(), iso_files.end(),
            [&](const PS1::ISOFileEntry& item) {
                if (item.is_directory) return false;
                const uint64_t indexed_begin = entries[file.index].lba;
                const uint64_t indexed_end = indexed_begin + sector_count_for(
                    static_cast<uint32_t>(entries[file.index].size));
                const uint64_t iso_begin = item.lba;
                const uint64_t iso_end = iso_begin + sector_count_for(item.size);
                return indexed_begin < iso_end && iso_begin < indexed_end;
            });
        if (iso_visible)
            return fail(error,
                        "Xenogears indexed-file index is also ISO9660-visible");
        if (file.payload.empty() || file.payload.size() > std::numeric_limits<int32_t>::max())
            return fail(error, "Xenogears replacement payload size is invalid");

        std::string stock_hash;
        if (!hash_file(disc, entries[file.index], stock_hash))
            return fail(error, "cannot read stock Xenogears indexed file");
        if (stock_hash != file.expected_sha256)
            return fail(error, file.package_id + "/" + file.feature_id +
                               ": stock Xenogears indexed-file checksum failed");

        const uint32_t payload_size = static_cast<uint32_t>(file.payload.size());
        const uint32_t sectors = sector_count_for(payload_size);
        if (uint64_t(sectors) > kMaxVirtualSectors ||
            next_lba + sectors > kSentinelLba ||
            next_lba + sectors > kMaxMsfLba ||
            next_lba + sectors > uint64_t(base_sector_count) + kMaxVirtualSectors)
            return fail(error, "Xenogears virtual sector count is excessive");
        write_entry(table.data() + file.index * kEntrySize,
                     static_cast<uint32_t>(next_lba), payload_size);
        allocations.push_back({&file, static_cast<uint32_t>(next_lba), sectors});
        next_lba += sectors;
    }

    built.appended_start_lba = base_sector_count;
    built.appended_raw_sectors.reserve(
        static_cast<size_t>(next_lba - base_sector_count));
    for (const ReplacementAllocation& allocation : allocations) {
        const auto& file = *allocation.file;
        size_t replacement_table_offset = 0;
        if (file.index == embedded.entry_index) {
            if (!locate_replacement_table(file.payload,
                                          table_size / kEntrySize,
                                          table_size,
                                          replacement_table_offset))
                return fail(error,
                            "replacement containing the embedded Xenogears table "
                            "does not contain one unambiguous table copy");
        }
        for (uint32_t i = 0; i < allocation.sectors; ++i) {
            const size_t offset = size_t(i) * kUserSectorSize;
            const size_t amount = std::min(kUserSectorSize,
                                            file.payload.size() - offset);
            const uint8_t* sector_payload = file.payload.data() + offset;
            std::array<uint8_t, kUserSectorSize> patched_user{};
            if (file.index == embedded.entry_index &&
                offset < replacement_table_offset + table_size &&
                replacement_table_offset < offset + amount) {
                std::memcpy(patched_user.data(), sector_payload, amount);
                const size_t patch_start =
                    std::max(offset, replacement_table_offset);
                const size_t patch_end = std::min(
                    offset + amount, replacement_table_offset + table_size);
                std::memcpy(
                    patched_user.data() + patch_start - offset,
                    table.data() + patch_start - replacement_table_offset,
                    patch_end - patch_start);
                sector_payload = patched_user.data();
            }
            built.appended_raw_sectors.push_back(
                make_sector(allocation.lba + i, sector_payload, amount,
                            i + 1 == allocation.sectors));
        }
    }

    if (!patch_serialized_table(disc, kPrimaryTableLba, 0, table, table_size,
                                built, error) ||
        !patch_serialized_table(disc, embedded.lba, embedded.offset, table,
                                table_size, built, error))
        return false;
    for (auto& [lba, sector] : built.raw_sectors) {
        if (lba < base_sector_count) regenerate_mode2_form1(sector);
    }
    built.sector_count = static_cast<uint32_t>(next_lba);
    output = std::move(built);
    if (error) error->clear();
    return true;
} catch (const std::exception& ex) {
    output = {};
    return fail(error, std::string("cannot build Xenogears virtual disc: ") +
                       ex.what());
}

bool build_indexed_disc(
    const std::filesystem::path& disc_path,
    const std::vector<PSXRecompV4::ModResolution::IndexedFile>& files,
    uint32_t base_sector_count,
    PSXRecompV4::ModVirtualDisc& output,
    std::string* error) try {
    const auto resolved = PSXRecompV4::resolve_disc_path(disc_path);
    PS1::ISOReader disc;
    if (resolved.mount.empty() || !disc.Open(resolved.mount.string()))
        return fail(error, "cannot open the selected Xenogears disc");
    return build_indexed_disc(disc, files, base_sector_count, output, error);
} catch (const std::exception& ex) {
    output = {};
    return fail(error, std::string("cannot open Xenogears virtual disc: ") +
                       ex.what());
}

#if !defined(XG_INDEXED_DISC_NO_REGISTRATION)
[[maybe_unused]] const bool registered =
    PSXRecompV4::mod_runtime_register_indexed_file_handler(
        kIndexedDiscFormat,
        static_cast<PSXRecompV4::ModIndexedFileHandler>(&build_indexed_disc));
#endif

} // namespace XenogearsRecomp
