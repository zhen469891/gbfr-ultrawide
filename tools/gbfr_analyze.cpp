// gbfr_analyze.cpp - offline analysis CLI for granblue_fantasy_relink.exe (v2.0.2)
// Subcommands:
//   disasm <fileOffsetHex> <numBytes>   - disassemble from file offset (addresses shown as RVA)
//   scan   <pattern>                    - byte pattern scan over .text ("??" wildcards)
//   xref   <targetRVAHex>               - brute-force rip-relative disp32 references to target RVA
//
// PE layout constants (granblue_fantasy_relink.exe v2.0.2):
//   .text RawPtr = 0x400, VirtAddr = 0x1000  =>  RVA = fileOffset + 0xC00
//   .text file offset range: [0x400, 0x049AFE00)
//
// Zydis dependency: uses the amalgamated Zydis vendored at vendor\safetyhook\.
// Build with tools\build_tools.ps1 (adds -I..\vendor\safetyhook and compiles
// vendor\safetyhook\Zydis.c). The Zydis header already defines ZYDIS_STATIC_BUILD.

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdint>
#include <cctype>
#include <string>
#include <vector>

#include "Zydis.h"

static const char*    kDefaultExe  = "D:\\Steam\\steamapps\\common\\Granblue Fantasy Relink\\granblue_fantasy_relink.exe";
static const uint64_t kTextRawPtr  = 0x400;
static const uint64_t kTextRawEnd  = 0x049AFE00;   // exclusive
static const uint64_t kRvaDelta    = 0xC00;        // RVA = fileOffset + kRvaDelta

static uint64_t FileOffToRva(uint64_t off) { return off + kRvaDelta; }
static uint64_t RvaToFileOff(uint64_t rva) { return rva - kRvaDelta; }

// ---- PE section table (parsed lazily from the on-disk exe) ----
struct Section {
    char     name[9];
    uint32_t vaddr;     // VirtualAddress (RVA)
    uint32_t vsize;     // Misc.VirtualSize
    uint32_t rawptr;    // PointerToRawData (file offset)
    uint32_t rawsize;   // SizeOfRawData
};
static std::vector<Section> g_sections;

static void LoadSections(const char* path)
{
    if (!g_sections.empty()) return;
    FILE* f = nullptr;
    if (fopen_s(&f, path, "rb") != 0 || !f) return;
    uint8_t hdr[4]; _fseeki64(f, 0x3C, SEEK_SET); fread(hdr, 1, 4, f);
    uint32_t peOff; memcpy(&peOff, hdr, 4);
    // PE sig(4) + COFF header(20). NumberOfSections at peOff+6. SizeOfOptionalHeader at peOff+20.
    uint8_t coff[24]; _fseeki64(f, peOff, SEEK_SET); fread(coff, 1, 24, f);
    uint16_t numSec; memcpy(&numSec, coff + 6, 2);
    uint16_t optSize; memcpy(&optSize, coff + 20, 2);
    uint64_t secTab = peOff + 24 + optSize;
    _fseeki64(f, (long long)secTab, SEEK_SET);
    for (int i = 0; i < numSec; i++) {
        uint8_t s[40]; if (fread(s, 1, 40, f) != 40) break;
        Section sec{};
        memcpy(sec.name, s, 8); sec.name[8] = 0;
        memcpy(&sec.vsize,  s + 8,  4);
        memcpy(&sec.vaddr,  s + 12, 4);
        memcpy(&sec.rawsize,s + 16, 4);
        memcpy(&sec.rawptr, s + 20, 4);
        g_sections.push_back(sec);
    }
    fclose(f);
}

// Map a file offset to an RVA using the containing section.
static bool FileOffToRvaSec(uint64_t off, uint64_t& rvaOut, const char*& secName)
{
    for (auto& s : g_sections) {
        if (s.rawptr && off >= s.rawptr && off < (uint64_t)s.rawptr + s.rawsize) {
            rvaOut = s.vaddr + (off - s.rawptr);
            secName = s.name;
            return true;
        }
    }
    return false;
}
// Map an RVA to a file offset using the containing section.
static bool RvaToFileOffSec(uint64_t rva, uint64_t& offOut)
{
    for (auto& s : g_sections) {
        if (rva >= s.vaddr && rva < (uint64_t)s.vaddr + s.vsize) {
            offOut = s.rawptr + (rva - s.vaddr);
            return true;
        }
    }
    return false;
}

static std::vector<uint8_t> ReadFileRange(const char* path, uint64_t off, uint64_t len)
{
    FILE* f = nullptr;
    if (fopen_s(&f, path, "rb") != 0 || !f) {
        fprintf(stderr, "ERROR: cannot open %s\n", path);
        exit(2);
    }
    _fseeki64(f, (long long)off, SEEK_SET);
    std::vector<uint8_t> buf(len);
    size_t got = fread(buf.data(), 1, (size_t)len, f);
    buf.resize(got);
    fclose(f);
    return buf;
}

