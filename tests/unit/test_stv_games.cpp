// The ST-V games from real ROM sets, one [.stv-rom] case each: boot to the attract mode without phantom credits, one
// coin is one credit, Start leads into the game, then a minute of scripted play for both players with sound. The ROM
// sets and BIOS files are read from BRIMIR_STV_ROM_DIR and BRIMIR_STV_BIOS_DIR (the cases SKIP without them);
// screenshots go to BRIMIR_STV_SCREENS_DIR/<set> when that is set, for comparison with MAME's.
//
// Every case carries the tag of its game ([colmns97]) and the quick cases, one per game that only boot to the attract
// mode and take one coin, also [.stv-rom-quick]:
//   brimir_tests "[.stv-rom-quick]"         every game, boot and one coin
//   brimir_tests "[.stv-rom][rsgun]"        everything about one game
//   brimir_tests "[.stv-rom]"               all of it

#include "catch_amalgamated.hpp"

#include "unit/stv_rom_test_helpers.hpp"

#include <libretro.h>

#include <brimir/core_wrapper.hpp>

#include <ymir/sys/saturn.hpp>

#include <stv/stv_game_db.hpp>
#include <stv/stv_input.hpp>
#include <stv/stv_state.hpp>

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <functional>
#include <set>
#include <string>
#include <utility>
#include <vector>

using namespace brimir;

namespace {

constexpr uint16_t Pad(unsigned retroId) {
    return static_cast<uint16_t>(1u << retroId);
}

constexpr uint16_t kCoin = Pad(RETRO_DEVICE_ID_JOYPAD_SELECT);
constexpr uint16_t kStart = Pad(RETRO_DEVICE_ID_JOYPAD_START);
constexpr uint16_t kButton1 = Pad(RETRO_DEVICE_ID_JOYPAD_B);
constexpr uint16_t kButton2 = Pad(RETRO_DEVICE_ID_JOYPAD_A);
constexpr uint16_t kButton3 = Pad(RETRO_DEVICE_ID_JOYPAD_Y);
constexpr uint16_t kButton4 = Pad(RETRO_DEVICE_ID_JOYPAD_X); // buttons 4-6: the six-button games
constexpr uint16_t kButton5 = Pad(RETRO_DEVICE_ID_JOYPAD_L);
constexpr uint16_t kButton6 = Pad(RETRO_DEVICE_ID_JOYPAD_R);
constexpr uint16_t kService = Pad(RETRO_DEVICE_ID_JOYPAD_L3);
constexpr uint16_t kTest = Pad(RETRO_DEVICE_ID_JOYPAD_R3);
constexpr uint16_t kLeft = Pad(RETRO_DEVICE_ID_JOYPAD_LEFT);
constexpr uint16_t kRight = Pad(RETRO_DEVICE_ID_JOYPAD_RIGHT);
constexpr uint16_t kDown = Pad(RETRO_DEVICE_ID_JOYPAD_DOWN);
constexpr uint16_t kUp = Pad(RETRO_DEVICE_ID_JOYPAD_UP);

// The BIOS's credit count in work RAM (its coin handler adds to it, the game's Start takes from it)
constexpr uint32_t kBiosCredits = 0x0600'0740;

// One ST-V game loaded from the real ROM set, driven frame by frame through CoreWrapper::SetSTVInput as the libretro
// layer drives it. Every frame's audio is drained and its peak kept.
class GameSession {
public:
    // `tag` tells apart the scratch folders of two sessions of one game alive at once
    explicit GameSession(const char *set, const std::string &tag = "")
        : m_set(set), m_scratch((std::string("brimir_stv_game_") + set + tag).c_str()) {}

    // False (and the reason in Skipped()) when the folders or the zip are not there
    bool Load() {
        const auto biosDir = test::EnvDir("BRIMIR_STV_BIOS_DIR");
        const auto romDir = test::EnvDir("BRIMIR_STV_ROM_DIR");
        const auto zipPath = romDir / (m_set + ".zip");
        if (biosDir.empty() || romDir.empty() || !std::filesystem::exists(zipPath)) {
            m_skipped = "BRIMIR_STV_BIOS_DIR and BRIMIR_STV_ROM_DIR with " + m_set + ".zip are needed";
            return false;
        }
        const auto screens = test::EnvDir("BRIMIR_STV_SCREENS_DIR");
        if (!screens.empty()) {
            m_screens = screens / m_set;
            std::filesystem::create_directories(m_screens);
        }
        test::CopyBiosFiles(biosDir, m_scratch.path);
        REQUIRE(m_core.Initialize());
        const std::string system = m_scratch.path.string();
        REQUIRE(m_core.LoadGame(zipPath.string().c_str(), system.c_str(), system.c_str()));
        REQUIRE(m_core.IsSTVLoaded());
        REQUIRE(std::string(m_core.GetSTVGame()->shortName) == m_set);
        m_core.SetThreadedVDP1(false);
        m_core.SetThreadedVDP2(false);
        return true;
    }

    const std::string &Skipped() const { return m_skipped; }
    CoreWrapper &Core() { return m_core; }
    int Frame() const { return m_frame; }
    uint8_t Credits() { return m_core.GetSaturn()->mainBus.Peek<uint8_t>(kBiosCredits); }

    void Press(unsigned player, uint16_t retropadMask) {
        m_core.SetSTVInput(player, stv::MapSTVInput(retropadMask, m_core.GetSTVGame()->controls));
    }

    // Runs until `frame` (absolute), calling `each` after every frame
    void RunTo(int frame, const std::function<void()> &each = {}) {
        while (m_frame < frame) {
            m_core.RunFrame();
            ++m_frame;
            m_hashValid = false;
            m_shareValid = false;
            const size_t samples = m_core.GetAudioSamples(m_audio.data(), m_audio.size() / 2);
            for (size_t i = 0; i < samples * 2; i++) {
                m_audioPeak = std::max(m_audioPeak, std::abs(static_cast<int>(m_audio[i])));
            }
            if (each) {
                each();
            }
        }
    }

    // Holds `retropadMask` on `player` for `frames` frames, then releases it
    void Tap(unsigned player, uint16_t retropadMask, int frames = 5) {
        Press(player, retropadMask);
        RunTo(m_frame + frames);
        Press(player, 0);
    }

    void ResetAudioPeak() { m_audioPeak = 0; }
    int AudioPeak() const { return m_audioPeak; }

    // The frame's hash and background share are worked out once per frame, however often the callbacks ask
    uint64_t VideoHash() {
        if (!m_hashValid) {
            m_hash = test::VideoHash(m_core);
            m_hashValid = true;
        }
        return m_hash;
    }

    // True when every pixel has the same colour (the black frames between scenes, in the attract mode and in the game)
    bool VideoUniform() { return VideoBackgroundShare() == 1.0; }

    // The share of the pixels that have the colour of the top-left one (1.0 when the frame is uniform)
    double VideoBackgroundShare() {
        if (m_shareValid) {
            return m_share;
        }
        m_shareValid = true;
        m_share = 1.0;
        const auto *pixels = static_cast<const uint32_t *>(m_core.GetFramebuffer());
        const unsigned width = m_core.GetFramebufferWidth();
        const unsigned height = m_core.GetFramebufferHeight();
        const unsigned pitch = m_core.GetFramebufferPitch() / 4;
        if (pixels == nullptr || width == 0 || height == 0) {
            return m_share;
        }
        const uint32_t first = pixels[0] & 0xFFFFFF;
        size_t same = 0;
        for (unsigned y = 0; y < height; y++) {
            for (unsigned x = 0; x < width; x++) {
                same += (pixels[static_cast<size_t>(y) * pitch + x] & 0xFFFFFF) == first ? 1 : 0;
            }
        }
        m_share = static_cast<double>(same) / (static_cast<double>(width) * height);
        return m_share;
    }

    // A copy of the frame's pixels (height rows of the pitch)
    std::vector<uint32_t> Pixels() {
        const auto *fb = static_cast<const uint32_t *>(m_core.GetFramebuffer());
        const size_t count = static_cast<size_t>(m_core.GetFramebufferHeight()) * (m_core.GetFramebufferPitch() / 4);
        return fb != nullptr ? std::vector<uint32_t>(fb, fb + count) : std::vector<uint32_t>(count);
    }

