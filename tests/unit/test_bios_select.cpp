#include "catch_amalgamated.hpp"

#include <brimir/bios_select.hpp>
#include <brimir/core_wrapper.hpp>
#include <brimir/lockstep.hpp>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <map>
#include <optional>
#include <string>
#include <vector>

using brimir::BiosCandidatesForRegion;
using brimir::ChooseAutoBios;
using brimir::CoreWrapper;
using brimir::DiscRegion;
using brimir::DiscRegionFromAreaCodes;

namespace {

// ymir::media::AreaCode bits
constexpr uint16_t kJ = 1u << 0x1;
constexpr uint16_t kT = 1u << 0x2;
constexpr uint16_t kU = 1u << 0x4;
constexpr uint16_t kE = 1u << 0xC;
constexpr uint16_t kK = 1u << 0x6;
constexpr uint16_t kB = 1u << 0x5;
constexpr uint16_t kA = 1u << 0xA;
constexpr uint16_t kL = 1u << 0xD;

constexpr const char* kJp101 = "Sega Saturn BIOS v1.01 (JAP).bin";
constexpr const char* kJp100 = "Sega Saturn BIOS v1.00 (JAP).bin";
constexpr const char* kUs = "mpr-17933.bin";

// The 0.7.0 order (libretro.cpp before 0.7.1).
const std::vector<std::string> kOldOrder = {
    "Sega Saturn BIOS v1.01 (JAP).bin", "Sega Saturn BIOS v1.00 (JAP).bin", "sega_101.bin",
    "mpr-17933.bin",                    "sega_100.bin",                     "Sega Saturn BIOS (EUR).bin",
};

// A fake system folder: file name -> the region the IPL ROM database gives it. Records the files tried.
struct FakeBiosFolder {
    std::map<std::string, DiscRegion> files;
    std::vector<std::string> tried;

    brimir::BiosIdentify Identify() {
        return [this](const std::string& name) -> std::optional<DiscRegion> {
            tried.push_back(name);
            const auto it = files.find(name);
            if (it == files.end()) {
                return std::nullopt;
            }
            return it->second;
        };
    }
};

std::filesystem::path Fixture(const char* name) {
    return std::filesystem::path(__FILE__).parent_path().parent_path() / "fixtures" / name;
}

// A temp folder removed at scope exit.
struct TempDir {
    std::filesystem::path path;
    explicit TempDir(const char* name) : path(std::filesystem::temp_directory_path() / name) {
        std::filesystem::remove_all(path);
        std::filesystem::create_directories(path);
    }
    ~TempDir() {
        std::error_code ec;
        std::filesystem::remove_all(path, ec);
    }
};

// A 16-sector 2048-byte ISO whose first sector holds a Saturn header with the given area code symbols.
std::filesystem::path WriteSaturnIso(const std::filesystem::path& dir, const std::string& name, const char* areas) {
    std::vector<char> image(16 * 2048, 0);
    const std::string fields = "SEGA SEGASATURN SEGA TP T-000   T-99999   V1.000  19950101CD-1/1  ";
    std::copy(fields.begin(), fields.end(), image.begin());
    std::string area(areas);
    area.resize(16, ' ');
    std::copy(area.begin(), area.end(), image.begin() + 0x40);
    const auto path = dir / name;
    std::ofstream out(path, std::ios::binary);
    out.write(image.data(), static_cast<std::streamsize>(image.size()));
    return path;
}

} // namespace

