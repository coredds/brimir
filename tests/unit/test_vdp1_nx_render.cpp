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
#include <unordered_map>
#include <utility>
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

TEST_CASE("VDP1 N x scan of captured frames for non-overwrite fragments (BRIMIR_VDP1_CAPTURES)", "[.][nx-scan]") {
    // Per capture: native fragments, half-transparency fragments and every other non-overwrite fragment (shadow,
    // MSB-on), from the N = 1 replay
    const char *capDir = std::getenv("BRIMIR_VDP1_CAPTURES");
    if (capDir == nullptr) {
        SKIP("BRIMIR_VDP1_CAPTURES not set");
    }
    std::vector<fs::path> files;
    for (const auto &e : fs::recursive_directory_iterator(capDir)) {
        if (e.path().extension() == ".bin" && e.path().filename().string().rfind("vdp1ops_", 0) == 0) {
            files.push_back(e.path());
        }
    }
    std::sort(files.begin(), files.end());
    for (const fs::path &file : files) {
        gpu::Vdp1OpDump d;
        REQUIRE(gpu::ReadVdp1OpDump(file.string(), d));
        auto native = std::make_unique<ref::Fbram>();
        ToFbram(d.fbramStart, *native);
        auto surface = std::make_unique<nx::NxFbram>(1);
        surface->ReplicateFrom(*native);
        nx::ReferenceNx r{*native, *surface, nx::Options{.scale = 1}, d.vramStart};
        for (const auto &l : d.lists) {
            r.Replay(l);
        }
        const nx::Stats &s = r.GetStats();
        std::printf("scan %s/%s: batches %llu fragments %llu half-transparency %llu other non-overwrite %llu mesh %llu\n",
                    file.parent_path().filename().string().c_str(), file.filename().string().c_str(),
                    static_cast<unsigned long long>(s.batches), static_cast<unsigned long long>(s.Fragments()),
                    static_cast<unsigned long long>(s.halfTransparent),
                    static_cast<unsigned long long>(s.Fragments() - s.overwrite - s.halfTransparent),
                    static_cast<unsigned long long>(s.mesh));
    }
}

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
        {"R1-T2-L2", {R::FloorHalfDown, nx::ThinRule::Parallel, nx::LineRule::Parallel}}, // the chosen rules
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
        {
            // Where the frame wrote the mesh plane (transparent meshes: VDP2 blends it), by 16 x 16 cells of FB pixels
            const uint32 meshSlice = gpu::Vdp1MeshSlice(0, drawFB);
            const uint32 rowWords = state.pixel8Bits ? state.fbSizeH / 2 : state.fbSizeH;
            std::map<uint32, uint32> cells;
            for (uint32 w = 0; w < ref::kWordsPerFB; ++w) {
                if (nx::NativeWord(*native, meshSlice, w) != 0) {
                    ++cells[(w / rowWords / 16) << 16 | (w % rowWords / 16)];
                }
            }
            std::vector<std::pair<uint32, uint32>> top(cells.begin(), cells.end());
            std::sort(top.begin(), top.end(), [](auto &a, auto &b) { return a.second > b.second; });
            for (size_t i = 0; i < std::min<size_t>(top.size(), 5); ++i) {
                std::printf("mesh %s: FB cell x %u y %u: %u words\n", game.c_str(), (top[i].first & 0xFFFF) * 16,
                            (top[i].first >> 16) * 16, top[i].second);
            }
        }
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
                const bool full = v.tag == "R1" || v.tag == "R2" || v.tag == "R3" || v.tag == "R1-T2-L2";
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

