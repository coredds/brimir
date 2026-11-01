# VDP1 compute shaders (GPU stages 2 and 3)

The compute shaders of the Vulkan VDP1 back half (`src/gpu/vdp1/vdp1_vulkan_backend.*`). They replay, in emulation
order, the op lists of the Vulkan-free front half (`src/gpu/vdp1/vdp1_front.*`): draw batches, erases and CPU
framebuffer write runs. FBRAM is one storage buffer shared with the VDP2 sprite pass (`cs_vdp2_render_sprite.hlsl`
reads it at the display framebuffer); its slice layout (main fb0/fb1, alt fb0/fb1, mesh[alt][fb]) is in
`src/gpu/vdp1/vdp1_hw_params.hpp`, the bindings in `src/gpu/vdp1/vdp1_bindings.hpp`.

| Shader | Does | Dispatch |
|---|---|---|
| `cs_vdp1_raster.hlsl` | one thread per span: the software line loop (`VDP1PlotLine` / `VDP1PlotTexturedLine` body and the read half of `VDP1PlotPixel`: clipping, mesh skip, double interlace, address); writes every slot of its span (fragment or 0) and counts fragments per FBRAM word | ceil(spans / 64) |
| `cs_vdp1_batch_range.hlsl` | turns the raster pass's min/max accumulator into the batch's unit range `[unitLo, unitHi)` and the indirect arguments of bucket and resolve (Ruling 10: work proportional to the words touched, not to all 262,144) | 1 group |
| `cs_vdp1_bucket.hlsl`, `cs_vdp1_bucket_sums.hlsl` | two-level exclusive scan of the per-word counts over the range (shared memory, no subgroup operations) | indirect / 1 group |
| `cs_vdp1_scatter.hlsl` | every live slot appends its index to its word's bucket (order arbitrary) | ceil(fragments / 256) |
| `cs_vdp1_resolve.hlsl` | one thread per word: the bucket's slots sorted by slot index (= software emission order: span order, then step, then main before antialias) and folded with the write half of `VDP1PlotPixel` into the (main, mesh) words; k ≤ 32 in registers, larger stacks by the whole group (histogram windows + bitonic sort), exact for any k | indirect, ≤ 4096 groups |
| `cs_vdp1_erase.hlsl` | `VDP1DoEraseFramebuffer` (the front half computes the clamped, shifted region; the cycle budget becomes a linear-index limit), alt mirror and mesh clear by flags | 8×8 over the region |
| `cs_vdp1_fb_write_mark.hlsl`, `cs_vdp1_fb_write.hlsl`, `cs_vdp1_fb_write_clear.hlsl` | a run of CPU framebuffer writes (`VDP1WriteFB`) in order: mark the last writer of each byte, write the winners, clear the marks | ceil(records / 64) each |
| `cs_vdp1_layout_probe.hlsl` | not dispatched: declares every struct so the build reflects the std430 layouts for `tests/unit/test_gpu_vdp1_params.cpp` | — |

N x passes (GPU stage 3, research `docs/ymir-hw-renderer-notes/stage3-research.md` §1.4 and §2; the CPU reference
`tests/unit/vdp1_nx_reference.*` is their specification). They draw each N x sub-batch of the front half's N x
generator (`src/gpu/vdp1/vdp1_nx_front.*`) into the N x surface (binding 24: the eight slices, native word w as the
N×N block of words w·N² + j·N + i), with `batch_range`, `bucket` and `scatter` reused on N x units (N²·262,144):

| Shader | Does | Dispatch |
|---|---|---|
| `cs_vdp1_raster_nx.hlsl` | one thread per N x span: rect (native row, N sub-rows, clamp-and-phase sub-steps, reversed mapping backwards), quad (32-bit steppers over N x end points) or replicate (the native line loop, each fragment as its N×N block); N x targets (unit in bits 21-0, flags 28-22) | ceil(spans / 64) |
| `cs_vdp1_bucket_sums_nx.hlsl`, `cs_vdp1_bucket_sums_nx_add.hlsl` | the second scan level for up to N²·4096 block totals: chunks of 4096 scanned in place with their totals stored after the blocks, then each chunk adds the totals before it | N² groups each |
| `cs_vdp1_resolve_nx.hlsl` | the native ordered resolve (`include/vdp1_resolve.hlsli`) with the N x unit decode, in rounds of 64·G units per group | indirect, ≤ 4096 groups |
| `cs_vdp1_erase_nx.hlsl` | the native erase's region and write limit, each erased word's N×N blocks (main, alt, mesh) | 8×8 over the region |
| `cs_vdp1_fb_write_nx.hlsl` | after the native run: each record's blocks take its mask's bits of the final native word (main, alt mirror) | ceil(records / 64) |
| `cs_vdp1_rebuild_nx.hlsl` | the slices of a mask replicated from native FBRAM (attach, reset, load, scale change) | N²·1024 × 8 |
| `cs_vdp1_nx_layout_probe.hlsl` | not dispatched: the N x structs' layouts and accessors for the params test | — |

Headers: `include/vdp1_defs.hlsli` (constants and packed-field accessors), `include/vdp1_params.hlsli` (the uint-only
structs), `include/vdp1_steppers.hlsli` (32-bit ports of `LineStepper<13>`, `TextureStepper`, the Gouraud steppers;
the line stepper also without the 13-bit shift for the N x quad path), `include/vdp1_raster_line.hlsli` (the native
line loop and slot writer, shared by both rasters), `include/vdp1_resolve.hlsli` (the ordered resolve, shared by both
resolves), `include/vdp1_apply_fragment.hlsli` (`ApplyFragment`), `include/vdp1_fbram_words.hlsli` (atomic 16-bit word
access on an FBRAM-layout buffer), `include/vdp1_fbram.hlsli` and `include/vdp1_fbram_nx.hlsli` (FBRAM and N x surface
access), `include/vdp1_fb_write.hlsli` (the FB-write runs).

