// The ST-V games from real ROM sets, one [.stv-rom] case each: boot to the attract mode without phantom credits, one
// coin is one credit, Start leads into the game, then a minute of scripted play for both players with sound. The ROM
// sets and BIOS files are read from BRIMIR_STV_ROM_DIR and BRIMIR_STV_BIOS_DIR (the cases SKIP without them);
// screenshots go to BRIMIR_STV_SCREENS_DIR/<set> when that is set, for comparison with MAME's.

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
    uint64_t VideoHash() { return test::VideoHash(m_core); }

    // True when every pixel has the same colour (the black frames between scenes, in the attract mode and in the game)
    bool VideoUniform() { return VideoBackgroundShare() == 1.0; }

    // The share of the pixels that have the colour of the top-left one (1.0 when the frame is uniform)
    double VideoBackgroundShare() {
        const auto *pixels = static_cast<const uint32_t *>(m_core.GetFramebuffer());
        const unsigned width = m_core.GetFramebufferWidth();
        const unsigned height = m_core.GetFramebufferHeight();
        const unsigned pitch = m_core.GetFramebufferPitch() / 4;
        if (pixels == nullptr || width == 0 || height == 0) {
            return 1.0;
        }
        const uint32_t first = pixels[0] & 0xFFFFFF;
        size_t same = 0;
        for (unsigned y = 0; y < height; y++) {
            for (unsigned x = 0; x < width; x++) {
                same += (pixels[static_cast<size_t>(y) * pitch + x] & 0xFFFFFF) == first ? 1 : 0;
            }
        }
        return static_cast<double>(same) / (static_cast<double>(width) * height);
    }

    void Save(const char *name) {
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
};

// Scripted play for one player: each action is held for 4 frames and released for 4, so every press is an edge.
// The two players run the same script out of phase.
uint16_t ScriptedInput(int frame, unsigned player, const std::vector<uint16_t> &actions) {
    const int step = frame / 4 + static_cast<int>(player) * 5;
    if (step % 2 != 0) {
        return 0;
    }
    return actions[static_cast<size_t>(step / 2) % actions.size()];
}

} // namespace

TEST_CASE("stv: colmns97 boots, takes coin and start, and plays for a minute", "[.stv-rom]") {
    GameSession session("colmns97");
    if (!session.Load()) {
        SKIP(session.Skipped());
    }
    INFO(session.Core().GetSTVLoadDescription());
    CHECK(session.Core().GetSTVLoadDescription().find("epr-20091.ic8") != std::string::npos);

    // The BIOS checks the board for about 1300 frames, then the title screen and the demo follow. No phantom credits.
    session.RunTo(1300);
    int creditFrames = 0;
    std::set<uint64_t> attract;
    session.RunTo(1750, [&] {
        creditFrames += session.Credits() != 0 ? 1 : 0;
        attract.insert(session.VideoHash());
    });
    session.Save("brimir_frame1750_attract.png");
    CHECK(creditFrames == 0);
    INFO("distinct attract frames: " << attract.size());
    CHECK(attract.size() >= 30);

    // One coin, held for 5 frames, is one credit
    session.RunTo(1800);
    session.Tap(0, kCoin);
    session.RunTo(1850);
    CHECK(session.Credits() == 1);
    session.Save("brimir_frame1850_credit1.png");

    // Start takes the credit and leads to the level select: no frame looks like the attract mode any more
    session.RunTo(1900);
    session.Tap(0, kStart);
    int attractLike = 0;
    session.RunTo(1960, [&] { attractLike += attract.count(session.VideoHash()) != 0 ? 1 : 0; });
    CHECK(session.Credits() == 0);
    session.Save("brimir_frame1960_level_select.png");
    session.Tap(0, kButton1); // EASY

    // Player 2 joins with a coin of its own
    session.RunTo(2100);
    session.Tap(1, kCoin);
    session.RunTo(2130);
    CHECK(session.Credits() == 1);
    session.Tap(1, kStart);
    session.RunTo(2200);
    CHECK(session.Credits() == 0);
    session.Tap(1, kButton1);

    // A minute of play: both players move, rotate and drop their columns
    const std::vector<uint16_t> script = {kLeft,  kButton1, kLeft, kLeft,  0,     kRight, kButton2, kRight,
                                          kRight, 0,        kLeft, kRight, kUp,   kButton1, kRight, 0,
                                          kLeft,  kLeft,    kLeft, 0,      kDown, kRight,   kRight,   kRight};
    session.RunTo(2300);
    session.ResetAudioPeak();
    std::set<uint64_t> play;
    const int playEnd = session.Frame() + 3600;
    session.RunTo(playEnd, [&] {
        const int frame = session.Frame();
        session.Press(0, ScriptedInput(frame, 0, script));
        session.Press(1, ScriptedInput(frame, 1, script));
        const uint64_t hash = session.VideoHash();
        attractLike += attract.count(hash) != 0 ? 1 : 0;
        play.insert(hash);
        if (frame == 2700) {
            session.Save("brimir_frame2700_play.png");
        } else if (frame == 4500) {
            session.Save("brimir_frame4500_play.png");
        }
    });
    session.Save("brimir_frame5900_play.png");
    CHECK(attractLike == 0);
    INFO("distinct play frames: " << play.size());
    CHECK(play.size() >= 30);
    INFO("audio peak over the minute: " << session.AudioPeak());
    CHECK(session.AudioPeak() > 16);
    CHECK(session.Core().GetLastError().empty());
}