    void Save(const std::string &name) {
        if (!m_screens.empty()) {
            test::WritePng(m_core, m_screens / name);
        }
    }

private:
    std::string m_set;
    std::string m_skipped;
    test::ScratchDir m_scratch;
    std::filesystem::path m_screens;
    CoreWrapper m_core;
    int m_frame = 0;
    std::vector<int16_t> m_audio = std::vector<int16_t>(8192);
    int m_audioPeak = 0;
    uint64_t m_hash = 0;
    bool m_hashValid = false;
    double m_share = 1.0;
    bool m_shareValid = false;
};

// The distinct frames of the attract mode and, once the game runs, the count of frames that look like them. `skip`
// names the frames to leave out (the black cards between scenes, which the game fades through as well).
class AttractWatch {
public:
    explicit AttractWatch(GameSession &session, std::function<bool()> skip = {})
        : m_session(session), m_skip(std::move(skip)) {}

    // In the attract mode: counts the frames that show a credit and collects the distinct frames
    void Observe() {
        creditFrames += m_session.Credits() != 0 ? 1 : 0;
        if (!Skipped()) {
            attract.insert(m_session.VideoHash());
        }
    }

    // After Start: counts the frames that look like the attract mode
    void Check() { attractLike += !Skipped() && attract.count(m_session.VideoHash()) != 0 ? 1 : 0; }
    std::function<void()> Checker() {
        return [this] { Check(); };
    }

    std::set<uint64_t> attract;
    int creditFrames = 0;
    int attractLike = 0;

private:
    bool Skipped() { return m_skip && m_skip(); }

    GameSession &m_session;
    std::function<bool()> m_skip;
};

using Screens = std::vector<std::pair<int, const char *>>;

// Runs the attract mode up to `to` (the BIOS's check is behind the session already): no phantom credits, enough
// different frames. `screens` are saved at their frames.
void CheckAttract(GameSession &session, AttractWatch &watch, int to, const Screens &screens = {}) {
    session.RunTo(to, [&] {
        watch.Observe();
        for (const auto &[frame, name] : screens) {
            if (session.Frame() == frame) {
                session.Save(name);
            }
        }
    });
    CHECK(watch.creditFrames == 0);
    INFO("distinct attract frames: " << watch.attract.size());
    CHECK(watch.attract.size() >= 30);
}

// One coin, held for 5 frames, is one credit
void InsertCoin(GameSession &session, int coinFrame, int creditFrame) {
    session.RunTo(coinFrame);
    session.Tap(0, kCoin);
    session.RunTo(creditFrame);
    CHECK(session.Credits() == 1);
    session.Save("brimir_frame" + std::to_string(creditFrame) + "_credit1.png");
}

// Player 2 joins with a coin of its own: 30 frames to the credit, then Start takes it
void Player2Joins(GameSession &session, const std::function<void()> &each = {}) {
    session.Press(0, 0); // player 1's button is not held through the coin
    const int at = session.Frame();
    session.Tap(1, kCoin);
    session.RunTo(at + 30, each);
    CHECK(session.Credits() == 1);
    session.Tap(1, kStart);
}

// Button 1 for 5 frames of every 60 once the game has had time to get past its first screens
uint16_t Button1Now(int frame, int from = 2650) {
    return frame >= from && frame % 60 < 5 ? kButton1 : 0;
}

// Both players press Button 1 for 5 frames of every 60 between two frames (the character or level select)
uint16_t BothSelect(int frame, int from, int to) {
    return frame >= from && frame < to && frame % 60 < 5 ? kButton1 : 0;
}

// Scripted play for one player: each action is held for 4 frames and released for 4, so every press is an edge.
// The two players run the same script out of phase.
uint16_t ScriptedInput(int frame, unsigned player, const std::vector<uint16_t> &actions) {
    const int step = frame / 4 + static_cast<int>(player) * 5;
    if (step % 2 != 0) {
        return 0;
    }
    return actions[static_cast<size_t>(step / 2) % actions.size()];
}

// The puzzle games (colmns97, puyosun): move, rotate and drop
const std::vector<uint16_t> kPuzzleScript = {kLeft,  kButton1, kLeft, kLeft,  0,     kRight, kButton2, kRight,
                                             kRight, 0,        kLeft, kRight, kUp,   kButton1, kRight, 0,
                                             kLeft,  kLeft,    kLeft, 0,      kDown, kRight,   kRight,   kRight};

// The shooters (cotton2, grdforce): fly about firing, now and then a magic or a special weapon
const std::vector<uint16_t> kShooterScript = {
    kRight | kButton1, kButton1, kUp | kButton1,    kButton1, kDown | kButton1, 0,
    kLeft | kButton1,  kButton2, kUp,               kButton1, kDown | kButton1, kButton3,
    kRight,            kButton1, kUp | kButton1,    0,        kDown | kButton1, kButton1,
    kLeft | kButton1,  kButton2, kRight | kButton1, kButton1, kUp,              kDown | kButton1};

// A minute of play for both players. With `continues` a credit is kept in hand (player 1 coins in whenever there is
// none) and both players press Start every 400 frames, so whoever is shot down continues: shooters are short-lived.
// Screens are saved at the two frames and at the end.
// `input` gives a player's buttons for a frame (the overload below plays a script with ScriptedInput).
using PlayInput = std::function<uint16_t(int frame, unsigned player)>;

void PlayMinute(GameSession &session, AttractWatch &watch, const PlayInput &input, int firstScreen, int secondScreen,
                bool continues = false) {
    session.ResetAudioPeak();
    std::set<uint64_t> play;
    int coins = 0;
    bool coining = false;
    const int playEnd = session.Frame() + 3600;
    session.RunTo(playEnd, [&] {
        const int frame = session.Frame();
        uint16_t p1 = input(frame, 0);
        uint16_t p2 = input(frame, 1);
        if (continues) {
            if (frame % 400 == 0 && session.Credits() == 0) {
                coining = true;
                ++coins;
            } else if (frame % 400 == 5) {
                coining = false;
            }
            if (coining) {
                p1 = kCoin;
            } else if (frame % 400 >= 20 && frame % 400 < 25) {
                p1 = kStart;
                p2 = kStart;
            }
        }
        session.Press(0, p1);
        session.Press(1, p2);
        watch.Check();
        play.insert(session.VideoHash());
        if (frame == firstScreen || frame == secondScreen) {
            session.Save("brimir_frame" + std::to_string(frame) + "_play.png");
        }
    });
    session.Save("brimir_frame" + std::to_string(playEnd) + "_play.png");
    CHECK(watch.attractLike == 0);
    INFO("coins for continues: " << coins);
    INFO("distinct play frames: " << play.size());
    CHECK(play.size() >= 30);
    INFO("audio peak over the minute: " << session.AudioPeak());
    CHECK(session.AudioPeak() > 16);
    CHECK(session.Core().GetLastError().empty());
}

void PlayMinute(GameSession &session, AttractWatch &watch, const std::vector<uint16_t> &script, int firstScreen,
                int secondScreen, bool continues = false) {
    PlayMinute(
        session, watch, [&script](int frame, unsigned player) { return ScriptedInput(frame, player, script); },
        firstScreen, secondScreen, continues);
}

// The Test switch opens the ST-V TEST MENU with the cursor on EXIT; Service moves it, Test selects. Nine steps reach
// EACH GAME TEST, which opens the game's TEST MENU (cursor on EXIT again); one step is its INPUT TEST, opened at
// `openFrame`.
void OpenInputTest(GameSession &session, int openFrame) {
    session.RunTo(1800);
    session.Tap(0, kTest);
    for (int i = 0; i < 9; i++) {
        session.RunTo(2000 + i * 60);
        session.Tap(0, kService);
    }
    session.RunTo(2600);
    session.Tap(0, kTest);
    session.RunTo(2800);
    session.Tap(0, kService);
    session.RunTo(openFrame);
    session.Tap(0, kTest);
}

// The middle of the pixels that differ from the idle screen
void ChangedCentre(GameSession &session, const std::vector<uint32_t> &idlePixels, unsigned &cx, unsigned &cy) {
    const unsigned width = session.Core().GetFramebufferWidth();
    const unsigned height = session.Core().GetFramebufferHeight();
    const unsigned pitch = session.Core().GetFramebufferPitch() / 4;
    const std::vector<uint32_t> now = session.Pixels();
    uint64_t sx = 0, sy = 0, n = 0;
    for (unsigned y = 0; y < height; y++) {
        for (unsigned x = 0; x < width; x++) {
            const size_t i = static_cast<size_t>(y) * pitch + x;
            if ((now[i] & 0xFFFFFF) != (idlePixels[i] & 0xFFFFFF)) {
                sx += x;
                sy += y;
                n++;
            }
        }
    }
    cx = n != 0 ? static_cast<unsigned>(sx / n) : 0;
    cy = n != 0 ? static_cast<unsigned>(sy / n) : 0;
}

// In the INPUT TEST (UP, DOWN, LEFT, RIGHT and the switches for 1P and 2P, each OFF until pressed) every button of
// either player turns one entry ON: as many different screens as buttons, all different from the idle one, which comes
// back on release. The entry that changes is in the player's column (1P left of 60% of the width, 2P right of it), one
// row lower for each button, or with `perRow` buttons to a row, left to right along the row and then one row lower.
// `slotsPerPlayer` is how many 60-frame slots a player gets; `unusedButton` is one the game does not read (0 when there
// is none): it changes nothing.
void CheckInputTest(GameSession &session, const std::vector<uint16_t> &buttons, unsigned slotsPerPlayer,
                    uint16_t unusedButton = 0, unsigned perRow = 1) {
    session.RunTo(2990);
    session.Save("brimir_frame2990_input_test.png");
    const uint64_t idle = session.VideoHash();
    const unsigned width = session.Core().GetFramebufferWidth();
    const std::vector<uint32_t> idlePixels = session.Pixels();
    std::set<uint64_t> pressed;
    for (unsigned player = 0; player < 2; player++) {
        unsigned lastRow = 0;
        unsigned lastColumn = 0;
        for (unsigned b = 0; b < buttons.size(); b++) {
            INFO("player " << player + 1 << " SW " << b + 1);
            session.RunTo(3000 + static_cast<int>(player * slotsPerPlayer + b) * 60);
            session.Press(player, buttons[b]);
            session.RunTo(session.Frame() + 20);
            const uint64_t hash = session.VideoHash();
            CHECK(hash != idle);
            pressed.insert(hash);
            unsigned cx = 0, cy = 0;
            ChangedCentre(session, idlePixels, cx, cy);
            INFO("changed pixels centred at " << cx << "," << cy);
            CHECK((player == 0 ? cx < width * 6 / 10 : cx > width * 6 / 10));
            if (b % perRow == 0) {
                CHECK(cy > lastRow);
                lastRow = cy;
            } else {
                CHECK(cy + 2 >= lastRow);
                CHECK(cy <= lastRow + 2);
                CHECK(cx > lastColumn);
            }
            lastColumn = cx;
            session.Save("brimir_input_test_p" + std::to_string(player + 1) + "_sw" + std::to_string(b + 1) + ".png");
            session.RunTo(session.Frame() + 10);
            session.Press(player, 0);
            session.RunTo(session.Frame() + 20);
            CHECK(session.VideoHash() == idle);
        }
        if (unusedButton != 0) {
            INFO("player " << player + 1 << " unused button");
            session.RunTo(3000 + static_cast<int>(player * slotsPerPlayer + buttons.size()) * 60);
            session.Press(player, unusedButton);
            session.RunTo(session.Frame() + 20);
            CHECK(session.VideoHash() == idle);
            session.Press(player, 0);
        }
    }
    CHECK(pressed.size() == buttons.size() * 2);
}

// A quick case: the load picks `bios` (a file name in the load description), the BIOS's check and the attract mode run
// without phantom credits, and one coin is one credit. `coinFrame` is where the game's attract mode has been showing for
// a while.
void QuickBoot(const char *set, const char *bios, int coinFrame) {
    GameSession session(set);
    if (!session.Load()) {
        SKIP(session.Skipped());
    }
    INFO(session.Core().GetSTVLoadDescription());
    CHECK(session.Core().GetSTVLoadDescription().find(std::string("BIOS ") + bios) != std::string::npos);
    session.RunTo(1300);
    int creditFrames = 0;
    std::set<uint64_t> shown;
    session.RunTo(coinFrame, [&] {
        creditFrames += session.Credits() != 0 ? 1 : 0;
        if (session.Frame() > coinFrame - 300 && session.Frame() % 10 == 0 && !session.VideoUniform()) {
            shown.insert(session.VideoHash());
        }
    });
    CHECK(creditFrames == 0);
    INFO("distinct attract frames: " << shown.size());
    CHECK(shown.size() >= 5);

    session.Tap(0, kCoin);
    session.RunTo(coinFrame + 50);
    CHECK(session.Credits() == 1);
    CHECK(session.Core().GetLastError().empty());
}

} // namespace