namespace {

// The confinement metric's view of one capture at one N (see the test below)
struct ConfineResult {
    uint64 drawn = 0, quad = 0, mismatches = 0, quadMismatches = 0, nonConst = 0, nonConstQuad = 0, nonConstOther = 0;
    // Mismatches whose N x winner at sub-texel (0, 0), or else the native winner, is not quad-kind (rect-path and
    // replicated content must be native at the sample points)
    uint64 rectMismatches = 0;
    std::vector<uint32> unconfined; // fb << 18 | unit
    std::array<std::vector<uint8>, 2> allowed;
    std::array<std::vector<uint16>, 2> rowWords; // per drawn unit: the row width (words) of the command that drew it
};

constexpr uint32 kConfineUnits = 2 * ref::kWordsPerFB; // word | alternate field

void ToFbramStart(const gpu::Vdp1OpDump &d, ref::Fbram &fb) {
    ToFbram(d.fbramStart, fb);
}

// Replays the capture at N with the allowance observer (and the raster probe, when given); fills `r` (first pass) or
// only the probe events
void ReplayConfine(const gpu::Vdp1OpDump &d, uint32 n, ConfineResult *r, nx::RasterProbe *probe,
                   std::unique_ptr<ref::Fbram> &native, std::unique_ptr<nx::NxFbram> &surface) {
    native = std::make_unique<ref::Fbram>();
    ToFbramStart(d, *native);
    surface = std::make_unique<nx::NxFbram>(n);
    surface->ReplicateFrom(*native);
    nx::ReferenceNx ref{*native, *surface, nx::Options{.scale = n}, d.vramStart};
    ref.SetRasterProbe(probe);
    if (r != nullptr) {
        for (uint32 fb = 0; fb < 2; ++fb) {
            r->allowed[fb].assign(kConfineUnits, 0);
            r->rowWords[fb].assign(kConfineUnits, 0);
        }
        ref.SetBatchObserver([r](const nx::BatchObservation &b) {
            std::vector<uint8> &allowed = r->allowed[b.drawFB];
            std::vector<uint16> &rows = r->rowWords[b.drawFB];
            std::vector<uint8> quadCmd(b.commands.size(), 0);
            for (const nx::NxSpan &s : b.nxSpans) {
                if (s.kind == nx::SpanKind::Quad) {
                    quadCmd[s.cmdIndex] = 1;
                }
            }
            // Per quad-path command: (word | alt | odd byte) -> (fragments, last colour)
            std::vector<std::unordered_map<uint32, std::pair<uint32, uint32>>> perCmd(b.commands.size());
            constexpr uint32 kKeyMask = ref::kTargetUnitMask | ref::kTargetOddBit;
            for (const ref::Fragment &fr : b.nativeFragments) {
                const auto it = std::upper_bound(b.spans.begin(), b.spans.end(), fr.key,
                                                 [](uint32 k, const ref::Span &s) { return k < s.fragBase; });
                const uint32 cmd = std::prev(it)->cmdIndex;
                const ref::RasterState &st = b.states[cmd];
                rows[fr.target & ref::kTargetUnitMask] =
                    static_cast<uint16>(st.pixel8Bits ? st.fbSizeH / 2 : st.fbSizeH);
                if (quadCmd[cmd] != 0) {
                    auto &e = perCmd[cmd][fr.target & kKeyMask];
                    ++e.first;
                    e.second = fr.color;
                }
            }
            for (size_t cmd = 0; cmd < perCmd.size(); ++cmd) {
                if (quadCmd[cmd] == 0) {
                    continue;
                }
                const ref::RasterState &st = b.states[cmd];
                const uint32 rw = st.pixel8Bits ? st.fbSizeH / 2 : st.fbSizeH; // this command's row width
                const auto &m = perCmd[cmd];
                auto inside = [rw](sint32 x, sint32 y) {
                    return x >= 0 && x < static_cast<sint32>(rw) && y >= 0 &&
                           static_cast<uint32>(y) * rw + static_cast<uint32>(x) < ref::kWordsPerFB;
                };
                for (const auto &[key, e] : m) {
                    const uint32 unit = key & ref::kTargetUnitMask;
                    const uint32 word = unit & ref::kTargetWordMask;
                    const uint32 alt = unit & ref::kTargetAltBit;
                    const uint32 high = key & ~ref::kTargetWordMask; // alt and odd-byte bits
                    bool seed = e.first > 1;
                    const sint32 x = static_cast<sint32>(word % rw), y = static_cast<sint32>(word / rw);
                    for (sint32 dy = -1; dy <= 1 && !seed; ++dy) {
                        for (sint32 dx = -1; dx <= 1 && !seed; ++dx) {
                            if (!inside(x + dx, y + dy)) {
                                seed = true;
                                break;
                            }
                            const auto other = m.find((static_cast<uint32>(y + dy) * rw + static_cast<uint32>(x + dx)) |
                                                      high);
                            seed = other == m.end() || other->second.second != e.second;
                        }
                    }
                    if (!seed) {
                        continue;
                    }
                    // The allowance: the seed and its 8 neighbours, in this command's row width
                    for (sint32 dy = -1; dy <= 1; ++dy) {
                        for (sint32 dx = -1; dx <= 1; ++dx) {
                            if (inside(x + dx, y + dy)) {
                                allowed[(static_cast<uint32>(y + dy) * rw + static_cast<uint32>(x + dx)) | alt] = 1;
                            }
                        }
                    }
                }
            }
        });
    }
    for (const auto &l : d.lists) {
        ref.Replay(l);
    }
}

const nx::ProbeEvent *LastDrawn(const std::vector<const nx::ProbeEvent *> &events) {
    const nx::ProbeEvent *last = nullptr;
    for (const nx::ProbeEvent *e : events) {
        // (fragments for the transparent-mesh framebuffer leave the main word alone)
        if (e->status == nx::ProbeEvent::Drawn && (e->mode & ref::kTargetMeshBit) == 0) {
            last = e;
        }
    }
    return last;
}

// A drawn fragment whose result depends on the word below it (shadow, half-transparency, MSB-on)
bool ReadsDestination(const nx::ProbeEvent *e) {
    if (e == nullptr) {
        return false;
    }
    const uint32 calc = (e->mode >> ref::kTargetCalcShift) & 3u;
    const bool pixel8 = (e->mode & ref::kTargetPixel8Bit) != 0;
    return (!pixel8 && (calc == 1 || calc == 3)) || (e->mode & ref::kTargetMsbOnBit) != 0;
}

// Whether a quad-kind command drew this word before `last` (in the same event list)
bool QuadDrawnBefore(const std::vector<const nx::ProbeEvent *> &events, const nx::ProbeEvent *last) {
    for (const nx::ProbeEvent *e : events) {
        if (e == last) {
            return false;
        }
        if (e->status == nx::ProbeEvent::Drawn && e->kind == nx::SpanKind::Quad) {
            return true;
        }
    }
    return false;
}

std::string Describe(const nx::ProbeEvent *e) {
    if (e == nullptr) {
        return "none";
    }
    char text[128];
    if (e->texelKnown) {
        std::snprintf(text, sizeof(text), "b%llu c%u %s %s u%d v%d texel %04x colour %04x",
                      static_cast<unsigned long long>(e->batch), e->cmdIndex, nx::Name(e->kind), nx::Name(e->status),
                      e->u, e->v, e->texel, e->color);
    } else {
        std::snprintf(text, sizeof(text), "b%llu c%u %s %s colour %04x", static_cast<unsigned long long>(e->batch),
                      e->cmdIndex, nx::Name(e->kind), nx::Name(e->status), e->color);
    }
    return text;
}

} // namespace