TEST_CASE("bios auto: disc area codes select the region", "[bios][auto]") {
    CHECK(DiscRegionFromAreaCodes(0) == DiscRegion::Unknown);
    CHECK(DiscRegionFromAreaCodes(kJ) == DiscRegion::Japan);
    // Japanese-hardware regions only (J, T Asia NTSC, K Korea): the JP BIOS
    CHECK(DiscRegionFromAreaCodes(kJ | kT) == DiscRegion::Japan);
    CHECK(DiscRegionFromAreaCodes(kJ | kT | kK) == DiscRegion::Japan);
    CHECK(DiscRegionFromAreaCodes(kT) == DiscRegion::Japan);
    CHECK(DiscRegionFromAreaCodes(kK) == DiscRegion::Japan);
    CHECK(DiscRegionFromAreaCodes(kT | kK) == DiscRegion::Japan);
    CHECK(DiscRegionFromAreaCodes(kU) == DiscRegion::UsEu);
    CHECK(DiscRegionFromAreaCodes(kE) == DiscRegion::UsEu);
    CHECK(DiscRegionFromAreaCodes(kU | kE) == DiscRegion::UsEu);
    CHECK(DiscRegionFromAreaCodes(kT | kU | kE) == DiscRegion::UsEu);
    // Multi-region with a western region: the US/EU BIOS
    CHECK(DiscRegionFromAreaCodes(kJ | kT | kU | kE) == DiscRegion::UsEu);
    CHECK(DiscRegionFromAreaCodes(kJ | kE) == DiscRegion::UsEu);
    CHECK(DiscRegionFromAreaCodes(kJ | kB) == DiscRegion::UsEu);
    CHECK(DiscRegionFromAreaCodes(kJ | kA) == DiscRegion::UsEu);
    CHECK(DiscRegionFromAreaCodes(kJ | kL) == DiscRegion::UsEu);
    CHECK(DiscRegionFromAreaCodes(kK | kB) == DiscRegion::UsEu);
    // Western regions only
    CHECK(DiscRegionFromAreaCodes(kB) == DiscRegion::UsEu);
    CHECK(DiscRegionFromAreaCodes(kA | kL) == DiscRegion::UsEu);
}

TEST_CASE("bios auto: Japanese disc picks the JP BIOS", "[bios][auto]") {
    FakeBiosFolder folder;
    folder.files = {{kJp101, DiscRegion::Japan}, {kUs, DiscRegion::UsEu}};
    CHECK(ChooseAutoBios(DiscRegion::Japan, folder.Identify()) == std::optional<std::string>(kJp101));

    // Only v1.00
    FakeBiosFolder v100;
    v100.files = {{kJp100, DiscRegion::Japan}, {kUs, DiscRegion::UsEu}};
    CHECK(ChooseAutoBios(DiscRegion::Japan, v100.Identify()) == std::optional<std::string>(kJp100));

    // Japanese dumps under Mednafen's names
    FakeBiosFolder mednafen;
    mednafen.files = {{"sega_101.bin", DiscRegion::Japan}, {kUs, DiscRegion::UsEu}};
    CHECK(ChooseAutoBios(DiscRegion::Japan, mednafen.Identify()) == std::optional<std::string>("sega_101.bin"));

    const auto candidates = BiosCandidatesForRegion(DiscRegion::Japan);
    REQUIRE(candidates.size() == kOldOrder.size());
    CHECK(candidates.front() == kJp101);
}

TEST_CASE("bios auto: US disc picks the US/EU BIOS", "[bios][auto]") {
    FakeBiosFolder folder;
    folder.files = {{kJp101, DiscRegion::Japan}, {kUs, DiscRegion::UsEu}};
    CHECK(ChooseAutoBios(DiscRegion::UsEu, folder.Identify()) == std::optional<std::string>(kUs));

    // sega_101.bin is a Japanese dump (Mednafen's name), skipped for a US disc (hash and name agree here; the
    // "name and hash disagree" case below tells them apart)
    FakeBiosFolder mednafen;
    mednafen.files = {{"sega_101.bin", DiscRegion::Japan}, {kJp101, DiscRegion::Japan}, {kUs, DiscRegion::UsEu}};
    CHECK(ChooseAutoBios(DiscRegion::UsEu, mednafen.Identify()) == std::optional<std::string>(kUs));

    // EU file
    FakeBiosFolder eu;
    eu.files = {{kJp101, DiscRegion::Japan}, {"Sega Saturn BIOS (EUR).bin", DiscRegion::UsEu}};
    CHECK(ChooseAutoBios(DiscRegion::UsEu, eu.Identify()) ==
          std::optional<std::string>("Sega Saturn BIOS (EUR).bin"));

    // A file the database does not know is taken at its name's word
    FakeBiosFolder unknownHash;
    unknownHash.files = {{kJp101, DiscRegion::Japan}, {kUs, DiscRegion::Unknown}};
    CHECK(ChooseAutoBios(DiscRegion::UsEu, unknownHash.Identify()) == std::optional<std::string>(kUs));

    const auto candidates = BiosCandidatesForRegion(DiscRegion::UsEu);
    REQUIRE(candidates.size() == kOldOrder.size());
    CHECK(candidates.front() == kUs);
}

