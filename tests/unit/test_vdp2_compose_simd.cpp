// The compose fast path's vector helpers (vdp2_compose_simd.hpp): the SSE2 / NEON bodies (kVector = true) and the
// scalar code alone (kVector = false) against plain references (the layer sort against upstream's insertion of
// VDP2ComposeLine), on random data at every count from 0 to 50 (vector body + scalar tail: 17, 33, ...) and 704, from
// misaligned pointers.

#include "catch_amalgamated.hpp"

#include <ymir/hw/vdp/renderer/vdp2_compose_simd.hpp>

#include <array>
#include <cstdint>
#include <random>
#include <vector>

namespace {

using namespace ymir;
using namespace ymir::vdp;
using Special = SpriteData::Special;

constexpr uint32_t kOffset = 1; // misaligned start

std::vector<uint32_t> Counts() {
    std::vector<uint32_t> counts;
    for (uint32_t n = 0; n <= 50; ++n) {
        counts.push_back(n);
    }
    counts.push_back(704);
    return counts;
}

// Random sorted stacks as the sort builds them: keys of distinct layers (or the back screen's 1), descending
void RandomStacks(std::mt19937 &rng, std::vector<uint8> &k0, std::vector<uint8> &k1, std::vector<uint8> &k2) {
    for (size_t i = 0; i < k0.size(); ++i) {
        std::array<uint8, 3> e{1, 1, 1};
        for (uint32_t layer = 0; layer < 6; ++layer) {
            if (rng() % 3 == 0) {
                const uint8 key = static_cast<uint8>((layer ^ 7u) | ((1 + rng() % 7) << 3));
                for (int j = 0; j < 3; ++j) {
                    if (key > e[j]) {
                        for (int m = 2; m > j; --m) {
                            e[m] = e[m - 1];
                        }
                        e[j] = key;
                        break;
                    }
                }
            }
        }
        k0[i] = e[0];
        k1[i] = e[1];
        k2[i] = e[2];
    }
}

template <bool kVector>
void SortCase(uint32_t count, uint32_t seed, bool sprite) {
    std::mt19937 rng{seed};
    const size_t size = count + kOffset;
    std::vector<uint8> k0(size), k1(size), k2(size), prio(size);
    std::vector<Special> special(size);
    RandomStacks(rng, k0, k1, k2);
    for (size_t i = 0; i < size; ++i) {
        // Mostly 0-7, some larger values (the key's uint8 truncation)
        prio[i] = static_cast<uint8>(rng() % 8 == 0 ? rng() : rng() % 8);
        special[i] = static_cast<Special>(rng() % 3);
    }
    const uint32_t layer = sprite ? 0 : 1 + rng() % 5;
    const uint8 layerKey = static_cast<uint8>(layer ^ 7u);

    // Upstream's push() (VDP2ComposeLine)
    std::vector<uint8> r0 = k0, r1 = k1, r2 = k2;
    for (size_t x = kOffset; x < size; ++x) {
        if (prio[x] == 0 || (sprite && special[x] != Special::Normal)) {
            continue;
        }
        std::array<uint8, 3> e{r0[x], r1[x], r2[x]};
        const uint8 key = static_cast<uint8>(layerKey | (prio[x] << 3u));
        for (int i = 0; i < 3; i++) {
            if (key > e[i]) {
                for (int j = 2; j > i; j--) {
                    e[j] = e[j - 1];
                }
                e[i] = key;
                break;
            }
        }
        r0[x] = e[0];
        r1[x] = e[1];
        r2[x] = e[2];
    }

    compose::ComposeSortInsert<kVector>(k0.data() + kOffset, k1.data() + kOffset, k2.data() + kOffset,
                                        prio.data() + kOffset, sprite ? special.data() + kOffset : nullptr, layerKey,
                                        count);
    CHECK(k0 == r0);
    CHECK(k1 == r1);
    CHECK(k2 == r2);
}

template <bool kVector>
void KeysCase(uint32_t count, uint32_t seed) {
    std::mt19937 rng{seed};
    const size_t size = count + kOffset;
    std::vector<uint8> keys(size), layers(size, 0xAA), prios(size, 0xAA);
    for (auto &k : keys) {
        k = static_cast<uint8>(rng());
    }
    std::vector<uint8> rl = layers, rp = prios;
    for (size_t i = kOffset; i < size; ++i) {
        rl[i] = static_cast<uint8>(~keys[i] & 7u);
        rp[i] = static_cast<uint8>(keys[i] >> 3u);
    }
    compose::ComposeKeysToStack<kVector>(keys.data() + kOffset, layers.data() + kOffset, prios.data() + kOffset, count);
    CHECK(layers == rl);
    CHECK(prios == rp);
}

std::vector<bool> RandomBools(std::mt19937 &rng, size_t size) {
    std::vector<bool> v(size);
    for (size_t i = 0; i < size; ++i) {
        v[i] = rng() % 2 == 0;
    }
    return v;
}

// bool arrays as contiguous storage (std::vector<bool> is packed)
using Bools = std::vector<uint8>;
Bools ToBytes(const std::vector<bool> &v) {
    return Bools(v.begin(), v.end());
}
const bool *B(const Bools &v) {
    return reinterpret_cast<const bool *>(v.data());
}
bool *B(Bools &v) {
    return reinterpret_cast<bool *>(v.data());
}

template <bool kVector>
void FlagsCase(uint32_t count, uint32_t seed) {
    std::mt19937 rng{seed};
    const size_t size = count + kOffset;
    std::vector<uint8> keys(size);
    for (auto &k : keys) {
        k = static_cast<uint8>(rng());
    }
    const Bools cc = ToBytes(RandomBools(rng, size));
    const Bools shadowSrc = ToBytes(RandomBools(rng, size));
    Bools ccOut = ToBytes(RandomBools(rng, size)), shOut = ToBytes(RandomBools(rng, size)),
          ofOut = ToBytes(RandomBools(rng, size));
    const uint32_t layer = rng() % 8;
    const bool useSource = rng() % 2 == 0;
    const bool shadowConst = rng() % 2 == 0;
    const bool offsetConst = rng() % 2 == 0;

    Bools rc = ccOut, rs = shOut, ro = ofOut;
    for (size_t i = kOffset; i < size; ++i) {
        if ((~keys[i] & 7u) == layer) {
            rc[i] = rc[i] || cc[i];
            rs[i] = rs[i] || (useSource ? shadowSrc[i] != 0 : shadowConst);
            ro[i] = ro[i] || offsetConst;
        }
    }
    compose::ComposeGatherLayerFlags<kVector>(keys.data() + kOffset, layer, B(cc) + kOffset,
                                              useSource ? B(shadowSrc) + kOffset : nullptr, shadowConst, offsetConst,
                                              B(ccOut) + kOffset, B(shOut) + kOffset, B(ofOut) + kOffset, count);
    CHECK(ccOut == rc);
    CHECK(shOut == rs);
    CHECK(ofOut == ro);
}

template <bool kVector>
void FinishCase(uint32_t count, uint32_t seed) {
    std::mt19937 rng{seed};
    const size_t size = count + kOffset;
    std::vector<uint8> keys(size), spritePrio(size);
    std::vector<Special> special(size);
    for (size_t i = 0; i < size; ++i) {
        keys[i] = static_cast<uint8>(rng());
        spritePrio[i] = static_cast<uint8>(rng() % 8);
        special[i] = static_cast<Special>(rng() % 3);
    }
    const Bools window = ToBytes(RandomBools(rng, size));
    const Bools sow = ToBytes(RandomBools(rng, size));
    Bools ccOut = ToBytes(RandomBools(rng, size)), shOut = ToBytes(RandomBools(rng, size));
    const bool msbShadow = rng() % 2 == 0;

    // The layer 0 rules of VDP2ComposeLine (colour calculation window, shadow conditions)
    Bools rc = ccOut, rs = shOut;
    for (size_t i = kOffset; i < size; ++i) {
        rc[i] = rc[i] && !window[i];
        const bool shadowDot = special[i] == Special::Shadow || (msbShadow && sow[i]);
        rs[i] = rs[i] && !(spritePrio[i] < (keys[i] >> 3u)) && shadowDot;
    }
    compose::ComposeFinishLayer0Flags<kVector>(keys.data() + kOffset, B(window) + kOffset, spritePrio.data() + kOffset,
                                               special.data() + kOffset, B(sow) + kOffset, msbShadow,
                                               B(ccOut) + kOffset, B(shOut) + kOffset, count);
    CHECK(ccOut == rc);
    CHECK(shOut == rs);
}

} // namespace

TEST_CASE("Compose fast path vector helpers match the references, vector and scalar",
          "[vdp][vdp2][compose][fastpath][brimir]") {
    const bool vector = GENERATE(true, false);
    CAPTURE(vector);
    for (const uint32_t count : Counts()) {
        CAPTURE(count);
        for (uint32_t seed = 0; seed < 8; ++seed) {
            CAPTURE(seed);
            const uint32_t s = count * 131 + seed;
            if (vector) {
                SortCase<true>(count, s, seed % 2 == 0);
                KeysCase<true>(count, s);
                FlagsCase<true>(count, s);
                FinishCase<true>(count, s);
            } else {
                SortCase<false>(count, s, seed % 2 == 0);
                KeysCase<false>(count, s);
                FlagsCase<false>(count, s);
                FinishCase<false>(count, s);
            }
        }
    }
}