static void HexDump(const uint8_t* p, size_t n, char* out, size_t outCap)
{
    size_t pos = 0;
    for (size_t i = 0; i < n && pos + 3 < outCap; i++)
        pos += snprintf(out + pos, outCap - pos, "%02X ", p[i]);
    if (pos > 0) out[pos - 1] = 0; else out[0] = 0;
}

static int CmdDisasm(const char* exe, uint64_t fileOff, uint64_t numBytes)
{
    std::vector<uint8_t> buf = ReadFileRange(exe, fileOff, numBytes);
    if (buf.empty()) { fprintf(stderr, "ERROR: read failed\n"); return 2; }

    ZydisDecoder decoder;
    ZydisDecoderInit(&decoder, ZYDIS_MACHINE_MODE_LONG_64, ZYDIS_STACK_WIDTH_64);
    ZydisFormatter formatter;
    ZydisFormatterInit(&formatter, ZYDIS_FORMATTER_STYLE_INTEL);

    uint64_t pos = 0;
    while (pos < buf.size()) {
        ZydisDecodedInstruction instr;
        ZydisDecodedOperand ops[ZYDIS_MAX_OPERAND_COUNT];
        uint64_t rva = FileOffToRva(fileOff + pos);
        ZyanStatus st = ZydisDecoderDecodeFull(&decoder, buf.data() + pos, buf.size() - pos, &instr, ops);
        if (!ZYAN_SUCCESS(st)) {
            printf("0x%08llX  %02X                       (bad)\n", (unsigned long long)rva, buf[pos]);
            pos += 1;
            continue;
        }
        char text[256];
        ZydisFormatterFormatInstruction(&formatter, &instr, ops, instr.operand_count_visible,
                                        text, sizeof(text), rva, ZYAN_NULL);
        char hex[128];
        HexDump(buf.data() + pos, instr.length, hex, sizeof(hex));
        printf("0x%08llX  %-30s  %s\n", (unsigned long long)rva, hex, text);
        pos += instr.length;
    }
    return 0;
}

struct PatByte { uint8_t val; bool wild; };

static bool ParsePattern(const char* s, std::vector<PatByte>& out)
{
    out.clear();
    const char* p = s;
    while (*p) {
        while (*p == ' ' || *p == '\t') p++;
        if (!*p) break;
        if (p[0] == '?' ) {
            out.push_back({0, true});
            p += (p[1] == '?') ? 2 : 1;
        } else {
            char hexbuf[3] = { p[0], p[1], 0 };
            if (!isxdigit((unsigned char)p[0]) || !isxdigit((unsigned char)p[1])) return false;
            out.push_back({ (uint8_t)strtoul(hexbuf, nullptr, 16), false });
            p += 2;
        }
    }
    return !out.empty();
}

static int CmdScan(const char* exe, const char* patternStr)
{
    std::vector<PatByte> pat;
    if (!ParsePattern(patternStr, pat)) { fprintf(stderr, "ERROR: bad pattern\n"); return 2; }

    std::vector<uint8_t> text = ReadFileRange(exe, kTextRawPtr, kTextRawEnd - kTextRawPtr);
    if (text.empty()) { fprintf(stderr, "ERROR: read failed\n"); return 2; }

    size_t n = text.size(), m = pat.size();
    int hits = 0;
    for (size_t i = 0; i + m <= n; i++) {
        bool ok = true;
        for (size_t j = 0; j < m; j++) {
            if (!pat[j].wild && text[i + j] != pat[j].val) { ok = false; break; }
        }
        if (ok) {
            uint64_t off = kTextRawPtr + i;
            printf("HIT %d: fileOffset=0x%08llX  RVA=0x%08llX\n",
                   ++hits, (unsigned long long)off, (unsigned long long)FileOffToRva(off));
            if (hits >= 200) { printf("(stopping after 200 hits)\n"); break; }
        }
    }
    printf("Total hits: %d\n", hits);
    return 0;
}

static int CmdXref(const char* exe, uint64_t targetRva)
{
    std::vector<uint8_t> text = ReadFileRange(exe, kTextRawPtr, kTextRawEnd - kTextRawPtr);
    if (text.empty()) { fprintf(stderr, "ERROR: read failed\n"); return 2; }

    size_t n = text.size();
    int hits = 0;
    for (size_t i = 0; i + 4 <= n; i++) {
        int32_t d;
        memcpy(&d, text.data() + i, 4);
        uint64_t off = kTextRawPtr + i;
        // rip-relative: next-instruction RVA = RVA(disp32 field) + 4, target = that + d
        uint64_t computed = FileOffToRva(off) + 4 + (int64_t)d;
        if (computed == targetRva) {
            printf("XREF %d: disp32 at fileOffset=0x%08llX RVA=0x%08llX (instruction ends at RVA+4; verify with disasm)\n",
                   ++hits, (unsigned long long)off, (unsigned long long)FileOffToRva(off));
            if (hits >= 500) { printf("(stopping after 500 hits)\n"); break; }
        }
    }
    printf("Total xref candidates: %d\n", hits);
    return 0;
}

