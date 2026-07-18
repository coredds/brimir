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