TEST_CASE("bios auto: fallback when the matching region is missing", "[bios][auto]") {
    // US disc, only a Japanese BIOS
    FakeBiosFolder jpOnly;
    jpOnly.files = {{kJp100, DiscRegion::Japan}};
    CHECK(ChooseAutoBios(DiscRegion::UsEu, jpOnly.Identify()) == std::optional<std::string>(kJp100));

    // Japanese disc, only a US BIOS
    FakeBiosFolder usOnly;
    usOnly.files = {{kUs, DiscRegion::UsEu}};
    CHECK(ChooseAutoBios(DiscRegion::Japan, usOnly.Identify()) == std::optional<std::string>(kUs));

    // Japanese disc, a US dump under a Japanese name and a real US one: the first that loads (candidate order)
    FakeBiosFolder misnamed;
    misnamed.files = {{kJp101, DiscRegion::UsEu}, {kUs, DiscRegion::UsEu}};
    CHECK(ChooseAutoBios(DiscRegion::Japan, misnamed.Identify()) == std::optional<std::string>(kJp101));

    // Nothing available
    FakeBiosFolder empty;
    CHECK_FALSE(ChooseAutoBios(DiscRegion::Japan, empty.Identify()).has_value());
    CHECK_FALSE(ChooseAutoBios(DiscRegion::UsEu, empty.Identify()).has_value());
    CHECK_FALSE(ChooseAutoBios(DiscRegion::Unknown, empty.Identify()).has_value());
    CHECK(empty.tried.size() == 3 * kOldOrder.size()); // every candidate tried, each time
}

TEST_CASE("bios auto: no content keeps the old order", "[bios][auto]") {
    CHECK(BiosCandidatesForRegion(DiscRegion::Unknown) == kOldOrder);

    // Every file present: the first one, without trying the others (as 0.7.0)
    FakeBiosFolder all;
    for (const auto& name : kOldOrder) {
        all.files[name] = brimir::BiosRegionFromFileName(name);
    }
    CHECK(ChooseAutoBios(DiscRegion::Unknown, all.Identify()) == std::optional<std::string>(kJp101));
    CHECK(all.tried == std::vector<std::string>{kJp101});

    FakeBiosFolder usOnly;
    usOnly.files = {{kUs, DiscRegion::UsEu}};
    CHECK(ChooseAutoBios(DiscRegion::Unknown, usOnly.Identify()) == std::optional<std::string>(kUs));

    // Both regions' candidate lists hold the same files
    auto sorted = [](std::vector<std::string> v) {
        std::sort(v.begin(), v.end());
        return v;
    };
    CHECK(sorted(BiosCandidatesForRegion(DiscRegion::Japan)) == sorted(kOldOrder));
    CHECK(sorted(BiosCandidatesForRegion(DiscRegion::UsEu)) == sorted(kOldOrder));
}

TEST_CASE("bios auto: the content's area codes are read before it loads", "[bios][auto]") {
    TempDir dir("brimir_bios_auto_area");
    CoreWrapper core;
    REQUIRE(core.Initialize());

    const auto jp = WriteSaturnIso(dir.path, "jp.iso", "J");
    const auto multi = WriteSaturnIso(dir.path, "multi.iso", "JTUE");
    CHECK(core.ReadContentAreaCodes(jp.string().c_str()) == kJ);
    CHECK(core.ReadContentAreaCodes(multi.string().c_str()) == (kJ | kT | kU | kE));

    // M3U: the disc LoadGame would load (the first one, or the frontend's initial image)
    const auto m3u = dir.path / "game.m3u";
    {
        std::ofstream out(m3u);
        out << "# comment\njp.iso\nmulti.iso\n";
    }
    CHECK(core.ReadContentAreaCodes(m3u.string().c_str()) == kJ);
    core.SetInitialDisc(1, multi.string().c_str());
    CHECK(core.ReadContentAreaCodes(m3u.string().c_str()) == (kJ | kT | kU | kE));

    // Unreadable content: no area code
    CHECK(core.ReadContentAreaCodes((dir.path / "missing.iso").string().c_str()) == 0);
    CHECK(core.ReadContentAreaCodes(nullptr) == 0);
}

