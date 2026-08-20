#pragma once

#include "mod_runtime.h"

namespace XenogearsRecomp {

inline constexpr const char* kIndexedDiscFormat = "xenogears";

bool build_indexed_disc(
    PS1::ISOReader& disc,
    const std::vector<PSXRecompV4::ModResolution::IndexedFile>& files,
    uint32_t base_sector_count,
    PSXRecompV4::ModVirtualDisc& output,
    std::string* error);

bool build_indexed_disc(
    const std::filesystem::path& disc_path,
    const std::vector<PSXRecompV4::ModResolution::IndexedFile>& files,
    uint32_t base_sector_count,
    PSXRecompV4::ModVirtualDisc& output,
    std::string* error);

} // namespace XenogearsRecomp
