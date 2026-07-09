#include "stdafx.h"
#include <stdio.h>

using namespace std;

namespace Memory
{
    template<typename T>
    void Write(uintptr_t writeAddress, T value)
    {
        DWORD oldProtect;
        VirtualProtect((LPVOID)(writeAddress), sizeof(T), PAGE_EXECUTE_WRITECOPY, &oldProtect);
        *(reinterpret_cast<T*>(writeAddress)) = value;
        VirtualProtect((LPVOID)(writeAddress), sizeof(T), oldProtect, &oldProtect);
    }

    void PatchBytes(uintptr_t address, const char* pattern, unsigned int numBytes)
    {
        DWORD oldProtect;
        VirtualProtect((LPVOID)address, numBytes, PAGE_EXECUTE_READWRITE, &oldProtect);
        memcpy((LPVOID)address, pattern, numBytes);
        VirtualProtect((LPVOID)address, numBytes, oldProtect, &oldProtect);
    }


    static HMODULE GetThisDllHandle()
    {
        MEMORY_BASIC_INFORMATION info;
        size_t len = VirtualQueryEx(GetCurrentProcess(), (void*)GetThisDllHandle, &info, sizeof(info));
        assert(len == sizeof(info));
        return len ? (HMODULE)info.AllocationBase : NULL;
    }

    uint32_t ModuleTimestamp(void* module)
    {
        auto dosHeader = (PIMAGE_DOS_HEADER)module;
        auto ntHeaders = (PIMAGE_NT_HEADERS)((std::uint8_t*)module + dosHeader->e_lfanew);
        return ntHeaders->FileHeader.TimeDateStamp;
    }

    // CSGOSimple's pattern scan (pattern syntax), rewritten to scan safely:
    // https://github.com/OneshotGH/CSGOSimple-master/blob/master/CSGOSimple/helpers/utils.cpp
    //
    // GBFR v2.0.2 note: scanning the whole SizeOfImage crashes at runtime. The SteamStub
    // (.bind) section decommits itself once the game's entry point has run, so any scan
    // that touches those pages AVs after the injection delay - this is also why stock
    // GBFRelinkFix 1.1.1 crashes on game v2.x. We therefore only scan executable sections,
    // and VirtualQuery every region so uncommitted/guarded pages are skipped instead of
    // dereferenced. (All our patterns live in .text, so this is also strictly correct.)
    namespace detail
    {
        inline std::vector<int> pattern_to_byte(const char* pattern)
        {
            auto bytes = std::vector<int>{};
            auto start = const_cast<char*>(pattern);
            auto end = const_cast<char*>(pattern) + strlen(pattern);

            for (auto current = start; current < end; ++current) {
                if (*current == '?') {
                    ++current;
                    if (*current == '?')
                        ++current;
                    bytes.push_back(-1);
                }
                else {
                    bytes.push_back(strtoul(current, &current, 16));
                }
            }
            return bytes;
        }

        // Committed & readable slices of the module's executable sections.
        inline std::vector<std::pair<std::uint8_t*, std::size_t>> ExecutableScanRanges(void* module)
        {
            std::vector<std::pair<std::uint8_t*, std::size_t>> ranges;

            auto dosHeader = (PIMAGE_DOS_HEADER)module;
            auto ntHeaders = (PIMAGE_NT_HEADERS)((std::uint8_t*)module + dosHeader->e_lfanew);
            auto section = IMAGE_FIRST_SECTION(ntHeaders);

            for (WORD i = 0; i < ntHeaders->FileHeader.NumberOfSections; ++i, ++section) {
                if (!(section->Characteristics & IMAGE_SCN_MEM_EXECUTE))
                    continue;

                auto start = (std::uint8_t*)module + section->VirtualAddress;
                auto end = start + section->Misc.VirtualSize;

                for (auto p = start; p < end;) {
                    MEMORY_BASIC_INFORMATION mbi{};
                    if (!VirtualQuery(p, &mbi, sizeof(mbi)))
                        break;

                    auto regionEnd = (std::uint8_t*)mbi.BaseAddress + mbi.RegionSize;
                    if (regionEnd > end)
                        regionEnd = end;

                    constexpr DWORD readableMask = PAGE_READONLY | PAGE_READWRITE | PAGE_WRITECOPY |
                        PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY;
                    if (mbi.State == MEM_COMMIT && (mbi.Protect & readableMask) && !(mbi.Protect & PAGE_GUARD))
                        ranges.emplace_back(p, (std::size_t)(regionEnd - p));

                    p = regionEnd;
                }
            }
            return ranges;
        }

        // OnHit: bool(std::uint8_t*) - return true to stop scanning.
        template <typename OnHit>
        inline void PatternScanRanges(void* module, const char* signature, OnHit onHit)
        {
            auto patternBytes = pattern_to_byte(signature);
            auto s = patternBytes.size();
            auto d = patternBytes.data();
            if (s == 0)
                return;

            for (auto& [base, size] : ExecutableScanRanges(module)) {
                if (size < s)
                    continue;
                for (std::size_t i = 0; i <= size - s; ++i) {
                    bool found = true;
                    for (std::size_t j = 0; j < s; ++j) {
                        if (base[i + j] != d[j] && d[j] != -1) {
                            found = false;
                            break;
                        }
                    }
                    if (found && onHit(base + i))
                        return;
                }
            }
        }
    }

    std::uint8_t* PatternScan(void* module, const char* signature)
    {
        std::uint8_t* result = nullptr;
        detail::PatternScanRanges(module, signature, [&](std::uint8_t* hit) {
            result = hit;
            return true; // first hit -> stop
        });
        return result;
    }

    // All-matches variant of PatternScan (same pattern syntax).
    // Returns every hit in ascending address order; empty vector if none.
    std::vector<std::uint8_t*> PatternScanAll(void* module, const char* signature)
    {
        std::vector<std::uint8_t*> results;
        detail::PatternScanRanges(module, signature, [&](std::uint8_t* hit) {
            results.push_back(hit);
            return false; // collect all hits
        });
        return results;
    }

    uintptr_t GetAbsolute64(uintptr_t address) noexcept
    {
        return (address + 4 + *reinterpret_cast<std::int32_t*>(address));
    }

    std::uintptr_t* FindFromOffsets(std::uintptr_t ptr, std::vector<std::ptrdiff_t> offsets)
    {
        std::uintptr_t* addr = &ptr; // creates a pointer to the first parameter
        for (int i = 0; i < offsets.size(); i++) // Loops through each offset
        {
            addr = reinterpret_cast<std::uintptr_t*>(*addr + offsets[i]); // Reads the current pointer of current address + offset
            if (!addr)
                return nullptr; // Checking if it is null and stopping so it doesnt error
        }

        return addr; // Returns address
    };
}

namespace Util
{
    std::pair<int, int> GetPhysicalDesktopDimensions() {
        if (DEVMODE devMode{ .dmSize = sizeof(DEVMODE) }; EnumDisplaySettings(nullptr, ENUM_CURRENT_SETTINGS, &devMode))
            return { devMode.dmPelsWidth, devMode.dmPelsHeight };

        return {};
    }
}