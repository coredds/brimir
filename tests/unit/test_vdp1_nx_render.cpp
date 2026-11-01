// Render mode of the N x VDP1 reference (GPU stage 3, Task 1): replays captured frames (brimir_vkhost --dump-vdp1-ops;
// src/gpu/vdp1/vdp1_op_dump.hpp) through the N x reference for the candidate rule sets, decodes the drawn framebuffer
// as VDP2's sprite layer (sprite layer only, over a neutral background) and writes PPM images, crop sheets and a
// workload table. Hidden; run by hand:
//   BRIMIR_VDP1_CAPTURES=<dir with <game>/vdp1ops_*.bin> BRIMIR_NX_RENDER_OUT=<dir> [BRIMIR_NX_CROPS=<file>]
//   brimir_tests "[nx-render]"
// Crops file: one "<game> <x> <y> <w> <h>" per line, in the 1x image's pixels.

#include "catch_amalgamated.hpp"

#include "vdp1_nx_reference.hpp"
#include "vdp1_scenario.hpp"

#include <gpu/vdp1/vdp1_op_dump.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

namespace {

namespace fs = std::filesystem;
namespace gpu = brimir::gpu::vdp1;
namespace ref = brimir::test::vdp1ref;
namespace nx = brimir::test::vdp1nx;

struct Image {
    uint32 w = 0, h = 0;
    std::vector<uint8> rgb;
    Image(uint32 w_, uint32 h_)
        : w(w_)
        , h(h_)
        , rgb(size_t{w_} * h_ * 3, 0) {}
    void Set(uint32 x, uint32 y, uint32 c) {
        uint8 *p = &rgb[(size_t{y} * w + x) * 3];
        p[0] = static_cast<uint8>(c);
        p[1] = static_cast<uint8>(c >> 8);
        p[2] = static_cast<uint8>(c >> 16);
    }
    uint32 Get(uint32 x, uint32 y) const {
        const uint8 *p = &rgb[(size_t{y} * w + x) * 3];
        return p[0] | (p[1] << 8) | (p[2] << 16);
    }
};

void WritePpm(const fs::path &path, const Image &img) {
    std::ofstream out(path, std::ios::binary);
    out << "P6\n" << img.w << " " << img.h << "\n255\n";
    out.write(reinterpret_cast<const char *>(img.rgb.data()), static_cast<std::streamsize>(img.rgb.size()));
}

// -----------------------------------------------------------------------------
// VDP2 sprite decode (sw VDP2DrawSpritePixel / VDP2FetchSpriteData), sprite layer only

constexpr uint32 kBackground = 0x404040; // neutral grey
constexpr uint32 kShadow = 0x202020;

struct SpriteDecoder {
    const gpu::Vdp1OpDump &d;
    uint32 type = 0;
    bool mixed = false, spriteWindow = false;
    uint32 colorOffset = 0;
    bool pixel8 = false, doubleDensity = false, deinterlace = false, transparentMeshes = false;
    uint32 fbSizeH = 512;
    uint32 width = 320, height = 224; // output pixels (1x)
    uint32 xOutputShift = 0, xReadoutShift = 0;

    explicit SpriteDecoder(const gpu::Vdp1OpDump &dump, const ref::RasterState &state)
        : d(dump) {
        const uint16 spctl = d.vdp2Regs[0xE0 / 2];
        type = spctl & 0xF;
        spriteWindow = (spctl >> 4) & 1;
        mixed = (spctl >> 5) & 1;
        colorOffset = ((d.vdp2Regs[0xE6 / 2] >> 4) & 7) << 8;
        const uint16 tvmd = d.vdp2Regs[0];
        const uint32 hreso = tvmd & 7;
        pixel8 = state.pixel8Bits;
        fbSizeH = state.fbSizeH;
        doubleDensity = ((tvmd >> 6) & 3) == 3;
        deinterlace = d.deinterlace != 0;
        transparentMeshes = d.transparentMeshes != 0;
        const bool hdtv = (d.tvmr >> 2) & 1;
        const bool exclMon = (hreso & 4) != 0;
        const bool doubleResH = !hdtv && !pixel8 && (hreso & 6) == 2;
        const bool halfResH = !hdtv && pixel8 && (hreso & 6) == 0;
        xOutputShift = doubleResH || exclMon ? 1 : 0;
        xReadoutShift = halfResH ? 1 : 0;
        width = d.hres;
        height = d.vres;
    }

