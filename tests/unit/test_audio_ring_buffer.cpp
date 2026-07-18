// Audio ring buffer unit tests
#include "catch_amalgamated.hpp"
#include <brimir/audio_ring_buffer.hpp>
#include <vector>

using namespace brimir;

TEST_CASE("Audio ring buffer push/pop round-trip", "[audio][unit]") {
    AudioRingBuffer<64> buf;

    REQUIRE(buf.Push(1, 2));
    REQUIRE(buf.Push(3, 4));
    REQUIRE(buf.Push(5, 6));
    REQUIRE(buf.Available() == 3);

    std::vector<int16_t> out(6);
    size_t popped = buf.Pop(out.data(), 2);
    REQUIRE(popped == 2);
    REQUIRE(out[0] == 1);
    REQUIRE(out[1] == 2);
    REQUIRE(out[2] == 3);
    REQUIRE(out[3] == 4);
    REQUIRE(buf.Available() == 1);

    popped = buf.Pop(out.data(), 10);
    REQUIRE(popped == 1);
    REQUIRE(out[0] == 5);
    REQUIRE(out[1] == 6);
}

TEST_CASE("Audio ring buffer overflow is safe", "[audio][unit]") {
    AudioRingBuffer<4> buf;

    // The ring buffer reserves one stereo slot to distinguish empty/full.
    REQUIRE(buf.Push(1, 1));
    REQUIRE(buf.Push(2, 2));
    REQUIRE(buf.Push(3, 3));
    REQUIRE(buf.Available() == 3);

    // The 4th push must be rejected (overflow)
    REQUIRE_FALSE(buf.Push(4, 4));

    // Previously stored data is intact
    std::vector<int16_t> out(8);
    REQUIRE(buf.Pop(out.data(), 4) == 3);
    REQUIRE(out[0] == 1);
    REQUIRE(out[4] == 3);
}
