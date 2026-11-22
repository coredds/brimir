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

#include <algorithm>
#include <cstdint>
#include <cstdlib>
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
    explicit GameSession(const char *set) : m_set(set), m_scratch((std::string("brimir_stv_game_") + set).c_str()) {}

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
void PlayMinute(GameSession &session, AttractWatch &watch, const std::vector<uint16_t> &script, int firstScreen,
                int secondScreen, bool continues = false) {
    session.ResetAudioPeak();
    std::set<uint64_t> play;
    int coins = 0;
    bool coining = false;
    const int playEnd = session.Frame() + 3600;
    session.RunTo(playEnd, [&] {
        const int frame = session.Frame();
        uint16_t p1 = ScriptedInput(frame, 0, script);
        uint16_t p2 = ScriptedInput(frame, 1, script);
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
// row lower for each button. `slotsPerPlayer` is how many 60-frame slots a player gets; `unusedButton` is one the game
// does not read (0 when there is none): it changes nothing.
void CheckInputTest(GameSession &session, const std::vector<uint16_t> &buttons, unsigned slotsPerPlayer,
                    uint16_t unusedButton = 0) {
    session.RunTo(2990);
    session.Save("brimir_frame2990_input_test.png");
    const uint64_t idle = session.VideoHash();
    const unsigned width = session.Core().GetFramebufferWidth();
    const std::vector<uint32_t> idlePixels = session.Pixels();
    std::set<uint64_t> pressed;
    for (unsigned player = 0; player < 2; player++) {
        unsigned lastRow = 0;
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
            CHECK(cy > lastRow);
            lastRow = cy;
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