## Provenance

**Brimir only.** The shaders were written from the CPU reference of the back half (`tests/unit/vdp1_raster_reference.*`),
which is itself written from the software renderer (`src/core/src/ymir/hw/vdp/renderer/vdp_renderer_sw.cpp` and
`src/core/include/ymir/hw/vdp/renderer/common/vdp1_steppers.hpp`) and checked against it (random command tables,
targeted cases, apex stacks, erase grids). Ymir's Direct3D 12 VDP1 shaders (`libs/ymir-core/res/shaders/src/vdp/` at
`8cdf0a53`) were consulted for the overall pass structure only; no file is derived from them, and their
`vdp1_*_params.hlsli` bitfield layouts were not used. Every file says so in its header comment. Shared with VDP2:
`../include/util/bit_ops.hlsli` (the shared include directory; included by `vdp1_defs.hlsli` and `vdp1_fbram.hlsli`).

The structs have host mirrors in `src/gpu/vdp1/vdp1_hw_params.hpp`; their offsets and every accessor are checked
against the SPIR-V reflection and the probe shader (`tests/unit/test_gpu_vdp1_params.cpp`), and the modules against the
baseline (`tests/unit/test_gpu_vdp1_pipelines.cpp`: Shader capability only, no int64, no images, 64-thread groups
(erase 8×8), exactly the pass's bindings, uint-only blocks, the stated shared-memory sizes).

Build: as the VDP2 shaders (`tools/shaders/build_shaders.cmake`, DXC `-spirv`, Vulkan 1.2, `spirv-val`; generated
headers committed in `src/gpu/shaders/generated/`; `-DBRIMIR_SHADERS_CHECK=ON` checks they are current).

## Design (research `docs/ymir-hw-renderer-notes/stage2-vdp1-research.md` §4.3)

- **Exactness model.** Software is the reference: GPU FBRAM (main, alt, mesh) equals the software renderer's FBRAM byte
  for byte after every op. The front half ports the command handlers and the quad stepping exactly (including master's
  quad termination, Ruling 3) and emits spans `(cmdIndex, quadStep)`; the raster pass steps each span iteratively, as
  `vdp1_steppers.hpp` does (no `SetStep` jumps), with wrapping 32-bit arithmetic identical to the reference.
- **Deterministic slots.** A span owns `2 × Length()` consecutive fragment slots (step × 2 + antialias); the slot index
  is the order key, so fragments never need an atomic order counter, and there is no cap on fragments per word (a
  batch closes between spans at the fragment-buffer limit, `kMaxFragmentsPerBatch`).
- **Ordered resolve.** Each FBRAM word folds its fragments in key order with software's write semantics: main/mesh
  pairs (transparent meshes: colour calculation reads main, writes mesh; a main write clears mesh), 8 bpp per byte,
  MSB-on at 16-bit-word granularity, shadow and half-transparency sequentially. Words are owned by one thread, so the
  only atomics are the word write-back (two words share a uint) and the counts.
- **Batches and barriers.** Batches end at an op-kind change, a VRAM hazard (a write into a range a pending command
  reads: texture, look-up table, Gouraud table, command table), a draw-framebuffer swap, a capacity limit, a VDP2
  frame boundary or the op take (end reasons in `--probe`). Every pass ends with a compute→compute (and
  indirect-argument) barrier; Record waits for earlier compute/transfer work and leaves FBRAM readable by the VDP2
  sprite pass and the readback copies.
- **Stage 3 anticipation.** The raster push constants carry `scale` (fixed at 1, not read); commands keep the four
  transformed vertices, Gouraud corners and texture parameters.

## Divergences from Ymir

Ymir is never a reason to differ from software, so these are deliberate (research §4.2):

| Item | Ymir D3D12 | Brimir |
|---|---|---|
| Half-transparency, shadow | per-pixel lists capped at 32, shadow by count | ordered resolve, unbounded, sequential formulas |
| Copy-mode ordering | `InterlockedMax(seq << 16 \| colour)`, 65,535-span batches | slot key = software emission order |
| Quad stepping, Gouraud | `SetStep` / `Skip(spanStep)` jumps | iterative steppers, as software |
| Quad termination | "survives SystemClip" | master's predicate (Ruling 3): stop only when no remaining line can reach the clip area |
| 13-bit coordinate wrap | disabled | exact port of the steppers |
| Clip commands | masked to 10/9 bits | raw 16-bit |
| Mesh with transparent meshes | sequence-number survival; shadow/half-transparency TODO | software's (main, mesh) formulas |
| Lines with doubleV | no doubleV | `y << doubleV`, as software |
| Erase | skips x1 == x3 / y1 == y3, inclusive x3 | software's region, clamps and cycle budget |
| CPU FB write | mesh clear, double-density-only mirror | no mesh clear, mirror on any interlace (software) |
| VRAM hazards | texture only | texture, look-up table, Gouraud and command tables |

## CPU side (not in the shaders)

FBRAM is GPU-authoritative in Gpu VDP1 mode. Every submit ends with readbacks of the slices the GPU changed into a CPU
shadow (`mem1.FBRAM`, the mesh shadow, and alt while deinterlacing), merged when the command buffer is known complete;
CPU writes since the readback keep their bytes (a byte mask). A CPU read of newer GPU data costs one sync submit and a
fence wait per read burst; `retro_serialize` only waits for fences. See `src/gpu/vdp/vulkan_vdp_renderer.hpp` (rules
1-8) and the design doc's stage 2 section.
