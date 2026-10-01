// Brimir profiler tests
// Licensed under GPL-3.0

#include "catch_amalgamated.hpp"
#include <brimir/core_wrapper.hpp>
#include <brimir/profiler.hpp>
#include <barrier>
#include <thread>
#include <type_traits>
#include <vector>

TEST_CASE("Profiler collection is disabled by default", "[profiler][unit]") {
    brimir::Profiler profiler;
    {
        brimir::ScopedTimer timer(profiler, "A section name longer than the small string buffer");
    }
    CHECK(profiler.GetAllTimings().empty());
    CHECK(profiler.GetReport().empty());
}

TEST_CASE("Default-disabled wrapper frame collects no profiling samples", "[profiler][integration]") {
    brimir::CoreWrapper core;
    REQUIRE(core.Initialize());
    core.RunFrame();
    CHECK(core.GetLastError().empty());
    CHECK(core.GetProfilingReport().empty());
}

TEST_CASE("Enabled profiler aggregates nested scopes independently", "[profiler][unit]") {
    brimir::Profiler profiler;
    profiler.SetEnabled(true);
    {
        brimir::ScopedTimer outer(profiler, "nested");
        brimir::ScopedTimer inner(profiler, "nested");
        brimir::ScopedTimer other(profiler, std::string("Temporary section name longer than SSO"));
    }
    const auto timing = profiler.GetTiming("nested");
    REQUIRE(timing);
    CHECK(timing->count == 2);
    CHECK(timing->minMs >= 0.0);
    CHECK(timing->minMs <= timing->avgMs());
    CHECK(timing->avgMs() <= timing->maxMs);
    CHECK(profiler.GetAllTimings().size() == 2);
    CHECK(profiler.GetReport().find("nested: avg=") != std::string::npos);
    CHECK(profiler.GetReport().find("samples=2") != std::string::npos);
    CHECK_FALSE(profiler.GetTiming("missing"));
    CHECK_FALSE(std::is_copy_constructible_v<brimir::ScopedTimer>);
    CHECK_FALSE(std::is_move_constructible_v<brimir::ScopedTimer>);
}

TEST_CASE("Profiler reset invalidates active scopes without disabling collection", "[profiler][unit]") {
    brimir::Profiler profiler;
    profiler.SetEnabled(true);
    {
        brimir::ScopedTimer completed(profiler, "completed");
    }
    const auto snapshot = profiler.GetAllTimings();
    const auto timing = profiler.GetTiming("completed");
    {
        brimir::ScopedTimer stale(profiler, "same");
        profiler.Reset();
        CHECK(profiler.GetReport().empty());
        {
            brimir::ScopedTimer fresh(profiler, "same");
        }
    }
    const auto current = profiler.GetAllTimings();
    REQUIRE(current.size() == 1);
    CHECK(current.at("same").count == 1);
    CHECK(snapshot.at("completed").count == 1);
    REQUIRE(timing);
    CHECK(timing->count == 1);
}

TEST_CASE("Profiler live toggles start fresh and discard stale scopes", "[profiler][unit]") {
    brimir::Profiler profiler;
    {
        brimir::ScopedTimer disabled(profiler, "disabled");
        profiler.SetEnabled(true);
    }
    CHECK(profiler.GetAllTimings().empty());
    {
        brimir::ScopedTimer stale(profiler, "stale");
        {
            brimir::ScopedTimer completed(profiler, "completed");
        }
        profiler.SetEnabled(false);
        CHECK(profiler.GetReport().empty());
        profiler.Reset();
        {
            brimir::ScopedTimer disabled(profiler, "still disabled");
        }
        CHECK(profiler.GetAllTimings().empty());
        profiler.SetEnabled(true);
        brimir::ScopedTimer fresh(profiler, "fresh");
        profiler.SetEnabled(true); // Applying an unchanged option must not reset.
    }
    const auto timings = profiler.GetAllTimings();
    REQUIRE(timings.size() == 1);
    CHECK(timings.at("fresh").count == 1);
}

TEST_CASE("Profiler aggregates concurrent same-name scopes and snapshots", "[profiler][unit][threading]") {
    brimir::Profiler profiler;
    profiler.SetEnabled(true);
    std::barrier start(5);
    std::vector<std::jthread> workers;
    for (int worker = 0; worker < 4; ++worker) {
        workers.emplace_back([&] {
            start.arrive_and_wait();
            for (int i = 0; i < 500; ++i) {
                brimir::ScopedTimer outer(profiler, "shared");
                brimir::ScopedTimer inner(profiler, "shared");
            }
        });
    }
    start.arrive_and_wait();
    for (int i = 0; i < 100; ++i) {
        (void)profiler.GetAllTimings();
        (void)profiler.GetTiming("shared");
        (void)profiler.GetReport();
    }
    workers.clear(); // Join before checking exact sample counts.
    const auto timing = profiler.GetTiming("shared");
    REQUIRE(timing);
    CHECK(timing->count == 4000);
}