TEST_CASE("stv: bakubaku boots to the attract mode and takes one coin", "[.stv-rom][.stv-rom-quick][bakubaku]") {
    QuickBoot("bakubaku", "epr-20091.ic8", 1500);
}

TEST_CASE("stv: colmns97 boots to the attract mode and takes one coin", "[.stv-rom][.stv-rom-quick][colmns97]") {
    QuickBoot("colmns97", "epr-20091.ic8", 1800);
}

TEST_CASE("stv: puyosun boots to the attract mode and takes one coin", "[.stv-rom][.stv-rom-quick][puyosun]") {
    QuickBoot("puyosun", "epr-20091.ic8", 1800);
}

TEST_CASE("stv: diehard boots to the attract mode and takes one coin", "[.stv-rom][.stv-rom-quick][diehard]") {
    QuickBoot("diehard", "epr-17952a.ic8", 1850);
}

TEST_CASE("stv: gaxeduel boots to the attract mode and takes one coin", "[.stv-rom][.stv-rom-quick][gaxeduel]") {
    QuickBoot("gaxeduel", "epr-20091.ic8", 2450);
}

TEST_CASE("stv: cotton2 boots to the attract mode and takes one coin", "[.stv-rom][.stv-rom-quick][cotton2]") {
    QuickBoot("cotton2", "epr-20091.ic8", 2450);
}

TEST_CASE("stv: grdforce boots to the attract mode and takes one coin", "[.stv-rom][.stv-rom-quick][grdforce]") {
    QuickBoot("grdforce", "epr-20091.ic8", 2450);
}

TEST_CASE("stv: rsgun boots to the attract mode and takes one coin", "[.stv-rom][.stv-rom-quick][rsgun]") {
    QuickBoot("rsgun", "epr-20091.ic8", 2450);
}

TEST_CASE("stv: shienryu boots to the attract mode and takes one coin", "[.stv-rom][.stv-rom-quick][shienryu]") {
    QuickBoot("shienryu", "epr-20091.ic8", 2800);
}

TEST_CASE("stv: ffreveng boots to the attract mode and takes one coin", "[.stv-rom][.stv-rom-quick][ffreveng]") {
    QuickBoot("ffreveng", "epr-20091.ic8", 2450);
}

TEST_CASE("stv: colmns97 boots, takes coin and start, and plays for a minute", "[.stv-rom][colmns97]") {
    GameSession session("colmns97");
    if (!session.Load()) {
        SKIP(session.Skipped());
    }
    INFO(session.Core().GetSTVLoadDescription());
    CHECK(session.Core().GetSTVLoadDescription().find("epr-20091.ic8") != std::string::npos);

    // The BIOS checks the board for about 1300 frames, then the title screen and the demo follow. No phantom credits.
    AttractWatch watch(session);
    session.RunTo(1300);
    CheckAttract(session, watch, 1750, {{1750, "brimir_frame1750_attract.png"}});
    InsertCoin(session, 1800, 1850);

    // Start takes the credit and leads to the level select: no frame looks like the attract mode any more
    session.RunTo(1900);
    session.Tap(0, kStart);
    session.RunTo(1960, watch.Checker());
    CHECK(session.Credits() == 0);
    session.Save("brimir_frame1960_level_select.png");
    session.Tap(0, kButton1); // EASY

    // Player 2 joins with a coin of its own
    session.RunTo(2100);
    Player2Joins(session);
    session.RunTo(2200);
    CHECK(session.Credits() == 0);
    session.Tap(1, kButton1);

    // A minute of play: both players move, rotate and drop their columns
    session.RunTo(2300);
    PlayMinute(session, watch, kPuzzleScript, 2700, 4500);
}