TEST_CASE("bios auto: the core loads the BIOS of the disc's region", "[bios][auto]") {
    if (!std::filesystem::exists(Fixture(kJp101)) || !std::filesystem::exists(Fixture(kUs)) ||
        !std::filesystem::exists(Fixture("sega_101.bin"))) {
        SKIP("JP/US BIOS fixtures missing");
    }
    TempDir dir("brimir_bios_auto_load");
    const auto system = dir.path / "system";
    std::filesystem::create_directories(system);
    const auto usDisc = WriteSaturnIso(dir.path, "us.iso", "UE");
    const auto jpDisc = WriteSaturnIso(dir.path, "jp.iso", "J");
    const auto multiDisc = WriteSaturnIso(dir.path, "multi.iso", "JTUE");
    const auto asiaDisc = WriteSaturnIso(dir.path, "asia.iso", "JT");
    const auto copy = [&](const char* name) { std::filesystem::copy_file(Fixture(name), system / name); };

    auto load = [&](const std::filesystem::path* disc) {
        CoreWrapper core;
        REQUIRE(core.Initialize());
        const std::string content = disc ? disc->string() : std::string();
        const DiscRegion region = DiscRegionFromAreaCodes(core.ReadContentAreaCodes(disc ? content.c_str() : nullptr));
        const std::string chosen = core.LoadAutoIPL(system.string().c_str(), region);
        CHECK(core.IsIPLLoaded() == !chosen.empty());
        if (!chosen.empty()) {
            const DiscRegion loaded = core.GetLoadedIPLRegion();
            return std::make_pair(chosen, loaded);
        }
        return std::make_pair(chosen, DiscRegion::Unknown);
    };

    SECTION("both regions present") {
        copy(kJp101);
        copy(kUs);
        copy("sega_101.bin"); // a Japanese dump the 0.7.0 list labels US
        CHECK(load(&usDisc) == std::make_pair(std::string(kUs), DiscRegion::UsEu));
        CHECK(load(&jpDisc) == std::make_pair(std::string(kJp101), DiscRegion::Japan));
        CHECK(load(&multiDisc) == std::make_pair(std::string(kUs), DiscRegion::UsEu));
        CHECK(load(&asiaDisc) == std::make_pair(std::string(kJp101), DiscRegion::Japan));
        CHECK(load(nullptr) == std::make_pair(std::string(kJp101), DiscRegion::Japan));
    }
    SECTION("Japanese dumps under Mednafen's names and a US BIOS") {
        copy("sega_101.bin");
        copy(kUs);
        CHECK(load(&jpDisc) == std::make_pair(std::string("sega_101.bin"), DiscRegion::Japan));
        CHECK(load(&usDisc) == std::make_pair(std::string(kUs), DiscRegion::UsEu));
    }
    SECTION("only the Japanese BIOS") {
        copy(kJp101);
        CHECK(load(&usDisc) == std::make_pair(std::string(kJp101), DiscRegion::Japan));
    }
    SECTION("only the US BIOS") {
        copy(kUs);
        CHECK(load(&jpDisc) == std::make_pair(std::string(kUs), DiscRegion::UsEu));
        CHECK(load(nullptr) == std::make_pair(std::string(kUs), DiscRegion::UsEu));
    }
    SECTION("none") {
        CHECK(load(&usDisc).first.empty());
    }
}

TEST_CASE("bios auto: the hash decides when name and hash disagree", "[bios][auto]") {
    // US disc: a US/EU dump under a Japanese name is chosen over a real Japanese one (by name alone: neither file is
    // US/EU, so the fallback would take the first candidate that loads, kJp101)
    FakeBiosFolder usDump;
    usDump.files = {{kJp101, DiscRegion::Japan}, {kJp100, DiscRegion::UsEu}};
    CHECK(ChooseAutoBios(DiscRegion::UsEu, usDump.Identify()) == std::optional<std::string>(kJp100));

    // Japanese disc: a Japanese dump under the EUR name is chosen over a real US one (by name alone: kUs)
    FakeBiosFolder jpDump;
    jpDump.files = {{kUs, DiscRegion::UsEu}, {"Sega Saturn BIOS (EUR).bin", DiscRegion::Japan}};
    CHECK(ChooseAutoBios(DiscRegion::Japan, jpDump.Identify()) ==
          std::optional<std::string>("Sega Saturn BIOS (EUR).bin"));
}