    // The FB value (16-bit word, or 8-bit byte in the low bits) to a colour over `below`; false if transparent
    bool Decode(uint32 raw, uint32 below, uint32 &out) const {
        if (mixed && !pixel8 && (raw & 0x8000)) {
            if (type >= 8 ? (raw & 0xFF) == 0 : (type >= 2 && spriteWindow && (raw & 0x7FFF) == 0)) {
                return false;
            }
            auto x8 = [](uint32 v) { return (v << 3) | (v >> 2); };
            out = x8(raw & 0x1F) | (x8((raw >> 5) & 0x1F) << 8) | (x8((raw >> 10) & 0x1F) << 16);
            return true;
        }
        uint32 data = raw;
        if (pixel8) {
            data = raw & 0xFF;
            if (type < 8) {
                data |= 0xFF00;
            }
        }
        static constexpr uint8 kColorBits[16] = {11, 11, 11, 11, 10, 11, 10, 9, 7, 6, 6, 6, 8, 8, 8, 8};
        static constexpr bool kWindowBit[16] = {false, false, true, true, true, true, true, true,
                                                false, false, false, false, false, false, false, false};
        const uint32 bits = kColorBits[type];
        const uint32 colorData = data & ((1u << bits) - 1);
        if ((data & 0x7FFF) == 0) {
            return false;
        }
        if (kWindowBit[type] && spriteWindow && (data & 0x8000)) {
            return false;
        }
        if (colorData == (1u << bits) - 2u) { // normal shadow
            out = (below >> 1) & 0x7F7F7F;
            return true;
        }
        out = d.palette[(colorOffset + colorData) & 0x7FF];
        return true;
    }
};

uint32 Average(uint32 a, uint32 b) {
    return ((a >> 1) & 0x7F7F7F) + ((b >> 1) & 0x7F7F7F);
}

// Decodes framebuffer `fb` of a surface whose words are read by `word(slice, nativeWord, i, j)` at scale n
template <typename WordFn>
Image DecodeSprites(const SpriteDecoder &dec, uint32 fb, uint32 n, WordFn &&word) {
    const bool interlacedRows = dec.doubleDensity && dec.deinterlace;
    Image img(dec.width * n, dec.height * n);
    for (uint32 Y = 0; Y < img.h; ++Y) {
        const uint32 y = Y / n, j = Y % n;
        const bool alt = interlacedRows && (y & 1);
        const uint32 fbRow = interlacedRows ? y >> 1 : y;
        const uint32 mainSlice = alt ? gpu::Vdp1AltSlice(fb) : gpu::Vdp1MainSlice(fb);
        const uint32 meshSlice = gpu::Vdp1MeshSlice(alt ? 1 : 0, fb);
        for (uint32 X = 0; X < img.w; ++X) {
            // output pixel -> FB pixel at N x (doubled output pixels share an FB pixel)
            const uint32 xo = X >> dec.xOutputShift;   // N x FB-space column before readout shift
            const uint32 fx = xo / n, i = xo % n;
            uint32 fbx = fx, fi = i;
            if (dec.xReadoutShift) { // 8 bpp read at half resolution: every other N x texel
                const uint32 v = 2 * (n * fx + i);
                fbx = v / n;
                fi = v % n;
            }
            const uint32 offset = fbRow * dec.fbSizeH + fbx;
            auto value = [&](uint32 slice) -> uint32 {
                if (dec.pixel8) {
                    const uint32 w = (offset >> 1) & 0x1FFFF;
                    const uint16 v = word(slice, w, fi, j);
                    return (offset & 1) ? (v & 0xFF) : (v >> 8);
                }
                return word(slice, offset & 0x1FFFF, fi, j);
            };
            uint32 c = kBackground;
            uint32 decoded = 0;
            if (dec.Decode(value(mainSlice), c, decoded)) {
                c = decoded;
            }
            if (dec.transparentMeshes && dec.Decode(value(meshSlice), c, decoded)) {
                c = Average(c, decoded);
            }
            img.Set(X, Y, c);
        }
    }
    return img;
}

Image Nearest(const Image &src, uint32 n) {
    Image out(src.w * n, src.h * n);
    for (uint32 y = 0; y < out.h; ++y) {
        for (uint32 x = 0; x < out.w; ++x) {
            out.Set(x, y, src.Get(x / n, y / n));
        }
    }
    return out;
}

void ToFbram(const std::vector<uint8> &bytes, ref::Fbram &fb) {
    ymir::vdp::SpriteFB *slices[8] = {&fb.main[0],    &fb.main[1],    &fb.alt[0],     &fb.alt[1],
                                      &fb.mesh[0][0], &fb.mesh[0][1], &fb.mesh[1][0], &fb.mesh[1][1]};
    for (uint32 i = 0; i < 8; ++i) {
        std::copy_n(bytes.begin() + i * gpu::kFbramSliceSize, gpu::kFbramSliceSize, slices[i]->begin());
    }
}

struct Variant {
    std::string tag;
    nx::Rules rules;
};

struct Crop {
    uint32 x, y, w, h;
};

} // namespace

