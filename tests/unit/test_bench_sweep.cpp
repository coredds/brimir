// brimir_bench's compatibility sweep helpers (tools/bench_sweep.hpp, header-only): the input script parser, the PNG
// encoder, the hang and silence tracker and the summary's JSON text.
#include "catch_amalgamated.hpp"

#include "../../tools/bench_sweep.hpp"

#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>

using namespace brimir_tools;

namespace {

constexpr uint16_t Bit(unsigned id) {
    return static_cast<uint16_t>(1u << id);
}

std::vector<int16_t> Constant(size_t stereoFrames, int16_t value) {
    return std::vector<int16_t>(stereoFrames * 2, value);
}

} // namespace

TEST_CASE("bench sweep: input script parses the brief's example", "[bench][sweep]") {
    InputScript script;
    std::string error;
    REQUIRE(ParseInputScript("start:120/600,a:30/900", script, error));
    REQUIRE(script.size() == 2);
    CHECK(script[0].joypadId == 3); // Start
    CHECK(script[0].hold == 120);
    CHECK(script[0].period == 600);
    CHECK(script[1].joypadId == 0); // Saturn A is RetroPad B (id 0) in the Control Pad layout
    CHECK(script[1].hold == 30);
    CHECK(script[1].period == 900);
}

TEST_CASE("bench sweep: input script accepts every Saturn Control Pad button, in any case", "[bench][sweep]") {
    for (const char* name : {"start", "a", "b", "c", "x", "y", "z", "l", "r", "up", "down", "left", "right", "START", "Up"}) {
        InputScript script;
        std::string error;
        CHECK(ParseInputScript(std::string(name) + ":1/2", script, error));
    }
}

TEST_CASE("bench sweep: input script rejects malformed specs", "[bench][sweep]") {
    for (const char* spec : {"", "start", "start:120", "start:/600", "start:120/", "start:0/600", "start:700/600",
                             "start:-1/600", "start:1x/600", "select:1/2", "l2:1/2", ":1/2", "a:1/2,", ",a:1/2",
                             "a:1/2,,b:1/2", "a/1:2", "a:1/2/3", "a:99999999999/99999999999"}) {
        InputScript script;
        std::string error;
        CHECK_FALSE(ParseInputScript(spec, script, error));
        CHECK_FALSE(error.empty());
    }
}

TEST_CASE("bench sweep: JoypadAt holds a button at the start of each period", "[bench][sweep]") {
    InputScript script;
    std::string error;
    REQUIRE(ParseInputScript("start:120/600,a:30/900", script, error));
    CHECK(JoypadAt(script, 0) == (Bit(3) | Bit(0)));
    CHECK(JoypadAt(script, 29) == (Bit(3) | Bit(0)));
    CHECK(JoypadAt(script, 30) == Bit(3));
    CHECK(JoypadAt(script, 119) == Bit(3));
    CHECK(JoypadAt(script, 120) == 0);
    CHECK(JoypadAt(script, 599) == 0);
    CHECK(JoypadAt(script, 600) == Bit(3));
    CHECK(JoypadAt(script, 900) == Bit(0)); // A's second period starts; Start's (600) ended at 719
    CHECK(JoypadAt(script, 899) == 0);
    CHECK(JoypadAt(script, 1200) == Bit(3));
}

TEST_CASE("bench sweep: hold equal to period keeps the button down", "[bench][sweep]") {
    InputScript script;
    std::string error;
    REQUIRE(ParseInputScript("right:5/5", script, error));
    for (int frame : {0, 4, 5, 1000}) {
        CHECK(JoypadAt(script, frame) == Bit(7));
    }
}

TEST_CASE("bench sweep: EncodePng writes a valid PNG container", "[bench][sweep]") {
    const uint32_t w = 3, h = 2;
    const uint32_t pitch = w * 4 + 8; // padded rows
    std::vector<uint8_t> pixels(static_cast<size_t>(pitch) * h, 0);
    // XRGB8888 little-endian: B, G, R, X
    pixels[0] = 0x30; pixels[1] = 0x20; pixels[2] = 0x10;

    const std::vector<uint8_t> png = EncodePng(w, h, pixels.data(), pitch);
    REQUIRE(png.size() > 8 + 25);
    const uint8_t signature[8] = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n'};
    CHECK(std::equal(signature, signature + 8, png.begin()));
    // IHDR: length 13, type, width, height, 8-bit RGB
    CHECK(png[11] == 13);
    CHECK(std::string(png.begin() + 12, png.begin() + 16) == "IHDR");
    CHECK(png[19] == w);
    CHECK(png[23] == h);
    CHECK(png[24] == 8);
    CHECK(png[25] == 2);
    // IEND with its CRC ends the file
    const std::string tail(png.end() - 8, png.end() - 4);
    CHECK(tail == "IEND");
    CHECK(png[png.size() - 4] == 0xAE);
    CHECK(png[png.size() - 3] == 0x42);
    CHECK(png[png.size() - 2] == 0x60);
    CHECK(png[png.size() - 1] == 0x82);
    // The first pixel's R, G, B follow the filter byte in the stored block (zlib header 2 + block header 5)
    const std::string idat(png.begin() + 33 + 4, png.begin() + 33 + 8);
    CHECK(idat == "IDAT");
    const size_t data = 33 + 8 + 2 + 5;
    CHECK(png[data] == 0); // filter
    CHECK(png[data + 1] == 0x10);
    CHECK(png[data + 2] == 0x20);
    CHECK(png[data + 3] == 0x30);
}

