// Brimir - Core options metadata tests
// Validates that options.cpp exposes the expected keys and defaults.

#include "catch_amalgamated.hpp"
#include "options.hpp"
#include <cstring>

TEST_CASE("Core options include profiling option", "[options][libretro]") {
    const struct retro_core_options_v2* options = brimir_get_core_options_v2();
    REQUIRE(options != nullptr);
    REQUIRE(options->definitions != nullptr);

    const retro_core_option_v2_definition* found = nullptr;
    for (int i = 0; options->definitions[i].key != nullptr; ++i) {
        if (std::strcmp(options->definitions[i].key, "brimir_profiling") == 0) {
            found = &options->definitions[i];
            break;
        }
    }

    REQUIRE(found != nullptr);
    REQUIRE(found->default_value != nullptr);
    REQUIRE(std::strcmp(found->default_value, "disabled") == 0);
}

TEST_CASE("Core options default values match OptionCache defaults", "[options][libretro]") {
    const struct retro_core_options_v2* options = brimir_get_core_options_v2();
    REQUIRE(options != nullptr);
    REQUIRE(options->definitions != nullptr);

    struct Expected {
        const char* key;
        const char* default_value;
    };

    Expected expected[] = {
        { "brimir_bios",                 "auto"     },
        { "brimir_deinterlacing",        "enabled"  },
        { "brimir_deinterlace_mode",     "bob"      },
        { "brimir_autodetect_region",    "enabled"  },
        { "brimir_audio_interpolation",  "linear"   },
        { "brimir_audio_volume",         "100"      },
        { "brimir_rotation",             "0"        },
        { "brimir_overscan",             "0"        },
        { "brimir_cd_speed",             "2"        },
        { "brimir_sh2_overclock",        "100"      },
        { "brimir_sh2_jit",              "disabled" },
        { "brimir_sh2_jit_compile_thread", "auto"   },
        { "brimir_profiling",            "disabled" },
    };

    for (const auto& e : expected) {
        const retro_core_option_v2_definition* found = nullptr;
        for (int i = 0; options->definitions[i].key != nullptr; ++i) {
            if (std::strcmp(options->definitions[i].key, e.key) == 0) {
                found = &options->definitions[i];
                break;
            }
        }

        INFO("Looking for option: " << e.key);
        REQUIRE(found != nullptr);
        REQUIRE(found->default_value != nullptr);
        INFO("Default for " << e.key << ": expected '" << e.default_value << "', got '" << found->default_value << "'");
        REQUIRE(std::strcmp(found->default_value, e.default_value) == 0);
    }
}

namespace {

const char* const kModes[] = { "bob", "weave", "blend", "current", "none" };

bool ExpectedEffective(bool deinterlacingOn, const char* mode) {
    return deinterlacingOn && std::strcmp(mode, "none") != 0;
}

const char* DeinterlacingValue(bool on) {
    return on ? "enabled" : "disabled";
}

// Applies one option set the way apply_core_options does and returns the flag last written to the core.
struct FakeCoreFlag {
    BrimirDeinterlaceOptions cache;
    bool flag = true; // CoreWrapper::Initialize turns deinterlacing on
    int writes = 0;

    void Apply(const char* deinterlacing, const char* mode, bool force) {
        bool effective = true;
        if (brimir_update_deinterlace(cache, deinterlacing, mode, force, effective)) {
            flag = effective;
            ++writes;
        }
    }
};

} // namespace

TEST_CASE("Deinterlacing effective flag is Deinterlacing ON and mode not None", "[options][libretro]") {
    for (bool on : {false, true}) {
        for (const char* mode : kModes) {
            INFO("deinterlacing " << DeinterlacingValue(on) << ", mode " << mode);
            CHECK(brimir_deinterlace_effective(DeinterlacingValue(on), mode) == ExpectedEffective(on, mode));
        }
    }
}