TEST_CASE("VDP1 N x renders of a half-transparent mesh scene (BRIMIR_NX_RENDER_OUT)", "[.][nx-render]") {
    // Seams: a rotated 6 x 4 mesh of half-transparent Gouraud quads sharing vertices, a shadow-mode mesh and a fan of
    // thin quads, over an opaque textured background, 16 bpp RGB (decoded directly)
    using namespace brimir::test::vdp1scene;
    const char *outDir = std::getenv("BRIMIR_NX_RENDER_OUT");
    if (outDir == nullptr) {
        SKIP("BRIMIR_NX_RENDER_OUT not set");
    }
    const fs::path out = outDir;
    fs::create_directories(out);
    Scenario s;
    const uint16 tex = s.AddTexture(RGBTexture(32, 32, 7));
    s.Op0(Op::Kind::BeginFrame);
    s.Distorted(0, 0x00E8, 0, tex, 0x0420, {0, 0}, {319, 0}, {319, 223}, {0, 223}); // background, RGB, opaque
    auto mesh = [&](sint32 cx, sint32 cy, double angle, uint16 pmod, uint16 color) {
        const double c = std::cos(angle), sn = std::sin(angle);
        auto at = [&](sint32 i, sint32 j) {
            const double x = (i - 3) * 18.0, y = (j - 2) * 18.0;
            return CoordS32{cx + static_cast<sint32>(std::lround(x * c - y * sn)),
                            cy + static_cast<sint32>(std::lround(x * sn + y * c))};
        };
        for (sint32 j = 0; j < 4; ++j) {
            for (sint32 i = 0; i < 6; ++i) {
                const uint16 grda = s.AddGouraud({static_cast<uint16>(0x4210 + i * 0x0421), 0x5294, 0x318C,
                                                  static_cast<uint16>(0x4210 + j * 0x0842)});
                s.Polygon(pmod, color, at(i, j), at(i + 1, j), at(i + 1, j + 1), at(i, j + 1), grda);
            }
        }
    };
    mesh(90, 70, 0.35, 0x00C7, 0xFC1F);  // half-transparency + Gouraud
    mesh(230, 75, -0.5, 0x00C1, 0x8000); // shadow
    mesh(160, 165, 0.15, 0x00C3, 0x83FF); // half-transparency
    for (sint32 k = 0; k < 12; ++k) {    // thin quads (lines drawn as degenerate polygons)
        const CoordS32 a{20 + k * 4, 120}, b{40 + k * 9, 215};
        s.Polygon(0x00C0, static_cast<uint16>(0xFFE0 - k * 0x0400), a, b, b, a);
    }
    s.Op0(Op::Kind::EndFrame);

    std::vector<gpu::Vdp1OpList> lists;
    (void)RunReference(s, {}, 0, [&](const gpu::Vdp1OpList &ops) { lists.push_back(ops); });
    gpu::Vdp1OpDump d;
    d.vdp2Regs[0xE0 / 2] = 0x0020; // mixed format: RGB words decode directly
    d.hres = 320;
    d.vres = 224;
    const SpriteDecoder dec{d, ref::RasterState{}};
    using R = nx::VertexRule;
    const std::vector<Variant> variants = {
        {"R1", {R::FloorHalfDown, nx::ThinRule::Hairline, nx::LineRule::NativeThick}},
        {"R2", {R::FloorHalf, nx::ThinRule::Hairline, nx::LineRule::NativeThick}},
        {"R3", {R::CentroidSign, nx::ThinRule::Hairline, nx::LineRule::NativeThick}},
        {"R1-T2", {R::FloorHalfDown, nx::ThinRule::Parallel, nx::LineRule::NativeThick}},
        {"R3-T2", {R::CentroidSign, nx::ThinRule::Parallel, nx::LineRule::NativeThick}},
    };
    for (const uint32 n : {1u, 2u, 4u}) {
        for (const Variant &v : variants) {
            if (n == 1 && v.tag != "R1") {
                continue;
            }
            auto native = std::make_unique<ref::Fbram>();
            auto surface = std::make_unique<nx::NxFbram>(n);
            nx::ReferenceNx r{*native, *surface, nx::Options{.scale = n, .rules = v.rules}};
            for (const auto &l : lists) {
                r.Replay(l);
            }
            const Image img = DecodeSprites(dec, 0, n, [&](uint32 sl, uint32 w, uint32 i, uint32 j) {
                return surface->Word(sl, w, i, j);
            });
            WritePpm(out / ("seams_" + std::to_string(n) + "x_" + (n == 1 ? std::string("native") : v.tag) + ".ppm"),
                     img);
            if (n == 1) {
                for (const uint32 m : {2u, 4u}) {
                    WritePpm(out / ("seams_" + std::to_string(m) + "x_nearest.ppm"), Nearest(img, m));
                }
            }
        }
    }
}

