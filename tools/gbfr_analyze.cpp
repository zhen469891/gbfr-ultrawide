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
    fprintf(stderr, "ERROR: bad arguments\n");
    return 1;
}
