#include "cdrom.h"
#include "mod_plugins.h"

#include <stdint.h>
#include <stdlib.h>

enum {
    XG_FAT_POINTER = 0x8004FDF0u,
    XG_DIRECTORY_POINTER = 0x8004FDF4u,
    XG_FIELD_MAPS = 0xB7u,
    XG_FIELD_MODELS = 0x6B8u,
    XG_MAP_FILE_COUNT = 1460u,
    XG_FAT_BYTES = 0x8000u,
    XG_FAST_MAP_SECTORS = 32u
};

static int ram_span(uint32_t address, uint32_t size) {
    return address >= 0x80000000u && size <= 0x200000u &&
           address - 0x80000000u <= 0x200000u - size;
}

static uint32_t entry_lba(uint32_t entry) {
    return psx_mod_read_byte(entry) |
           ((uint32_t)psx_mod_read_byte(entry + 1u) << 8) |
           ((uint32_t)psx_mod_read_byte(entry + 2u) << 16);
}

/* The resident archive reader resolves directory 4 through its u16 directory
 * table and seven-byte FAT records. Field's 0xB7 marker owns 730 map/graphics
 * pairs. The actual table handles both discs; replay LBAs are never selectors.
 * Faster sector delivery removes the coordinator's pre-fade archive wait,
 * without changing VBlank pacing, decompression, actor updates, or fade steps.
 */
CDROMDataReadRange xenogears_map_read_range(int lba) {
    const CDROMDataReadRange normal = {lba, lba + 1, 0};
    if (lba < 0 || !psx_mod_game_started()) return normal;
    const uint32_t fat = psx_mod_read_word(XG_FAT_POINTER);
    const uint32_t directories = psx_mod_read_word(XG_DIRECTORY_POINTER);
    if (!ram_span(fat, XG_FAT_BYTES) || !ram_span(directories, 10u)) return normal;
    const uint32_t directory = psx_mod_read_half(directories + 8u);
    if (!directory) return normal;
    const uint32_t offset = directory - 1u;
    if (offset + XG_FIELD_MODELS > XG_FAT_BYTES / 7u) return normal;
    const uint32_t marker = fat + (offset + XG_FIELD_MAPS - 1u) * 7u;
    const uint32_t models = fat + (offset + XG_FIELD_MODELS - 1u) * 7u;
    if ((int32_t)psx_mod_read_word(marker + 3u) != -(int32_t)XG_MAP_FILE_COUNT ||
        (int32_t)psx_mod_read_word(models + 3u) != -145) return normal;
    /* Ordinary files are packed between these two group markers. Relocated or
     * unknown layouts retain authentic timing rather than widening the range. */
    const uint32_t first_map_lba = entry_lba(marker);
    const uint32_t end_map_lba = entry_lba(models);
    if ((uint32_t)lba < first_map_lba || (uint32_t)lba >= end_map_lba)
        return normal;
    for (uint32_t i = 1u; i <= XG_MAP_FILE_COUNT; ++i) {
        const uint32_t entry = marker + i * 7u;
        const int32_t bytes = (int32_t)psx_mod_read_word(entry + 3u);
        if (bytes <= 0) continue;
        const uint32_t start = entry_lba(entry);
        const uint32_t sectors = ((uint32_t)bytes + 2047u) / 2048u;
        if (start < first_map_lba || start >= end_map_lba ||
            sectors > end_map_lba - start) continue;
        if ((uint32_t)lba >= start && (uint32_t)lba - start < sectors)
            return (CDROMDataReadRange){(int)start, (int)(start + sectors),
                                       XG_FAST_MAP_SECTORS};
    }
    return normal;
}

PSX_MOD_CONSTRUCTOR(xg_register_map_read_policy) {
    cdrom_set_data_read_policy(xenogears_map_read_range);
    const char *enabled = getenv("XG_FAST_MAP_READS");
    if (enabled) cdrom_set_data_read_policy_enabled(enabled[0] != '0');
}