TEST_CASE("VDP1 N x renders of captured frames (BRIMIR_VDP1_CAPTURES, BRIMIR_NX_RENDER_OUT)", "[.][nx-render]") {
    const char *capDir = std::getenv("BRIMIR_VDP1_CAPTURES");
    const char *outDir = std::getenv("BRIMIR_NX_RENDER_OUT");
    if (capDir == nullptr || outDir == nullptr) {
        SKIP("BRIMIR_VDP1_CAPTURES and BRIMIR_NX_RENDER_OUT not set");
    }
    const fs::path out = outDir;
    fs::create_directories(out);
    std::multimap<std::string, Crop> crops;
    if (const char *cropFile = std::getenv("BRIMIR_NX_CROPS")) {
        std::ifstream in(cropFile);
        std::string line;
        while (std::getline(in, line)) {
            std::istringstream s(line);
            std::string game;
            Crop c{};
            if (s >> game >> c.x >> c.y >> c.w >> c.h) {
                crops.emplace(game, c);
            }
        }
    }

    using R = nx::VertexRule;
    const std::vector<Variant> variants = {
        {"R1", {R::FloorHalfDown, nx::ThinRule::Hairline, nx::LineRule::NativeThick}},
        {"R2", {R::FloorHalf, nx::ThinRule::Hairline, nx::LineRule::NativeThick}},
        {"R3", {R::CentroidSign, nx::ThinRule::Hairline, nx::LineRule::NativeThick}},
        {"R1-T2", {R::FloorHalfDown, nx::ThinRule::Parallel, nx::LineRule::NativeThick}},
        {"R1-L2", {R::FloorHalfDown, nx::ThinRule::Hairline, nx::LineRule::Parallel}},
        {"R3-T2", {R::CentroidSign, nx::ThinRule::Parallel, nx::LineRule::NativeThick}},
        {"R3-L2", {R::CentroidSign, nx::ThinRule::Hairline, nx::LineRule::Parallel}},
    };

    std::ofstream table(out / "workload.md");
    table << "| capture | N | rules | native spans | native slots | native fragments | N x spans (rect / quad / "
             "replicate) | N x slots | N x fragments | sub-batches | max sub-batch slots | max per unit | generate ms | "
             "raster ms | resolve ms |\n|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|\n";
    std::ofstream crackList(out / "cracks.txt"); // game N rules fb x,y unwritten-sub-texels
    std::ofstream metrics(out / "quality.md");
    metrics << "| capture | N | rules | interior native words | unwritten N x sub-texels inside them | "
               "half-transparency fragments / N^2 | half-transparency double-blend units / N^2 | overwrite-class share |\n"
               "|---|---|---|---|---|---|---|---|\n";
    std::ofstream diffs(out / "diffs.md");
    diffs << "| capture | N | variant | differing N x pixels | bounding box (1x pixels) |\n|---|---|---|---|---|\n";
    std::map<std::string, Image> bases;
    std::ofstream classes(out / "commands.md");
    classes << "| capture | N | rules | ";
    for (uint32 c = 0; c < nx::CommandCounts::Count; ++c) {
        classes << nx::CommandCounts::Name(c) << " | ";
    }
    classes << "native long lines |\n|---|---|---|";
    for (uint32 c = 0; c <= nx::CommandCounts::Count; ++c) {
        classes << "---|";
    }
    classes << "\n";

    std::vector<fs::path> files;
    for (const auto &e : fs::recursive_directory_iterator(capDir)) {
        if (e.path().extension() == ".bin" && e.path().filename().string().rfind("vdp1ops_", 0) == 0) {
            files.push_back(e.path());
        }
    }
    std::sort(files.begin(), files.end());
    REQUIRE(!files.empty());

    for (const fs::path &file : files) {
        const std::string game = file.parent_path().filename().string();
        CAPTURE(game);
        gpu::Vdp1OpDump d;
        std::string error;
        REQUIRE(gpu::ReadVdp1OpDump(file.string(), d, &error));
        auto start = std::make_unique<ref::Fbram>();
        ToFbram(d.fbramStart, *start);
        auto end = std::make_unique<ref::Fbram>();
        ToFbram(d.fbramEnd, *end);

        // The drawn framebuffer and the raster state of the last command
        const gpu::Vdp1OpList *lastList = nullptr;
        size_t batches = 0, spans = 0;
        for (const auto &l : d.lists) {
            if (!l.batches.empty()) {
                lastList = &l;
            }
            batches += l.batches.size();
            spans += l.spans.size();
        }
        std::printf("capture %s frame %u: %zu op lists, %zu batches, %zu spans\n", game.c_str(), d.frame,
                    d.lists.size(), batches, spans);
        if (lastList == nullptr) {
            WARN("no draw batch in " << file.string());
            continue;
        }
        const gpu::Vdp1DrawBatch &lastBatch = lastList->batches.back();
        const ref::RasterState state = ref::DecodeRasterState(
            lastList->commands[lastBatch.cmdOffset + lastBatch.cmdCount - 1].rasterFlags, lastBatch.drawFB);
        const uint32 drawFB = lastBatch.drawFB & 1;
        const SpriteDecoder dec{d, state};

        // Native (1x)
        auto native = std::make_unique<ref::Fbram>(*start);
        {
            auto surface = std::make_unique<nx::NxFbram>(1);
            surface->ReplicateFrom(*native);
            nx::ReferenceNx r{*native, *surface, nx::Options{.scale = 1}, d.vramStart};
            for (const auto &l : d.lists) {
                r.Replay(l);
            }
        }
        uint64 endDiff = 0;
        for (uint32 s = 0; s < 8; ++s) {
            for (uint32 w = 0; w < ref::kWordsPerFB; ++w) {
                endDiff += nx::NativeWord(*native, s, w) != nx::NativeWord(*end, s, w);
            }
        }
        CHECK(endDiff == 0);
        const Image nativeImg = DecodeSprites(dec, drawFB, 1, [&](uint32 s, uint32 w, uint32, uint32) {
            return nx::NativeWord(*native, s, w);
        });
        WritePpm(out / (game + "_1x.ppm"), nativeImg);

        for (const uint32 n : {1u, 2u, 3u, 4u}) {
            for (const Variant &v : variants) {
                const bool render = n == 2 || n == 4;
                if (!render && v.tag != "R1") {
                    continue;
                }
                nx::Options o;
                o.scale = n;
                o.rules = v.rules;
                // Timing: the best of three runs (generation is the CPU part Task 3 moves to the front half)
                nx::Stats stats;
                std::unique_ptr<nx::NxFbram> surface;
                const int runs = v.tag == "R1" ? 3 : 1;
                for (int run = 0; run < runs; ++run) {
                    auto nat = std::make_unique<ref::Fbram>(*start);
                    auto surf = std::make_unique<nx::NxFbram>(n);
                    surf->ReplicateFrom(*nat);
                    nx::ReferenceNx r{*nat, *surf, o, d.vramStart};
                    for (const auto &l : d.lists) {
                        r.Replay(l);
                    }
                    const nx::Stats &s = r.GetStats();
                    if (run == 0 || s.generateSeconds < stats.generateSeconds) {
                        stats = s;
                    }
                    surface = std::move(surf);
                }
                // Cracks: replay again from a draw framebuffer filled with a sentinel; count the N x sub-texels left
                // unwritten whose native word and its eight neighbours were all written (interior of drawn content)
                uint64 cracks = 0, interior = 0;
                if (render) {
                    constexpr uint16 kSentinel = 0x5A5A;
                    auto nat = std::make_unique<ref::Fbram>(*start);
                    const uint32 mainSlice = gpu::Vdp1MainSlice(drawFB);
                    for (uint32 b = 0; b < gpu::kFbramSliceSize; b += 2) {
                        nat->main[drawFB][b] = kSentinel >> 8;
                        nat->main[drawFB][b + 1] = kSentinel & 0xFF;
                    }
                    auto surf = std::make_unique<nx::NxFbram>(n);
                    surf->ReplicateFrom(*nat);
                    nx::ReferenceNx r{*nat, *surf, o, d.vramStart};
                    for (gpu::Vdp1OpList l : d.lists) {
                        for (auto &e : l.erases) {
                            e.value = kSentinel; // erased is not drawn
                        }
                        r.Replay(l);
                    }
                    const uint32 rowWords = state.pixel8Bits ? state.fbSizeH / 2 : state.fbSizeH;
                    const uint32 rows = ref::kWordsPerFB / rowWords;
                    auto drawn = [&](uint32 x, uint32 y) {
                        return nx::NativeWord(*nat, mainSlice, y * rowWords + x) != kSentinel;
                    };
                    for (uint32 y = 1; y + 1 < rows; ++y) {
                        for (uint32 x = 1; x + 1 < rowWords; ++x) {
                            bool all = true;
                            for (int dy = -1; dy <= 1 && all; ++dy) {
                                for (int dx = -1; dx <= 1 && all; ++dx) {
                                    all = drawn(x + dx, y + dy);
                                }
                            }
                            if (!all) {
                                continue;
                            }
                            ++interior;
                            const uint32 w = y * rowWords + x;
                            uint32 here = 0;
                            for (uint32 s = 0; s < n * n; ++s) {
                                here += surf->slices[mainSlice][size_t{w} * n * n + s] == kSentinel;
                            }
                            if (here != 0) {
                                crackList << game << " " << n << " " << v.tag << " fb " << x << "," << y << " "
                                          << here << "\n";
                            }
                            cracks += here;
                        }
                    }
                }
                metrics << "| " << game << " | " << n << " | " << v.tag << " | " << interior << " | " << cracks << " | "
                        << static_cast<double>(stats.halfTransparent) / (n * n) << " | "
                        << static_cast<double>(stats.multiHalfTransparent) / (n * n) << " | "
                        << (stats.Fragments() ? 100.0 * stats.overwrite / stats.Fragments() : 0.0) << " % |\n";

                char line[512];
                std::snprintf(line, sizeof(line),
                              "| %s | %u | %s | %llu | %llu | %llu | %llu (%llu / %llu / %llu) | %llu | %llu | %llu | "
                              "%u | %u | %.2f | %.1f | %.1f |\n",
                              game.c_str(), n, v.tag.c_str(), (unsigned long long)stats.nativeSpans,
                              (unsigned long long)stats.nativeSlots, (unsigned long long)stats.nativeFragments,
                              (unsigned long long)stats.Spans(), (unsigned long long)stats.spans[0],
                              (unsigned long long)stats.spans[1], (unsigned long long)stats.spans[2],
                              (unsigned long long)stats.Slots(), (unsigned long long)stats.Fragments(),
                              (unsigned long long)stats.subBatches, stats.maxSubBatchSlots, stats.maxPerUnit,
                              stats.generateSeconds * 1e3, stats.rasterSeconds * 1e3, stats.resolveSeconds * 1e3);
                table << line;
                classes << "| " << game << " | " << n << " | " << v.tag << " | ";
                for (uint32 c = 0; c < nx::CommandCounts::Count; ++c) {
                    classes << stats.commands.counts[c] << " | ";
                }
                classes << stats.nativeLongLines << " |\n";
                if (!render) {
                    continue;
                }
                const nx::NxFbram &sf = *surface;
                const Image img = DecodeSprites(dec, drawFB, n, [&](uint32 s, uint32 w, uint32 i, uint32 j) {
                    return sf.Word(s, w, i, j);
                });
                const std::string prefix = game + "_" + std::to_string(n) + "x_";
                const bool full = v.tag == "R1" || v.tag == "R2" || v.tag == "R3";
                if (full) {
                    WritePpm(out / (prefix + v.tag + ".ppm"), img);
                    bases.insert_or_assign(v.tag, img);
                } else {
                    // Where the thin-quad / line rule changes the picture: the base rule's image darkened, the
                    // differing pixels magenta; the bounding box in 1x pixels in diffs.md
                    const Image &base = bases.at(v.tag.substr(0, 2));
                    Image diff(img.w, img.h);
                    uint32 count = 0, x0 = img.w, y0 = img.h, x1 = 0, y1 = 0;
                    for (uint32 y = 0; y < img.h; ++y) {
                        for (uint32 x = 0; x < img.w; ++x) {
                            if (img.Get(x, y) != base.Get(x, y)) {
                                diff.Set(x, y, 0xFF00FF);
                                ++count;
                                x0 = std::min(x0, x);
                                y0 = std::min(y0, y);
                                x1 = std::max(x1, x);
                                y1 = std::max(y1, y);
                            } else {
                                diff.Set(x, y, (base.Get(x, y) >> 2) & 0x3F3F3F);
                            }
                        }
                    }
                    diffs << "| " << game << " | " << n << " | " << v.tag << " vs " << v.tag.substr(0, 2) << " | "
                          << count << " | ";
                    if (count != 0) {
                        WritePpm(out / (prefix + "diff_" + v.tag + ".ppm"), diff);
                        diffs << x0 / n << "," << y0 / n << " - " << x1 / n << "," << y1 / n;
                    }
                    diffs << " |\n";
                }
                // crops: tiles of this variant, collected into one sheet per crop below
                auto range = crops.equal_range(game);
                uint32 k = 0;
                for (auto it = range.first; it != range.second; ++it, ++k) {
                    const Crop &c = it->second;
                    Image tile(c.w * n, c.h * n);
                    for (uint32 y = 0; y < tile.h; ++y) {
                        for (uint32 x = 0; x < tile.w; ++x) {
                            const uint32 sx = c.x * n + x, sy = c.y * n + y;
                            tile.Set(x, y, sx < img.w && sy < img.h ? img.Get(sx, sy) : 0);
                        }
                    }
                    WritePpm(out / ("tile_" + prefix + "crop" + std::to_string(k) + "_" + v.tag + ".ppm"), tile);
                    if (v.tag == "R1") {
                        Image nearTile(c.w * n, c.h * n);
                        for (uint32 y = 0; y < nearTile.h; ++y) {
                            for (uint32 x = 0; x < nearTile.w; ++x) {
                                const uint32 sx = c.x + x / n, sy = c.y + y / n;
                                nearTile.Set(x, y, sx < nativeImg.w && sy < nativeImg.h ? nativeImg.Get(sx, sy) : 0);
                            }
                        }
                        WritePpm(out / ("tile_" + prefix + "crop" + std::to_string(k) + "_nearest.ppm"), nearTile);
                    }
                }
                if (v.tag == "R1") {
                    WritePpm(out / (prefix + "nearest.ppm"), Nearest(nativeImg, n));
                }
            }
        }
    }
}
