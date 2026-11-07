#include "catch_amalgamated.hpp"

#include <brimir/cheat_engine.hpp>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <regex>
#include <string>
#include <vector>

using brimir::CheatEngine;
using brimir::CheatOp;
using brimir::ParseCheat;
using Kind = CheatOp::Kind;
using Reason = CheatOp::IgnoreReason;

namespace {

namespace fs = std::filesystem;

struct WorkRam {
    std::vector<uint8_t> low = std::vector<uint8_t>(CheatEngine::kHalfSize);
    std::vector<uint8_t> high = std::vector<uint8_t>(CheatEngine::kHalfSize);

    void Apply(const CheatEngine &engine) {
        engine.Apply(std::span<uint8_t, CheatEngine::kHalfSize>(low.data(), low.size()),
                     std::span<uint8_t, CheatEngine::kHalfSize>(high.data(), high.size()));
    }
    bool AllZero() const {
        return std::all_of(low.begin(), low.end(), [](uint8_t b) { return b == 0; }) &&
               std::all_of(high.begin(), high.end(), [](uint8_t b) { return b == 0; });
    }
};

std::vector<CheatOp> Parse(const char *code) {
    const auto ops = ParseCheat(code);
    INFO(code);
    REQUIRE(ops.has_value());
    return *ops;
}

void RequireOp(const CheatOp &op, Kind kind, uint32_t address, uint16_t value) {
    CHECK(op.kind == kind);
    CHECK(op.address == address);
    CHECK(op.value == value);
}

// cheatN_code / cheatN_handler of a RetroArch .cht file (values quoted or not).
struct ChtEntry {
    std::string code;
    bool retroArchHandled = false;
};

std::map<unsigned, ChtEntry> ReadCht(const fs::path &path) {
    std::map<unsigned, ChtEntry> entries;
    std::ifstream in(path);
    const std::regex line_re(R"re(^\s*cheat(\d+)_(code|handler)\s*=\s*"?([^"]*?)"?\s*$)re");
    std::string line;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        std::smatch m;
        if (std::regex_match(line, m, line_re)) {
            ChtEntry &entry = entries[static_cast<unsigned>(std::stoul(m[1].str()))];
            if (m[2] == "code") {
                entry.code = m[3].str();
            } else {
                entry.retroArchHandled = m[3] == "1";
            }
        }
    }
    return entries;
}

// What Beetle Saturn's ss_cheat_parse_one (libretro.c) makes of the sub-code "AAAAAAAA VVVV": a PAR "type byte"
// 0x14/0x16/0x18 (High RAM) or 0x34/0x36/0x38 (Low RAM) at 8/16/32 bits, otherwise a raw 32-bit address written at
// 16 bits. No conditionals and no master codes.
struct Effect {
    enum class What { None, Write, Conditional } what = What::None;
    uint32_t address = 0; // after the 27-bit bus mask
    unsigned bytes = 0;
    uint32_t value = 0;
    bool operator==(const Effect &) const = default;
};

Effect BeetleEffect(uint32_t a, uint16_t v) {
    const uint32_t offset = a & 0xFFFFFF;
    Effect e{Effect::What::Write, 0, 2, v};
    switch (a >> 24) {
    case 0x14: e = {Effect::What::Write, 0x06000000 + offset, 1, v & 0xFFu}; break;
    case 0x16: e = {Effect::What::Write, 0x06000000 + offset, 2, v}; break;
    case 0x18: e = {Effect::What::Write, 0x06000000 + offset, 4, v}; break;
    case 0x34: e = {Effect::What::Write, 0x00200000 + offset, 1, v & 0xFFu}; break;
    case 0x36: e = {Effect::What::Write, 0x00200000 + offset, 2, v}; break;
    case 0x38: e = {Effect::What::Write, 0x00200000 + offset, 4, v}; break;
    default: e.address = a; break;
    }
    e.address &= 0x07FFFFFF;
    return e;
}

// The sub-code's "AAAAAAAA" (type digit included) as written, from a parsed op.
uint32_t TypedAddress(const CheatOp &op) {
    switch (op.kind) {
    case Kind::Write16: return 0x10000000 | op.address;
    case Kind::Write8: return 0x30000000 | op.address;
    case Kind::IfEqual16: return 0xD0000000 | op.address;
    case Kind::Ignored: return op.address;
    }
    return 0;
}

Effect OurEffect(const CheatOp &op) {
    switch (op.kind) {
    case Kind::Write16: return {Effect::What::Write, op.address & 0x07FFFFFE, 2, op.value};
    case Kind::Write8: return {Effect::What::Write, op.address & 0x07FFFFFF, 1, op.value & 0xFFu};
    case Kind::IfEqual16: return {Effect::What::Conditional, op.address & 0x07FFFFFE, 2, op.value};
    case Kind::Ignored: return {};
    }
    return {};
}

std::string Describe(const Effect &e) {
    char text[96];
    switch (e.what) {
    case Effect::What::None: return "nothing";
    case Effect::What::Conditional:
        std::snprintf(text, sizeof(text), "if 16-bit @%08X == %04X", e.address, e.value);
        return text;
    case Effect::What::Write:
        std::snprintf(text, sizeof(text), "%u-bit write @%08X = %X", e.bytes * 8, e.address, e.value);
        return text;
    }
    return "?";
}

// Beetle's lexer: every '+' segment must be "hex{1,8}[ \t:]+hex{1,8}" (a separator between address and value).
bool BeetleLexAccepts(const std::string &code) {
    static const std::regex segment_re(R"(^\s*[0-9A-Fa-f]{1,8}[ \t:]+[0-9A-Fa-f]{1,8}\s*$)");
    size_t start = 0;
    bool any = false;
    while (start <= code.size()) {
        size_t end = code.find('+', start);
        if (end == std::string::npos) {
            end = code.size();
        }
        const std::string segment = code.substr(start, end - start);
        if (segment.find_first_not_of(" \t") != std::string::npos) {
            if (!std::regex_match(segment, segment_re)) {
                return false;
            }
            any = true;
        }
        start = end + 1;
    }
    return any;
}

struct SweepResult {
    size_t files = 0;
    size_t entries = 0;
    size_t emptyCodes = 0;
    size_t retroArchHandled = 0;
    size_t codes = 0;
    size_t subCodes = 0;
    size_t write16 = 0, write8 = 0, ifEqual = 0;
    size_t master = 0, unknownType = 0, malformed = 0;
    size_t codesWithIgnored = 0;
    size_t inertWithWrites = 0;   // codes with a malformed sub-code and a write beside it (kept inert)
    size_t beetleDifferent = 0;   // sub-codes Beetle's type-byte reading applies differently
    size_t beetleLexAccepts = 0;  // codes Beetle's lexer accepts as stored
    std::map<std::string, size_t> beetleDiffByType; // by "TT" (type byte)
    std::vector<std::string> failures;
    std::vector<std::string> notes; // ignored sub-codes and Beetle differences, one line each
};

SweepResult Sweep(const fs::path &dir) {
    SweepResult r;
    std::vector<fs::path> files;
    for (const auto &item : fs::directory_iterator(dir)) {
        if (item.is_regular_file() && item.path().extension() == ".cht") {
            files.push_back(item.path());
        }
    }
    std::sort(files.begin(), files.end());
    for (const fs::path &file : files) {
        ++r.files;
        for (const auto &[index, entry] : ReadCht(file)) {
            ++r.entries;
            const std::string where = file.filename().string() + " cheat" + std::to_string(index);
            if (entry.retroArchHandled) {
                ++r.retroArchHandled;
                continue;
            }
            if (entry.code.empty()) {
                ++r.emptyCodes;
                continue;
            }
            ++r.codes;
            const auto ops = ParseCheat(entry.code);
            if (!ops) {
                r.failures.push_back(where + ": '" + entry.code + "'");
                continue;
            }
            if (BeetleLexAccepts(entry.code)) {
                ++r.beetleLexAccepts;
            }
            bool ignored = false;
            const bool hasMalformed = std::any_of(ops->begin(), ops->end(), [](const CheatOp &op) {
                return op.kind == Kind::Ignored && op.reason == Reason::Malformed;
            });
            const bool hasWrite = std::any_of(ops->begin(), ops->end(), [](const CheatOp &op) {
                return op.kind == Kind::Write8 || op.kind == Kind::Write16;
            });
            if (hasMalformed && hasWrite) {
                ++r.inertWithWrites;
                r.notes.push_back(where + ": inert (a malformed sub-code beside writes) '" + entry.code + "'");
            }
            for (const CheatOp &op : *ops) {
                ++r.subCodes;
                switch (op.kind) {
                case Kind::Write16: ++r.write16; break;
                case Kind::Write8: ++r.write8; break;
                case Kind::IfEqual16: ++r.ifEqual; break;
                case Kind::Ignored:
                    ignored = true;
                    if (op.reason == Reason::MasterCode) {
                        ++r.master;
                    } else if (op.reason == Reason::UnknownType) {
                        ++r.unknownType;
                        char text[64];
                        std::snprintf(text, sizeof(text), "unknown type %08X %04X", op.address, op.value);
                        r.notes.push_back(where + ": " + text + " in '" + entry.code + "'");
                    } else {
                        ++r.malformed;
                        r.notes.push_back(where + ": malformed sub-code in '" + entry.code + "'");
                    }
                    break;
                }
                if (op.kind == Kind::Ignored && op.reason == Reason::Malformed) {
                    continue; // no "AAAAAAAA VVVV" to compare
                }
                const uint32_t typed = TypedAddress(op);
                const Effect ours = OurEffect(op);
                const Effect beetle = BeetleEffect(typed, op.value);
                if (!(ours == beetle)) {
                    ++r.beetleDifferent;
                    char tt[8];
                    std::snprintf(tt, sizeof(tt), "%02X", typed >> 24);
                    ++r.beetleDiffByType[tt];
                    char text[48];
                    std::snprintf(text, sizeof(text), "%08X %04X", typed, op.value);
                    r.notes.push_back(where + ": " + text + ": PAR " + Describe(ours) + ", Beetle " +
                                      Describe(beetle));
                }
            }
            if (ignored) {
                ++r.codesWithIgnored;
            }
        }
    }
    return r;
}

void PrintSweep(const char *name, const SweepResult &r) {
    std::printf("%s: %zu files, %zu entries: %zu codes parsed (%zu sub-codes: %zu 16-bit writes, %zu 8-bit writes, "
                "%zu conditionals, %zu master codes ignored, %zu unknown types ignored, %zu malformed ignored; %zu "
                "codes with an ignored sub-code; %zu inert with writes beside a malformed sub-code), %zu "
                "RetroArch-handled, %zu empty codes, %zu parse failures\n",
                name, r.files, r.entries, r.codes, r.subCodes, r.write16, r.write8, r.ifEqual, r.master,
                r.unknownType, r.malformed, r.codesWithIgnored, r.inertWithWrites, r.retroArchHandled, r.emptyCodes,
                r.failures.size());
    std::printf("%s: Beetle's lexer accepts %zu of the %zu codes as stored; its type-byte reading applies %zu "
                "sub-codes differently:",
                name, r.beetleLexAccepts, r.codes, r.beetleDifferent);
    for (const auto &[tt, count] : r.beetleDiffByType) {
        std::printf(" %s:%zu", tt.c_str(), count);
    }
    std::printf("\n");
    if (const char *out = std::getenv("BRIMIR_CHT_NOTES")) {
        std::ofstream notes(out);
        for (const std::string &line : r.notes) {
            notes << line << '\n';
        }
    }
}

} // namespace

TEST_CASE("cheats: parse database forms", "[cheats]") {
    auto ops = Parse("16033c7400c0");
    REQUIRE(ops.size() == 1);
    RequireOp(ops[0], Kind::Write16, 0x06033C74, 0x00C0);

    ops = Parse("1601dba4 0900");
    REQUIRE(ops.size() == 1);
    RequireOp(ops[0], Kind::Write16, 0x0601DBA4, 0x0900);

    for (const char *code : {"36012345 00FF", "3601234500ff", "36012345+00FF"}) {
        ops = Parse(code);
        REQUIRE(ops.size() == 1);
        CHECK(ops[0].kind == Kind::Write8);
        CHECK(ops[0].address == 0x06012345);
        CHECK((ops[0].value & 0xFF) == 0xFF);
    }

    ops = Parse("D6033C60+270F");
    REQUIRE(ops.size() == 1);
    RequireOp(ops[0], Kind::IfEqual16, 0x06033C60, 0x270F);

    ops = Parse("F6000924+C305+B6002800+0000");
    REQUIRE(ops.size() == 2);
    for (const CheatOp &op : ops) {
        CHECK(op.kind == Kind::Ignored);
        CHECK(op.reason == Reason::MasterCode);
    }

    ops = Parse("b60028000000");
    REQUIRE(ops.size() == 1);
    CHECK(ops[0].kind == Kind::Ignored);
    CHECK(ops[0].reason == Reason::MasterCode);

    ops = Parse("1020 1234 0001");
    REQUIRE(ops.size() == 1);
    RequireOp(ops[0], Kind::Write16, 0x00201234, 0x0001);
    CHECK(brimir::CheatAddressInWorkRam(ops[0].address));

    // '+'-joined pairs and 12-digit sub-codes in one cheat
    ops = Parse("16033C5E+270F+16033C60+270F");
    REQUIRE(ops.size() == 2);
    RequireOp(ops[0], Kind::Write16, 0x06033C5E, 0x270F);
    RequireOp(ops[1], Kind::Write16, 0x06033C60, 0x270F);
    ops = Parse("160b9cb200fb+160be37e00fb");
    REQUIRE(ops.size() == 2);
    RequireOp(ops[1], Kind::Write16, 0x060BE37E, 0x00FB);

    CHECK_FALSE(ParseCheat("zz").has_value());
    CHECK_FALSE(ParseCheat("").has_value());
    CHECK_FALSE(ParseCheat(" + ").has_value());
}

TEST_CASE("cheats: sub-codes typed with spaces only", "[cheats]") {
    auto ops = Parse("1601dba4 0900 16033C5E 270F");
    REQUIRE(ops.size() == 2);
    RequireOp(ops[0], Kind::Write16, 0x0601DBA4, 0x0900);
    RequireOp(ops[1], Kind::Write16, 0x06033C5E, 0x270F);

    ops = Parse("D6033C60 270F 16033C5E 270F 36012345 00FF+16000000+0001");
    REQUIRE(ops.size() == 4);
    RequireOp(ops[0], Kind::IfEqual16, 0x06033C60, 0x270F);
    RequireOp(ops[1], Kind::Write16, 0x06033C5E, 0x270F);
    RequireOp(ops[2], Kind::Write8, 0x06012345, 0x00FF);
    RequireOp(ops[3], Kind::Write16, 0x06000000, 0x0001);

    // Not a multiple of 12 digits: still one malformed sub-code
    ops = Parse("1601dba4 0900 16033C5E");
    REQUIRE(ops.size() == 1);
    CHECK(ops[0].reason == Reason::Malformed);

    CheatEngine engine;
    WorkRam ram;
    engine.Set(0, true, "16000010 1234 36000021 0077");
    REQUIRE(engine.ActiveCount() == 1);
    ram.Apply(engine);
    CHECK(ram.high[0x10] == 0x12);
    CHECK(ram.high[0x11] == 0x34);
    CHECK(ram.high[0x21] == 0x77);
}

TEST_CASE("cheats: a malformed sub-code keeps the whole cheat inert", "[cheats]") {
    CheatEngine engine;
    WorkRam ram;
    engine.Set(0, true, "16000010+1234+160B374E+00??");
    engine.Set(1, true, "16000020 5678+160CE42+0001");
    engine.Set(2, true, "16000030 9ABC");
    CHECK(engine.ActiveCount() == 1);
    ram.Apply(engine);
    CHECK(ram.high[0x10] == 0x00);
    CHECK(ram.high[0x11] == 0x00);
    CHECK(ram.high[0x20] == 0x00);
    CHECK(ram.high[0x21] == 0x00);
    CHECK(ram.high[0x30] == 0x9A);
    CHECK(ram.high[0x31] == 0xBC);

    // Replacing it with a good code makes it apply.
    engine.Set(0, true, "16000010+1234");
    ram.Apply(engine);
    CHECK(ram.high[0x10] == 0x12);
}

TEST_CASE("cheats: a type-0 sub-code is flagged as having no type digit", "[cheats]") {
    auto ops = Parse("06048072+0005+16048074+0006");
    REQUIRE(ops.size() == 2);
    CHECK(ops[0].noTypeDigit);
    CHECK_FALSE(ops[1].noTypeDigit);
}

TEST_CASE("cheats: a conditional gating an ignored sub-code skips only that one", "[cheats]") {
    // if [0x06000100] == 0x270F: (a master code, nothing to do); then always 0x0002 at 0x06000300
    CheatEngine engine;
    WorkRam ram;
    engine.Set(0, true, "D6000100+270F+F6000924+C305+16000300+0002");
    ram.Apply(engine);
    CHECK(ram.high[0x301] == 0x02);

    ram.high[0x301] = 0x00;
    ram.high[0x100] = 0x27;
    ram.high[0x101] = 0x0F;
    ram.Apply(engine);
    CHECK(ram.high[0x301] == 0x02);

    // An unknown type in the gated place behaves the same way.
    CheatEngine unknown;
    WorkRam ram2;
    unknown.Set(0, true, "D6000100 270F C6000000 0001 16000300 0003");
    ram2.Apply(unknown);
    CHECK(ram2.high[0x301] == 0x03);
}

TEST_CASE("cheats: erasing a slot", "[cheats]") {
    CheatEngine engine;
    WorkRam ram;
    engine.Set(3, true, "16000000 1234");
    engine.Set(4, true, "16000002 5678");
    REQUIRE(engine.ActiveCount() == 2);
    engine.Erase(3);
    engine.Erase(9); // not set: nothing happens
    CHECK(engine.ActiveCount() == 1);
    ram.Apply(engine);
    CHECK(ram.high[0] == 0x00);
    CHECK(ram.high[2] == 0x56);
}

TEST_CASE("cheats: database oddities are ignored sub-codes, not errors", "[cheats]") {
    // A template value meant to be edited: the conditional stays, the write is ignored.
    auto ops = Parse("D60B374E+0000+160B374E+00??");
    REQUIRE(ops.size() == 2);
    RequireOp(ops[0], Kind::IfEqual16, 0x060B374E, 0x0000);
    CHECK(ops[1].kind == Kind::Ignored);
    CHECK(ops[1].reason == Reason::Malformed);

    // 7- and 9-digit addresses, spreadsheet decimals
    for (const char *code : {"160CE42+0001", "160067C92+0064", "1607073000000.00", "6000924+FFFF"}) {
        ops = Parse(code);
        REQUIRE(ops.size() == 1);
        CHECK(ops[0].kind == Kind::Ignored);
        CHECK(ops[0].reason == Reason::Malformed);
    }

    // A plain address without a type digit is a 16-bit write (Daytona USA (USA).cht)
    ops = Parse("06048072+0005");
    REQUIRE(ops.size() == 1);
    RequireOp(ops[0], Kind::Write16, 0x06048072, 0x0005);

    // A type digit other than 0, 1, 3, D, F, B
    ops = Parse("C6000000 0001");
    REQUIRE(ops.size() == 1);
    CHECK(ops[0].kind == Kind::Ignored);
    CHECK(ops[0].reason == Reason::UnknownType);
    CHECK(ops[0].address == 0xC6000000);
}

TEST_CASE("cheats: big-endian writes", "[cheats]") {
    CheatEngine engine;
    WorkRam ram;
    engine.Set(0, true, "16000000 1234");
    engine.Set(1, true, "10200010 ABCD");
    engine.Set(2, true, "36000021 0077");
    engine.Set(3, true, "302FFFFF 09EE"); // the value's high byte is not used
    REQUIRE(engine.ActiveCount() == 4);
    ram.Apply(engine);
    CHECK(ram.high[0] == 0x12);
    CHECK(ram.high[1] == 0x34);
    CHECK(ram.low[0x10] == 0xAB);
    CHECK(ram.low[0x11] == 0xCD);
    CHECK(ram.high[0x21] == 0x77);
    CHECK(ram.high[0x20] == 0x00);
    CHECK(ram.low[0xFFFFF] == 0xEE);
    CHECK(ram.low[0xFFFFE] == 0x00);

    // High RAM mirrors (0x06000000-0x07FFFFFF) and an odd 16-bit address (the bus aligns it)
    WorkRam mirrored;
    CheatEngine mirror;
    mirror.Set(0, true, "17F00100 5566");
    mirror.Set(1, true, "16000203 7788");
    mirrored.Apply(mirror);
    CHECK(mirrored.high[0x100] == 0x55);
    CHECK(mirrored.high[0x101] == 0x66);
    CHECK(mirrored.high[0x202] == 0x77);
    CHECK(mirrored.high[0x203] == 0x88);
}

TEST_CASE("cheats: conditional applies next op only when equal", "[cheats]") {
    CheatEngine engine;
    WorkRam ram;
    // if [0x06000100] == 0x270F: write 0x0001 at 0x06000200; then always 0x0002 at 0x06000300
    engine.Set(0, true, "D6000100+270F+16000200+0001+16000300+0002");
    ram.Apply(engine);
    CHECK(ram.high[0x201] == 0x00);
    CHECK(ram.high[0x301] == 0x02);

    ram.high[0x100] = 0x27;
    ram.high[0x101] = 0x0F;
    ram.Apply(engine);
    CHECK(ram.high[0x201] == 0x01);

    // Only the next op is gated: a false condition skips one op, the rest of the cheat applies.
    ram.high[0x101] = 0x10;
    ram.high[0x201] = 0x00;
    ram.high[0x301] = 0x00;
    ram.Apply(engine);
    CHECK(ram.high[0x201] == 0x00);
    CHECK(ram.high[0x301] == 0x02);

    // Conditions see the writes of earlier cheats in the same frame (index order).
    CheatEngine ordered;
    WorkRam ram2;
    ordered.Set(0, true, "16000400 BEEF");
    ordered.Set(1, true, "D6000400 BEEF+36000500 0042");
    ram2.Apply(ordered);
    CHECK(ram2.high[0x500] == 0x42);

    // A conditional outside work RAM is false.
    CheatEngine outside;
    WorkRam ram3;
    outside.Set(0, true, "D5C00000 0000+16000600 0001");
    ram3.Apply(outside);
    CHECK(ram3.AllZero());
}

TEST_CASE("cheats: disabled and reset stop writing", "[cheats]") {
    CheatEngine engine;
    WorkRam ram;
    engine.Set(0, false, "16000000 1234");
    CHECK(engine.ActiveCount() == 0);
    ram.Apply(engine);
    CHECK(ram.AllZero());

    engine.Set(0, true, "16000000 1234");
    engine.Set(5, true, "10200000 5678");
    CHECK(engine.ActiveCount() == 2);
    ram.Apply(engine);
    CHECK(ram.high[0] == 0x12);
    CHECK(ram.low[0] == 0x56);

    // Disabling one cheat: the game's own value stays (no write).
    engine.Set(0, false, "16000000 1234");
    ram.high[0] = 0x99;
    ram.Apply(engine);
    CHECK(ram.high[0] == 0x99);
    CHECK(engine.ActiveCount() == 1);

    // A code that does not parse empties the slot.
    engine.Set(5, true, "zz");
    CHECK(engine.ActiveCount() == 0);

    engine.Set(5, true, "10200000 5678");
    engine.Reset();
    CHECK(engine.ActiveCount() == 0);
    ram.low[0] = 0x01;
    ram.Apply(engine);
    CHECK(ram.low[0] == 0x01);
}

TEST_CASE("cheats: out-of-WRAM address ignored with no write", "[cheats]") {
    CheatEngine engine;
    WorkRam ram;
    // VDP1 VRAM, the BIOS ROM, just past Work RAM Low, below Work RAM High
    for (const char *code : {"15C00000 FFFF", "10000000 FFFF", "10300000 FFFF", "35FFFFFF 00FF", "105FFFFE FFFF"}) {
        INFO(code);
        const auto ops = ParseCheat(code);
        REQUIRE(ops.has_value());
        CHECK_FALSE(brimir::CheatAddressInWorkRam((*ops)[0].address));
        engine.Set(0, true, code);
        CHECK(engine.ActiveCount() == 0);
        ram.Apply(engine);
        CHECK(ram.AllZero());
    }
    // Master codes and ignored sub-codes write nothing either.
    engine.Set(0, true, "F6000924+C305+B6002800+0000");
    engine.Set(1, true, "160B374E+00??");
    ram.Apply(engine);
    CHECK(ram.AllZero());
    CHECK(engine.ActiveCount() == 0);
}

TEST_CASE("cheats: sample .cht fixture parses", "[cheats]") {
    const fs::path dir = fs::path(BRIMIR_SOURCE_DIR) / "tests" / "fixtures" / "cht";
    REQUIRE(fs::is_directory(dir));
    const SweepResult r = Sweep(dir);
    PrintSweep("cht fixture", r);
    for (const std::string &failure : r.failures) {
        UNSCOPED_INFO(failure);
    }
    CHECK(r.failures.empty());
    CHECK(r.files >= 1);
    CHECK(r.retroArchHandled >= 1);
    CHECK(r.master >= 1);
    CHECK(r.malformed >= 1);
}

// Timing only (wall clock): hidden, run with "[cheats-bench]".
TEST_CASE("cheats: per-frame cost of 20 cheats", "[.][cheats-bench]") {
    CheatEngine engine;
    WorkRam ram;
    for (unsigned i = 0; i < 20; ++i) {
        char code[64];
        std::snprintf(code, sizeof(code), "D6%06X+0000+16%06X+270F+36%06X+0063", 0x1000 + i * 8, 0x2000 + i * 8,
                      0x3001 + i * 8);
        engine.Set(i, true, code);
    }
    REQUIRE(engine.ActiveCount() == 20);
    constexpr int kFrames = 2000;
    const auto start = std::chrono::steady_clock::now();
    for (int f = 0; f < kFrames; ++f) {
        ram.Apply(engine);
    }
    const double us =
        std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - start).count() / kFrames;
    std::printf("cheats: 20 cheats (60 sub-codes) apply in %.3f us per frame\n", us);
    CHECK(us < 50.0);
}

// RetroArch's Saturn database (libretro-database, cht/Sega - Saturn): every non-empty code parses. Not committed;
// point BRIMIR_CHT_DIR at the downloaded folder. BRIMIR_CHT_NOTES=<file> writes the ignored sub-codes and the
// sub-codes Beetle Saturn's parser reads differently.
TEST_CASE("cheats: RetroArch Saturn database parses", "[.cheat-db]") {
    const char *dir = std::getenv("BRIMIR_CHT_DIR");
    if (dir == nullptr || !fs::is_directory(dir)) {
        SKIP("BRIMIR_CHT_DIR is not a directory");
    }
    const SweepResult r = Sweep(dir);
    PrintSweep("cheat database", r);
    for (const std::string &failure : r.failures) {
        UNSCOPED_INFO(failure);
    }
    CHECK(r.failures.empty());
    CHECK(r.files > 0);
}