TEST_CASE("stv: puyosun boots, takes coin and start, and plays for a minute", "[.stv-rom][puyosun]") {
    GameSession session("puyosun");
    if (!session.Load()) {
        SKIP(session.Skipped());
    }
    INFO(session.Core().GetSTVLoadDescription());
    CHECK(session.Core().GetSTVLoadDescription().find("epr-20091.ic8") != std::string::npos);

    // The BIOS checks the board for about 1300 frames, then the intro and the demo follow. No phantom credits. The
    // black frames between scenes are left out: the game fades through black too.
    AttractWatch watch(session, [&] { return session.VideoUniform(); });
    session.RunTo(1300);
    CheckAttract(session, watch, 1750, {{1750, "brimir_frame1750_attract.png"}});
    InsertCoin(session, 1800, 1850);

    // Start takes the credit and leads to the game select: no frame looks like the attract mode any more
    session.RunTo(1900);
    session.Tap(0, kStart);
    session.RunTo(1960, watch.Checker());
    CHECK(session.Credits() == 0);
    session.Save("brimir_frame1960_game_select.png");
    session.Tap(0, kButton1); // one player

    // Button 1 now and then picks the default course and skips the story, then a round against the computer
    session.RunTo(4000, [&] {
        watch.Check();
        const int frame = session.Frame();
        if (frame >= 2200 && frame % 120 == 0) {
            session.Press(0, kButton1);
        } else if (frame % 120 == 5) {
            session.Press(0, 0);
        }
        if (frame == 2200) {
            session.Save("brimir_frame2200_course_select.png");
        } else if (frame == 3700) {
            session.Save("brimir_frame3700_play_cpu.png");
        }
    });
    session.Press(0, 0);

    // Player 2 breaks in with a coin of its own; both pick their character and level with Button 1
    session.RunTo(4400, watch.Checker());
    Player2Joins(session, watch.Checker());
    session.RunTo(5800, [&] {
        watch.Check();
        const int frame = session.Frame();
        const uint16_t press = frame >= 4500 && frame % 120 < 5 ? kButton1 : 0;
        session.Press(0, press);
        session.Press(1, press);
        if (frame == 4900) {
            session.Save("brimir_frame4900_character_select.png");
        }
    });
    CHECK(session.Credits() == 0);

    // A minute of play: both players move, rotate and drop their pairs
    PlayMinute(session, watch, kPuzzleScript, 6200, 7600);
}

TEST_CASE("stv: diehard boots on the USA BIOS, takes coin and start, and plays for a minute", "[.stv-rom][diehard]") {
    GameSession session("diehard");
    if (!session.Load()) {
        SKIP(session.Skipped());
    }
    // Die Hard Arcade has no Japan region: with every BIOS in the folder the load picks the USA one and says so
    INFO(session.Core().GetSTVLoadDescription());
    CHECK(session.Core().GetSTVLoadDescription().find("BIOS epr-17952a.ic8 (USA BIOS") != std::string::npos);
    CHECK(session.Core().GetSTVLoadDescription().find("SMPC area code 1") != std::string::npos);

    // The patents and the FBI screen until about frame 850, then the intro over the city at night with INSERT
    // COIN(S) / CREDIT 0. No phantom credits.
    AttractWatch watch(session, [&] { return session.VideoUniform(); });
    session.RunTo(900);
    CheckAttract(session, watch, 1800, {{1600, "brimir_frame1600_attract.png"}});
    InsertCoin(session, 1850, 1900);

    // Start takes the credit and starts the game's intro (the helicopter lands on the roof): no frame looks like the
    // attract mode any more
    session.RunTo(1950);
    session.Tap(0, kStart);
    session.RunTo(2050, watch.Checker());
    CHECK(session.Credits() == 0);

    // Player 2 joins with a coin of its own
    session.RunTo(2600, watch.Checker());
    session.Save("brimir_frame2600_intro.png");
    Player2Joins(session, watch.Checker());
    session.RunTo(2700, watch.Checker());
    CHECK(session.Credits() == 0);

    // A minute of play: both players walk, punch, kick and jump
    const std::vector<uint16_t> script = {kRight, kButton1, kRight, kButton2, kRight, 0,        kButton1, kButton1,
                                          kLeft,  kButton3, kUp,    kButton1, kDown,  kButton2, kRight,   kRight,
                                          0,      kButton2, kRight, kButton3, kLeft,  kButton1, kUp,      kDown};
    session.RunTo(3000, watch.Checker());
    PlayMinute(session, watch, script, 3600, 5000);
}

TEST_CASE("stv: gaxeduel boots, takes coin and start, and plays for a minute", "[.stv-rom][gaxeduel]") {
    GameSession session("gaxeduel");
    if (!session.Load()) {
        SKIP(session.Skipped());
    }
    INFO(session.Core().GetSTVLoadDescription());
    CHECK(session.Core().GetSTVLoadDescription().find("epr-20091.ic8") != std::string::npos);

    // The BIOS checks the board, the Sega and InVision logos follow, then from about frame 1500 the title (AXE PROJECT
    // 3 over fire) and the story told over a stormy castle, with INSERT COIN(S) / CREDIT 0. No phantom credits.
    AttractWatch watch(session, [&] { return session.VideoUniform(); });
    session.RunTo(1500);
    CheckAttract(session, watch, 2400, {{1700, "brimir_frame1700_title.png"}, {2200, "brimir_frame2200_attract.png"}});
    InsertCoin(session, 2450, 2500);

    // Start takes the credit and leads to SELECT PLAYERS; Button 1 now and then picks the fighter, then the round
    // against the computer starts. No frame looks like the attract mode any more.
    session.RunTo(2550);
    session.Tap(0, kStart);
    session.RunTo(3300, [&] {
        watch.Check();
        const int frame = session.Frame();
        session.Press(0, Button1Now(frame));
        if (frame == 2600) {
            session.Save("brimir_frame2600_select_players.png");
        } else if (frame == 3200) {
            session.Save("brimir_frame3200_play_cpu.png");
        }
    });
    CHECK(session.Credits() == 0);

    // Player 2 challenges with a coin of its own; both pick their fighter with Button 1
    Player2Joins(session, watch.Checker());
    session.RunTo(4000, [&] {
        watch.Check();
        const int frame = session.Frame();
        const uint16_t press = BothSelect(frame, 3400, 3800);
        session.Press(0, press);
        session.Press(1, press);
        if (frame == 3500) {
            session.Save("brimir_frame3500_challenger.png");
        }
    });
    CHECK(session.Credits() == 0);

    // A minute of play: both players walk, jump, crouch and use all six buttons
    const std::vector<uint16_t> script = {kRight, kButton1, kRight, kButton4, kRight, 0,        kButton2, kButton5,
                                          kLeft,  kButton3, kUp,    kButton6, kDown,  kButton2, kRight,   kRight,
                                          0,      kButton4, kRight, kButton5, kLeft,  kButton6, kUp,      kDown};
    PlayMinute(session, watch, script, 4500, 6000);
}

TEST_CASE("stv: gaxeduel sees all six buttons of both players in its input test", "[.stv-rom][gaxeduel]") {
    GameSession session("gaxeduel");
    if (!session.Load()) {
        SKIP(session.Skipped());
    }
    OpenInputTest(session, 2900);
    CheckInputTest(session, {kButton1, kButton2, kButton3, kButton4, kButton5, kButton6}, 6);
}

TEST_CASE("stv: cotton2 boots, takes coin and start, and plays for a minute", "[.stv-rom][cotton2]") {
    GameSession session("cotton2");
    if (!session.Load()) {
        SKIP(session.Skipped());
    }
    INFO(session.Core().GetSTVLoadDescription());
    CHECK(session.Core().GetSTVLoadDescription().find("epr-20091.ic8") != std::string::npos);

    // The BIOS checks the board and shows the warning and the patents, then from about frame 1200 the story scrolls
    // up over the blue sky with INSERT COIN(S) / CREDIT 0, then the characters are introduced. No phantom credits.
    AttractWatch watch(session, [&] { return session.VideoUniform(); });
    session.RunTo(1300);
    CheckAttract(session, watch, 2400, {{1600, "brimir_frame1600_attract.png"}, {2300, "brimir_frame2300_attract.png"}});
    InsertCoin(session, 2450, 2500);

    // Start takes the credit; Button 1 now and then passes the stage card and the first stage begins. No frame looks
    // like the attract mode any more.
    session.RunTo(2550);
    session.Tap(0, kStart);
    session.RunTo(3300, [&] {
        watch.Check();
        const int frame = session.Frame();
        session.Press(0, Button1Now(frame));
        if (frame == 2800) {
            session.Save("brimir_frame2800_stage_ready.png");
        } else if (frame == 3200) {
            session.Save("brimir_frame3200_play_1p.png");
        }
    });
    CHECK(session.Credits() == 0);

    // Player 2 joins with a coin of its own
    Player2Joins(session, watch.Checker());
    session.RunTo(4000, [&] {
        watch.Check();
        const uint16_t press = BothSelect(session.Frame(), 3400, 3800);
        session.Press(0, press);
        session.Press(1, press);
    });
    CHECK(session.Credits() == 0);

    // A minute of play: both players fly about firing, now and then a magic or a bomb, with continues
    PlayMinute(session, watch, kShooterScript, 4500, 6000, true);
}