TEST_CASE("stv: puyosun boots, takes coin and start, and plays for a minute", "[.stv-rom]") {
    GameSession session("puyosun");
    if (!session.Load()) {
        SKIP(session.Skipped());
    }
    INFO(session.Core().GetSTVLoadDescription());
    CHECK(session.Core().GetSTVLoadDescription().find("epr-20091.ic8") != std::string::npos);

    // The BIOS checks the board for about 1300 frames, then the intro and the demo follow. No phantom credits. The
    // black frames between scenes are left out: the game fades through black too.
    session.RunTo(1300);
    int creditFrames = 0;
    std::set<uint64_t> attract;
    session.RunTo(1750, [&] {
        creditFrames += session.Credits() != 0 ? 1 : 0;
        if (!session.VideoUniform()) {
            attract.insert(session.VideoHash());
        }
    });
    session.Save("brimir_frame1750_attract.png");
    CHECK(creditFrames == 0);
    INFO("distinct attract frames: " << attract.size());
    CHECK(attract.size() >= 30);

    // One coin, held for 5 frames, is one credit
    session.RunTo(1800);
    session.Tap(0, kCoin);
    session.RunTo(1850);
    CHECK(session.Credits() == 1);
    session.Save("brimir_frame1850_credit1.png");

    // Start takes the credit and leads to the game select: no frame looks like the attract mode any more
    session.RunTo(1900);
    session.Tap(0, kStart);
    int attractLike = 0;
    const auto notAttract = [&] {
        attractLike += !session.VideoUniform() && attract.count(session.VideoHash()) != 0 ? 1 : 0;
    };
    session.RunTo(1960, notAttract);
    CHECK(session.Credits() == 0);
    session.Save("brimir_frame1960_game_select.png");
    session.Tap(0, kButton1); // one player

    // Button 1 now and then picks the default course and skips the story, then a round against the computer
    session.RunTo(4000, [&] {
        notAttract();
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
    session.RunTo(4400, notAttract);
    session.Tap(1, kCoin);
    session.RunTo(4430, notAttract);
    CHECK(session.Credits() == 1);
    session.Tap(1, kStart);
    session.RunTo(5800, [&] {
        notAttract();
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
    const std::vector<uint16_t> script = {kLeft,  kButton1, kLeft, kLeft,  0,     kRight, kButton2, kRight,
                                          kRight, 0,        kLeft, kRight, kUp,   kButton1, kRight, 0,
                                          kLeft,  kLeft,    kLeft, 0,      kDown, kRight,   kRight,   kRight};
    session.ResetAudioPeak();
    std::set<uint64_t> play;
    const int playEnd = session.Frame() + 3600;
    session.RunTo(playEnd, [&] {
        const int frame = session.Frame();
        session.Press(0, ScriptedInput(frame, 0, script));
        session.Press(1, ScriptedInput(frame, 1, script));
        notAttract();
        play.insert(session.VideoHash());
        if (frame == 6200) {
            session.Save("brimir_frame6200_play.png");
        } else if (frame == 7600) {
            session.Save("brimir_frame7600_play.png");
        }
    });
    session.Save("brimir_frame9400_play.png");
    CHECK(attractLike == 0);
    INFO("distinct play frames: " << play.size());
    CHECK(play.size() >= 30);
    INFO("audio peak over the minute: " << session.AudioPeak());
    CHECK(session.AudioPeak() > 16);
    CHECK(session.Core().GetLastError().empty());
}

TEST_CASE("stv: diehard boots on the USA BIOS, takes coin and start, and plays for a minute", "[.stv-rom]") {
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
    session.RunTo(900);
    int creditFrames = 0;
    std::set<uint64_t> attract;
    session.RunTo(1800, [&] {
        creditFrames += session.Credits() != 0 ? 1 : 0;
        if (!session.VideoUniform()) {
            attract.insert(session.VideoHash());
        }
        if (session.Frame() == 1600) {
            session.Save("brimir_frame1600_attract.png");
        }
    });
    CHECK(creditFrames == 0);
    INFO("distinct attract frames: " << attract.size());
    CHECK(attract.size() >= 30);

    // One coin, held for 5 frames, is one credit
    session.RunTo(1850);
    session.Tap(0, kCoin);
    session.RunTo(1900);
    CHECK(session.Credits() == 1);
    session.Save("brimir_frame1900_credit1.png");

    // Start takes the credit and starts the game's intro (the helicopter lands on the roof): no frame looks like the
    // attract mode any more
    session.RunTo(1950);
    session.Tap(0, kStart);
    int attractLike = 0;
    const auto notAttract = [&] {
        attractLike += !session.VideoUniform() && attract.count(session.VideoHash()) != 0 ? 1 : 0;
    };
    session.RunTo(2050, notAttract);
    CHECK(session.Credits() == 0);

    // Player 2 joins with a coin of its own
    session.RunTo(2600, notAttract);
    session.Save("brimir_frame2600_intro.png");
    session.Tap(1, kCoin);
    session.RunTo(2630, notAttract);
    CHECK(session.Credits() == 1);
    session.Tap(1, kStart);
    session.RunTo(2700, notAttract);
    CHECK(session.Credits() == 0);

    // A minute of play: both players walk, punch, kick and jump
    const std::vector<uint16_t> script = {kRight, kButton1, kRight, kButton2, kRight, 0,        kButton1, kButton1,
                                          kLeft,  kButton3, kUp,    kButton1, kDown,  kButton2, kRight,   kRight,
                                          0,      kButton2, kRight, kButton3, kLeft,  kButton1, kUp,      kDown};
    session.RunTo(3000, notAttract);
    session.ResetAudioPeak();
    std::set<uint64_t> play;
    const int playEnd = session.Frame() + 3600;
    session.RunTo(playEnd, [&] {
        const int frame = session.Frame();
        session.Press(0, ScriptedInput(frame, 0, script));
        session.Press(1, ScriptedInput(frame, 1, script));
        notAttract();
        play.insert(session.VideoHash());
        if (frame == 3600) {
            session.Save("brimir_frame3600_play.png");
        } else if (frame == 5000) {
            session.Save("brimir_frame5000_play.png");
        }
    });
    session.Save("brimir_frame6600_play.png");
    CHECK(attractLike == 0);
    INFO("distinct play frames: " << play.size());
    CHECK(play.size() >= 30);
    INFO("audio peak over the minute: " << session.AudioPeak());
    CHECK(session.AudioPeak() > 16);
    CHECK(session.Core().GetLastError().empty());
}

TEST_CASE("stv: gaxeduel boots, takes coin and start, and plays for a minute", "[.stv-rom]") {
    GameSession session("gaxeduel");
    if (!session.Load()) {
        SKIP(session.Skipped());
    }
    INFO(session.Core().GetSTVLoadDescription());
    CHECK(session.Core().GetSTVLoadDescription().find("epr-20091.ic8") != std::string::npos);

    // The BIOS checks the board, the Sega and InVision logos follow, then from about frame 1500 the title (AXE PROJECT
    // 3 over fire) and the story told over a stormy castle, with INSERT COIN(S) / CREDIT 0. No phantom credits.
    session.RunTo(1500);
    int creditFrames = 0;
    std::set<uint64_t> attract;
    session.RunTo(2400, [&] {
        creditFrames += session.Credits() != 0 ? 1 : 0;
        if (!session.VideoUniform()) {
            attract.insert(session.VideoHash());
        }
        if (session.Frame() == 1700) {
            session.Save("brimir_frame1700_title.png");
        } else if (session.Frame() == 2200) {
            session.Save("brimir_frame2200_attract.png");
        }
    });
    CHECK(creditFrames == 0);
    INFO("distinct attract frames: " << attract.size());
    CHECK(attract.size() >= 30);

    // One coin, held for 5 frames, is one credit
    session.RunTo(2450);
    session.Tap(0, kCoin);
    session.RunTo(2500);
    CHECK(session.Credits() == 1);
    session.Save("brimir_frame2500_credit1.png");

    // Start takes the credit and leads to SELECT PLAYERS; Button 1 now and then picks the fighter, then the round
    // against the computer starts. No frame looks like the attract mode any more.
    session.RunTo(2550);
    session.Tap(0, kStart);
    int attractLike = 0;
    const auto notAttract = [&] {
        attractLike += !session.VideoUniform() && attract.count(session.VideoHash()) != 0 ? 1 : 0;
    };
    session.RunTo(3300, [&] {
        notAttract();
        const int frame = session.Frame();
        session.Press(0, frame >= 2650 && frame % 60 < 5 ? kButton1 : 0);
        if (frame == 2600) {
            session.Save("brimir_frame2600_select_players.png");
        } else if (frame == 3200) {
            session.Save("brimir_frame3200_play_cpu.png");
        }
    });
    CHECK(session.Credits() == 0);

    // Player 2 challenges with a coin of its own; both pick their fighter with Button 1
    session.Tap(1, kCoin);
    session.RunTo(3330, notAttract);
    CHECK(session.Credits() == 1);
    session.Tap(1, kStart);
    session.RunTo(4000, [&] {
        notAttract();
        const int frame = session.Frame();
        const uint16_t press = frame >= 3400 && frame < 3800 && frame % 60 < 5 ? kButton1 : 0;
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
    session.ResetAudioPeak();
    std::set<uint64_t> play;
    const int playEnd = session.Frame() + 3600;
    session.RunTo(playEnd, [&] {
        const int frame = session.Frame();
        session.Press(0, ScriptedInput(frame, 0, script));
        session.Press(1, ScriptedInput(frame, 1, script));
        notAttract();
        play.insert(session.VideoHash());
        if (frame == 4500) {
            session.Save("brimir_frame4500_play.png");
        } else if (frame == 6000) {
            session.Save("brimir_frame6000_play.png");
        }
    });
    session.Save("brimir_frame7600_play.png");
    CHECK(attractLike == 0);
    INFO("distinct play frames: " << play.size());
    CHECK(play.size() >= 30);
    INFO("audio peak over the minute: " << session.AudioPeak());
    CHECK(session.AudioPeak() > 16);
    CHECK(session.Core().GetLastError().empty());
}

TEST_CASE("stv: gaxeduel sees all six buttons of both players in its input test", "[.stv-rom]") {
    GameSession session("gaxeduel");
    if (!session.Load()) {
        SKIP(session.Skipped());
    }
    // The Test switch opens the ST-V TEST MENU with the cursor on EXIT; Service moves it, Test selects. Nine steps
    // reach EACH GAME TEST, which opens the game's TEST MENU (cursor on EXIT again); one step is its INPUT TEST.
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
    session.RunTo(2900);
    session.Tap(0, kTest);

    // INPUT TEST lists UP, DOWN, LEFT, RIGHT and SW 1-6 for 1P and 2P, each OFF until pressed. Every button of either
    // player turns one entry ON: twelve different screens, all different from the idle one, which comes back on
    // release. The entry that changes is in the player's column (1P left of 60% of the width, 2P right of it), one row
    // lower for each button.
    session.RunTo(2990);
    session.Save("brimir_frame2990_input_test.png");
    const uint64_t idle = session.VideoHash();
    const unsigned width = session.Core().GetFramebufferWidth();
    const unsigned height = session.Core().GetFramebufferHeight();
    const unsigned pitch = session.Core().GetFramebufferPitch() / 4;
    const auto pixels = [&] {
        const auto *fb = static_cast<const uint32_t *>(session.Core().GetFramebuffer());
        return std::vector<uint32_t>(fb, fb + static_cast<size_t>(height) * pitch);
    };
    const std::vector<uint32_t> idlePixels = pixels();
    // The middle of the pixels that differ from the idle screen
    const auto changedCentre = [&](unsigned &cx, unsigned &cy) {
        const std::vector<uint32_t> now = pixels();
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
    };
    const uint16_t buttons[6] = {kButton1, kButton2, kButton3, kButton4, kButton5, kButton6};
    std::set<uint64_t> pressed;
    for (unsigned player = 0; player < 2; player++) {
        unsigned lastRow = 0;
        for (unsigned b = 0; b < 6; b++) {
            INFO("player " << player + 1 << " SW " << b + 1);
            session.RunTo(3000 + static_cast<int>(player * 6 + b) * 60);
            session.Press(player, buttons[b]);
            session.RunTo(session.Frame() + 20);
            const uint64_t hash = session.VideoHash();
            CHECK(hash != idle);
            pressed.insert(hash);
            unsigned cx = 0, cy = 0;
            changedCentre(cx, cy);
            INFO("changed pixels centred at " << cx << "," << cy);
            CHECK((player == 0 ? cx < width * 6 / 10 : cx > width * 6 / 10));
            CHECK(cy > lastRow);
            lastRow = cy;
            session.Save(("brimir_input_test_p" + std::to_string(player + 1) + "_sw" + std::to_string(b + 1) + ".png")
                             .c_str());
            session.RunTo(session.Frame() + 10);
            session.Press(player, 0);
            session.RunTo(session.Frame() + 20);
            CHECK(session.VideoHash() == idle);
        }
    }
    CHECK(pressed.size() == 12);
}

TEST_CASE("stv: cotton2 boots, takes coin and start, and plays for a minute", "[.stv-rom]") {
    GameSession session("cotton2");
    if (!session.Load()) {
        SKIP(session.Skipped());
    }
    INFO(session.Core().GetSTVLoadDescription());
    CHECK(session.Core().GetSTVLoadDescription().find("epr-20091.ic8") != std::string::npos);

    // The BIOS checks the board and shows the warning and the patents, then from about frame 1200 the story scrolls
    // up over the blue sky with INSERT COIN(S) / CREDIT 0, then the characters are introduced. No phantom credits.
    session.RunTo(1300);
    int creditFrames = 0;
    std::set<uint64_t> attract;
    session.RunTo(2400, [&] {
        creditFrames += session.Credits() != 0 ? 1 : 0;
        if (!session.VideoUniform()) {
            attract.insert(session.VideoHash());
        }
        if (session.Frame() == 1600) {
            session.Save("brimir_frame1600_attract.png");
        } else if (session.Frame() == 2300) {
            session.Save("brimir_frame2300_attract.png");
        }
    });
    CHECK(creditFrames == 0);
    INFO("distinct attract frames: " << attract.size());
    CHECK(attract.size() >= 30);

    // One coin, held for 5 frames, is one credit
    session.RunTo(2450);
    session.Tap(0, kCoin);
    session.RunTo(2500);
    CHECK(session.Credits() == 1);
    session.Save("brimir_frame2500_credit1.png");

    // Start takes the credit; Button 1 now and then passes the stage card and the first stage begins. No frame looks
    // like the attract mode any more.
    session.RunTo(2550);
    session.Tap(0, kStart);
    int attractLike = 0;
    const auto notAttract = [&] {
        attractLike += !session.VideoUniform() && attract.count(session.VideoHash()) != 0 ? 1 : 0;
    };
    session.RunTo(3300, [&] {
        notAttract();
        const int frame = session.Frame();
        session.Press(0, frame >= 2650 && frame % 60 < 5 ? kButton1 : 0);
        if (frame == 2800) {
            session.Save("brimir_frame2800_stage_ready.png");
        } else if (frame == 3200) {
            session.Save("brimir_frame3200_play_1p.png");
        }
    });
    CHECK(session.Credits() == 0);

    // Player 2 joins with a coin of its own
    session.Tap(1, kCoin);
    session.RunTo(3330, notAttract);
    CHECK(session.Credits() == 1);
    session.Tap(1, kStart);
    session.RunTo(4000, [&] {
        notAttract();
        const int frame = session.Frame();
        const uint16_t press = frame >= 3400 && frame < 3800 && frame % 60 < 5 ? kButton1 : 0;
        session.Press(0, press);
        session.Press(1, press);
    });
    CHECK(session.Credits() == 0);

    // A minute of play: both players fly about firing, now and then a magic or a bomb. Shooters are short-lived: a
    // credit is kept in hand and both players press Start every 400 frames, so whoever is shot down continues.
    const std::vector<uint16_t> script = {
        kRight | kButton1, kButton1, kUp | kButton1,    kButton1, kDown | kButton1, 0,
        kLeft | kButton1,  kButton2, kUp,               kButton1, kDown | kButton1, kButton3,
        kRight,            kButton1, kUp | kButton1,    0,        kDown | kButton1, kButton1,
        kLeft | kButton1,  kButton2, kRight | kButton1, kButton1, kUp,              kDown | kButton1};
    session.ResetAudioPeak();
    std::set<uint64_t> play;
    int coins = 0;
    bool coining = false;
    const int playEnd = session.Frame() + 3600;
    session.RunTo(playEnd, [&] {
        const int frame = session.Frame();
        uint16_t p1 = ScriptedInput(frame, 0, script);
        uint16_t p2 = ScriptedInput(frame, 1, script);
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
        session.Press(0, p1);
        session.Press(1, p2);
        notAttract();
        play.insert(session.VideoHash());
        if (frame == 4500) {
            session.Save("brimir_frame4500_play.png");
        } else if (frame == 6000) {
            session.Save("brimir_frame6000_play.png");
        }
    });
    session.Save("brimir_frame7600_play.png");
    CHECK(attractLike == 0);
    INFO("coins for continues: " << coins);
    INFO("distinct play frames: " << play.size());
    CHECK(play.size() >= 30);
    INFO("audio peak over the minute: " << session.AudioPeak());
    CHECK(session.AudioPeak() > 16);
    CHECK(session.Core().GetLastError().empty());
}

TEST_CASE("stv: grdforce boots, takes coin and start, and plays for a minute", "[.stv-rom]") {
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
    const auto mostlyBlack = [&] { return session.VideoBackgroundShare() > 0.9; };
    session.RunTo(1300);
    int creditFrames = 0;
    std::set<uint64_t> attract;
    session.RunTo(2400, [&] {
        creditFrames += session.Credits() != 0 ? 1 : 0;
        if (!mostlyBlack()) {
            attract.insert(session.VideoHash());
        }
        if (session.Frame() == 1600) {
            session.Save("brimir_frame1600_title.png");
        } else if (session.Frame() == 2300) {
            session.Save("brimir_frame2300_attract.png");
        }
    });
    CHECK(creditFrames == 0);
    INFO("distinct attract frames: " << attract.size());
    CHECK(attract.size() >= 30);

    // One coin, held for 5 frames, is one credit
    session.RunTo(2450);
    session.Tap(0, kCoin);
    session.RunTo(2500);
    CHECK(session.Credits() == 1);
    session.Save("brimir_frame2500_credit1.png");

    // Start takes the credit and the practice stage begins; Button 1 now and then fires. No frame looks like the
    // attract mode any more.
    session.RunTo(2550);
    session.Tap(0, kStart);
    int attractLike = 0;
    const auto notAttract = [&] {
        attractLike += !mostlyBlack() && attract.count(session.VideoHash()) != 0 ? 1 : 0;
    };
    session.RunTo(3300, [&] {
        notAttract();
        const int frame = session.Frame();
        session.Press(0, frame >= 2650 && frame % 60 < 5 ? kButton1 : 0);
        if (frame == 2800) {
            session.Save("brimir_frame2800_practice_stage.png");
        } else if (frame == 3200) {
            session.Save("brimir_frame3200_play_1p.png");
        }
    });
    CHECK(session.Credits() == 0);

    // Player 2 joins with a coin of its own
    session.Tap(1, kCoin);
    session.RunTo(3330, notAttract);
    CHECK(session.Credits() == 1);
    session.Tap(1, kStart);
    session.RunTo(4000, [&] {
        notAttract();
        const int frame = session.Frame();
        const uint16_t press = frame >= 3400 && frame < 3800 && frame % 60 < 5 ? kButton1 : 0;
        session.Press(0, press);
        session.Press(1, press);
    });
    CHECK(session.Credits() == 0);

    // A minute of play: both tanks drive about firing, now and then a special weapon. As in cotton2 a credit is kept in
    // hand and both players press Start every 400 frames, so whoever is shot down continues.
    const std::vector<uint16_t> script = {
        kRight | kButton1, kButton1, kUp | kButton1,    kButton1, kDown | kButton1, 0,
        kLeft | kButton1,  kButton2, kUp,               kButton1, kDown | kButton1, kButton3,
        kRight,            kButton1, kUp | kButton1,    0,        kDown | kButton1, kButton1,
        kLeft | kButton1,  kButton2, kRight | kButton1, kButton1, kUp,              kDown | kButton1};
    session.ResetAudioPeak();
    std::set<uint64_t> play;
    int coins = 0;
    bool coining = false;
    const int playEnd = session.Frame() + 3600;
    session.RunTo(playEnd, [&] {
        const int frame = session.Frame();
        uint16_t p1 = ScriptedInput(frame, 0, script);
        uint16_t p2 = ScriptedInput(frame, 1, script);
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
        session.Press(0, p1);
        session.Press(1, p2);
        notAttract();
        play.insert(session.VideoHash());
        if (frame == 4500) {
            session.Save("brimir_frame4500_play.png");
        } else if (frame == 6000) {
            session.Save("brimir_frame6000_play.png");
        }
    });
    session.Save("brimir_frame7600_play.png");
    CHECK(attractLike == 0);
    INFO("coins for continues: " << coins);
    INFO("distinct play frames: " << play.size());
    CHECK(play.size() >= 30);
    INFO("audio peak over the minute: " << session.AudioPeak());
    CHECK(session.AudioPeak() > 16);
    CHECK(session.Core().GetLastError().empty());
}