TEST_CASE("Profiler reset and toggles invalidate scopes held by callback threads", "[profiler][unit][threading]") {
    brimir::Profiler profiler;
    profiler.SetEnabled(true);
    std::barrier phase(2);
    std::jthread callback([&] {
        for (int i = 0; i < 100; ++i) {
            {
                brimir::ScopedTimer stale(profiler, "callback");
                phase.arrive_and_wait();
                phase.arrive_and_wait();
            }
            phase.arrive_and_wait();
        }
    });
    for (int i = 0; i < 100; ++i) {
        phase.arrive_and_wait();
        if (i % 2 == 0) {
            profiler.Reset();
        } else {
            profiler.SetEnabled(false);
            profiler.SetEnabled(true);
        }
        {
            brimir::ScopedTimer fresh(profiler, "callback");
        }
        phase.arrive_and_wait();
        phase.arrive_and_wait();
        const auto timings = profiler.GetAllTimings();
        CHECK(timings.size() == 1);
        CHECK((timings.contains("callback") && timings.at("callback").count == 1));
    }
}

TEST_CASE("Wrapper profiling can be enabled reset and disabled live", "[profiler][integration]") {
    brimir::CoreWrapper core;
    REQUIRE(core.Initialize());
    core.SetProfilingEnabled(true);
    core.RunFrame();
    CHECK(core.GetLastError().empty());
    const auto report = core.GetProfilingReport();
    CHECK(report.find("RunFrame_Total: avg=") != std::string::npos);
    CHECK(report.find("Ymir_RunFrame: avg=") != std::string::npos);
    CHECK(report.find("OnFrameComplete_Total: avg=") != std::string::npos);
    CHECK(report.find("PixelConversion: avg=") != std::string::npos);
    core.ResetProfiling();
    CHECK(core.GetProfilingReport().empty());
    core.RunFrame();
    CHECK_FALSE(core.GetProfilingReport().empty());
    core.SetProfilingEnabled(false);
    core.RunFrame();
    CHECK(core.GetProfilingReport().empty());
    core.SetProfilingEnabled(true);
    CHECK(core.GetProfilingReport().empty());
    core.RunFrame();
    CHECK_FALSE(core.GetProfilingReport().empty());
}

TEST_CASE("Profiler AddSample aggregates only while enabled", "[profiler][unit]") {
    brimir::Profiler profiler;
    REQUIRE_FALSE(profiler.IsEnabled());
    profiler.AddSample("X", 1.0);
    REQUIRE_FALSE(profiler.GetTiming("X").has_value());

    profiler.SetEnabled(true);
    REQUIRE(profiler.IsEnabled());
    profiler.AddSample("X", 1.0);
    profiler.AddSample("X", 3.0);

    const auto timing = profiler.GetTiming("X");
    REQUIRE(timing.has_value());
    REQUIRE(timing->count == 2);
    REQUIRE(timing->totalMs == Catch::Approx(4.0));
    REQUIRE(timing->minMs == Catch::Approx(1.0));
    REQUIRE(timing->maxMs == Catch::Approx(3.0));
}

TEST_CASE("SH-2 host time accounting is off by default and consumable", "[profiler][sh2]") {
    brimir::CoreWrapper core;
    REQUIRE(core.Initialize());
    auto* saturn = core.GetSaturn();
    REQUIRE(saturn != nullptr);

    // Uses the built-in null IPL program (no BIOS loaded).
    core.RunFrame();
    REQUIRE(saturn->masterSH2.ConsumeHostTimeNs() == 0);

    saturn->masterSH2.SetHostTimeProfiling(true);
    core.RunFrame();
    REQUIRE(saturn->masterSH2.ConsumeHostTimeNs() > 0);
    REQUIRE(saturn->masterSH2.ConsumeHostTimeNs() == 0);

    saturn->masterSH2.SetHostTimeProfiling(false);
    core.RunFrame();
    REQUIRE(saturn->masterSH2.ConsumeHostTimeNs() == 0);
}

TEST_CASE("Enabled wrapper profiling records SH-2 time per frame", "[profiler][integration]") {
    brimir::CoreWrapper core;
    REQUIRE(core.Initialize());
    core.SetProfilingEnabled(true);
    for (int i = 0; i < 3; ++i) {
        core.RunFrame();
    }

    const auto master = core.GetProfiler().GetTiming("SH2_Master");
    const auto slave = core.GetProfiler().GetTiming("SH2_Slave");
    REQUIRE(master.has_value());
    REQUIRE(slave.has_value());
    REQUIRE(master->count == 3);
    REQUIRE(slave->count == 3);
    REQUIRE(master->totalMs > 0.0);
}

TEST_CASE("Profiling enabled before Initialize applies to the SH-2s", "[profiler][integration]") {
    brimir::CoreWrapper core;
    core.SetProfilingEnabled(true);
    REQUIRE(core.Initialize());
    core.RunFrame();
    const auto master = core.GetProfiler().GetTiming("SH2_Master");
    REQUIRE(master.has_value());
    REQUIRE(master->totalMs > 0.0);
}