static std::vector<uint8_t> ReadWholeFile(const char* path)
{
    FILE* f = nullptr;
    if (fopen_s(&f, path, "rb") != 0 || !f) { fprintf(stderr, "ERROR: cannot open %s\n", path); exit(2); }
    _fseeki64(f, 0, SEEK_END);
    long long sz = _ftelli64(f);
    _fseeki64(f, 0, SEEK_SET);
    std::vector<uint8_t> buf((size_t)sz);
    fread(buf.data(), 1, (size_t)sz, f);
    fclose(f);
    return buf;
}

// Search the whole image for an ASCII substring (and UTF-16LE variant).
// Prints file offset + section RVA for each hit.
static int CmdStr(const char* exe, const char* needle)
{
    LoadSections(exe);
    std::vector<uint8_t> buf = ReadWholeFile(exe);
    size_t n = buf.size();
    size_t m = strlen(needle);
    if (m == 0) { fprintf(stderr, "ERROR: empty needle\n"); return 2; }

    int hits = 0;
    // ASCII
    for (size_t i = 0; i + m <= n; i++) {
        if (memcmp(buf.data() + i, needle, m) == 0) {
            uint64_t rva = 0; const char* sec = "?";
            bool mapped = FileOffToRvaSec(i, rva, sec);
            // print a little context (the containing C-string, up to 64 chars around)
            char ctx[80]; size_t cs = 0;
            size_t start = (i >= 8) ? i - 8 : 0;
            for (size_t k = start; k < i + m + 24 && k < n && cs + 1 < sizeof(ctx); k++) {
                uint8_t c = buf[k];
                ctx[cs++] = (c >= 0x20 && c < 0x7F) ? (char)c : '.';
            }
            ctx[cs] = 0;
            printf("ASCII hit %d: off=0x%08llX RVA=0x%08llX [%s] ctx=\"%s\"\n",
                   ++hits, (unsigned long long)i,
                   (unsigned long long)(mapped ? rva : 0), mapped ? sec : "NOMAP", ctx);
            if (hits >= 300) { printf("(stopping after 300 ASCII hits)\n"); break; }
        }
    }
    // UTF-16LE
    std::vector<uint8_t> w(m * 2);
    for (size_t k = 0; k < m; k++) { w[k*2] = (uint8_t)needle[k]; w[k*2+1] = 0; }
    int whits = 0;
    for (size_t i = 0; i + w.size() <= n; i++) {
        if (memcmp(buf.data() + i, w.data(), w.size()) == 0) {
            uint64_t rva = 0; const char* sec = "?";
            bool mapped = FileOffToRvaSec(i, rva, sec);
            printf("UTF16 hit %d: off=0x%08llX RVA=0x%08llX [%s]\n",
                   ++whits, (unsigned long long)i,
                   (unsigned long long)(mapped ? rva : 0), mapped ? sec : "NOMAP");
            if (whits >= 100) { printf("(stopping after 100 UTF16 hits)\n"); break; }
        }
    }
    printf("Total: %d ASCII, %d UTF16\n", hits, whits);
    return 0;
}

// Convert a file offset to an RVA (section-aware).
static int CmdF2r(const char* exe, uint64_t off)
{
    LoadSections(exe);
    uint64_t rva = 0; const char* sec = "?";
    if (FileOffToRvaSec(off, rva, sec))
        printf("fileOff=0x%08llX  RVA=0x%08llX  section=%s\n",
               (unsigned long long)off, (unsigned long long)rva, sec);
    else
        printf("fileOff=0x%08llX  NOT MAPPED to any section\n", (unsigned long long)off);
    return 0;
}

// Convert an RVA to a file offset (section-aware).
static int CmdR2f(const char* exe, uint64_t rva)
{
    LoadSections(exe);
    uint64_t off = 0;
    if (RvaToFileOffSec(rva, off))
        printf("RVA=0x%08llX  fileOff=0x%08llX\n",
               (unsigned long long)rva, (unsigned long long)off);
    else
        printf("RVA=0x%08llX  NOT MAPPED\n", (unsigned long long)rva);
    return 0;
}

// Dump the section table.
static int CmdSecs(const char* exe)
{
    LoadSections(exe);
    for (auto& s : g_sections)
        printf("%-8s VA=0x%08X VSize=0x%08X Raw=0x%08X RawSize=0x%08X\n",
               s.name, s.vaddr, s.vsize, s.rawptr, s.rawsize);
    return 0;
}

