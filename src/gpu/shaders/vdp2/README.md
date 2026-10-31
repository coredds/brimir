# VDP2 compute shaders (GPU stages 1B and 1C)

The compute shaders of the Vulkan VDP2 back half (`src/gpu/vdp2/vdp2_vulkan_backend.*`), replayed per batch of
lines from the front half's frame record (`src/gpu/vdp2/vdp2_front.*`):

| Shader | Does | Ymir source |
|---|---|---|
| `cs_vdp2_render_sprite.hlsl` | VDP1 framebuffer → sprite (and transparent mesh) layer + sprite attributes | `cs_vdp2_render_sprite.hlsl` |
| `cs_vdp2_render_bgs.hlsl` | NBG0-3, RBG0-1 → layer images, RBG line colours, colour calculation window | `cs_vdp2_render_bgs.hlsl` |
| `cs_vdp2_compose.hlsl` | layers + LNCL/BACK + sprite attributes → the composite (packed XBGR, as software's framebuffer) | `cs_vdp2_compose.hlsl` |
| `cs_vdp2_nbg_fetch.hlsl` | the NBG character table: per pixel, the character software's fetcher shows (character pattern delay, vertical cell scroll on 2x2 characters), windows included; runs between sprite and bgs for the batches that need it (stage 1C) | Brimir only |
| `cs_vdp2_rescale.hlsl` | one-shot nearest rescale of the previous composite into the new one after an internal resolution change (the sample points keep their values, so a woven field survives; stage 1C) | Brimir only |

## Provenance

Derived from Ymir's Direct3D 12 renderer shaders, `libs/ymir-core/res/shaders/src/vdp/` (and `include/util/`) at commit
**`8cdf0a53`** (reference clone: `C:\Users\david\projects\Ymir`). Every file names its source file and the commit in
its header comment. Ymir is GPL-3.0, as Brimir.

| Brimir file | Source |
|---|---|
| `include/vdp2_defs.hlsli` | `vdp2_defs.hlsli`; Brimir additions at the end |
| `include/vdp2_common_params.hlsli`, `include/vdp2_compose_params.hlsli` | same names; layout changes marked |
| `include/vdp2_render_params_layer.hlsli`, `include/vdp2_render_params_rotation.hlsli`, `include/vdp2_window_params.hlsli` | same names, unchanged |
| `../include/util/bit_ops.hlsli`, `../include/util/data_ops.hlsli` (shared with VDP1) | `include/util/`, unchanged |
| `include/vdp2_nbg_line.hlsli` | Brimir only (per-row NBG state from the front half) |
| `include/vdp2_nbg_chars.hlsli` | Brimir only (the NBG character table's layout and which NBGs use it) |
| `../include/int64_emu.hlsli` | Brimir only (32-bit emulation of Ymir's 64-bit rotation arithmetic) |

The parameter structs have host mirrors in `src/gpu/vdp2/vdp2_hw_params.hpp`; their offsets are checked against the
SPIR-V reflection (`src/gpu/shaders/generated/*.layout.h`, `tests/unit/test_gpu_vdp2_params.cpp`).

Build: `tools/shaders/build_shaders.cmake` compiles them with DXC (`-spirv`, Vulkan 1.2, register shifts t→0, u→16,
b→32) and validates them with `spirv-val`; the generated SPIR-V headers are committed in `src/gpu/shaders/generated/`,
so a normal build needs no Vulkan SDK (`-DBRIMIR_SHADERS_CHECK=ON` checks that the headers are current).

## Divergences from Ymir

Every intentional change is marked `// Brimir:` in the source with its reason (`rg "Brimir:" src/gpu/shaders/vdp2`).
There are two kinds.

### 1. Vulkan 1.2 baseline (no behaviour change)

Brimir's GPU baseline: Vulkan 1.2 core, no 64-bit integers in shaders, at most 64 invocations per workgroup (the frame output pass's 8 x 8; the VDP2 shaders use 32; Vulkan guarantees 128), std430
layouts without `scalarBlockLayout`, storage images only `rgba8ui`/`r32ui`, no robustness features. Groupshared
memory: only the NBG fetch pass uses it, 14208 bytes (`kNBGFetchSharedMemorySize` in `src/gpu/vdp2/vdp2_bindings.hpp`,
a required limit in `src/gpu/gpu_version.hpp`; Vulkan guarantees 16384); `tests/unit/test_gpu_vdp2_pipelines.cpp`
checks every module's Workgroup variables against it.

- **Push constants** instead of the D3D12 `b0` root constants (`CommonRenderParams`, 56 bytes); **registers renumbered**
  per `src/gpu/vdp2/vdp2_bindings.hpp` (all three shaders; the NBG fetch pass adds its row count after the block).
- **No texel buffers:** CRAM colours (all three) and LNCL/BACK (compose) are `StructuredBuffer<uint>` of packed RGBA8
  instead of `Buffer<uint4>`; LNCL/BACK share one buffer, BACK at `kBackScreenOffset` (512) + line (`vdp2_defs.hlsli`).
- **Explicit storage image formats** (`rgba8ui`, `r32ui`) where DXC would infer `Rgba32ui` (sprite, bgs).
- **Compose output** is packed XBGR in a `RWStructuredBuffer<uint>` (pitch N * HRes; the software framebuffer's byte order)
  instead of a float4 UNORM image; `min()` keeps the UNORM clamp.
- **Workgroup size:** bgs is `[numthreads(32, 1, 1)]` dispatched with z = 7 instead of `[numthreads(32, 1, 7)]`
  (224 > 128 invocations); `id.z` is still the layer.
- **No Int64:** the rotation arithmetic (`(int64 a0 * b0 + …) >> 10` and `((int64 k * scr) >> 16 + p) >> 10`) uses
  the 32-bit helpers of `int64_emu.hlsli` (bgs).
- **Bounds guards** replace D3D12's robust out-of-range accesses: nothing outside HRes x VRes (or beyond the mesh layer)
  is written; framebuffer reads beyond the buffer, LNCL/BACK and RBG line colour reads outside their image return 0, as
  D3D12 does; RBG line colours outside their image are dropped. The output size (`CommonRenderParams::outputSize`) is
  clamped to the images (all three).
- **Layout:** `vdp2_compose_params.hlsli` follows std430 (int3 aligns to 16; explicit tail padding) and carries the
  per-layer shadow enables in a former hole; `vdp2_common_params.hlsli` carries the latched TVMD.BDCLMD, the output
  size and the internal resolution.
- **Resources:** the VDP1 framebuffer buffer holds eight 256 KiB slices (display FB 0/1 of main, alt, mesh0, mesh1);
  rotation parameter bases use a 512-line stride per parameter (exclusive monitor modes; Ymir indexes 256); NBG/RBG page
  base addresses are read from the layer parameters (`nbg[index]`, `rbg[index]` with `rotSel`) instead of a
  `pbaResized[16]` copy.

### 1b. Internal resolution (stage 1C)

`CommonRenderParams::renderScale` (N = 1-4; 1 is exactly the stage 1B output):
- the layer image is split: NBG0-3 and RBG0-1 in a background layer image at N x the native size (bindings 10/16),
  the sprite and mesh layers in a native image of their own (bindings 14/22, sprite pass and compose);
- bgs and compose are dispatched over N x N threads per native pixel (`(N * HRes / 32, N * lines, z)`), mapping thread
  (X, Y) to the native pixel (X / N, startY + Y / N) and the sub-pixel (X % N, Y % N); bgs writes the background layer
  texel (X, N * row + j), compose writes the composite at `Y * (N * HRes) + X` and reads every native input (sprite
  layers and attributes, colour calculation window, RBG line colours, LNCL/BACK) at the native pixel, colour gradation
  taps at X - N and X - 2N; the colour calculation window and the RBG line colours are native and written by the
  sub-pixel (0, 0) only; a vertical mosaic load reads the same sub-pixel of the earlier row;
- the sprite pass and the NBG fetch pass are native; `outputSize` stays the native size everywhere;
- bgs evaluates each sample as `native value + (sub-position * step) / span` (integer division; the native value at
  sub-position 0, so 1x and the sample points (N x, N y) are unchanged; stage1c-upscaling-research.md sections
  1.2-1.4):
  - NBGs (`NBGSubPosition`, `NBGScrollPos`): horizontal scroll plus i / N of the line's increment (zoom or line zoom),
    the vertical cell scroll column at that position, vertical scroll plus j / N of the vertical increment, or t / 2N
    with t = (composite row & 1) N + j in single-density interlace (both rows of a pair show one line);
  - RBGs (`RBGSubPosition`, `CalcRotationScreenCoords`): screen coordinates plus s / (S N) of the horizontal increment
    and Xst/Yst plus t / (R N) of the line's own deltaXst/deltaYst (no interpolation towards the next line); S = 2 in
    hi-res (s = (x & 1) N + i), R = 2 in interlaced modes outside exclusive monitor modes (t = (composite row & 1) N
    + j, deinterlaced or woven: both fields share the line's rotation bases, so the woven pair is monotone too); the
    RBG line colour is stored by sub-pixel (0, 0) with the native screen coordinates;
  - native: windows, NBG mosaic per axis (i = 0 when the mosaic is wider than one dot, the vertical position when it
    is taller than one line), RBG mosaic (the whole layer), coefficient addresses and values, rotation parameter
    selection, NBGs using the character table (character pattern delay, vertical cell scroll on 2x2 characters).

Sample points: at N = 2-4 the composite at (N x, N y) equals the 1x output at (x, y) for every native pixel, except
where the span rules above deliberately move the sample point (correct by design, excluded from the comparisons in
`tests/unit/vdp2_scene.*` and, conservatively from the frame size alone, in `brimir_vkhost --hash-sample`): odd native
columns where an RBG is enabled in a hi-res mode; odd composite rows where an RBG is drawn in an interlaced mode
(deinterlaced or woven); the second row of each identical pair for NBGs in single-density interlace. Known limits:
- a perspective floor whose depth comes from per-line coefficients gains detail mostly along x, and stays vertically
  stepped at N > 1. This is accepted (design Ruling 11): the design keeps per-line tables, the coefficient tables that
  give floors their perspective included, per native line, so the sub-lines of a line only step by the line's own
  deltaXst/deltaYst (stage 1C validation: on the dumped frames 0.07-8% of the blocks of Virtua Fighter 2's and Panzer
  Dragoon II Zwei's floors have vertical sub-pixel detail, against 34-55% on Panzer Dragoon II's zoomed sky; the
  plan's 25% figure was an estimate, and the magnified texels near the camera are content, not a rule). 1C still meets
  "visibly sharper on VDP2 layers" (horizontal detail, rotation, zoomed NBGs, sky). Follow-up, a design change:
  sub-line coefficient interpolation (scale-mode coefficients between KA(l) and KA(l) + dKAst when both entries are
  non-transparent) would make floors vertically smoother;
- a double-density bitmap NBG with vertical line scroll (alternate-line off) can show a sub-row sawtooth at N > 1
  (parked; the sample points are unchanged);
- exception W5 is unchanged at N (its NBG does not use the character table).

### 2. Software semantics (Brimir's software renderer is the reference)

Where Ymir's shaders differ from Brimir's software renderer (`src/core/src/ymir/hw/vdp/renderer/vdp_renderer_sw.cpp`,
"sw:" line references in the comments), the software behaviour is ported, marked `// Brimir: matches software
(sw:<lines>)`. Letters are those of `docs/ymir-hw-renderer-notes/stage1b-port-inventory.md` section 4.1 and the stage
1B rulings; each has a scene in `tests/unit/test_gpu_vdp2_scenes.cpp`.

Sprite (`cs_vdp2_render_sprite.hlsl`):
- framebuffer offsets wrap within the 256 KiB framebuffer (q);
- under deinterlacing the row is `(line << 1) | alt field`: rotated sprites use the line's coordinates and the alt
  fields read the framebuffers software reads (s, u);
- doubled hi-res output (16-bit framebuffer; rotated or not) repeats the even pixel, with rotated coordinates at half x
  (v; software fixed on master in 53bfa4c for rotated 8-bit hi-res sprites).

Backgrounds (`cs_vdp2_render_bgs.hlsl`):
- RBG line colours are the RGB555 colour of CRAM word `address & 0x7FF` (r);
- NBG scroll, line scroll/zoom, mosaic, vertical cell scroll (with zoom, repeat and delay) and the data access shift use
  the per-row state the front half computes exactly as software (`g_nbgLines`, `vdp2_nbg_line.hlsli`) instead of
  Ymir's closed-form scroll (b, d-f, h);
- character pattern delay, and vertical cell scroll on 2x2 characters: a pixel shows the character software's fetcher
  shows there, from the NBG character table (`g_nbgChars`, written by `cs_vdp2_nbg_fetch.hlsl`: the fetcher replayed
  per row with windows, mosaic, the per-fetch bank delay, the 2x2 cell switch rule and the fetcher's `nextChar` at the
  line start, decoded on the CPU); no cell swap for delayed two-word 2x2 characters (g; stage 1C replaced the per-pixel
  walk back over earlier fetches, O(width²) per line with windows);
- `FetchScrollBGPixel` split into locate + fetch so that the NBGs using the character table take only the dot and cell
  from the pixel's own position;
- RBG mosaic is applied by `DrawRBG` (scroll RBGs repeat every mosaicH-th pixel, line colour of the repeated pixel;
  software fixed on master in 53bfa4c); bitmap RBGs have no mosaic, as in software (c).

Compose (`cs_vdp2_compose.hlsl`):
- per-layer shadow enable; a sprite on top uses its own shadow bit (a);
- extended colour calculation: layer 1's enable checks the layer itself (`IsLayer1ColorCalcEnabled`) (i);
- the back screen uses its own colour calculation ratio (j);
- LNCL/BACK and the RBG line colours are per VDP line (field line under interlace), and display-off lines use the
  latched border colour mode with the back colour as last computed (k, t);
- line colour screen reads use the output position;
- transparent meshes: only normal mesh pixels are inserted; the mesh layer's colour calculation uses the sprite enable
  and condition with the mesh pixel's own colour MSB (l; software fixed on master in 7d1cb66);
- the sprite layer's colour offset enable and select as software;
- no first-interlaced-frame duplication: Ymir's irstInterlacedFrame (copying the row into the other field on the first
  frame after switching to interlace with deinterlacing off) is removed, since software clears the composite on a
  resolution change instead and the front half never sets the bit.

## Documented exceptions

Cases where the GPU output intentionally differs from software (software stale or ambiguous) are listed, with their
tests, in the design doc (`docs/superpowers/specs/2026-10-24-vulkan-gpu-renderer-design.md`, stage 1): U2 (sprites in
exclusive-monitor modes), X1 (the alternate field in exclusive-monitor modes under deinterlacing) and W2-W5 (character
delay / window / mosaic edge cases), each a `[!shouldfail]` scene named
"exception <id>", with a must-pass companion ("exception <id> is confined ...") that pins the difference to the pixels
the exception describes.
