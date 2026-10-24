// brimir-gpu build plumbing: vendored Vulkan headers, gpu_version.hpp and the offline SPIR-V headers.
#include "catch_amalgamated.hpp"

#include <gpu/gpu_version.hpp>
#include <gpu/shaders/generated/cs_fill_test.spv.h>

#include <cstdint>
#include <iterator>

using brimir::gpu::shaders::kCsFillTestSpv;

TEST_CASE("Generated SPIR-V header holds a SPIR-V module", "[gpu][build]") {
    REQUIRE(std::size(kCsFillTestSpv) > 5); // SPIR-V header is 5 words
    CHECK(kCsFillTestSpv[0] == 0x07230203u);
    CHECK(sizeof(kCsFillTestSpv) % 4 == 0);
}

TEST_CASE("brimir-gpu requires Vulkan 1.2", "[gpu][build]") {
    STATIC_CHECK(brimir::gpu::kRequiredApiVersion == VK_API_VERSION_1_2);
    CHECK(VK_API_VERSION_MAJOR(brimir::gpu::kRequiredApiVersion) == 1);
    CHECK(VK_API_VERSION_MINOR(brimir::gpu::kRequiredApiVersion) == 2);
}