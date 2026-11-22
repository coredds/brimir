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
    bool VideoUniform() {
        const auto *pixels = static_cast<const uint32_t *>(m_core.GetFramebuffer());
        const unsigned width = m_core.GetFramebufferWidth();
        const unsigned height = m_core.GetFramebufferHeight();
        const unsigned pitch = m_core.GetFramebufferPitch() / 4;
        if (pixels == nullptr || width == 0 || height == 0) {
            return true;
        }
        const uint32_t first = pixels[0] & 0xFFFFFF;
        for (unsigned y = 0; y < height; y++) {
            for (unsigned x = 0; x < width; x++) {
                if ((pixels[static_cast<size_t>(y) * pitch + x] & 0xFFFFFF) != first) {
                    return false;
                }
            }
        }
        return true;
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