TEST_CASE("stv: grdforce boots, takes coin and start, and plays for a minute", "[.stv-rom][grdforce]") {
    GameSession session("grdforce");
    if (!session.Load()) {
        SKIP(session.Skipped());
    }
    INFO(session.Core().GetSTVLoadDescription());
    CHECK(session.Core().GetSTVLoadDescription().find("epr-20091.ic8") != std::string::npos);

    // The BIOS checks the board and shows the warning, then from about frame 1200 the title (the tank under the
    // GUARDIAN FORCE logo) with CREDIT 0, then from about frame 1900 the PRACTICE-STAGE demo. No phantom credits.
    // The demo and the game both open on the same PRACTICE-STAGE card (text on black), so the frames that are mostly
    // one colour are left out.
    AttractWatch watch(session, [&] { return session.VideoBackgroundShare() > 0.9; });
    session.RunTo(1300);
    CheckAttract(session, watch, 2400, {{1600, "brimir_frame1600_title.png"}, {2300, "brimir_frame2300_attract.png"}});
    InsertCoin(session, 2450, 2500);

    // Start takes the credit and the practice stage begins; Button 1 now and then fires. No frame looks like the
    // attract mode any more.
    session.RunTo(2550);
    session.Tap(0, kStart);
    session.RunTo(3300, [&] {
        watch.Check();
        const int frame = session.Frame();
        session.Press(0, Button1Now(frame));
        if (frame == 2800) {
            session.Save("brimir_frame2800_practice_stage.png");
        } else if (frame == 3200) {
            session.Save("brimir_frame3200_play_1p.png");
        }
    });
    CHECK(session.Credits() == 0);

    // Player 2 joins with a coin of its own
    Player2Joins(session, watch.Checker());
    session.RunTo(4000, [&] {
        watch.Check();
        const uint16_t press = BothSelect(session.Frame(), 3400, 3800);
        session.Press(0, press);
        session.Press(1, press);
    });
    CHECK(session.Credits() == 0);

    // A minute of play: both tanks drive about firing, now and then a special weapon, with continues
    PlayMinute(session, watch, kShooterScript, 4500, 6000, true);
}