TEST_CASE("Deinterlacing options at load give the same flag for every combination", "[options][libretro]") {
    for (bool on : {false, true}) {
        for (const char* mode : kModes) {
            INFO("deinterlacing " << DeinterlacingValue(on) << ", mode " << mode);
            FakeCoreFlag core;
            core.Apply(DeinterlacingValue(on), mode, true);
            CHECK(core.flag == ExpectedEffective(on, mode));
        }
    }
}

TEST_CASE("Deinterlacing options live change of either option, in either order", "[options][libretro]") {
    for (bool startOn : {false, true}) {
        for (const char* startMode : kModes) {
            for (bool endOn : {false, true}) {
                for (const char* endMode : kModes) {
                    for (bool deinterlacingFirst : {false, true}) {
                        INFO("from " << DeinterlacingValue(startOn) << "/" << startMode << " to "
                                     << DeinterlacingValue(endOn) << "/" << endMode
                                     << (deinterlacingFirst ? ", Deinterlacing changed first" : ", mode changed first"));
                        FakeCoreFlag core;
                        core.Apply(DeinterlacingValue(startOn), startMode, true);
                        REQUIRE(core.flag == ExpectedEffective(startOn, startMode));

                        // Frontend applies each change on a frame of its own.
                        if (deinterlacingFirst) {
                            core.Apply(DeinterlacingValue(endOn), startMode, false);
                            core.Apply(DeinterlacingValue(endOn), endMode, false);
                        } else {
                            core.Apply(DeinterlacingValue(startOn), endMode, false);
                            core.Apply(DeinterlacingValue(endOn), endMode, false);
                        }
                        CHECK(core.flag == ExpectedEffective(endOn, endMode));

                        // Both changes in the same poll give the same result.
                        FakeCoreFlag together;
                        together.Apply(DeinterlacingValue(startOn), startMode, true);
                        together.Apply(DeinterlacingValue(endOn), endMode, false);
                        CHECK(together.flag == ExpectedEffective(endOn, endMode));
                    }
                }
            }
        }
    }
}

TEST_CASE("Deinterlacing off is not undone by the mode option", "[options][libretro]") {
    FakeCoreFlag core;
    core.Apply("disabled", "bob", true);
    CHECK_FALSE(core.flag);
    core.Apply("disabled", "weave", false);
    CHECK_FALSE(core.flag);
    core.Apply("enabled", "weave", false);
    CHECK(core.flag);
    core.Apply("enabled", "none", false);
    CHECK_FALSE(core.flag);
}

TEST_CASE("Deinterlacing options write the core only on a change or when forced", "[options][libretro]") {
    FakeCoreFlag core;
    core.Apply("enabled", "bob", false);
    CHECK(core.writes == 0);
    core.Apply("enabled", "bob", true);
    CHECK(core.writes == 1);
    core.Apply("disabled", "bob", false);
    CHECK(core.writes == 2);
    core.Apply("disabled", "bob", false);
    CHECK(core.writes == 2);
}

TEST_CASE("Deinterlacing options keep the cached value when the frontend returns none", "[options][libretro]") {
    FakeCoreFlag core;
    core.Apply("disabled", "none", true);
    core.Apply(nullptr, nullptr, false);
    CHECK(core.cache.deinterlacing == "disabled");
    CHECK(core.cache.mode == "none");
    CHECK_FALSE(core.flag);
}

TEST_CASE("Deinterlacing option cache defaults match the option definitions", "[options][libretro]") {
    const struct retro_core_options_v2* options = brimir_get_core_options_v2();
    BrimirDeinterlaceOptions cache;
    for (int i = 0; options->definitions[i].key != nullptr; ++i) {
        if (std::strcmp(options->definitions[i].key, "brimir_deinterlacing") == 0) {
            CHECK(cache.deinterlacing == options->definitions[i].default_value);
        }
        if (std::strcmp(options->definitions[i].key, "brimir_deinterlace_mode") == 0) {
            CHECK(cache.mode == options->definitions[i].default_value);
        }
    }
}
