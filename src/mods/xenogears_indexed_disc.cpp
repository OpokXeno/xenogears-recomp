#include "xenogears_indexed_disc.h"

#include "disc_path.h"
#include "iso_reader.h"
#include "psx_sha256.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <exception>
#include <limits>
#include <map>
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
constexpr uint32_t kMaxVirtualSectors = 256u * 1024u;
constexpr char kPwbFmvCompose[] = "xenogears-pwb-fmv-0.11.2";
constexpr std::array<uint8_t, 8> kPwbFmvMagic = {
    'X', 'G', 'F', 'M', 'V', '1', '1', '2'};

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

struct FmvRawSector {
    uint32_t lba = 0;
    std::array<uint8_t, 32> expected_sha256{};
    std::array<uint8_t, kRawSectorSize> replacement{};
};

struct FmvSubtitle {
    uint32_t index = 0;
    std::vector<uint8_t> payload;
};

struct FmvBundle {
    uint32_t disc = 0;
    std::vector<FmvRawSector> raw_sectors;
    std::vector<FmvSubtitle> subtitles;
    std::vector<uint8_t> executable;
};

struct FmvAllocation {
    const FmvSubtitle* subtitle = nullptr;
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

void write_u32(uint8_t* p, uint32_t value);

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

std::string hash_bytes(const std::vector<uint8_t>& bytes) {
    psx_sha256_ctx hash;
    psx_sha256_init(&hash);
    psx_sha256_update(&hash, bytes.data(), bytes.size());
    uint8_t digest[32];
    psx_sha256_final(&hash, digest);
    static constexpr char hex[] = "0123456789abcdef";
    std::string result(64, '0');
    for (size_t i = 0; i < 32; ++i) {
        result[i * 2] = hex[digest[i] >> 4];
        result[i * 2 + 1] = hex[digest[i] & 15];
    }
    return result;
}

std::array<uint8_t, 32> hash_memory(const uint8_t* bytes, size_t size) {
    std::array<uint8_t, 32> digest{};
    psx_sha256_compute(bytes, size, digest.data());
    return digest;
}

bool parse_fmv_bundle(const std::vector<uint8_t>& payload,
                      FmvBundle& bundle, std::string* error) {
    constexpr uint32_t kMaxRawSectors = 20000;
    constexpr uint32_t kMaxSubtitles = 16;
    constexpr uint32_t kMaxExecutableSize = 1024 * 1024;
    size_t at = 0;
    auto take_u32 = [&](uint32_t& value) {
        if (at + 4 > payload.size()) return false;
        value = read_u32(payload.data() + at);
        at += 4;
        return true;
    };
    if (payload.size() < kPwbFmvMagic.size() ||
        !std::equal(kPwbFmvMagic.begin(), kPwbFmvMagic.end(), payload.begin()))
        return fail(error, "Perfect Works FMV bundle has an invalid signature");
    at = kPwbFmvMagic.size();
    uint32_t version = 0;
    uint32_t raw_count = 0;
    uint32_t subtitle_count = 0;
    uint32_t executable_size = 0;
    if (!take_u32(version) || !take_u32(bundle.disc) ||
        !take_u32(raw_count) || !take_u32(subtitle_count) ||
        !take_u32(executable_size) || version != 1 ||
        (bundle.disc != 1 && bundle.disc != 2) ||
        raw_count == 0 || raw_count > kMaxRawSectors ||
        subtitle_count == 0 || subtitle_count > kMaxSubtitles ||
        executable_size == 0 || executable_size > kMaxExecutableSize)
        return fail(error, "Perfect Works FMV bundle header is invalid");

    bundle.raw_sectors.clear();
    bundle.raw_sectors.reserve(raw_count);
    std::set<uint32_t> raw_lbas;
    for (uint32_t i = 0; i < raw_count; ++i) {
        FmvRawSector sector;
        if (!take_u32(sector.lba) ||
            at + sector.expected_sha256.size() + sector.replacement.size() >
                payload.size() ||
            !raw_lbas.insert(sector.lba).second)
            return fail(error, "Perfect Works FMV raw-sector list is invalid");
        std::memcpy(sector.expected_sha256.data(), payload.data() + at,
                    sector.expected_sha256.size());
        at += sector.expected_sha256.size();
        std::memcpy(sector.replacement.data(), payload.data() + at,
                    sector.replacement.size());
        at += sector.replacement.size();
        bundle.raw_sectors.push_back(std::move(sector));
    }

    bundle.subtitles.clear();
    bundle.subtitles.reserve(subtitle_count);
    std::set<uint32_t> subtitle_indices;
    for (uint32_t i = 0; i < subtitle_count; ++i) {
        FmvSubtitle subtitle;
        uint32_t size = 0;
        if (!take_u32(subtitle.index) || !take_u32(size) || size == 0 ||
            size > kMaxScannedFileBytes || at + size > payload.size() ||
            !subtitle_indices.insert(subtitle.index).second)
            return fail(error, "Perfect Works FMV subtitle list is invalid");
        subtitle.payload.assign(payload.begin() + at, payload.begin() + at + size);
        at += size;
        bundle.subtitles.push_back(std::move(subtitle));
    }
    if (at + executable_size != payload.size())
        return fail(error, "Perfect Works FMV bundle length is invalid");
    bundle.executable.assign(payload.begin() + at, payload.end());
    return true;
}

bool split_fmv_claim(
    const std::vector<PSXRecompV4::ModResolution::IndexedFile>& files,
    std::vector<PSXRecompV4::ModResolution::IndexedFile>& regular,
    const PSXRecompV4::ModResolution::IndexedFile*& claim,
    FmvBundle& bundle, std::string* error) {
    regular.clear();
    claim = nullptr;
    for (const auto& file : files) {
        if (file.compose != kPwbFmvCompose) {
            regular.push_back(file);
            continue;
        }
        if (claim || file.feature_id != "perfect-works")
            return fail(error, "Perfect Works FMV claim identity is invalid");
        claim = &file;
    }
    return !claim || parse_fmv_bundle(claim->payload, bundle, error);
}

bool merge_three_way(const std::vector<uint8_t>& stock,
                     std::vector<uint8_t>& current,
                     const std::vector<uint8_t>& incoming,
                     const std::string& owner,
                     uint32_t index,
                     std::string* error) {
    if (current.size() != stock.size() || incoming.size() != stock.size())
        return fail(error,
                    "Xenogears three-way composition requires equal-size payloads at index " +
                        std::to_string(index));
    for (size_t offset = 0; offset < stock.size(); ++offset) {
        if (incoming[offset] == stock[offset]) continue;
        if (current[offset] != stock[offset] &&
            current[offset] != incoming[offset])
            return fail(error, owner +
                " overlaps another change while composing Xenogears index " +
                std::to_string(index) + " at byte " +
                std::to_string(offset));
        current[offset] = incoming[offset];
    }
    return true;
}

bool lzss_decompress(const std::vector<uint8_t>& input,
                     std::vector<uint8_t>& output) {
    if (input.size() < 4) return false;
    const uint32_t size = read_u32(input.data());
    if (size == 0 || size > kMaxScannedFileBytes) return false;
    output.assign(size, 0);
    std::array<uint8_t, 4096> window{};
    size_t window_at = 4096 - 18;
    size_t input_at = 4;
    size_t output_at = 0;
    while (output_at < size) {
        // The original decompressor leaves a zero-filled tail when a stream
        // ends between complete tokens. PWB 0.11.2 contains such files.
        if (input_at >= input.size()) return true;
        const uint8_t control = input[input_at++];
        for (unsigned bit = 0; bit < 8 && output_at < size; ++bit) {
            if (input_at >= input.size()) return true;
            if ((control & (1u << bit)) == 0) {
                const uint8_t value = input[input_at++];
                output[output_at++] = value;
                window[window_at] = value;
                window_at = (window_at + 1) & 4095;
                continue;
            }
            if (input_at + 1 >= input.size()) return false;
            const uint8_t low = input[input_at++];
            const uint8_t high = input[input_at++];
            const size_t distance = size_t(low) | (size_t(high & 0x0F) << 8);
            const size_t length = size_t(high >> 4) + 3;
            for (size_t i = 0; i < length && output_at < size; ++i) {
                const uint8_t value = window[(window_at - distance) & 4095];
                output[output_at++] = value;
                window[window_at] = value;
                window_at = (window_at + 1) & 4095;
            }
        }
    }
    return true;
}

std::vector<uint8_t> lzss_store_literals(const std::vector<uint8_t>& input) {
    // Xenogears checks the output size only between eight-token groups. Pad
    // the final literal group so the guest decoder reaches the declared end.
    const size_t stored_size = (input.size() + 7) & ~size_t(7);
    std::vector<uint8_t> output;
    output.reserve(4 + stored_size + stored_size / 8);
    output.resize(4);
    write_u32(output.data(), static_cast<uint32_t>(stored_size));
    for (size_t offset = 0; offset < stored_size; offset += 8) {
        output.push_back(0);
        const size_t amount = std::min<size_t>(8, input.size() - offset);
        output.insert(output.end(), input.begin() + offset,
                      input.begin() + offset + amount);
        output.insert(output.end(), 8 - amount, 0);
    }
    return output;
}

bool packet_unpack(const std::vector<uint8_t>& input,
                   std::vector<std::vector<uint8_t>>& files) {
    if (input.size() < 8) return false;
    const uint32_t count = read_u32(input.data());
    if (count == 0 || count > 4096 ||
        4ull + uint64_t(count + 1) * 4 > input.size())
        return false;
    files.clear();
    files.reserve(count);
    uint32_t previous = read_u32(input.data() + 4);
    if (previous < 4 + (count + 1) * 4 || previous > input.size() ||
        read_u32(input.data() + 4 + count * 4) != input.size())
        return false;
    for (uint32_t index = 0; index < count; ++index) {
        const uint32_t end = index + 1 == count
            ? static_cast<uint32_t>(input.size())
            : read_u32(input.data() + 4 + (index + 1) * 4);
        if (end < previous || end > input.size()) return false;
        files.emplace_back(input.begin() + previous, input.begin() + end);
        previous = end;
    }
    return true;
}

bool packet_pack(const std::vector<std::vector<uint8_t>>& files,
                 std::vector<uint8_t>& output) {
    if (files.empty() || files.size() > 4096) return false;
    uint64_t total = 4 + (files.size() + 1) * 4;
    for (const auto& file : files) total += file.size();
    if (total > std::numeric_limits<uint32_t>::max()) return false;
    output.assign(static_cast<size_t>(4 + (files.size() + 1) * 4), 0);
    write_u32(output.data(), static_cast<uint32_t>(files.size()));
    uint32_t offset = static_cast<uint32_t>(output.size());
    for (size_t index = 0; index < files.size(); ++index) {
        write_u32(output.data() + 4 + index * 4, offset);
        output.insert(output.end(), files[index].begin(), files[index].end());
        offset += static_cast<uint32_t>(files[index].size());
    }
    write_u32(output.data() + 4 + files.size() * 4, offset);
    return true;
}

bool is_pwb_package(
    const PSXRecompV4::ModResolution::IndexedFile& file,
    const char* key) {
    return file.package_id ==
        std::string("org.perfectworksbuild.individual.") + key;
}

bool represents_pwb_package(
    const PSXRecompV4::ModResolution::IndexedFile& file,
    const char* key) {
    const std::string package_id =
        std::string("org.perfectworksbuild.individual.") + key;
    return file.package_id == package_id ||
        std::find(file.supersedes.begin(), file.supersedes.end(), package_id) !=
            file.supersedes.end();
}

int pwb_copy_priority(
    const PSXRecompV4::ModResolution::IndexedFile& file) {
    static constexpr std::array<const char*, 14> order = {
        "rebalanced-items", "retranslation", "jpn-controls",
        "half-encounters", "story-mode", "bug-fixes", "portraits",
        "rebalanced-enemies", "arena", "text-speed", "battle-undub",
        "title-screen", "pw-roni", "emeralda-cafe-fix",
    };
    for (size_t index = 0; index < order.size(); ++index)
        if (is_pwb_package(file, order[index]))
            return static_cast<int>(index);
    return -1;
}

bool apply_packet_delta(const std::vector<uint8_t>& stock,
                        std::vector<uint8_t>& current,
                        const std::vector<uint8_t>& incoming,
                        uint32_t index,
                        std::string* error) {
    std::vector<std::vector<uint8_t>> stock_files;
    std::vector<std::vector<uint8_t>> current_files;
    std::vector<std::vector<uint8_t>> incoming_files;
    if (!packet_unpack(stock, stock_files) ||
        !packet_unpack(current, current_files) ||
        !packet_unpack(incoming, incoming_files) ||
        stock_files.size() != current_files.size() ||
        stock_files.size() != incoming_files.size())
        return fail(error,
                    "Perfect Works packet structure disagrees at Xenogears index " +
                        std::to_string(index));
    for (size_t file = 0; file < stock_files.size(); ++file) {
        if (incoming_files[file] == stock_files[file]) continue;
        if (incoming_files[file].size() != stock_files[file].size() ||
            current_files[file].size() != stock_files[file].size()) {
            current_files[file] = incoming_files[file];
            continue;
        }
        for (size_t offset = 0; offset < stock_files[file].size(); ++offset)
            if (incoming_files[file][offset] != stock_files[file][offset])
                current_files[file][offset] = incoming_files[file][offset];
    }
    if (!packet_pack(current_files, current))
        return fail(error, "cannot rebuild a Perfect Works packet archive");
    return true;
}

bool apply_music_delta(const std::vector<uint8_t>& incoming,
                       std::vector<uint8_t>& current,
                       uint32_t index,
                       std::string* error) {
    if (incoming.size() < 332 || current.size() < 332)
        return fail(error, "Perfect Works music target is truncated");
    const uint32_t source_begin = read_u32(incoming.data() + 324);
    const uint32_t source_end = read_u32(incoming.data() + 328);
    const uint32_t target_begin = read_u32(current.data() + 324);
    const uint32_t target_end = read_u32(current.data() + 328);
    if (source_begin > source_end || source_end > incoming.size() ||
        target_begin > target_end || target_end > current.size())
        return fail(error,
                    "Perfect Works music region is invalid at Xenogears index " +
                        std::to_string(index));
    const size_t target_size = target_end - target_begin;
    const size_t source_size = source_end - source_begin;
    const size_t amount = std::min(target_size, source_size);
    std::copy_n(incoming.begin() + source_begin, amount,
                current.begin() + target_begin);
    std::fill(current.begin() + target_begin + amount,
              current.begin() + target_end, 0);
    return true;
}

uint32_t scale_value(uint32_t value, const std::string& multiplier) {
    if (multiplier == "1-5x") return value * 3 / 2;
    if (multiplier == "2x") return value * 2;
    return value;
}

bool apply_reward_scale(std::vector<uint8_t>& bytes,
                        const std::string& exp_multiplier,
                        const std::string& gold_multiplier,
                        uint32_t index,
                        std::string* error) {
    if (bytes.size() < 2) return fail(error, "Perfect Works monster file is truncated");
    const uint32_t data_size = uint32_t(bytes[0]) | (uint32_t(bytes[1]) << 8);
    for (uint32_t record = 126; record < data_size; record += 368) {
        if (record + 0x10C > bytes.size())
            return fail(error,
                        "Perfect Works monster record is truncated at Xenogears index " +
                            std::to_string(index));
        if (!exp_multiplier.empty()) {
            const size_t offset = record + 0x100;
            const uint32_t value = uint32_t(bytes[offset]) |
                                   (uint32_t(bytes[offset + 1]) << 8);
            write_u32(bytes.data() + offset,
                      scale_value(value, exp_multiplier));
        }
        if (!gold_multiplier.empty() && gold_multiplier != "1x") {
            const size_t offset = record + 0x10A;
            const uint32_t value = uint32_t(bytes[offset]) |
                                   (uint32_t(bytes[offset + 1]) << 8);
            const uint32_t scaled = scale_value(value, gold_multiplier);
            bytes[offset] = static_cast<uint8_t>(scaled);
            bytes[offset + 1] = static_cast<uint8_t>(scaled >> 8);
        }
    }
    return true;
}

bool compose_pwb_group(
    uint32_t index,
    const std::vector<const PSXRecompV4::ModResolution::IndexedFile*>& claims,
    const std::vector<uint8_t>& stock,
    std::vector<uint8_t>& composed,
    std::string* error) {
    std::vector<const PSXRecompV4::ModResolution::IndexedFile*> base;
    std::vector<const PSXRecompV4::ModResolution::IndexedFile*> battle_edits;
    std::vector<const PSXRecompV4::ModResolution::IndexedFile*> packet_edits;
    std::vector<const PSXRecompV4::ModResolution::IndexedFile*> music_edits;
    std::string exp_multiplier;
    std::string gold_multiplier;
    bool insert_music = false;

    std::vector<uint8_t> stock_lzss;
    const bool stock_is_lzss = lzss_decompress(stock, stock_lzss);
    for (const auto* claim : claims) {
        if (is_pwb_package(*claim, "exp")) {
            const auto value = claim->options.find("multiplier");
            if (value != claim->options.end()) exp_multiplier = value->second;
        } else if (is_pwb_package(*claim, "gold")) {
            const auto value = claim->options.find("multiplier");
            if (value != claim->options.end()) gold_multiplier = value->second;
        } else if (is_pwb_package(*claim, "music-changes")) {
            music_edits.push_back(claim);
        } else if (is_pwb_package(*claim, "no-battle-flashes") ||
                   is_pwb_package(*claim, "no-damage-cap")) {
            battle_edits.push_back(claim);
        } else if (is_pwb_package(*claim, "no-deathblow-levels")) {
            packet_edits.push_back(claim);
        } else if (is_pwb_package(*claim, "jpn-controls")) {
            if (index == 2593 || index == 2588 || index == 3958 ||
                index == 3953 || index == 2614 || index == 2609) {
                std::vector<std::vector<uint8_t>> stock_files;
                std::vector<std::vector<uint8_t>> incoming_files;
                if (packet_unpack(stock, stock_files) &&
                    packet_unpack(claim->payload, incoming_files))
                    packet_edits.push_back(claim);
                else
                    base.push_back(claim);
            } else if (index == 38 || index == 33) {
                std::vector<uint8_t> incoming;
                if (stock_is_lzss &&
                    lzss_decompress(claim->payload, incoming))
                    battle_edits.push_back(claim);
                else
                    base.push_back(claim);
            } else {
                base.push_back(claim);
            }
        } else {
            base.push_back(claim);
        }
    }

    composed = stock;
    if (!base.empty()) {
        const auto selected = std::max_element(
            base.begin(), base.end(), [](const auto* lhs, const auto* rhs) {
                return pwb_copy_priority(*lhs) < pwb_copy_priority(*rhs);
            });
        const int priority = pwb_copy_priority(**selected);
        if (priority < 0)
            return fail(error,
                        "unknown Perfect Works base claim at Xenogears index " +
                            std::to_string(index));
        const auto ambiguous = std::find_if(
            base.begin(), base.end(), [&](const auto* claim) {
                return claim != *selected &&
                    pwb_copy_priority(*claim) == priority &&
                    claim->payload != (*selected)->payload;
            });
        if (ambiguous != base.end())
            return fail(error,
                        "ambiguous Perfect Works copy priority at Xenogears index " +
                            std::to_string(index));
        composed = (*selected)->payload;
        insert_music = composed != stock &&
            (represents_pwb_package(**selected, "retranslation") ||
             represents_pwb_package(**selected, "text-speed") ||
             represents_pwb_package(**selected, "arena"));
    }

    if (!battle_edits.empty()) {
        std::vector<uint8_t> current_lzss;
        if (!stock_is_lzss || !lzss_decompress(composed, current_lzss))
            return fail(error,
                        "Perfect Works LZSS structure disagrees at Xenogears index " +
                            std::to_string(index));
        std::vector<uint8_t> delta = stock_lzss;
        for (const auto* claim : battle_edits) {
            std::vector<uint8_t> incoming;
            if (!lzss_decompress(claim->payload, incoming))
                return fail(error,
                            "Perfect Works battle edit is not valid LZSS at Xenogears index " +
                                std::to_string(index));
            const size_t common = std::min(stock_lzss.size(), incoming.size());
            for (size_t offset = 0; offset < common; ++offset) {
                if (incoming[offset] == stock_lzss[offset]) continue;
                if (delta[offset] != stock_lzss[offset] &&
                    delta[offset] != incoming[offset])
                    return fail(
                        error, claim->package_id + "/" + claim->feature_id +
                            " overlaps another Perfect Works battle edit at Xenogears index " +
                            std::to_string(index) + " byte " +
                            std::to_string(offset));
                delta[offset] = incoming[offset];
            }
        }
        for (size_t offset = 0; offset < stock_lzss.size(); ++offset)
            if (delta[offset] != stock_lzss[offset]) {
                if (offset >= current_lzss.size())
                    return fail(error,
                                "Perfect Works battle edit is outside the target at Xenogears index " +
                                    std::to_string(index));
                current_lzss[offset] = delta[offset];
            }
        composed = lzss_store_literals(current_lzss);
    }

    for (const auto* claim : packet_edits)
        if (!apply_packet_delta(
                stock, composed, claim->payload, index, error))
            return false;

    for (const auto* claim : music_edits) {
        if (insert_music) {
            if (!apply_music_delta(claim->payload, composed, index, error))
                return false;
        } else {
            composed = claim->payload;
        }
    }

    if ((!exp_multiplier.empty() || !gold_multiplier.empty()) &&
        !apply_reward_scale(
            composed, exp_multiplier, gold_multiplier, index, error))
        return false;
    return true;
}

bool compose_files(
    PS1::ISOReader& disc,
    const std::vector<TableEntry>& entries,
    const std::vector<PSXRecompV4::ModResolution::IndexedFile>& files,
    std::vector<PSXRecompV4::ModResolution::IndexedFile>& output,
    std::string* error) {
    std::vector<std::pair<
        uint32_t,
        std::vector<const PSXRecompV4::ModResolution::IndexedFile*>>> groups;
    std::map<uint32_t, size_t> group_by_index;
    for (const auto& file : files) {
        const auto [found, inserted] =
            group_by_index.emplace(file.index, groups.size());
        if (inserted) groups.push_back({file.index, {}});
        groups[found->second].second.push_back(&file);
    }
    output.clear();
    output.reserve(groups.size());
    for (const auto& [index, claims] : groups) {
        if (claims.size() == 1) {
            output.push_back(*claims.front());
            continue;
        }
        if (index >= entries.size() || entries[index].size <= 0)
            return fail(error, "cannot compose an invalid Xenogears indexed file");
        const std::string& expected = claims.front()->expected_sha256;
        if (std::any_of(
                claims.begin(), claims.end(),
                [&](const auto* claim) {
                    return claim->compose != claims.front()->compose ||
                           claim->expected_sha256 != expected;
                }))
            return fail(error,
                        "Xenogears indexed-file composition metadata disagrees");
        std::vector<uint8_t> stock;
        if (!read_file(disc, entries[index], stock) ||
            hash_bytes(stock) != expected)
            return fail(error,
                        "stock Xenogears indexed-file checksum failed during composition");
        std::vector<uint8_t> composed = stock;
        if (claims.front()->compose == "three-way") {
            for (const auto* claim : claims)
                if (!merge_three_way(
                        stock, composed, claim->payload,
                        claim->package_id + "/" + claim->feature_id,
                        index, error))
                    return false;
        } else if (claims.front()->compose == "xenogears-pwb-0.11.2") {
            if (!compose_pwb_group(index, claims, stock, composed, error))
                return false;
        } else {
            return fail(error,
                        "unsupported Xenogears indexed-file compositor: " +
                            claims.front()->compose);
        }
        auto resolved = *claims.front();
        resolved.payload = std::move(composed);
        resolved.payload_sha256 = hash_bytes(resolved.payload);
        resolved.package_id = "composed";
        resolved.feature_id = "composed";
        resolved.supersedes.clear();
        output.push_back(std::move(resolved));
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

bool patch_file_in_place(PS1::ISOReader& disc, const TableEntry& entry,
                         const std::vector<uint8_t>& payload,
                         PSXRecompV4::ModVirtualDisc& output,
                         std::string* error) {
    if (entry.size <= 0 || payload.size() != static_cast<uint32_t>(entry.size))
        return fail(error, "Perfect Works FMV executable size disagrees");
    for (uint32_t i = 0; i < sector_count_for(payload.size()); ++i) {
        const uint32_t lba = entry.lba + i;
        auto [it, inserted] = output.raw_sectors.try_emplace(lba);
        if (inserted && !disc.ReadRawSector(lba, it->second.data()))
            return fail(error, "cannot read the stock FMV executable sector");
        if (it->second[15] != 2 ||
            std::memcmp(it->second.data() + 16, it->second.data() + 20, 4) != 0 ||
            (it->second[18] & 0x20) != 0)
            return fail(error, "Perfect Works FMV executable is not Mode 2 Form 1");
        const size_t offset = size_t(i) * kUserSectorSize;
        const size_t amount =
            std::min(kUserSectorSize, payload.size() - offset);
        std::memcpy(it->second.data() + kUserOffset, payload.data() + offset,
                    amount);
        regenerate_mode2_form1(it->second);
    }
    return true;
}

bool install_fmv_raw_sectors(PS1::ISOReader& disc,
                             const FmvBundle& fmv,
                             uint32_t base_sector_count,
                             PSXRecompV4::ModVirtualDisc& output,
                             std::string* error) {
    for (const FmvRawSector& replacement : fmv.raw_sectors) {
        if (replacement.lba >= base_sector_count)
            return fail(error, "Perfect Works FMV sector is outside the stock disc");
        std::array<uint8_t, kRawSectorSize> stock{};
        if (!disc.ReadRawSector(replacement.lba, stock.data()) ||
            hash_memory(stock.data(), stock.size()) != replacement.expected_sha256)
            return fail(error, "Perfect Works FMV stock-sector checksum failed");
        if (!std::equal(stock.begin(), stock.begin() + kUserOffset,
                        replacement.replacement.begin()) ||
            replacement.replacement[15] != 2 ||
            std::memcmp(replacement.replacement.data() + 16,
                        replacement.replacement.data() + 20, 4) != 0 ||
            (replacement.replacement[18] & 0x20) == 0 ||
            !output.raw_sectors.emplace(
                replacement.lba, replacement.replacement).second)
            return fail(error, "Perfect Works FMV sector structure overlaps or disagrees");
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

    std::vector<PSXRecompV4::ModResolution::IndexedFile> regular_claims;
    const PSXRecompV4::ModResolution::IndexedFile* fmv_claim = nullptr;
    FmvBundle fmv;
    if (!split_fmv_claim(
            files, regular_claims, fmv_claim, fmv, error))
        return false;
    std::vector<PSXRecompV4::ModResolution::IndexedFile> composed_files;
    if (!regular_claims.empty() &&
        !compose_files(disc, entries, regular_claims, composed_files, error))
        return false;

    std::set<uint32_t> claimed;
    std::set<size_t> updated_table_indices;
    uint64_t next_lba = base_sector_count;
    const int64_t xa_children = -int64_t(entries.front().size);
    PSXRecompV4::ModVirtualDisc built;
    std::vector<ReplacementAllocation> allocations;
    allocations.reserve(composed_files.size());
    for (const auto& file : composed_files) {
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
        updated_table_indices.insert(file.index);
        allocations.push_back({&file, static_cast<uint32_t>(next_lba), sectors});
        next_lba += sectors;
    }

    std::vector<FmvAllocation> fmv_allocations;
    std::vector<uint8_t> fmv_executable;
    if (fmv_claim) {
        const bool disc1 = fmv.disc == 1;
        const uint32_t executable_index = disc1 ? 22 : 17;
        const size_t stock_stream_index = disc1 ? 4150 : 4145;
        const size_t final_sentinel_index = disc1 ? 4162 : 4157;
        const std::vector<uint32_t> expected_subtitles = disc1
            ? std::vector<uint32_t>{4150, 4151, 4152, 4153, 4154, 4155, 4156, 4157}
            : std::vector<uint32_t>{4153, 4154, 4155};
        if (fmv_claim->index != executable_index ||
            entries.size() != stock_stream_index + 1 ||
            embedded.entry_index != executable_index ||
            fmv.subtitles.size() != expected_subtitles.size() ||
            (final_sentinel_index + 1) * kEntrySize > table.size())
            return fail(error, "Perfect Works FMV disc layout disagrees");
        std::map<uint32_t, const FmvSubtitle*> subtitles;
        for (const FmvSubtitle& subtitle : fmv.subtitles)
            subtitles.emplace(subtitle.index, &subtitle);
        for (uint32_t index : expected_subtitles)
            if (subtitles.find(index) == subtitles.end())
                return fail(error, "Perfect Works FMV subtitle indices disagree");

        std::string executable_hash;
        if (!hash_file(disc, entries[executable_index], executable_hash) ||
            executable_hash != fmv_claim->expected_sha256)
            return fail(error, "Perfect Works FMV executable checksum failed");
        std::vector<uint8_t> stock_executable;
        if (!read_file(disc, entries[executable_index], stock_executable) ||
            fmv.executable.size() != stock_executable.size())
            return fail(error, "cannot read the stock FMV executable");
        if (embedded.lba < entries[executable_index].lba)
            return fail(error, "Perfect Works FMV embedded table is invalid");
        const size_t embedded_offset =
            size_t(embedded.lba - entries[executable_index].lba) *
                kUserSectorSize + embedded.offset;
        if (embedded_offset + table.size() > stock_executable.size())
            return fail(error, "Perfect Works FMV embedded table exceeds the executable");

        const TableEntry stock_stream = entries[stock_stream_index];
        for (size_t index = stock_stream_index;
             index <= final_sentinel_index; ++index)
            write_entry(table.data() + index * kEntrySize, 0, 0);
        fmv_allocations.reserve(expected_subtitles.size());
        for (uint32_t index : expected_subtitles) {
            const FmvSubtitle& subtitle = *subtitles.at(index);
            const uint32_t size = static_cast<uint32_t>(subtitle.payload.size());
            const uint32_t sectors = sector_count_for(size);
            if (next_lba + sectors > kSentinelLba ||
                next_lba + sectors > kMaxMsfLba ||
                next_lba + sectors >
                    uint64_t(base_sector_count) + kMaxVirtualSectors)
                return fail(error, "Perfect Works FMV virtual extension is excessive");
            write_entry(table.data() + index * kEntrySize,
                        static_cast<uint32_t>(next_lba), size);
            fmv_allocations.push_back(
                {&subtitle, static_cast<uint32_t>(next_lba), sectors});
            next_lba += sectors;
        }
        if (disc1) {
            write_entry(table.data() + 4160 * kEntrySize,
                        stock_stream.lba, 18688);
            write_entry(table.data() + 4161 * kEntrySize,
                        stock_stream.lba,
                        static_cast<uint32_t>(stock_stream.size));
        } else {
            write_entry(table.data() + 4156 * kEntrySize,
                        stock_stream.lba,
                        static_cast<uint32_t>(stock_stream.size));
        }
        write_entry(table.data() + final_sentinel_index * kEntrySize,
                    kSentinelLba, 0);
        table_size = (final_sentinel_index + 1) * kEntrySize;
        for (size_t index = stock_stream_index;
             index <= final_sentinel_index; ++index)
            updated_table_indices.insert(index);

        fmv_executable = fmv.executable;
        for (size_t index : updated_table_indices) {
            const size_t offset = index * kEntrySize;
            if (index < stock_stream_index &&
                !std::equal(
                    fmv.executable.begin() + embedded_offset + offset,
                    fmv.executable.begin() + embedded_offset + offset + kEntrySize,
                    stock_executable.begin() + embedded_offset + offset))
                return fail(error,
                            "indexed replacement overlaps Perfect Works soft-sub code");
            std::memcpy(fmv_executable.data() + embedded_offset + offset,
                        table.data() + offset, kEntrySize);
        }
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

    for (const FmvAllocation& allocation : fmv_allocations) {
        const std::vector<uint8_t>& payload = allocation.subtitle->payload;
        for (uint32_t i = 0; i < allocation.sectors; ++i) {
            const size_t offset = size_t(i) * kUserSectorSize;
            const size_t amount =
                std::min(kUserSectorSize, payload.size() - offset);
            built.appended_raw_sectors.push_back(
                make_sector(allocation.lba + i, payload.data() + offset,
                            amount, i + 1 == allocation.sectors));
        }
    }

    if (!patch_serialized_table(
            disc, kPrimaryTableLba, 0, table, table_size,
            built, error))
        return false;
    if (fmv_claim) {
        if (!patch_file_in_place(
                disc, entries[embedded.entry_index], fmv_executable,
                built, error) ||
            !install_fmv_raw_sectors(
                disc, fmv, base_sector_count, built, error))
            return false;
    } else if (!patch_serialized_table(
                   disc, embedded.lba, embedded.offset, table,
                   table_size, built, error)) {
        return false;
    }
    for (auto& [lba, sector] : built.raw_sectors) {
        if (lba < base_sector_count && sector[15] == 2 &&
            (sector[18] & 0x20) == 0)
            regenerate_mode2_form1(sector);
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