TEST_CASE("stv: rsgun's 315-5881 decrypts the game's boot check as in MAME", "[.stv-rom][rsgun]") {
    GameSession session("rsgun");
    if (!session.Load()) {
        SKIP(session.Skipped());
    }
    // The game's only use of the chip (MAME 0.289, watchpoints on 0x4FFFFF0-0x4FFFFFF through 90 s of boot, attract
    // and play): at the end of the BIOS check the code at 0x0600697C disables the chip, sets the ROM address 0x00100800
    // (words) and the subkey 0x7777, enables it and reads 128 longs at 0x4FFFFFC. MAME's values: 64 longs of the bytes
    // 0x00, 0x10, ... 0xF0, four each, then the bytes 0x00-0x0F over and over (a compressed block).
    auto &bus = session.Core().GetSaturn()->mainBus;
    bus.Write<uint8_t>(0x04FF'FFF1, 0x00);
    bus.Write<uint16_t>(0x04FF'FFF8, 0x0800);
    bus.Write<uint16_t>(0x04FF'FFFA, 0x0010);
    bus.Write<uint16_t>(0x04FF'FFFC, 0x7777);
    bus.Write<uint8_t>(0x04FF'FFF1, 0x01);
    int mismatches = 0;
    for (uint32_t i = 0; i < 128; i++) {
        const uint32_t expected =
            i < 64 ? (i / 4) * 0x10u * 0x0101'0101u : ((i % 4) * 4u) * 0x0101'0101u + 0x0001'0203u;
        const uint32_t value = bus.Read<uint32_t>(0x04FF'FFFC);
        if (value != expected) {
            UNSCOPED_INFO("long " << i << ": " << std::hex << value << " expected " << expected << std::dec);
            ++mismatches;
        }
    }
    bus.Write<uint8_t>(0x04FF'FFF1, 0x00);
    CHECK(mismatches == 0);
}

TEST_CASE("stv: rsgun boots, takes coin and start, and plays for a minute", "[.stv-rom][rsgun]") {
    GameSession session("rsgun");
    if (!session.Load()) {
        SKIP(session.Skipped());
    }
    INFO(session.Core().GetSTVLoadDescription());
    CHECK(session.Core().GetSTVLoadDescription().find("epr-20091.ic8") != std::string::npos);

    // The BIOS checks the board, then from about frame 1450 the title (RADIANT SILVERGUN, INSERT COIN(S), CREDIT 0),
    // then from about frame 2150 the story told over still pictures. No phantom credits. Text cards on black are left
    // out.
    AttractWatch watch(session, [&] { return session.VideoBackgroundShare() > 0.9; });
    session.RunTo(1300);
    CheckAttract(session, watch, 2400, {{1600, "brimir_frame1600_title.png"}, {2300, "brimir_frame2300_attract.png"}});
    InsertCoin(session, 2450, 2500);

    // Start takes the credit and the title fades out, through the same frames as the attract mode's own fade (the
    // title with CREDIT 0, so those 40 frames are not checked). Then the TRIAL RIDE MODE (the weapons shown one by one)
    // begins, then the first stage; Button 1 now and then fires. No frame looks like the attract mode any more.
    session.RunTo(2550);
    session.Tap(0, kStart);
    session.RunTo(2600);
    session.RunTo(3300, [&] {
        watch.Check();
        const int frame = session.Frame();
        session.Press(0, Button1Now(frame));
        if (frame == 2800) {
            session.Save("brimir_frame2800_trial_ride.png");
        } else if (frame == 3200) {
            session.Save("brimir_frame3200_play_1p.png");
        }
    });
    CHECK(session.Credits() == 0);

    // Player 2 joins with a coin of its own
    Player2Joins(session, watch.Checker());
    session.RunTo(4000, [&] {
        watch.Check();
        const uint16_t press = BothSelect(session.Frame(), 3400, 3800);
        session.Press(0, press);
        session.Press(1, press);
    });
    CHECK(session.Credits() == 0);

    // A minute of play: both ships fly about firing all seven weapons (A, B, C and the A+B, B+C, A+C and A+B+C
    // combinations), with continues
    const uint16_t ab = kButton1 | kButton2;
    const uint16_t bc = kButton2 | kButton3;
    const uint16_t ac = kButton1 | kButton3;
    const uint16_t abc = kButton1 | kButton2 | kButton3;
    const std::vector<uint16_t> script = {
        kRight | kButton1, kButton1, kUp | kButton2, kButton2, kDown | kButton3, 0,
        kLeft | ab,        ab,       kUp | bc,       bc,       kDown | ac,       ac,
        kRight | kButton1, abc,      kUp | kButton1, 0,        kDown | kButton2, kButton3,
        kLeft | kButton1,  kButton2, kRight | ab,    bc,       kUp,              kDown | ac};
    PlayMinute(session, watch, script, 4500, 6000, true);
}

TEST_CASE("stv: rsgun sees the three buttons of both players in its input test", "[.stv-rom][rsgun]") {
    GameSession session("rsgun");
    if (!session.Load()) {
        SKIP(session.Skipped());
    }
    // The game reads no fourth button (MAME's plain ST-V ports give it one): Button 4 changes nothing.
    OpenInputTest(session, 2850);
    CheckInputTest(session, {kButton1, kButton2, kButton3}, 4, kButton4);
}

TEST_CASE("stv: shienryu boots upright, takes coin and start, and plays for a minute", "[.stv-rom][shienryu]") {
    GameSession session("shienryu");
    if (!session.Load()) {
        SKIP(session.Skipped());
    }
    INFO(session.Core().GetSTVLoadDescription());
    CHECK(session.Core().GetSTVLoadDescription().find("epr-20091.ic8") != std::string::npos);

    // The one vertical game (MAME's ROT270): under Screen Rotation Auto (the default) the picture is turned a quarter
    // turn anticlockwise, portrait, and reported at 3:4
    session.Core().SetRotationAuto();
    CHECK(session.Core().GetEffectiveRotation() == stv::FindGame("shienryu")->rotation);
    CHECK(session.Core().GetEffectiveRotation() == 270);
    CHECK(session.Core().GetDisplayAspectRatio() == 3.0f / 4.0f);

    // The BIOS checks the board; its default EEPROM sets the cabinet up for the vertical monitor, so the BIOS screens
    // are drawn turned too (upright once rotated, 448x352 as in MAME). The game initialises its backup RAM, then the
    // Warashi logo from about frame 1220, the title, the story over the clouds and the bosses, and from about frame
    // 2700 the title with INSERT COIN(S) / CREDIT 0. No phantom credits. The frames that are mostly one colour are left
    // out: the attract mode fades through black with CREDIT 0 at the bottom, and so does the game after Start.
    AttractWatch watch(session, [&] { return session.VideoBackgroundShare() > 0.9; });
    session.RunTo(600);
    session.Save("brimir_frame600_bios_warning.png");
    CHECK(session.Core().GetFramebufferWidth() == 448);
    CHECK(session.Core().GetFramebufferHeight() == 352);
    session.RunTo(1300);
    CheckAttract(session, watch, 2750,
                 {{1600, "brimir_frame1600_title.png"}, {1900, "brimir_frame1900_attract.png"},
                  {2400, "brimir_frame2400_attract.png"}});
    CHECK(session.Core().GetFramebufferWidth() == 240);
    CHECK(session.Core().GetFramebufferHeight() == 320);
    InsertCoin(session, 2800, 2850);

    // Start takes the credit and the first stage begins (the STAGE 1-1 card over the carrier, then the forest). No
    // frame looks like the attract mode any more. Once the ship has flown off the carrier, Up, Left, Down and Right
    // move it that way on the turned screen.
    session.RunTo(2900);
    session.Tap(0, kStart);
    session.RunTo(3700, [&] {
        watch.Check();
        const int frame = session.Frame();
        uint16_t press = Button1Now(frame, 3000);
        if (frame >= 3450 && frame < 3650) {
            constexpr uint16_t kMoves[] = {kUp, kLeft, kDown, kRight};
            press = kMoves[(frame - 3450) / 50] | (frame % 8 < 4 ? kButton1 : 0);
        }
        session.Press(0, press);
        if (frame == 3050) {
            session.Save("brimir_frame3050_stage_card.png");
        } else if (frame >= 3450 && frame <= 3650 && frame % 50 == 0) {
            session.Save("brimir_frame" + std::to_string(frame) + "_play_1p.png");
        }
    });
    CHECK(session.Credits() == 0);

    // Player 2 joins with a coin of its own
    Player2Joins(session, watch.Checker());
    session.RunTo(4000, watch.Checker());
    CHECK(session.Credits() == 0);

    // A minute of play: both ships fly about firing, now and then a bomb, with continues
    PlayMinute(session, watch, kShooterScript, 4500, 6000, true);
}

TEST_CASE("stv: ffreveng's 315-5881 reads the game's uncompressed block as in MAME", "[.stv-rom][ffreveng]") {
    GameSession session("ffreveng");
    if (!session.Load()) {
        SKIP(session.Skipped());
    }
    // The game's only use of the chip (MAME 0.289, watchpoints on 0x4FFFFF0-0x4FFFFFF through boot, attract and play):
    // from the end of the BIOS check the code at 0x06010230 disables the chip, sets the ROM address 0x001349CA (words)
    // and the subkey 0x10D9; then about once a frame the code at 0x06010292 enables it, reads one long at 0x4FFFFFC
    // and disables it again, 16 longs in all, and it starts over. The block's header (logged from Brimir's chip in a
    // local build) is 0x0001FFFF: not compressed, 256 lines of 512 bytes, so these are the chip's plain decrypted
    // words. MAME's values, the same in all of its 403 rounds over 7600 frames of boot, attract and play:
    static constexpr uint32_t kMame[16] = {0x7C1F2D57, 0x1CCF2933, 0x18AD2511, 0x3DD55A9F, 0x3996671D, 0x100F1067,
                                           0x563F2496, 0x184F2073, 0x52571009, 0x31123954, 0x20AB18A7, 0x5A5849D3,
                                           0x00005E97, 0x62D85695, 0x396D4DF2, 0x5A7524E8};
    auto &bus = session.Core().GetSaturn()->mainBus;
    int mismatches = 0;
    for (int round = 0; round < 2; round++) {
        bus.Write<uint8_t>(0x04FF'FFF1, 0x00);
        bus.Write<uint16_t>(0x04FF'FFF8, 0x49CA);
        bus.Write<uint16_t>(0x04FF'FFFA, 0x0013);
        bus.Write<uint16_t>(0x04FF'FFFC, 0x10D9);
        for (uint32_t i = 0; i < 16; i++) {
            bus.Write<uint8_t>(0x04FF'FFF1, 0x01);
            const uint32_t value = bus.Read<uint32_t>(0x04FF'FFFC);
            bus.Write<uint8_t>(0x04FF'FFF1, 0x00);
            if (value != kMame[i]) {
                UNSCOPED_INFO("round " << round << " long " << i << ": " << std::hex << value << " expected "
                                       << kMame[i] << std::dec);
                ++mismatches;
            }
        }
    }
    CHECK(mismatches == 0);
}

TEST_CASE("stv: ffreveng boots, takes coin and start, and plays for a minute", "[.stv-rom][ffreveng]") {
    GameSession session("ffreveng");
    if (!session.Load()) {
        SKIP(session.Skipped());
    }
    INFO(session.Core().GetSTVLoadDescription());
    CHECK(session.Core().GetSTVLoadDescription().find("epr-20091.ic8") != std::string::npos);

    // The BIOS checks the board, then from about frame 1150 the WIN RANKING, the intro over a riveted wall with INSERT
    // COIN(S) / CREDIT 0, a demo fight and the CAPCOM logo. No phantom credits. The black frames between scenes are
    // left out: the game fades through black too.
    AttractWatch watch(session, [&] { return session.VideoUniform(); });
    session.RunTo(1300);
    CheckAttract(session, watch, 2400, {{1400, "brimir_frame1400_ranking.png"}, {2000, "brimir_frame2000_attract.png"}});
    InsertCoin(session, 2450, 2500);

    // Start takes the credit and leads to PLAYER SELECT; Button 1 now and then picks the fighter, then the round against
    // the computer starts. No frame looks like the attract mode any more.
    session.RunTo(2550);
    session.Tap(0, kStart);
    session.RunTo(3300, [&] {
        watch.Check();
        const int frame = session.Frame();
        session.Press(0, Button1Now(frame));
        if (frame == 2600) {
            session.Save("brimir_frame2600_player_select.png");
        } else if (frame == 3200) {
            session.Save("brimir_frame3200_play_cpu.png");
        }
    });
    CHECK(session.Credits() == 0);

    // Player 2 challenges with a coin of its own (HERE COMES A NEW CHALLENGER); both pick their fighter with Button 1
    Player2Joins(session, watch.Checker());
    session.RunTo(4000, [&] {
        watch.Check();
        const int frame = session.Frame();
        const uint16_t press = BothSelect(frame, 3400, 3800);
        session.Press(0, press);
        session.Press(1, press);
        if (frame == 3400) {
            session.Save("brimir_frame3400_challenger.png");
        }
    });
    CHECK(session.Credits() == 0);

    // A minute of play: both players walk, jump, crouch and use the five buttons the game reads
    const std::vector<uint16_t> script = {kRight, kButton1, kRight, kButton4, kRight, 0,        kButton2, kButton5,
                                          kLeft,  kButton3, kUp,    kButton1, kDown,  kButton2, kRight,   kRight,
                                          0,      kButton4, kRight, kButton5, kLeft,  kButton3, kUp,      kDown};
    PlayMinute(session, watch, script, 4500, 6000);
}

TEST_CASE("stv: ffreveng sees the five buttons of both players in its input test", "[.stv-rom][ffreveng]") {
    GameSession session("ffreveng");
    if (!session.Load()) {
        SKIP(session.Skipped());
    }
    // The INPUT TEST shows SW 1-3 in one row and SW 4-5 in the next for each player: the game reads buttons 1-5
    // (PORTA/PORTB bits 0-2 and PORTF), not button 6, which MAME's stv6b ports give it and whose press shows nothing in
    // MAME's input test either. Button 6 changes nothing.
    OpenInputTest(session, 2900);
    CheckInputTest(session, {kButton1, kButton2, kButton3, kButton4, kButton5}, 6, kButton6, 3);
}

TEST_CASE("stv: sss's 315-5881 decompresses the game's boot block as in MAME", "[.stv-rom][sss]") {
    GameSession session("sss");
    if (!session.Load()) {
        SKIP(session.Skipped());
    }
    // The game's only use of the chip (MAME 0.289, watchpoints on 0x4FFFFF0-0x4FFFFFF through boot, attract and play):
    // at the end of the BIOS check the code at 0x06007A64 disables the chip, sets the ROM address 0x00A60000 (words)
    // and the subkey 0x2C5B, enables it and reads 7344 longs at 0x4FFFFFC. The block's header (logged from Brimir's
    // chip in a local build) is 0x0002D787: compressed, 136 lines of 216 bytes, 29,376 bytes = exactly the 7344 longs,
    // so the whole block is read to its last line and the next header never. MAME's values: an 8-bit picture, the
    // 64-bit FNV-1a of their bytes (big-endian, in order) and three of them.
    auto &bus = session.Core().GetSaturn()->mainBus;
    bus.Write<uint8_t>(0x04FF'FFF1, 0x00);
    bus.Write<uint16_t>(0x04FF'FFF8, 0x0000);
    bus.Write<uint16_t>(0x04FF'FFFA, 0x00A6);
    bus.Write<uint16_t>(0x04FF'FFFC, 0x2C5B);
    bus.Write<uint8_t>(0x04FF'FFF1, 0x01);
    uint64_t hash = 0xCBF2'9CE4'8422'2325ull;
    std::vector<uint32_t> values;
    for (uint32_t i = 0; i < 7344; i++) {
        const uint32_t value = bus.Read<uint32_t>(0x04FF'FFFC);
        values.push_back(value);
        for (int shift = 24; shift >= 0; shift -= 8) {
            hash = (hash ^ ((value >> shift) & 0xFF)) * 0x0000'0100'0000'01B3ull;
        }
    }
    bus.Write<uint8_t>(0x04FF'FFF1, 0x00);
    CHECK(values[27] == 0x8E8E'8686u);
    CHECK(values[3000] == 0x5B5C'BFBFu);
    CHECK(values[7343] == 0x2424'2424u);
    CHECK(hash == 0xE1BE'C3C7'3F82'10B3ull);
}

TEST_CASE("stv: sss boots, takes coin and start, and plays for a minute", "[.stv-rom][sss]") {
    GameSession session("sss");
    if (!session.Load()) {
        SKIP(session.Skipped());
    }
    INFO(session.Core().GetSTVLoadDescription());
    CHECK(session.Core().GetSTVLoadDescription().find("epr-20091.ic8") != std::string::npos);

    // The BIOS checks the board (the set's default EEPROM gives the cabinet one player: with the BIOS's own two-player
    // assignment the BIOS refuses the cartridge, in MAME too), then from about frame 1300 the CAPCOM and cave logos,
    // the title with INSERT COIN(S) / CREDIT 0, and from about frame 2500 the demo run down the slope with its trick
    // lessons. No phantom credits. The frames that are mostly one colour (the logos and fades on black) are left out.
    AttractWatch watch(session, [&] { return session.VideoBackgroundShare() > 0.9; });
    session.RunTo(1300);
    CheckAttract(session, watch, 2950, {{2000, "brimir_frame2000_title.png"}, {2700, "brimir_frame2700_attract.png"}});
    InsertCoin(session, 3000, 3050);

    // Start takes the credit and leads to PLAYER SELECT; Button 1 now and then picks the rider and skips the coach's
    // talk and the stage card, then the run down the first course begins. No frame looks like the attract mode any
    // more.
    session.RunTo(3300);
    session.Tap(0, kStart);
    session.RunTo(4100, [&] {
        watch.Check();
        const int frame = session.Frame();
        session.Press(0, Button1Now(frame, 3400));
        if (frame == 3400) {
            session.Save("brimir_frame3400_player_select.png");
        } else if (frame == 4000) {
            session.Save("brimir_frame4000_play_1p.png");
        }
    });
    CHECK(session.Credits() == 0);

    // The cabinet is set up for one player (the default EEPROM's system assignment, which the game needs): player 2's
    // coin is a credit, but player 2's Start does not join. MAME does the same (credit 1 after the coin, still 1 after
    // the Start, a minute later too).
    session.Press(0, 0);
    session.Tap(1, kCoin);
    session.RunTo(4130, watch.Checker());
    CHECK(session.Credits() == 1);
    session.Tap(1, kStart);
    session.RunTo(4200, watch.Checker());
    CHECK(session.Credits() == 1);

    // A minute of play: the rider steers, jumps (Button 1) and spins (the lever and Button 2); Button 3, which the game
    // does not read, and player 2's inputs change nothing. The run's timer has not run out by the end. MAME, on the
    // same inputs, shows the same run: the same timer, trick and total points at 4700, 6200 and 7800.
    const uint16_t lb = kLeft | kButton2;
    const uint16_t rb = kRight | kButton2;
    const std::vector<uint16_t> script = {kRight, kButton1, kRight, lb,       kLeft, 0,        kButton1, rb,
                                          kLeft,  kButton3, kRight, kButton1, kLeft, kButton2, kRight,   kRight,
                                          0,      lb,       kRight, rb,       kLeft, kButton3, kButton1, kLeft};
    PlayMinute(session, watch, script, 4700, 6200);
    CHECK(session.Credits() == 1);
}

TEST_CASE("stv: sss sees the two buttons of player 1 in its input test", "[.stv-rom][sss]") {
    GameSession session("sss");
    if (!session.Load()) {
        SKIP(session.Skipped());
    }
    // A one-player game: the INPUT TEST shows PLAYER 1P only, with SW 1 and SW 2 (MAME's plain ST-V ports give it three
    // buttons a player). Buttons 1 and 2 of player 1 turn their entry ON, SW 2 one row below SW 1; button 3 and every
    // button of player 2 change nothing. MAME's input test shows the same.
    OpenInputTest(session, 2900);
    session.RunTo(2990);
    session.Save("brimir_frame2990_input_test.png");
    const uint64_t idle = session.VideoHash();
    const std::vector<uint32_t> idlePixels = session.Pixels();
    const auto press = [&](unsigned player, uint16_t button, const char *png = nullptr) {
        session.Press(player, button);
        session.RunTo(session.Frame() + 20);
        const uint64_t hash = session.VideoHash();
        if (png != nullptr) {
            session.Save(png);
        }
        unsigned cx = 0, cy = 0;
        ChangedCentre(session, idlePixels, cx, cy);
        session.Press(player, 0);
        session.RunTo(session.Frame() + 20);
        CHECK(session.VideoHash() == idle);
        return std::pair{hash, cy};
    };
    session.RunTo(3000);
    const auto [sw1, sw1Row] = press(0, kButton1, "brimir_input_test_sw1.png");
    const auto [sw2, sw2Row] = press(0, kButton2, "brimir_input_test_sw2.png");
    CHECK(sw1 != idle);
    CHECK(sw2 != idle);
    CHECK(sw1 != sw2);
    CHECK(sw2Row > sw1Row);
    CHECK(press(0, kButton3).first == idle);
    for (uint16_t button : {kButton1, kButton2, kButton3}) {
        CHECK(press(1, button).first == idle);
    }
}

TEST_CASE("stv: sss boots to the attract mode and takes one coin", "[.stv-rom][.stv-rom-quick][sss]") {
    QuickBoot("sss", "epr-20091.ic8", 2950);
}

TEST_CASE("stv: sanjeon boots, takes coin and start, and plays for a minute", "[.stv-rom][sanjeon]") {
    GameSession session("sanjeon");
    if (!session.Load()) {
        SKIP(session.Skipped());
    }
    // AJTUE: Japan is one of its regions, so the Japan BIOS
    INFO(session.Core().GetSTVLoadDescription());
    CHECK(session.Core().GetSTVLoadDescription().find("epr-20091.ic8") != std::string::npos);

    // The cartridge is scrambled (MAME's init_sanjeon); unscrambled at load, the BIOS finds the game. After the board
    // check: the SEGA logo, the intro on the rainy quay, the kart race demo and the title. No phantom credits. The
    // frames that are mostly one colour (the logo on white, the fades) are left out.
    AttractWatch watch(session, [&] { return session.VideoBackgroundShare() > 0.9; });
    session.RunTo(1300);
    CheckAttract(session, watch, 2450, {{2100, "brimir_frame2100_attract.png"}, {2200, "brimir_frame2200_attract.png"}});
    InsertCoin(session, 2500, 2600); // the title, CREDIT 1, PRESS START BUTTON

    // Start takes the credit and leads to CHARACTER SELECT; Button 1 now and then picks the character and skips the
    // round card and the how-to-play screens, then the first minigame against the computer (air hockey). No frame looks
    // like the attract mode any more.
    session.Tap(0, kStart);
    session.RunTo(4300, [&] {
        watch.Check();
        const int frame = session.Frame();
        session.Press(0, Button1Now(frame, 2700));
        if (frame == 2800) {
            session.Save("brimir_frame2800_character_select.png");
        } else if (frame == 4000) {
            session.Save("brimir_frame4000_play_cpu.png");
        }
    });
    CHECK(session.Credits() == 0);

    // A two-player game: player 2 breaks in (the red CHALLENGE screen) with a coin of its own and picks a character
    // with Button 1; the two-player match starts with the next minigame. The credit comes at 4306 in both emulators and
    // Start takes it at 4336 (MAME 4332).
    Player2Joins(session, watch.Checker());
    session.RunTo(4500, [&] {
        watch.Check();
        session.Press(1, BothSelect(session.Frame(), 4400, 4500));
        if (session.Frame() == 4400) {
            session.Save("brimir_frame4400_challenge.png");
        }
    });
    CHECK(session.Credits() == 0);

    // A minute of play for both players: the lever and the buttons through the minigames (a direction duel on a stage,
    // then a kart race). MAME, on the same inputs, shows the same screens at 6000 and 7000 and the same race and money
    // at the end.
    PlayMinute(session, watch, kPuzzleScript, 6000, 7000);
}

TEST_CASE("stv: sanjeon boots to the attract mode and takes one coin", "[.stv-rom][.stv-rom-quick][sanjeon]") {
    QuickBoot("sanjeon", "epr-20091.ic8", 2450);
}

namespace {

// Decathlete's run: the two run buttons pressed in turn, as fast as the game counts them (each held 2 frames), the same
// for both players
uint16_t DecathlonRun(int frame, unsigned /*player*/) {
    return (frame / 2) % 2 != 0 ? kButton2 : kButton1;
}

} // namespace

TEST_CASE("stv: decathlt boots, takes coin and start, and plays for a minute", "[.stv-rom][decathlt]") {
    GameSession session("decathlt");
    if (!session.Load()) {
        SKIP(session.Skipped());
    }
    // JUET: the Japan BIOS
    INFO(session.Core().GetSTVLoadDescription());
    CHECK(session.Core().GetSTVLoadDescription().find("epr-20091.ic8") != std::string::npos);

    // After the board check the SEGA logo, the sky, the DecAthlete title (from about 1900) and from about 2600 the
    // 100-METER DASH RANKING over the runners; the 315-5838 unpacks their graphics. No phantom credits. The frames that
    // are mostly one colour (the logo on white, the fades) are left out. libretro-stv (MAME does not run the game)
    // shows the same screens.
    AttractWatch watch(session, [&] { return session.VideoBackgroundShare() > 0.9; });
    session.RunTo(1300);
    CheckAttract(session, watch, 2950, {{2000, "brimir_frame2000_title.png"}, {2800, "brimir_frame2800_ranking.png"}});
    InsertCoin(session, 3000, 3050);

    // Start takes the credit and leads to CHARACTER SELECT, ENTRY open for player 2 (エントリー受付中). No frame looks
    // like the attract mode any more.
    session.RunTo(3100, watch.Checker());
    session.Tap(0, kStart);
    session.RunTo(3200, watch.Checker());
    CHECK(session.Credits() == 0);
    session.Save("brimir_frame3200_character_select.png");

    // Player 2 joins with a coin of its own; the select screen shows both athletes, its timer runs out and the first
    // event, the 100-METER DASH, begins for the two of them
    Player2Joins(session, watch.Checker());
    session.RunTo(3400, watch.Checker());
    CHECK(session.Credits() == 0);
    session.Save("brimir_frame3400_character_select_2p.png");
    session.RunTo(3900, watch.Checker());

    // A minute of running for both players: the introductions, false starts (pressing before the gun, FLYING), then
    // the race itself, both under the 13.00 s line. libretro-stv, on the same inputs, shows the same screens.
    PlayMinute(session, watch, DecathlonRun, 5400, 7200);
    CHECK(session.Credits() == 0);
}

TEST_CASE("stv: decathlt's 315-5838 state saved mid-block continues in a fresh core", "[.stv-rom][decathlt]") {
    GameSession session("decathlt");
    if (!session.Load()) {
        SKIP(session.Skipped());
    }
    // Into the game for one player: by 4000 the chip has its tables and the game unpacks the next event's graphics at
    // about 4110
    session.RunTo(3000);
    session.Tap(0, kCoin);
    session.RunTo(3100);
    session.Tap(0, kStart);
    session.RunTo(4000);

    // The game reads its blocks whole inside one frame, so a frame never ends inside one; the test starts one of the
    // game's blocks (bank 2, source 0x023FE0: CHARACTER SELECT's, 15,488 words) and saves after 6000 of its words.
    // libretro-stv's trace of the block: the 64-bit FNV-1a of its words (big-endian) is 0x2CE22CE133C10E95, word 6000
    // is 0x2158 and the last 0x2424.
    auto &bus = session.Core().GetSaturn()->mainBus;
    bus.Write<uint16_t>(0x037F'FFF0, 0x0002);
    bus.Write<uint16_t>(0x037F'FFF2, 0x3FE0);
    uint64_t hash = 0xCBF2'9CE4'8422'2325ull;
    const auto feed = [&hash](uint32_t value) {
        for (int shift = 24; shift >= 0; shift -= 8) {
            hash = (hash ^ ((value >> shift) & 0xFF)) * 0x0000'0100'0000'01B3ull;
        }
    };
    for (int i = 0; i < 3000; i++) {
        feed(bus.Read<uint32_t>(0x037F'FFF8));
    }
    const uint64_t hashAtSave = hash;
    stv::STVStateBlock saved{};
    REQUIRE(session.Core().CaptureSTVState(saved));
    CHECK(saved.comp5838.active == 1);
    CHECK(saved.comp5838.bank == 2);
    CHECK(saved.comp5838.srcStart == 0x023FE0);
    CHECK(saved.comp5838.srcOffset > saved.comp5838.srcStart);
    std::vector<uint8_t> state(session.Core().GetStateSize());
    REQUIRE(session.Core().SaveState(state.data(), state.size()));

    GameSession fresh("decathlt", "_fresh");
    REQUIRE(fresh.Load());
    REQUIRE(fresh.Core().LoadState(state.data(), state.size()));
    stv::STVStateBlock loaded{};
    REQUIRE(fresh.Core().CaptureSTVState(loaded));
    CHECK(std::memcmp(&loaded.comp5838, &saved.comp5838, sizeof saved.comp5838) == 0);

    // The rest of the block: the same words from both, and the whole block is libretro-stv's
    auto &freshBus = fresh.Core().GetSaturn()->mainBus;
    std::vector<uint32_t> rest;
    std::vector<uint32_t> freshRest;
    for (int i = 0; i < 4744; i++) {
        rest.push_back(bus.Read<uint32_t>(0x037F'FFF8));
        freshRest.push_back(freshBus.Read<uint32_t>(0x037F'FFF8));
    }
    CHECK(freshRest == rest);
    for (const uint32_t value : freshRest) {
        feed(value);
    }
    CHECK(freshRest.front() >> 16 == 0x2158);
    CHECK(freshRest.back() == 0x2424'2424u);
    CHECK(hash == 0x2CE2'2CE1'33C1'0E95ull);
    CHECK(hash != hashAtSave);

    // Then 300 frames from both: the same pictures, frame by frame, through the game's own transfers
    std::vector<uint64_t> frames;
    session.RunTo(4300, [&] { frames.push_back(session.VideoHash()); });
    std::vector<uint64_t> freshFrames;
    fresh.RunTo(300, [&] { freshFrames.push_back(fresh.VideoHash()); });
    CHECK(freshFrames == frames);
    CHECK(std::set<uint64_t>(frames.begin(), frames.end()).size() >= 30);
    stv::STVStateBlock after{};
    REQUIRE(session.Core().CaptureSTVState(after));
    CHECK(after.comp5838.srcStart != 0x023FE0); // the game started blocks of its own
    stv::STVStateBlock freshAfter{};
    REQUIRE(fresh.Core().CaptureSTVState(freshAfter));
    CHECK(std::memcmp(&freshAfter.comp5838, &after.comp5838, sizeof after.comp5838) == 0);
}

TEST_CASE("stv: decathlt boots to the attract mode and takes one coin", "[.stv-rom][.stv-rom-quick][decathlt]") {
    QuickBoot("decathlt", "epr-20091.ic8", 2950);
}
