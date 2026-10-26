// Brimir CoreWrapper with the Vulkan VDP renderer (BRIMIR_VULKAN builds): install, revert, threading, device frames.
// No Vulkan device needed: the renderer is installed without one (its front half still runs).
// Copyright (C) 2026 coredds
// Licensed under GPL-3.0

#include "catch_amalgamated.hpp"

#include <brimir/core_wrapper.hpp>
#include <gpu/vdp/vulkan_vdp_renderer.hpp>

#include <ymir/sys/saturn.hpp>

#include <cstring>

using namespace brimir;

TEST_CASE("CoreWrapper installs the Vulkan VDP renderer and reverts to software", "[core][vulkan]") {
    CoreWrapper core;
    REQUIRE(core.Initialize());
    REQUIRE(core.GetVulkanRenderer() == nullptr);
    REQUIRE(std::strcmp(core.GetActiveRenderer(), "Software") == 0);

    gpu::VulkanVDPRenderer *renderer = core.InstallVulkanRenderer();
    REQUIRE(renderer != nullptr);
    CHECK(core.GetVulkanRenderer() == renderer);
    CHECK(core.InstallVulkanRenderer() == renderer); // already installed
    CHECK(&core.GetSaturn()->VDP.GetRenderer() == renderer);
    CHECK(std::strcmp(core.GetActiveRenderer(), "Vulkan") == 0);
    CHECK_FALSE(renderer->Attached());

    core.UseSoftwareRenderer();
    CHECK(core.GetVulkanRenderer() == nullptr);
    CHECK(core.GetSaturn()->VDP.GetRendererAs<ymir::vdp::VDPRendererType::Software>() != nullptr);
    CHECK(std::strcmp(core.GetActiveRenderer(), "Software") == 0);
    core.UseSoftwareRenderer(); // no-op
    CHECK(core.GetSaturn()->VDP.GetRendererAs<ymir::vdp::VDPRendererType::Software>() != nullptr);
}

TEST_CASE("CoreWrapper SetThreadedVDP1 reaches the installed Vulkan VDP renderer", "[core][vulkan]") {
    CoreWrapper core;
    REQUIRE(core.Initialize());
    // Initialize enables threaded VDP1; the install applies it.
    gpu::VulkanVDPRenderer *renderer = core.InstallVulkanRenderer();
    REQUIRE(renderer != nullptr);
    CHECK(renderer->ThreadedVDP1());

    core.SetThreadedVDP1(false);
    CHECK_FALSE(renderer->ThreadedVDP1());
    core.SetThreadedVDP1(true);
    CHECK(renderer->ThreadedVDP1());

    // A renderer installed while VDP1 threading is off starts unthreaded.
    core.UseSoftwareRenderer();
    core.SetThreadedVDP1(false);
    renderer = core.InstallVulkanRenderer();
    REQUIRE(renderer != nullptr);
    CHECK_FALSE(renderer->ThreadedVDP1());

    // Unloading stops it (the VDP's own threading observers skip external renderers).
    core.SetThreadedVDP1(true);
    REQUIRE(renderer->ThreadedVDP1());
    core.UnloadGame();
    CHECK_FALSE(renderer->ThreadedVDP1());
}

TEST_CASE("CoreWrapper reports the Vulkan VDP renderer's frames as device frames", "[core][vulkan]") {
    CoreWrapper core;
    REQUIRE(core.Initialize());
    core.SetOverscanCrop(16, 8);
    core.SetRotation(90);
    gpu::VulkanVDPRenderer *renderer = core.InstallVulkanRenderer();
    REQUIRE(renderer != nullptr);
    CHECK(core.LastDeviceFrame().width == 0);

    // No device: the front half still runs and the frame signal still fires.
    core.RunFrame();
    const CoreWrapper::DeviceFrame &frame = core.LastDeviceFrame();
    CHECK(frame.width >= 320);
    CHECK(frame.height >= 224);
    CHECK(frame.settings.cropH == 16);
    CHECK(frame.settings.cropV == 8);
    CHECK(frame.settings.rotation == 90);
    CHECK_FALSE(renderer->HasPendingFrame());

    // Back on software, frames reach the bridge's CPU path again.
    core.UseSoftwareRenderer();
    CHECK(core.LastDeviceFrame().width == 0);
    core.SetOverscanCrop(0, 0);
    core.SetRotation(0);
    core.RunFrame();
    CHECK(core.GetFramebufferWidth() >= 320);
}
