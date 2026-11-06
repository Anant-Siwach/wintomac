# D3D12 directly on Metal: feasibility spike

Status: **prototype works**. `WINTOMAC_D3D12_BACKEND=metal` swaps libvkd3d → MoltenVK for our own D3D12-on-Metal
layer (`runtime/d3d12mtl*`); the default stays vkd3d. The layer is in the spirit of Apple's D3DMetal, which CrossOver
uses.

On the Metal backend:
* `dx12_offscreen`, the new descriptor-table test `dx12_tables` (both DXBC and DXIL), `dx12_present`,
  `dx12_async_present` and Microsoft's `D3D12HelloTriangle` give **byte-identical pixels** to the vkd3d path.
* At 2000 draws/frame, the Metal backend uses **half the CPU per draw** and gives 19% shorter frames.

The open question is shader translation. It decides how far the layer can go (SM 6.5/6.6, mesh shaders, ray
tracing), and its best candidate (Apple's Metal Shader Converter) needs an Apple developer sign-in to download.
**The user has to do that.**

Related: `docs/d3d12.md` (the vkd3d path and its MoltenVK walls).

## 1. Shader translation

### Candidates
| | path | obtained as | licence |
|---|---|---|---|
| (a) | **Apple Metal Shader Converter** (MSC): DXIL → Metal IR (metallib), with a D3D12-style root signature / top-level argument-buffer model and `metal_irconverter_runtime.h` helpers | `.pkg` from developer.apple.com/download/all. **Needs an Apple developer account sign-in.** | Apple licence, not readable without signing in (see §1.3) |
| (b) | **vkd3d-shader's MSL backend**: DXBC/DXIL → MSL | our vkd3d 1.19 sources, rebuilt with `-DVKD3D_SHADER_UNSUPPORTED_MSL` (`third_party/vkd3d-msl`, tools only) | LGPL-2.1 |
| (c) | **vkd3d-shader → SPIR-V → SPIRV-Cross → MSL**: what MoltenVK effectively does | already linked + `make spirv-cross` | LGPL-2.1 + Apache-2.0 |
| (d) | **dxil-spirv (+ dxbc-spirv) → SPIR-V → SPIRV-Cross → MSL**: vkd3d-proton's DXIL front end | `third_party/dxil-spirv` (tools only) | MIT + Apache-2.0 |

### Measured
`tools/d3d12mtl_shader_matrix.py` compiles every test shader:
* SM 5 from vkd3d-compiler;
* SM 6.x from our DXC build;
* the corpus: `tests/shaders/sm6x`, the dx12 test shaders (tri11, gs/*) and HelloTriangle.

It then translates each shader along every path. The result counts only if the MSL compiles with Apple's compiler
(`xcrun metal -std=metal3.1`, Metal 32023.922). Valid MSL is necessary, not sufficient; the runtime tests below check
pixels for path (c).

| shader | in | probes | (b) vkd3d MSL | (c) vkd3d+SPIRV-Cross | (d) dxil-spirv+SPIRV-Cross | (a) MSC |
|---|---|---|---|---|---|---|
| tri11 vs/ps | DXBC, DXIL | cbuffer, IA (dx12_offscreen) | **VS no** (unsupported path), PS yes | yes | yes | DXIL only: not run |
| HelloTriangle VS/PS | DXBC, DXIL | MS sample | yes | yes | yes | not run |
| ps_tex / ps_buf / ps_cube | DXBC, DXIL | Texture2D+sampler, typed buffer, cube | no | yes | yes | not run |
| gs_pass | DXBC, DXIL | geometry shader | no | no (Metal has no GS stage) | no | (MSC emulates GS) |
| wave60, quad_ps, wave_vs | DXIL 6.0 | wave + quad ops | no (except wave_vs) | yes | yes | not run |
| int64 | DXIL 6.0 | 64-bit ints | no | yes | yes | not run |
| bary_ps | DXIL 6.1 | SV_Barycentrics, GetAttributeAtVertex | no | no (vkd3d) | no (SPIRV-Cross: PerVertexKHR) | not run |
| viewid_vs | DXIL 6.1 | SV_ViewID | no | yes | yes | not run |
| f16 | DXIL 6.2 | native 16-bit | no | yes | yes with `--ssbo-uav --ssbo-srv` | not run |
| dot4 | DXIL 6.4 | packed dot | no | yes | yes | not run |
| wave65 | DXIL 6.5 | WaveMatch / MultiPrefix | no | **no** (vkd3d) | **yes** | not run |
| mesh | DXIL 6.5 | mesh shader | no | **no** (vkd3d) | **yes** (MSL mesh) | not run |
| rayquery | DXIL 6.5 | inline RT (RayQuery) | no | **no** (vkd3d) | **yes** (MSL intersection_query) | not run |
| atomic64 | DXIL 6.6 | 64-bit atomics (add, max; groupshared) | no | no | no: SPIRV-Cross refuses 64-bit atomics | not run |
| dynres | DXIL 6.6 | ResourceDescriptorHeap | no | **no** (vkd3d) | no: SPIRV-Cross needs custom bindless AB setup | not run |
| pack66 | DXIL 6.6 | pack/unpack, [WaveSize] | no | yes | yes | not run |
| csderiv | DXIL 6.6 | compute derivatives | no | no: Metal has no `dfdx` in kernels | no (same) | (MSC 4.0 adds 2D compute derivatives) |
| **valid MSL** | | 31 builds | **7** | **22** | **24** | – |

Facts behind the table:
* **Metal's 64-bit atomics** are `atomic_max`/`atomic_min` on `ulong` in device memory only. There is no add, CAS
  or exchange, and none in threadgroup memory. `xcrun metal`, std 3.1 and 4.0, rejects
  `atomic_fetch_add_explicit(atomic_ulong*)` and threadgroup `atomic_ulong` (tested on the M5).
  * SM 6.6 `InterlockedAdd64`/CAS must therefore be emulated whatever the converter. Typical options are a lock
    word, or 2x32-bit when the high half can't carry.
  * UE5 Nanite's visibility buffer only needs 64-bit **max**, which maps directly. Epic's own Metal RHI relies on the
    same thing on Apple8+.
* **No geometry stage and no derivatives in kernels.** GS and compute derivatives need emulation: compute or mesh
  shader expansion, and quad shuffles respectively.
* **vkd3d's MSL backend** is explicitly "unsupported" upstream. It can't yet do vertex shaders with a cbuffer +
  input layout, any texture or sampler binding, or SM 6 global flags. It is years from being usable here.
* **(c) and (d) differ only in the front end.** vkd3d-shader's DXIL parser is the bottleneck for SM 6.5/6.6
  (WaveMatch, mesh, RayQuery, ResourceDescriptorHeap, 64-bit groupshared). dxil-spirv already handles all of these
  for vkd3d-proton. (d) also takes DXBC through dxbc-spirv, DXVK's new front end.
* **MSC takes DXIL only.** SM 5 titles (most of UE4's D3D12 path) would need DXBC → DXIL first. Microsoft's
  **dxilconv** (DXBC→DXIL, MIT) is in the DXC tree we already build (`third_party/dxc/src/projects/dxilconv`); it is
  Windows-centric and untested on macOS here.

### Licence findings and what the user must do
* **Metal Shader Converter: download blocked on an Apple sign-in. Nothing was downloaded and nothing was accepted.**
  * `https://developer.apple.com/download/all/?q=metal%20shader%20converter` answers 302 to
    `idmsa.apple.com/IDMSWebAuth/signin.html`.
  * The direct package URLs (`download.developer.apple.com/Developer_Tools/Metal_Shader_Converter_*/...pkg`) answer
    302 to `developer.apple.com/unauthorized/`.
  * The product page lists 4.0 beta (macOS and Windows) and needs macOS 13 + Xcode 15 to run. Its output needs
    Tier 2 argument buffers on macOS 14+.
  * Publicly, Apple's documentation states only that its *code snippets* are Apache-2.0.
  * We could not read the licence for `libmetalirconverter.dylib`, the CLI or `metal_irconverter_runtime.h`
    without signing in. So whether the dylib may be redistributed in a game bundle, or driven from a tool like ours,
    is **unknown**.
  * A third-party GitHub mirror of the binaries exists. It has no licence file and was not used, because it would
    sidestep Apple's terms.
  * **User action:**
    1. Sign in at developer.apple.com/download/all, accept Apple's terms, and download "Metal shader converter"
       (3.x, or the 4.0 beta).
    2. Put the `.pkg` in `third_party/metal-shaderconverter/`. Don't install it.
    3. Then it can be inspected with `pkgutil --expand-full`: read the licence (redistribution of the dylib and
       runtime header), and run (a) through the matrix.
* **vkd3d-shader (b, c):** LGPL-2.1, dynamically linked as today; fine.
* **SPIRV-Cross (c, d):** Apache-2.0; static linking is fine with attribution (added to `CREDITS.md`).
* **dxil-spirv and dxbc-spirv (d):** MIT; fine.
* **DXC/dxilconv:** NCSA + MIT; fine.

### Decision
* **Prototype path: (c), vkd3d-shader → SPIR-V → SPIRV-Cross → MSL, compiled at runtime by `MTLDevice
  newLibraryWithSource:`.**
  * It is the only path we can ship today that handles SM 5 DXBC and SM 6.0–6.4 DXIL.
  * It reuses the front end the vkd3d path is tested with (our patched vkd3d-shader).
  * The player needs no Metal toolchain.
* **The binding layout follows MSC's model** (§2.2), so (a) can be swapped in for DXIL without touching heaps or root
  signatures.
* **Next steps:**
  1. Move the front end to **(d) dxil-spirv**. That unlocks WaveMatch, mesh and RayQuery now, with fewer licence
     questions than MSC.
  2. Evaluate **MSC** as soon as the user has downloaded it. MSC is the only option that already *emulates*
     geometry/tessellation stages and implements SM 6.6 dynamic resources on Metal. That is a lot of hard work we
     would otherwise write ourselves.

## 2. Architecture

### 2.1 Objects: same vtables, different host
* `runtime/d3d12mtl.m` implements the D3D12 interfaces as **plain C COM objects with the vtables libvkd3d's headers
  declare**: `ID3D12Device9`, `ID3D12GraphicsCommandList6`, `ID3D12Resource2`, …
* `runtime/d3d12.c` therefore wraps them **exactly like libvkd3d's objects**. Unchanged:
  * the generated Win64 thunks (`tools/gen_d3d12.py`);
  * the wrapper cache and interface identity;
  * the hand-written shims;
  * the GS/stream-output fallbacks (`d3d12_pso.c`);
  * Device10..14 / List7..10 (`d3d12_ext.c`);
  * DXVK's `dxgi.dll`.
* The guest sees the same vtables whichever backend runs.
* The backend is chosen once, in `D3D12CreateDevice` (`WINTOMAC_D3D12_BACKEND=metal`).
* Every vtable slot we don't implement logs `metal backend: ID3D12 <iface>::<method>` once (method names come from
  the generated tables) and returns `E_NOTIMPL`.
* The Objective-C file is built without ARC. A `#define BOOL` dance keeps vkd3d's `BOOL` (int) apart from
  Objective-C's.

Changes outside the new files, all guarded by the backend switch:
* `d3d12.c`: backend selection; method-name lookup.
* `d3d12_pso.c`: no Vulkan capability query on Metal.
* `d3d12_swapchain.c`: back buffers become committed resources; Present calls `d3d12mtl_present`.

### 2.2 Resource binding: descriptor heaps → Metal argument buffers
Per shader stage, the Metal buffer argument table is laid out like MSC's `kIR*BindPoint` model:

| Metal slot | contents |
|---|---|
| `buffer(2)` | **root argument block**, MSC's top-level argument buffer layout. One entry per root parameter in order, 8-byte aligned: 32-bit constants inline; root CBV/SRV/UAV and descriptor tables as 64-bit GPU addresses. Root constants are read straight from it (vkd3d push constants at those offsets); sent with `set{Vertex,Fragment}Bytes` when dirty. |
| `buffer(3 + k)` | the k-th non-constant root parameter. A **descriptor table** is the shader-visible heap's `MTLBuffer` bound at the table's byte offset, read in MSL as a **Tier 2 argument buffer**. A root CBV is the buffer at its offset. |
| `buffer(16 + slot)` | vertex buffers; dynamic strides (`setVertexBuffer:offset:attributeStride:atIndex:`), because D3D12 sets strides per bind |
| (`buffer(0)`, `buffer(1)`) | reserved for MSC's resource / sampler heap bindings (SM 6.6 `ResourceDescriptorHeap`) |

* **CBV_SRV_UAV and sampler heaps (shader-visible) are `MTLBuffer`s of 24-byte entries**, MSC's
  `IRDescriptorTableEntry`:
  * slot 0: GPU address (buffers) or sampler `MTLResourceID`;
  * slot 1: texture or texture-buffer `MTLResourceID` (`gpuResourceID._impl`);
  * slot 2: metadata (buffer size, sampler LOD bias).
* `GetCPUDescriptorHandleForHeapStart` is the buffer's CPU mapping and the GPU handle its `gpuAddress`. So
  `CreateShaderResourceView`, `CopyDescriptors` and `SetGraphicsRootDescriptorTable` are plain memory writes and
  address arithmetic, with no per-draw descriptor copying.
* Texture views and sampler states are retained per heap slot, so the `MTLResourceID`s stay valid.
* CPU-only heaps are `malloc`ed with the same layout.
* RTV/DSV heaps are CPU arrays of `{resource, texture, mip, slice, format}`. They are copied into the command stream
  at record time, as D3D12 specifies.
* **Shader side:** `d3d12mtl_shader.cpp` compiles each shader **against its PSO's root signature**:
  * every descriptor table visible to the stage becomes one SPIR-V descriptor set (binding = descriptor offset in
    the table);
  * SPIRV-Cross turns each set into an argument buffer whose `[[id(n)]]` is the 8-byte slot. Descriptor k at slot
    `3k` is a buffer or sampler; at slot `3k+1` it is a texture.
  * `pad_argument_buffer_resources` fills the gaps, so the struct layout *is* the heap layout;
  * root constants are a push-constant block at `buffer(2)`;
  * **static samplers become MSL `constexpr sampler`s**.
* `dx12_tables` checks a CBV + SRV table at a non-zero heap offset, a sampler table, a static sampler and root
  constants. The generated MSL:

```metal
struct spvDescriptorSetBuffer0 {            // = heap entries 1..2 of the table
    constant cb1_struct* cb0_0 [[id(0)]];   // entry 0, slot 0 (address)
    constant uint* _m1_pad [[id(1)]]; constant uint* _m2_pad [[id(2)]]; constant uint* _m3_pad [[id(3)]];
    texture2d<float> t0 [[id(4)]];          // entry 1, slot 1 (MTLResourceID)
};
fragment main0_out main0(..., constant spvDescriptorSetBuffer0& spvDescriptorSet0 [[buffer(3)]],
                         constant spvDescriptorSetBuffer1& spvDescriptorSet1 [[buffer(4)]],
                         constant push_cb_struct& push_cb [[buffer(2)]], ...)
{   constexpr sampler s1(filter::linear, mip_filter::linear, address::repeat);   // static sampler
```

* **Residency:** every buffer, texture and heap goes into one device-wide **`MTLResidencySet`** attached to the
  queues (macOS 15+). Resources reached only through argument buffers need no per-encoder `useResource:` calls.
* **Hazards:** resources are hazard-tracked by Metal, so D3D12 transition and aliasing barriers are no-ops.
* **Prototype limits:**
  * at most 7 descriptor tables per stage and 12 non-constant root parameters;
  * unbounded ranges are capped at 256 descriptors;
  * root SRV/UAV are only usable as plain buffers;
  * no UAV counters.

### 2.3 Command lists, queues, synchronisation
* **Recording.** `ID3D12GraphicsCommandList` records into a compact command stream. Descriptors (RTV/DSV) and root
  arguments are captured at record time; resource contents and descriptor-heap contents are read at execution time.
  This is what D3D12 specifies, and lets a closed list be executed many times (Metal command buffers are
  single-use).
* **Replay.** `ExecuteCommandLists` replays all lists into one `MTLCommandBuffer` and commits it:
  * **Render passes are formed lazily**: the first draw after `OMSetRenderTargets` opens a render encoder.
  * **Clears become load actions** of the next pass that uses the view. Clears that no pass consumes are flushed as
    empty clear passes, in order.
  * Copies use a blit encoder; dispatches a compute encoder.
  * Encoder state is dirty-tracked: pipeline, viewports/scissors, vertex buffers and root arguments are only re-sent
    when they change.
  * Scissors are clamped to the attachment (Metal validates that).
* **Fences.** `ID3D12Fence` is an **`MTLSharedEvent`**:
  * `Signal`/`Wait` on a queue encode `encodeSignalEvent`/`encodeWaitForEvent`;
  * `GetCompletedValue` is `signaledValue`;
  * `SetEventOnCompletion` with a guest event uses an `MTLSharedEventListener` block that sets the event (the job
    libvkd3d's fence thread does on the vkd3d path); with NULL it blocks.
* **Queues** are `MTLCommandQueue`s. Several queues synchronise through the same shared events, as in D3D12.
* **Presentation (`d3d12_swapchain.c`).**
  * It keeps DXVK's `dxgi.dll`, the presenter thread and the latency logic.
  * On Metal, back buffers are ordinary committed textures.
  * Present draws the back buffer into the window's `CAMetalLayer` drawable on the game's queue, with a full-screen
    triangle: CAMetalLayer has no RGBA8 format, and scaling comes for free.
  * Headless runs use the hidden SDL window's layer.

### 2.4 Pipeline state
* `CreateGraphicsPipelineState` translates VS and PS against the root signature.
  * The VS's outputs are remapped to the PS's input locations (vkd3d varying map), because D3D links varyings by
    semantic.
  * It builds the `MTLVertexDescriptor` from the input layout: attribute = the VS input register of the semantic,
    dynamic stride, per-instance step rates.
  * It builds the blend, depth/stencil (`MTLDepthStencilState`) and rasterizer state, and compiles one
    `MTLRenderPipelineState`.
  * D3D12 PSOs carry RT/DS formats and sample count, so a D3D12 PSO maps 1:1 onto a Metal pipeline. There are no
    draw-time pipeline variants.
* **Compute PSOs** take `[numthreads]` from the shader.
* **Cost: 6–11 ms per PSO**, plus ~350 ms once for the Metal compiler's first use. A **shader cache** is needed:
  `MTLBinaryArchive` / Metal 4 pipeline datasets keyed by DXBC hash + root signature, ideally warmed from the game's
  PSO library. On the vkd3d path, PSO creation looks cheap (0.3 ms) only because libvkd3d defers the real Vulkan
  pipeline to the first draw.

### 2.5 How the hard features map
| D3D12 feature | Metal mapping | effort |
|---|---|---|
| **Geometry shaders** | No GS stage. Options: (1) MSC's geometry emulation (VS+GS as an object+mesh pipeline); (2) our own: VS and GS as a compute pre-pass writing a vertex/index buffer, then an indirect draw (DXMT does this); (3) the existing pass-through-strip/skip fallbacks (`d3d12_pso.c`) already apply on this backend. Mesh-shader emulation is the best long-term path: no round trip through memory. | high |
| **Tessellation (HS/DS)** | Metal tessellation = compute kernel for the factors (HS converted to a kernel) + post-tessellation vertex function (DS). MSC also emulates HS/DS via mesh shaders. | high |
| **Mesh / amplification shaders** | Native: Metal 3 object/mesh shaders (`MTLMeshRenderPipelineDescriptor`, `drawMeshThreadgroups`). Translation exists in (d) (mesh) and MSC. | medium |
| **Ray tracing (DXR 1.1 RayQuery)** | Metal `intersection_query` + `MTLAccelerationStructure`. BLAS/TLAS builds map to `MTLAccelerationStructureCommandEncoder`; D3D12 instance descs need a repack (compute) into `MTLAccelerationStructureInstanceDescriptor`. Translation exists in (d) and MSC. | medium-high |
| **DXR 1.0 pipelines** (raygen/hit/miss) | Metal has no shader-binding-table pipeline: compile raygen as a kernel with an intersection-function table (MSC 2.0 "ray generation as compute kernels" does this). | high |
| **ExecuteIndirect** | Draw/dispatch-only signatures: `drawPrimitives:indirectBuffer:` (plus a tiny compute pass for the count buffer, or an indirect command buffer). Root-constant / VB / IB changes: encode an `MTLIndirectCommandBuffer` from a compute kernel that reads the argument buffer (ICBs support per-command vertex buffers, pipeline and draw). Better than the vkd3d path, which needs device-generated-commands extensions MoltenVK lacks. | medium-high |
| **SM 6.6 dynamic resources** | `ResourceDescriptorHeap[i]` = the CBV_SRV_UAV heap bound at `buffer(0)`; the 24-byte entry layout is already MSC's. Needs a front end that emits it (MSC, or dxil-spirv `--bindless` + a SPIRV-Cross runtime-array setup). | medium |
| **64-bit atomics** | max/min on device `ulong` native (Nanite's use); add/CAS/exchange and groupshared need emulation. | medium |
| **Wave ops** | Native SIMD-group functions (32 lanes on Apple GPUs); WaveMatch/MultiPrefix via (d)/MSC. | low |
| **Tiled / reserved resources** | Metal sparse textures (`MTLHeapTypeSparse`, `updateTextureMappings`), tile sizes differ from D3D12's 64 KiB standard shapes; sparse *buffers* are not available (emulate with page tables). Tier 1 is plausible; tier 2+ is a research item. | high |
| **Placed resources / heaps / aliasing** | `MTLHeap` (placement heaps, `newBufferWithLength:offset:`), aliasing via `makeAliasable`. Needs untracked hazard mode + our own barriers (`MTLFence`) for performance. | medium |
| **Queries** | occlusion: visibility result buffer; timestamps: `MTLCounterSampleBuffer` (stage-boundary sampling only on Apple GPUs); pipeline statistics: none. | medium |
| **Predication** | per-draw: read the predicate in a compute pre-pass and patch indirect args to 0 (works with ICBs). | medium |
| **Stream output** | compute/vertex-function writes to a buffer (Metal vertex functions can write device memory) + rasterization disabled. | medium |
| **D24S8, MirrorOnce border colours, logic ops** | Depth32Float_Stencil8 (repack on copies), closest MTL modes, no logic ops (rare). | low |

## 3. What the prototype does
Implemented (`runtime/d3d12mtl.m`, `runtime/d3d12mtl_shader.cpp`, `runtime/d3d12mtl.h`; ~2000 lines):
* **device**: feature reporting (FL 12_0, binding tier 2, SM 6.0, wave ops); command queue, allocator and list;
  fences; descriptor heaps (RTV, DSV, CBV_SRV_UAV, sampler).
* **descriptors**: CBV / SRV (textures, buffers) / UAV / sampler descriptors; `CopyDescriptors(Simple)`.
* **resources**: committed buffers and textures (default/upload/readback; 1D/2D/3D/arrays/MSAA); `Map`;
  `GetCopyableFootprints`; `GetResourceAllocationInfo`.
* **pipelines**: root signatures (1.0/1.1); graphics and compute PSOs.
* **command list**: `ResourceBarrier` (no-op); `CopyTextureRegion` (buffer↔texture, texture↔texture),
  `CopyBufferRegion`, `CopyResource`; `ClearRenderTargetView`, `ClearDepthStencilView`; `DrawInstanced`,
  `DrawIndexedInstanced`, `Dispatch`; root constants / tables / CBVs; viewports, scissors, blend factor, stencil ref.
* **presentation**: DXGI present through DXVK's dxgi.dll.

Results (Apple M5, macOS 26.7, recompiled Windows programs):

| program | Metal backend vs vkd3d → MoltenVK |
|---|---|
| `dx12_offscreen` (root constants, IA, clear + triangle, readback) | **byte-identical** output, pixel hashes `ae1339e40fa2a383` / `7f193605d7c6c4ff` (test `dx12mtl_offscreen`) |
| `dx12_tables` (new: tables, sampler heap, static sampler, upload via footprint, DXBC *and* DXIL) | **byte-identical**, hash `339884ba31c0a1af`, DXIL = DXBC (test `dx12mtl_tables`) |
| `D3D12HelloTriangle` (Microsoft sample, DXIL, DXGI swapchain, 60 presents to a hidden window) | same pixels: centre 127,64,64,255, corner 0,51,102,255 |
| `dx12_present`, `dx12_async_present` | identical output |
| `dx12_dxil` | identical pixels for all four SM5/SM6 draws; only the reported shader model differs (6.0, honest for this front end) |
| `dx12_gs_fallback` | identical except: no predication; reserved resources; VP/RT index from VS not reported |
| `dx12_sm6x`, `dx12_breadth` | fail where expected: SM 6.5/6.6 front-end gaps (§1); `CreatePipelineState` (streams), `CreateCommittedResource2` and more not implemented |

**Overhead** (`make dx12mtl-bench`, `tools/d3d12mtl/dx12_bench.c`). The bench draws a triangle many times per frame,
with new root constants per draw, into 256x256, and waits on the fence every frame; medians of 120 frames. CPU =
guest-side record + `ExecuteCommandLists`, including the Win64 thunks.

| draws/frame | CPU/draw vkd3d | CPU/draw Metal | frame vkd3d | frame Metal |
|---|---|---|---|---|
| 200 | 795 ns | **372 ns** | 0.54 ms | **0.50 ms** |
| 2 000 | 507 ns | **265 ns** | 3.26 ms | **2.63 ms** |
| 10 000 | 452 ns | **237 ns** | 11.2 ms | **8.9 ms** |

The Metal backend needs about half the CPU per draw. Frames are 7–20% shorter: one encoder per pass, root constants
via `setBytes`, no Vulkan descriptor/pipeline layer and no MoltenVK translation. PSO creation is slower (6–11 ms;
§2.4).

## 4. Plan and effort to "UE4/UE5 D3D12 titles run"
Estimates assume one engineer full-time who knows D3D12 and Metal (an agent-week ≈ an engineer-week here). Each step
ends with tests in `make check` (pixel-exact against the vkd3d path where it can render the same thing) and a real
title.

| # | milestone | contents | estimate |
|---|---|---|---|
| 1 | **SM 5 breadth (UE4 D3D12 baseline)** | placed resources + `MTLHeap`, aliasing; `CreatePipelineState` streams, `CreateCommittedResource2/3`, Device10+ entry points the vkd3d path handles in `d3d12_ext.c`; root SRV/UAV + UAV counters; all formats incl. D24S8 repack, BC, MSAA resolve; queries; `ClearUnorderedAccessView*`; compute + UAV barriers; copy/compute queues; bundles; partial clears; mip/array RTVs and SRV swizzles; `WriteToSubresource`; device-removed/out-of-memory paths | 6–8 wk |
| 2 | **Shader cache + PSO latency** | MSL/metallib cache on disk keyed by bytecode+root signature; `MTLBinaryArchive`; async compile threads; reuse identical shaders across PSOs | 2–3 wk |
| 3 | **Front end swap to dxil-spirv (DXIL) / keep vkd3d or dxbc-spirv (DXBC)**, and a decision on MSC once downloadable | SM 6.0–6.6 incl. WaveMatch, 16-bit, SM 6.6 dynamic resources (heaps at `buffer(0/1)`), barycentrics (custom SPIRV-Cross path), 64-bit atomic emulation; or MSC + dxilconv if its licence permits shipping | 4–6 wk |
| 4 | **ExecuteIndirect + predication** | MTLIndirectCommandBuffer encoding from compute, count buffers | 3–4 wk |
| 5 | **Geometry shaders, tessellation, stream output** | mesh-shader (or compute) emulation of GS; Metal tessellation for HS/DS; SO via device writes. UE4 uses tessellation in some titles, GS rarely | 6–10 wk |
| 6 | **Performance** | untracked resources + `MTLFence`s from D3D12 barriers, render-pass merging, parallel encoding per list (MTLParallelRenderCommandEncoder / multiple command buffers), upload ring for root blocks, Metal 4 argument tables | 4–6 wk |
| 7 | **UE5 features** | mesh shaders (Nanite's optional HW path), RayQuery + acceleration structures (Lumen HW RT; software Lumen works without), 64-bit max atomics on textures (Nanite), DXR 1.0 pipelines via kernels; sparse textures for virtual shadow maps / virtual textures (tier 1) | 8–12 wk |
| 8 | **Title bring-up** | real games: driver-bug workarounds, edge semantics (null descriptors, out-of-bounds robustness, NaN/denorm, half-pixel rules), memory budgets | ongoing, 4+ wk per engine generation |

Total, realistically:
* **~5–6 months** to "UE4 D3D12 titles generally run" (milestones 1–4 + parts of 5 and 6);
* **~9–12 months** for UE5 titles with Nanite/Lumen on M2+ (all milestones).

For calibration: DXMT (D3D11 → Metal only) took one developer about two years to reach broad compatibility; D3DMetal
is a multi-year Apple effort. The vkd3d path stays available meanwhile. Titles can be switched per game, and the
Metal backend already outruns it on CPU overhead.

## 5. Risks
* **Shader translation is the critical path.**
  * vkd3d-shader's DXIL front end can't go beyond SM 6.4 today.
  * dxil-spirv + SPIRV-Cross covers more, but SPIRV-Cross refuses 64-bit atomics and per-vertex attributes, and
    needs custom bindless setup.
  * MSC covers the most, including GS/tessellation emulation, but its licence and redistribution terms are unknown
    until the user signs in. It is a closed binary we can't fix, and it takes DXIL only (DXBC needs dxilconv).
* **Metal hardware/API limits.**
  * No geometry stage.
  * 64-bit atomics are min/max only.
  * No compute derivatives (MSC 4.0 emulates them).
  * 31 buffer slots per stage.
  * Sparse resources: textures only, with different tile shapes.
  * No pipeline-statistics queries.
  * Timestamps only at encoder boundaries on Apple GPUs.
  * No 24-bit depth, no logic ops.
  * Argument buffers must be Tier 2: macOS 14+ on M1+ for MSC output; our residency sets need macOS 15.
* **Binding scale.**
  * Real titles use unbounded tables and 500K–1M descriptors (SM 5.1 bindless, UE5).
  * The prototype caps ranges and relies on SPIRV-Cross argument-buffer padding, which is O(descriptors) in shader
    size.
  * The fix is MSC-style heap-pointer tables (`buffer(0)` + index arithmetic) rather than padded structs. That needs
    the front end to emit it.
* **Hazard tracking.**
  * Relying on Metal's tracked resources keeps the prototype correct and simple, but costs CPU on big scenes and
    forbids aliasing.
  * Moving to untracked resources + fences derived from D3D12 barriers is a correctness minefield: games under-specify
    barriers, and Windows drivers forgive that.
* **PSO compile hitches.** Runtime MSL compilation is ~10 ms per PSO, so a disk cache and pre-warming are mandatory
  for real games.
* **Semantics drift.** Clamped scissors, clear-with-rects, MSAA sample positions, depth-clip/bias and NaN behaviour
  each need checking against Windows output. Tests compare against the vkd3d path, which is itself an approximation
  of Windows.
* **Maintenance.** A second D3D12 implementation to keep alive next to vkd3d. The shared front end (COM thunks,
  `d3d12_ext.c` breadth) limits this, and the backend is switchable per game.

## 6. Files and how to run
* `runtime/d3d12mtl.m`: the D3D12 objects on Metal, command-stream replay, presentation.
* `runtime/d3d12mtl_shader.cpp`: shader translation and the root-signature binding layout.
* `runtime/d3d12mtl.h`: the binding model, shared by the two.
* `tests/src/dx12_tables.c` (+ `tests/shaders/tables/tables.hlsl`, blobs from `tools/d3d12mtl/gen_tables_blobs.sh`):
  binding test for both backends.
* `tests/src/dx12mtl_offscreen.c`, `tests/src/dx12mtl_tables.c`: the same programs on the Metal backend. `make
  check` runs `dx12mtl_*` with `WINTOMAC_D3D12_BACKEND=metal` against the native libvkd3d reference.
* `tools/d3d12mtl_shader_matrix.py` (+ `tools/d3d12mtl/spv11.c`): the shader-path comparison. It needs:
  * `third_party/vkd3d-msl`: vkd3d 1.19 + our patches, built with `-DVKD3D_SHADER_UNSUPPORTED_MSL` (configure flags
    as in `tools/build_vkd3d.sh`);
  * `third_party/dxil-spirv`: cmake build of github.com/HansKristian-Work/dxil-spirv with submodules;
  * `make spirv-cross`.
* `tools/d3d12mtl/dx12_bench.c`: `make dx12mtl-bench [DRAWS=n]`.
* Environment:
  * `WINTOMAC_D3D12_BACKEND=metal`: use the backend.
  * `WINTOMAC_D3D12_MSL_DUMP=<dir>`: write the generated MSL.
  * `WINTOMAC_D3D12_TRACE=1`: also logs PSO compile times and per-run draw statistics on this backend.
* Run HelloTriangle on Metal:
  `cd build/samples/HelloTriangle && WINTOMAC_D3D12_BACKEND=metal SDL_AUDIO_DRIVER=dummy ../../hellotriangle.bin D3D12HelloTriangle.exe`
  (after `make dx12-sample`).