TEST_CASE("VDP1 N x confinement and sharpness of captured frames (BRIMIR_VDP1_CAPTURES)", "[.][nx-confinement]") {
    // GPU stage 3 Task 9 (stage3-research.md sections 3.3 and 7). At the sample points (sub-texel (0, 0)) the N x
    // surface may differ from native FBRAM only on quad-path content: within one native pixel of a quad-path command's
    // edge or of a texel / Gouraud boundary (two neighbouring native fragments of the command with different colours),
    // or where one quad-path command draws a word twice (within-command overdraw). Per capture and N (BRIMIR_NX_SCALES,
    // default "24"): the words drawn, the share of their sample points equal to native, the mismatches whose winner is
    // not quad-kind (rect-path or replicated content: must be 0), the mismatches outside that allowance
    // ("unconfined"), and the share of drawn words whose N x N block is not constant (VDP1 sharpness), split by
    // quad-path and other content. The allowance is accumulated over the capture's batches (erases do not reset it).
    //
    // Every unconfined sample point is then attributed with the raster probe, at N and at N = 1 (equal to native,
    // Task 1): the N x step that resolves at sub-texel (0, 0) against the native winner (command, span kind, texel
    // u / v, colour), and classified:
    //   - texel boundary: the same command draws both with different texels, or a transparent texel / end code at one
    //     of them (minified textures: the native stepping skips texels the N x stepping reads, and the other way);
    //   - Gouraud boundary: the same command and texel, another colour;
    //   - later command: another command's N x step wins (an edge of that command), drawn after the native winner when
    //     the native winner has no N x step there;
    //   - edge reach: N x draws where native drew nothing and the command has no native step there;
    //   - coverage gap: native covers the word and no N x step of that command reaches sub-texel (0, 0), and no later
    //     command covers it at N x (a crack);
    //   - rect-path violation: the winner is rect or replicate kind.
    // The last two are findings. BRIMIR_NX_MAX_UNCONFINED=K fails the test when a capture has more than K unconfined
    // points outside the texel and Gouraud classes, or any finding.
    // The reference equals the GPU byte for byte (the [.captures] tests), so this measures what the GPU draws.
    const char *capDir = std::getenv("BRIMIR_VDP1_CAPTURES");
    if (capDir == nullptr) {
        SKIP("BRIMIR_VDP1_CAPTURES not set");
    }
    std::vector<uint32> scales;
    {
        const char *text = std::getenv("BRIMIR_NX_SCALES");
        for (const char c : std::string(text != nullptr ? text : "24")) {
            if (c >= '2' && c <= '4') {
                scales.push_back(static_cast<uint32>(c - '0'));
            }
        }
    }
    const char *maxText = std::getenv("BRIMIR_NX_MAX_UNCONFINED");
    const sint64 maxUnexplained = maxText != nullptr ? std::atoll(maxText) : -1;
    std::vector<fs::path> files;
    for (const auto &e : fs::recursive_directory_iterator(capDir)) {
        if (e.path().extension() == ".bin" && e.path().filename().string().rfind("vdp1ops_", 0) == 0) {
            files.push_back(e.path());
        }
    }
    std::sort(files.begin(), files.end());
    enum Class : uint32 { Texel, Gouraud, LaterCommand, EdgeReach, CoverageGap, RectPath, Other, ClassCount };
    static constexpr const char *kClassNames[ClassCount] = {"texel boundary", "Gouraud boundary", "later command",
                                                            "edge reach",     "coverage gap",     "rect-path",
                                                            "other"};
    std::array<uint64, ClassCount> totals{};
    for (const fs::path &file : files) {
        gpu::Vdp1OpDump d;
        REQUIRE(gpu::ReadVdp1OpDump(file.string(), d));
        const std::string name = file.parent_path().filename().string() + "/" + file.filename().string();
        for (const uint32 n : scales) {
            ConfineResult r;
            std::unique_ptr<ref::Fbram> native;
            std::unique_ptr<nx::NxFbram> surface;
            ReplayConfine(d, n, &r, nullptr, native, surface);
            // Drawn words: any native fragment (row width known) or a differing sample point
            std::unordered_map<uint32, uint8> mismatchUnits; // fb << 18 | unit -> quad-drawn (from the first pass)
            std::vector<uint32> mismatchList;
            for (uint32 fb = 0; fb < 2; ++fb) {
                for (uint32 unit = 0; unit < kConfineUnits; ++unit) {
                    const uint32 word = unit & ref::kTargetWordMask;
                    const uint32 slice = (unit & ref::kTargetAltBit) ? gpu::Vdp1AltSlice(fb) : gpu::Vdp1MainSlice(fb);
                    const uint16 sample = surface->Word(slice, word, 0, 0);
                    const bool mismatch = sample != nx::NativeWord(*native, slice, word);
                    const bool isDrawn = r.rowWords[fb][unit] != 0;
                    if (!isDrawn && !mismatch) {
                        continue;
                    }
                    r.drawn += isDrawn;
                    bool constant = true;
                    for (uint32 sub = 1; sub < n * n && constant; ++sub) {
                        constant = surface->Word(slice, word, sub % n, sub / n) == sample;
                    }
                    if (isDrawn && !constant) {
                        ++r.nonConst;
                    }
                    if (mismatch) {
                        ++r.mismatches;
                        mismatchList.push_back(fb << 18 | unit);
                        mismatchUnits[fb << 18 | unit] = 0;
                        if (r.allowed[fb][unit] == 0) {
                            r.unconfined.push_back(fb << 18 | unit);
                        }
                    }
                }
            }
            // The probe on every differing word (and every drawn word, for the quad / other split), at N and at N = 1
            std::unordered_map<uint32, std::vector<const nx::ProbeEvent *>> atN, atNative;
            nx::RasterProbe probeN, probe1;
            probeN.want = [&](uint32 fb, uint32 unit, uint32 sub) {
                return sub == 0 && (r.rowWords[fb][unit] != 0 || mismatchUnits.count(fb << 18 | unit) != 0);
            };
            probe1.want = probeN.want;
            {
                std::unique_ptr<ref::Fbram> n2;
                std::unique_ptr<nx::NxFbram> s2;
                ReplayConfine(d, n, nullptr, &probeN, n2, s2);
                ReplayConfine(d, 1, nullptr, &probe1, n2, s2);
            }
            for (const nx::ProbeEvent &e : probeN.events) {
                atN[e.drawFB << 18 | e.unit].push_back(&e);
            }
            for (const nx::ProbeEvent &e : probe1.events) {
                atNative[e.drawFB << 18 | e.unit].push_back(&e);
            }
            // Quad / other split of drawn words and mismatches by the winner's kind (N x winner, else native winner)
            auto winnerKind = [&](uint32 key, bool &known) {
                const nx::ProbeEvent *w = LastDrawn(atN[key]);
                if (w == nullptr) {
                    w = LastDrawn(atNative[key]);
                }
                known = w != nullptr;
                return w != nullptr ? w->kind : nx::SpanKind::Replicate;
            };
            for (uint32 fb = 0; fb < 2; ++fb) {
                for (uint32 unit = 0; unit < kConfineUnits; ++unit) {
                    if (r.rowWords[fb][unit] == 0) {
                        continue;
                    }
                    bool known = false;
                    const bool isQuad = winnerKind(fb << 18 | unit, known) == nx::SpanKind::Quad && known;
                    r.quad += isQuad;
                    if (isQuad) {
                        const uint32 word = unit & ref::kTargetWordMask;
                        const uint32 slice =
                            (unit & ref::kTargetAltBit) ? gpu::Vdp1AltSlice(fb) : gpu::Vdp1MainSlice(fb);
                        const uint16 sample = surface->Word(slice, word, 0, 0);
                        bool constant = true;
                        for (uint32 sub = 1; sub < n * n && constant; ++sub) {
                            constant = surface->Word(slice, word, sub % n, sub / n) == sample;
                        }
                        r.nonConstQuad += !constant;
                    }
                }
            }
            r.nonConstOther = r.nonConst - r.nonConstQuad;
            // A mismatch is quad-path when either winner (N x or native) is quad-kind: a rect-kind winner at N x over a
            // native quad winner is that quad's edge or texel step. Neither quad-kind: rect-path content differs.
            // A rect-kind winner that reads the destination (shadow, half-transparency, MSB-on) over
            // a word a quad-kind command drew before it, in either run, inherits that quad's difference.
            uint64 blendOverQuad = 0;
            std::string rectExamples;
            for (const uint32 key : mismatchList) {
                const nx::ProbeEvent *wN = LastDrawn(atN[key]);
                const nx::ProbeEvent *w1 = LastDrawn(atNative[key]);
                const bool quadWinner = (wN != nullptr && wN->kind == nx::SpanKind::Quad) ||
                                        (w1 != nullptr && w1->kind == nx::SpanKind::Quad);
                if (quadWinner) {
                    ++r.quadMismatches;
                    continue;
                }
                if (ReadsDestination(wN) && ReadsDestination(w1) && wN->batch == w1->batch &&
                    wN->cmdIndex == w1->cmdIndex && wN->color == w1->color &&
                    (QuadDrawnBefore(atN[key], wN) || QuadDrawnBefore(atNative[key], w1))) {
                    ++r.quadMismatches;
                    ++blendOverQuad;
                    continue;
                }
                if (++r.rectMismatches <= 5) {
                    char mode[48];
                    std::snprintf(mode, sizeof(mode), " (modes %06x / %06x)", wN != nullptr ? wN->mode : 0,
                                  w1 != nullptr ? w1->mode : 0);
                    rectExamples += "    rect-path mismatch fb" + std::to_string(key >> 18) + " unit " +
                                    std::to_string(key & (kConfineUnits - 1)) + ": N x winner " + Describe(wN) +
                                    "; native winner " + Describe(w1) + mode + "\n";
                }
            }
            // Attribute the unconfined points
            std::array<uint64, ClassCount> classes{};
            std::string details;
            for (const uint32 key : r.unconfined) {
                const uint32 fb = key >> 18, unit = key & (kConfineUnits - 1);
                const auto &evN = atN[key];
                const auto &ev1 = atNative[key];
                const nx::ProbeEvent *wN = LastDrawn(evN);
                const nx::ProbeEvent *w1 = LastDrawn(ev1);
                auto sameCmd = [](const nx::ProbeEvent *a, const nx::ProbeEvent *b) {
                    return a != nullptr && b != nullptr && a->batch == b->batch && a->cmdIndex == b->cmdIndex;
                };
                auto stepsOf = [](const std::vector<const nx::ProbeEvent *> &events, const nx::ProbeEvent *c) {
                    std::vector<const nx::ProbeEvent *> out;
                    for (const nx::ProbeEvent *e : events) {
                        if (e->batch == c->batch && e->cmdIndex == c->cmdIndex) {
                            out.push_back(e);
                        }
                    }
                    return out;
                };
                auto hasStatus = [](const std::vector<const nx::ProbeEvent *> &events, nx::ProbeEvent::Status s) {
                    return std::any_of(events.begin(), events.end(),
                                       [s](const nx::ProbeEvent *e) { return e->status == s; });
                };
                Class cls = Other;
                if (!(wN != nullptr && wN->kind == nx::SpanKind::Quad) &&
                    !(w1 != nullptr && w1->kind == nx::SpanKind::Quad)) {
                    cls = RectPath;
                } else if (sameCmd(wN, w1)) {
                    if (wN->texelKnown && w1->texelKnown && (wN->u != w1->u || wN->v != w1->v)) {
                        cls = Texel;
                    } else if (wN->color != w1->color) {
                        cls = Gouraud;
                    }
                } else if (w1 != nullptr) {
                    // The native winner's command at N x on this word
                    const auto steps = stepsOf(evN, w1);
                    // The N x winner drawn after the native winner (draw order: batch, then command)
                    const bool later = wN != nullptr && (wN->batch != w1->batch ? wN->batch > w1->batch
                                                                                  : wN->cmdIndex > w1->cmdIndex);
                    if (steps.empty()) {
                        // Not a gap only when a later command covers it at N x
                        cls = later ? LaterCommand : CoverageGap;
                    } else if (hasStatus(steps, nx::ProbeEvent::Transparent) ||
                               hasStatus(steps, nx::ProbeEvent::EndCode)) {
                        cls = Texel;
                    } else if (later) {
                        cls = LaterCommand;
                    }
                } else if (wN != nullptr) {
                    // Native drew nothing here: the N x winner's command at N = 1
                    const auto steps = stepsOf(ev1, wN);
                    if (hasStatus(steps, nx::ProbeEvent::Transparent) || hasStatus(steps, nx::ProbeEvent::EndCode)) {
                        cls = Texel;
                    } else if (steps.empty()) {
                        cls = EdgeReach;
                    }
                }
                ++classes[cls];
                const uint32 word = unit & ref::kTargetWordMask;
                const uint32 rw = r.rowWords[fb][unit] != 0 ? r.rowWords[fb][unit] : 512u;
                const uint32 slice = (unit & ref::kTargetAltBit) ? gpu::Vdp1AltSlice(fb) : gpu::Vdp1MainSlice(fb);
                std::string block;
                for (uint32 sub = 0; sub < n * n; ++sub) {
                    char hex[8];
                    std::snprintf(hex, sizeof(hex), "%s%04x", sub == 0 ? "" : (sub % n == 0 ? "|" : " "),
                                  surface->Word(slice, word, sub % n, sub / n));
                    block += hex;
                }
                char head[96];
                std::snprintf(head, sizeof(head), "    unconfined fb%u %u,%u%s [%s] native %04x: ", fb, word % rw,
                              word / rw,
                              (unit & ref::kTargetAltBit) ? " alt" : "", kClassNames[cls],
                              nx::NativeWord(*native, slice, word));
                details += head + std::string("N x winner ") + Describe(wN) + "; native winner " + Describe(w1) +
                           "; block " + block;
                if (w1 != nullptr && !sameCmd(wN, w1)) {
                    for (const nx::ProbeEvent *e : stepsOf(evN, w1)) {
                        details += "; N x step of the native winner " + Describe(e);
                    }
                }
                if (wN != nullptr && w1 == nullptr) {
                    for (const nx::ProbeEvent *e : stepsOf(ev1, wN)) {
                        details += "; native step of the N x winner " + Describe(e);
                    }
                }
                details += "\n";
            }
            for (uint32 c = 0; c < ClassCount; ++c) {
                totals[c] += classes[c];
            }
            const auto pct = [](uint64 a, uint64 b) { return b == 0 ? 0.0 : 100.0 * static_cast<double>(a) / b; };
            std::string classText;
            for (uint32 c = 0; c < ClassCount; ++c) {
                if (classes[c] != 0) {
                    classText += std::string(classText.empty() ? "" : ", ") + kClassNames[c] + " " +
                                 std::to_string(classes[c]);
                }
            }
            const uint64 unconfined = r.unconfined.size();
            std::printf("confine %s %ux: drawn %llu (quad-path %.1f%%), sample points equal %.3f%%, mismatches %llu "
                        "(quad-path %llu, of them rect blends over quad-path %llu; rect-path %llu), confined %.2f%%, "
                        "unconfined %llu%s%s%s; "
                        "non-constant blocks %.2f%% (quad-path %.2f%%, other %.2f%%)\n%s",
                        name.c_str(), n, static_cast<unsigned long long>(r.drawn), pct(r.quad, r.drawn),
                        100.0 - pct(r.mismatches, r.drawn), static_cast<unsigned long long>(r.mismatches),
                        static_cast<unsigned long long>(r.quadMismatches),
                        static_cast<unsigned long long>(blendOverQuad),
                        static_cast<unsigned long long>(r.rectMismatches),
                        r.mismatches == 0 ? 100.0 : 100.0 - pct(unconfined, r.mismatches),
                        static_cast<unsigned long long>(unconfined), classText.empty() ? "" : " (",
                        classText.c_str(), classText.empty() ? "" : ")", pct(r.nonConst, r.drawn),
                        pct(r.nonConstQuad, r.quad), pct(r.nonConstOther, r.drawn - r.quad),
                        (details + rectExamples).c_str());
            std::fflush(stdout);
            CAPTURE(name, n);
            CHECK(r.rectMismatches == 0);
            CHECK(classes[CoverageGap] == 0);
            CHECK(classes[RectPath] == 0);
            if (maxUnexplained >= 0) {
                CHECK(static_cast<sint64>(unconfined - classes[Texel] - classes[Gouraud]) <= maxUnexplained);
            }
        }
    }
    std::string totalText;
    for (uint32 c = 0; c < ClassCount; ++c) {
        totalText += std::string(totalText.empty() ? "" : ", ") + kClassNames[c] + " " + std::to_string(totals[c]);
    }
    std::printf("confine totals: unconfined by class: %s\n", totalText.c_str());
    CHECK(!files.empty());
}