TEST_CASE("bios auto: selection runs on every content load", "[bios][auto]") {
    // Auto (and the default, nullptr) chooses on each load; an explicit choice only while no BIOS is loaded
    CHECK(brimir::ShouldSelectBios("auto", false));
    CHECK(brimir::ShouldSelectBios("auto", true));
    CHECK(brimir::ShouldSelectBios(nullptr, true));
    CHECK(brimir::ShouldSelectBios("jp_101", false));
    CHECK_FALSE(brimir::ShouldSelectBios("jp_101", true));
    CHECK_FALSE(brimir::ShouldSelectBios("us_100", true));
}

namespace {

// A content folder (system, saves) with synthetic discs, for LoadGame.
struct RegionContent {
    TempDir dir;
    std::filesystem::path system;
    std::filesystem::path saves;

    explicit RegionContent(const char* name) : dir(name), system(dir.path / "system"), saves(dir.path / "saves") {
        std::filesystem::create_directories(system);
        std::filesystem::create_directories(saves);
    }
    std::filesystem::path Disc(const char* areas) {
        return WriteSaturnIso(dir.path, std::string("disc_") + areas + ".iso", areas);
    }
    void AddBios(const char* name) {
        std::filesystem::copy_file(Fixture(name), system / name, std::filesystem::copy_options::overwrite_existing);
    }
    // Loads the BIOS and the disc as retro_load_game does with brimir_bios = auto; returns the SMPC area code.
    uint8_t Load(CoreWrapper& core, const std::filesystem::path& disc) {
        const std::string path = disc.string();
        const std::string chosen = core.LoadAutoIPL(
            system.string().c_str(), DiscRegionFromAreaCodes(core.ReadContentAreaCodes(path.c_str())));
        REQUIRE_FALSE(chosen.empty());
        REQUIRE(core.LoadGame(path.c_str(), saves.string().c_str(), system.string().c_str()));
        brimir::ApplyFixedRtc(core, brimir::kDeterministicRtcDate); // same clock for every run (FrameHash)
        return core.GetSaturn()->SMPC.GetAreaCode();
    }
};

uint64_t FrameHash(const CoreWrapper& core) {
    const auto* pixels = static_cast<const uint8_t*>(core.GetFramebuffer());
    uint64_t hash = 1469598103934665603ull;
    for (unsigned y = 0; y < core.GetFramebufferHeight(); ++y) {
        const uint8_t* row = pixels + static_cast<size_t>(y) * core.GetFramebufferPitch();
        for (unsigned x = 0; x < core.GetFramebufferWidth() * 4; ++x) {
            hash = (hash ^ row[x]) * 1099511628211ull;
        }
    }
    return hash;
}

} // namespace