// xref that also searches the whole .rdata/.data-ish image, not just .text.
// Finds rip-relative disp32 anywhere in-file whose target == targetRva. Reports
// the referencing file offset and its RVA (section-aware).
static int CmdXrefAll(const char* exe, uint64_t targetRva)
{
    LoadSections(exe);
    std::vector<uint8_t> buf = ReadWholeFile(exe);
    size_t n = buf.size();
    int hits = 0;
    for (auto& s : g_sections) {
        // only scan executable-ish sections for rip-rel code refs; include .text
        if (strcmp(s.name, ".text") != 0) continue;
        uint64_t begin = s.rawptr, end = (uint64_t)s.rawptr + s.rawsize;
        for (uint64_t i = begin; i + 4 <= end && i + 4 <= n; i++) {
            int32_t d; memcpy(&d, buf.data() + i, 4);
            uint64_t refRva = 0; const char* sec = "?";
            FileOffToRvaSec(i, refRva, sec);
            uint64_t computed = refRva + 4 + (int64_t)d;
            if (computed == targetRva) {
                printf("XREF %d: off=0x%08llX RVA=0x%08llX [%s]\n",
                       ++hits, (unsigned long long)i, (unsigned long long)refRva, sec);
                if (hits >= 500) { printf("(stopping after 500)\n"); break; }
            }
        }
    }
    printf("Total xref candidates: %d\n", hits);
    return 0;
}

// disasm by RVA (section-aware): converts RVA->fileoff then disassembles.
static int CmdDisasmRva(const char* exe, uint64_t rva, uint64_t numBytes)
{
    LoadSections(exe);
    uint64_t off = 0;
    if (!RvaToFileOffSec(rva, off)) { fprintf(stderr, "ERROR: RVA not mapped\n"); return 2; }
    return CmdDisasm(exe, off, numBytes);
}

int main(int argc, char** argv)
{
    const char* exe = kDefaultExe;
    // optional: -f <path> before subcommand
    int argi = 1;
    if (argi + 1 < argc && strcmp(argv[argi], "-f") == 0) { exe = argv[argi + 1]; argi += 2; }

    if (argi >= argc) {
        fprintf(stderr,
            "usage: gbfr_analyze [-f exePath] disasm <fileOffsetHex> <numBytes>\n"
            "       gbfr_analyze [-f exePath] scan   <pattern with ?? wildcards>\n"
            "       gbfr_analyze [-f exePath] xref   <targetRVAHex>\n");
        return 1;
    }

    const char* cmd = argv[argi];
    if (strcmp(cmd, "disasm") == 0 && argi + 2 < argc) {
        uint64_t off = strtoull(argv[argi + 1], nullptr, 16);
        uint64_t len = strtoull(argv[argi + 2], nullptr, 0);
        return CmdDisasm(exe, off, len);
    }
    if (strcmp(cmd, "scan") == 0 && argi + 1 < argc) {
        // join remaining args so pattern can be unquoted
        std::string pat;
        for (int k = argi + 1; k < argc; k++) { if (!pat.empty()) pat += " "; pat += argv[k]; }
        return CmdScan(exe, pat.c_str());
    }
    if (strcmp(cmd, "xref") == 0 && argi + 1 < argc) {
        uint64_t rva = strtoull(argv[argi + 1], nullptr, 16);
        return CmdXref(exe, rva);
    }
    if (strcmp(cmd, "str") == 0 && argi + 1 < argc) {
        // join remaining args to allow spaces
        std::string s;
        for (int k = argi + 1; k < argc; k++) { if (!s.empty()) s += " "; s += argv[k]; }
        return CmdStr(exe, s.c_str());
    }
    if (strcmp(cmd, "f2r") == 0 && argi + 1 < argc)
        return CmdF2r(exe, strtoull(argv[argi + 1], nullptr, 16));
    if (strcmp(cmd, "r2f") == 0 && argi + 1 < argc)
        return CmdR2f(exe, strtoull(argv[argi + 1], nullptr, 16));
    if (strcmp(cmd, "secs") == 0)
        return CmdSecs(exe);
    if (strcmp(cmd, "xrefall") == 0 && argi + 1 < argc)
        return CmdXrefAll(exe, strtoull(argv[argi + 1], nullptr, 16));
    if (strcmp(cmd, "disasmrva") == 0 && argi + 2 < argc) {
        uint64_t rva = strtoull(argv[argi + 1], nullptr, 16);
        uint64_t len = strtoull(argv[argi + 2], nullptr, 0);
        return CmdDisasmRva(exe, rva, len);
    }
    fprintf(stderr, "ERROR: bad arguments\n");
    return 1;
}
