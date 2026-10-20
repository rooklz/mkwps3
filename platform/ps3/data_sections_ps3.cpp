// The game's DOL/REL data sections, read from USRDIR/mkwii_sections.bin
// (tools/pack-sections.py) straight into guest memory. Upstream embeds them in
// the executable; on the PS3 that would hold 7 MiB of the 213 MiB budget twice.

#include "memory.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <vector>

extern "C" int mkw_fs_read_range(const char* path, uint64_t offset, void* dst, uint32_t length);

namespace {
bool g_dataInitialized = false;

uint32_t Be32(const uint8_t* p)
{
    uint32_t v;
    std::memcpy(&v, p, 4);
    return v;
}
} // namespace

extern "C" void InitializeDataSections()
{
    if (g_dataInitialized)
        return;
    g_dataInitialized = true;

    const std::string path = (std::filesystem::current_path() / "mkwii_sections.bin").string();
    uint8_t header[8];
    if (mkw_fs_read_range(path.c_str(), 0, header, sizeof header) != 0 || std::memcmp(header, "MKWS", 4) != 0) {
        std::printf("[runtime] %s is missing or not a section pack\n", path.c_str());
        std::abort();
    }
    const uint32_t count = Be32(header + 4);
    std::vector<uint8_t> table(count * 12u);
    if (mkw_fs_read_range(path.c_str(), sizeof header, table.data(), static_cast<uint32_t>(table.size())) != 0) {
        std::printf("[runtime] short read of the section table\n");
        std::abort();
    }
    for (uint32_t i = 0; i < count; ++i) {
        const uint32_t addr = Be32(&table[i * 12]);
        const uint32_t size = Be32(&table[i * 12 + 4]);
        const uint32_t offset = Be32(&table[i * 12 + 8]);
        if (!Memory::Contains(addr, size) ||
            mkw_fs_read_range(path.c_str(), offset, Memory::GetPointer(addr, size), size) != 0) {
            std::printf("[runtime] section %u (%08x, %u bytes) failed to load\n", i, addr, size);
            std::abort();
        }
    }
    std::printf("[runtime] loaded %u data sections from %s\n", count, path.c_str());
}

extern "C" bool IsDataSectionsInitialized()
{
    return g_dataInitialized;
}