TEST_CASE("bench sweep: EncodePng splits large frames into stored blocks and refuses empty frames", "[bench][sweep]") {
    const uint32_t w = 320, h = 224;
    std::vector<uint32_t> pixels(static_cast<size_t>(w) * h, 0x00FF8040u);
    const std::vector<uint8_t> png = EncodePng(w, h, pixels.data(), w * 4);
    CHECK(png.size() > static_cast<size_t>(w) * h * 3);
    CHECK(EncodePng(0, h, pixels.data(), w * 4).empty());
    CHECK(EncodePng(w, 0, pixels.data(), w * 4).empty());
    CHECK(EncodePng(w, h, nullptr, w * 4).empty());
}

TEST_CASE("bench sweep: Crc32 matches the standard check value", "[bench][sweep]") {
    const char text[] = "123456789";
    CHECK(Crc32(reinterpret_cast<const uint8_t*>(text), 9) == 0xCBF43926u);
}

TEST_CASE("bench sweep: tracker finds the longest run of identical frames", "[bench][sweep]") {
    SweepTracker tracker;
    const auto audio = Constant(4, 1000);
    for (const uint64_t hash : {1, 2, 2, 3, 3, 3, 3, 4, 4, 3}) {
        tracker.AddFrame(hash, audio.data(), 4);
    }
    CHECK(tracker.Stats().frames == 10);
    CHECK(tracker.Stats().identicalStart == 3);
    CHECK(tracker.Stats().identicalLength == 4);
}

TEST_CASE("bench sweep: tracker keeps the first of equally long identical runs and handles one frame", "[bench][sweep]") {
    SweepTracker tracker;
    const auto audio = Constant(1, 0);
    tracker.AddFrame(7, audio.data(), 1);
    CHECK(tracker.Stats().identicalStart == 0);
    CHECK(tracker.Stats().identicalLength == 1);
    tracker.AddFrame(7, audio.data(), 1);
    tracker.AddFrame(8, audio.data(), 1);
    tracker.AddFrame(8, audio.data(), 1);
    CHECK(tracker.Stats().identicalStart == 0);
    CHECK(tracker.Stats().identicalLength == 2);
}

TEST_CASE("bench sweep: tracker measures audio peak and the longest silent stretch", "[bench][sweep]") {
    SweepTracker tracker;
    const auto silent = Constant(4, 16);          // |s| <= 16 is silent
    const auto quiet = Constant(4, -16);
    const auto loud = Constant(4, 17);            // one above the threshold is sound
    const auto peak = Constant(4, -32768);
    uint64_t hash = 0;
    const auto add = [&](const std::vector<int16_t>& pcm) { tracker.AddFrame(hash++, pcm.data(), 4); };
    add(loud);    // 0
    add(silent);  // 1
    add(quiet);   // 2
    add(loud);    // 3
    add(silent);  // 4
    add(silent);  // 5
    add(silent);  // 6
    add(peak);    // 7
    add(silent);  // 8
    CHECK(tracker.Stats().audioPeak == 32768);
    CHECK(tracker.Stats().silentStart == 4);
    CHECK(tracker.Stats().silentLength == 3);
}

TEST_CASE("bench sweep: a frame without samples counts as silent; all-silent run is one stretch", "[bench][sweep]") {
    SweepTracker tracker;
    for (uint64_t i = 0; i < 5; ++i) {
        tracker.AddFrame(i, nullptr, 0);
    }
    CHECK(tracker.Stats().audioPeak == 0);
    CHECK(tracker.Stats().silentStart == 0);
    CHECK(tracker.Stats().silentLength == 5);
}

TEST_CASE("bench sweep: summary JSON carries every field", "[bench][sweep]") {
    SweepSummary summary;
    summary.stats.frames = 1800;
    summary.stats.identicalStart = 5;
    summary.stats.identicalLength = 300;
    summary.stats.audioPeak = 12345;
    summary.stats.silentStart = 10;
    summary.stats.silentLength = 60;
    summary.hasPC = true;
    summary.masterPC = 0x06004000;
    summary.slavePC = 0x06001234;
    summary.wallSeconds = 12.3456;
    summary.bios = "mpr-17933.bin";
    summary.cartridge = "none";
    const std::string json = FormatSweepSummary(summary);
    CHECK(json.find("\"frames_run\": 1800") != std::string::npos);
    CHECK(json.find("\"load_error\": null") != std::string::npos);
    CHECK(json.find("\"identical_frames\": {\"start\": 5, \"length\": 300}") != std::string::npos);
    CHECK(json.find("\"peak\": 12345") != std::string::npos);
    CHECK(json.find("\"longest_silent_frames\": 60") != std::string::npos);
    CHECK(json.find("\"master_pc\": \"0x06004000\"") != std::string::npos);
    CHECK(json.find("\"slave_pc\": \"0x06001234\"") != std::string::npos);
    CHECK(json.find("\"wall_seconds\": 12.346") != std::string::npos);
    CHECK(json.find("\"bios\": \"mpr-17933.bin\"") != std::string::npos);
    CHECK(json.find("\"cartridge\": \"none\"") != std::string::npos);
}

TEST_CASE("bench sweep: summary JSON of a load failure has the escaped error and null PCs", "[bench][sweep]") {
    SweepSummary summary;
    summary.loadError = "cannot open \"C:\\games\\x.chd\"\nline two";
    const std::string json = FormatSweepSummary(summary);
    CHECK(json.find("\"load_error\": \"cannot open \\\"C:\\\\games\\\\x.chd\\\"\\nline two\"") != std::string::npos);
    CHECK(json.find("\"master_pc\": null") != std::string::npos);
    CHECK(json.find("\"slave_pc\": null") != std::string::npos);
    CHECK(json.find("\"frames_run\": 0") != std::string::npos);
}