TEST_CASE("bios auto: region autodetection prefers the BIOS's regions", "[bios][auto][region]") {
    using Order = std::vector<uint8_t>;
    CHECK(brimir::PreferredAreaOrder(DiscRegion::UsEu) == Order{0x4, 0xC, 0x1, 0x2});
    CHECK(brimir::PreferredAreaOrder(DiscRegion::Japan) == Order{0x1, 0x2, 0x4, 0xC});
    CHECK(brimir::PreferredAreaOrder(DiscRegion::Unknown) == Order{0x4, 0x1, 0xC, 0x2}); // Ymir's default

    if (!std::filesystem::exists(Fixture(kJp101)) || !std::filesystem::exists(Fixture(kUs))) {
        SKIP("JP/US BIOS fixtures missing");
    }
    RegionContent content("brimir_bios_auto_region");
    auto area = [&](const char* areas) {
        CoreWrapper core;
        REQUIRE(core.Initialize());
        return content.Load(core, content.Disc(areas));
    };

    SECTION("US/EU BIOS: North America, then Europe") {
        content.AddBios(kUs);
        CHECK(area("TU") == 0x4);   // Sega Rally Championship (USA); 0.7.0: 0x2 (Asia NTSC)
        CHECK(area("JTU") == 0x4);  // Panzer Dragoon II Zwei (USA); 0.7.0: 0x1
        CHECK(area("JTUE") == 0x4);
        CHECK(area("U") == 0x4);
        CHECK(area("JE") == 0xC);
        CHECK(area("E") == 0xC);
        CHECK(area("J") == 0x1); // fallback BIOS: the disc's only region
    }
    SECTION("Japanese BIOS: Japan, then Asia NTSC") {
        content.AddBios(kJp101);
        CHECK(area("JT") == 0x1);
        CHECK(area("J") == 0x1);
        CHECK(area("T") == 0x2);
        CHECK(area("TU") == 0x2);
        CHECK(area("U") == 0x4);
    }
    SECTION("both BIOSes: the disc's BIOS and area code agree") {
        content.AddBios(kUs);
        content.AddBios(kJp101);
        CHECK(area("TU") == 0x4);
        CHECK(area("JTU") == 0x4);
        CHECK(area("JT") == 0x1);
        CHECK(area("T") == 0x2);
    }
    SECTION("autodetection disabled: the area code is left alone") {
        content.AddBios(kUs);
        CoreWrapper core;
        REQUIRE(core.Initialize());
        core.SetAutodetectRegion(false);
        CHECK(content.Load(core, content.Disc("TU")) == 0x1); // the SMPC's power-on default
    }
}

TEST_CASE("bios auto: a second content load switches the BIOS", "[bios][auto]") {
    if (!std::filesystem::exists(Fixture(kJp101)) || !std::filesystem::exists(Fixture(kUs))) {
        SKIP("JP/US BIOS fixtures missing");
    }
    RegionContent content("brimir_bios_auto_reload");
    content.AddBios(kUs);
    content.AddBios(kJp101);
    const auto usDisc = content.Disc("U");
    const auto jpDisc = content.Disc("J");
    constexpr int kFrames = 300;

    // Reference: a fresh core with the Japanese disc
    CoreWrapper fresh;
    REQUIRE(fresh.Initialize());
    REQUIRE(content.Load(fresh, jpDisc) == 0x1);
    REQUIRE(fresh.GetLoadedIPLRegion() == DiscRegion::Japan);
    for (int i = 0; i < kFrames; ++i) {
        fresh.RunFrame();
    }
    const uint64_t expected = FrameHash(fresh);
    fresh.UnloadGame();

    // The check tells the BIOSes apart: the US/EU BIOS shows another frame for the same disc
    {
        CoreWrapper other;
        REQUIRE(other.Initialize());
        REQUIRE(other.LoadIPLFromFile((content.system / kUs).string().c_str()));
        const std::string path = jpDisc.string();
        REQUIRE(other.LoadGame(path.c_str(), content.saves.string().c_str(), content.system.string().c_str()));
        brimir::ApplyFixedRtc(other, brimir::kDeterministicRtcDate);
        for (int i = 0; i < kFrames; ++i) {
            other.RunFrame();
        }
        CHECK(FrameHash(other) != expected);
        other.UnloadGame();
    }
    std::filesystem::remove(content.system / "brimir_saturn_rtc_jp.smpc");
    std::filesystem::remove(content.system / "brimir_saturn_rtc_us_eu.smpc");

    // The same core: US disc (US/EU BIOS), unload, Japanese disc (Japanese BIOS)
    CoreWrapper core;
    REQUIRE(core.Initialize());
    CHECK(content.Load(core, usDisc) == 0x4);
    CHECK(core.GetLoadedIPLRegion() == DiscRegion::UsEu);
    for (int i = 0; i < 60; ++i) {
        core.RunFrame();
    }
    core.UnloadGame();
    CHECK(std::filesystem::exists(content.system / "brimir_saturn_rtc_us_eu.smpc"));

    CHECK(content.Load(core, jpDisc) == 0x1);
    CHECK(core.GetLoadedIPLRegion() == DiscRegion::Japan);
    for (int i = 0; i < kFrames; ++i) {
        core.RunFrame();
    }
    CHECK(FrameHash(core) == expected); // the Japanese BIOS booted from its reset vector
    core.UnloadGame();
    CHECK(std::filesystem::exists(content.system / "brimir_saturn_rtc_jp.smpc"));
}
