# PS5 graphics (AnyPS5 VideoOut / AGC on our Vulkan bridge)

Status (2026-11-06): a PS5 homebrew that opens video out, registers display buffers and flips them **renders
natively on Apple Silicon**: PS5 ELF -> AnyPS5 relinker `--windows` -> wintomac -> arm64; at run time AnyPS5's
`libSceVideoOut` hands each flip to `libSceAgcDriver`, which uploads the buffer and presents it through Vulkan ->
our bridge (`runtime/vulkan.c` + generated thunks) -> MoltenVK -> Metal. Frames read back on the GPU are
pixel-identical to what the program drew.

**GPU draws work too**: `tests/ps5/agc_draw.c` submits a raw AGC draw command buffer with RDNA2 vertex/pixel shader
machine code; AnyPS5 recompiles the shaders to SPIR-V and draws through our bridge and MoltenVK (frame hash
`5399ab95`, every sample point exact). Not covered yet: textures/samplers, depth, compute dispatches, real titles.

## Tests and demo

| program | what | how to run |
|---|---|---|
| `tests/ps5/gfx_flip.c` | 640x360 test pattern, 3 flips, checked pixel by pixel | `make ps5-draw` (headless, skips without the AnyPS5 libraries; `tools/ps5_gfx_check.sh`) |
| `tests/ps5/gfx_demo.c` | 1280x720 animated demo (gradients, bouncing boxes, text banner), ~60 fps | `dist/PS5Demo.app` via `tools/ps5_to_mac.sh` |
| `tests/ps5/agc_draw.c` | GPU work: raw AGC command buffer (draws: vertex buffers, textures + mips, null T#, depth; a compute dispatch) + RDNA2 shaders from `app0/agc_draw.bin` | `make ps5-draw PS5DRAW=agc_draw` |
| `tests/ps5/pad_input.c` | `scePadReadState` from keys typed into the window and from a virtual gamepad | `make ps5-draw PS5DRAW=pad_input` |

`make ps5-draw` (all three tests; `PS5DRAW=agc_draw` for one; `PS5_DRAW_NO_BUILD=1` reruns existing binaries) builds the ELF with the ps5-payload-sdk, relinks, recompiles with the AnyPS5 libraries it needs
(libSceVideoOut, libSceAgcDriver, ...; generated code at -O1 with `WINTOMAC_JOBS` (default 3) parallel compiles: a cold build after a lifter change takes 30-90 min on a busy machine, ~3 min per test with ccache), runs it with
`WINTOMAC_HEADLESS=1 SDL_AUDIO_DRIVER=dummy APS5_DUMP_FRAMES=3 APS5_DUMP_SCALE=1` and compares the
`frame_00N.bmp` files AnyPS5 reads back on the GPU with the expected pattern (`tests/ps5/check_frame.py`).
Expected hashes (FNV-1a of the BGRA pixels): `f877de05`, `05e632c5`, `e613e4c5` (the program prints the same
hashes for its CPU-side buffers).

Watch it in a window (log to a file):

    open dist/PS5Demo.app        # log: ~/Library/Logs/wintomac/PS5Demo.log
    # or: dist/PS5Demo.app/Contents/MacOS/PS5Demo > /tmp/ps5demo.log 2>&1
    # gfx_flip (after a rebuild with the current tests/ps5/gfx_flip.c):
    #   cd build/ps5/gfx_flip && GFX_FLIP_SECONDS=10 SDL_AUDIO_DRIVER=dummy ../../ps5_gfx_flip.bin gfx_flip.exe > gfx_flip.log 2>&1

The demo app runs until its window is closed (or Esc / Cmd-Q); `DEMO_SECONDS=n` stops it after n seconds,
`DEMO_DUMP=1` prints hashes of frames 0/60/120.

Useful AnyPS5 switches: `APS5_DUMP_FRAMES=n` (+`APS5_DUMP_SCALE=1` for full size) dumps the first n presented frames
as `frame_<i>.bmp` in the working directory; `APS5_TRACE_GPU`, `APS5_PROFILE_DRAW`; ours: `WINTOMAC_VK_TRACE=1`.

## GPU draws (AGC command buffers and PS5 shaders)

`tests/ps5/agc_draw.c` does what a title's renderer does at the lowest level: it submits a command buffer with
`sceAgcDriverSubmitDcb`, waits for a label the GPU writes, flips and prints hashes of buffers it is told to check.
Everything GPU-side is data: `app0/agc_draw.bin` (format `AGC2`: DCB, a data area with shader code, textures and
vertex buffers plus zero-filled depth/compute buffers, and relocations that patch addresses into both) is written by
`tests/ps5/gen_agc_draw.py`, which also writes `agc_draw.expect.json` (sample points, compute hash) for
`tests/ps5/check_agc_frame.py`. New cases need no rebuild: edit the generator, rerun it and the binary.

No Sony SDK or shader compiler is involved. PM4 packets (SET_UCONFIG_REG / SET_CONTEXT_REG / SET_SH_REG /
DRAW_INDEX_AUTO / DISPATCH_DIRECT / WRITE_DATA) and register values follow AnyPS5's driver tests and register decoders;
the shaders are RDNA2 (gfx10.3) machine code, assembled with `llvm-mc -arch=amdgcn -mcpu=gfx1030 -show-encoding`
(Homebrew LLVM; the generator re-assembles and compares when llvm-mc is installed). AnyPS5 recompiles them to SPIR-V.

The scene (640x360, every point checked exactly or within 2/255): full-screen background with depth ALWAYS;
a vertex-buffer quad with per-vertex colours interpolated; a vertex-buffer quad sampling a 64x64 RGBA8 checker texture
(`image_sample_lz`, point sampler, UVs interpolated); four rects sampling a 4-level mip chain with `image_sample_l`
LOD 0..3; a rect sampling an all-zero T# (null descriptor: black, as on hardware); pixel-stage and vertex-stage buffer
reads; vertex-stage user SGPRs passed on as an attribute; a Z32 depth buffer with two overlapping rects and LESS; and a
compute dispatch (4 x 64 threads, `out[i] = i*i + 0x1234`) whose 1 KiB result the CPU hashes (`646a21f4`).
Frame hash `effe7352`. The first version (3 constant-colour draws) rendered correctly on the first try (`5399ab95`).

ABI facts learned the hard way (both silent: the draw just reads zeros):
- AnyPS5's vertex stage follows the gfx10 merged ES/GS ABI: user SGPRs start at **s8** (s0-s7 are system SGPRs), the
  vertex index is in v5. Pixel and compute shaders get user SGPRs from s0 (compute: the group id follows them).
- T# mip count: LAST_LEVEL in word 3 and **MAX_MIP in word 5 bits 4-7** (`DecodeTextureResource`). Linear mip chains
  are stored smallest level first, rows padded to 256 bytes (`ComputeLinearMipLayout`).

Nothing in our Vulkan bridge had to be emulated for any of this: AnyPS5 checks MoltenVK's features (no
`nullDescriptor` -> placeholder images, no graphics pipeline library -> monolithic pipelines with one 128-byte push
block shared by the vertex and pixel stages). `WINTOMAC_VK_TRACE=1` now also logs pipeline layouts, push constants
(full contents) and pipeline shader stages with their specialization constants, which is how the above was found.

## What had to be fixed

1. **macOS address space: 0xFE0000000 is unmappable.** arm64 macOS reserves `[0xFC0000000, 0x7000000000)` in every
   process (`mmap` fails with EACCES, max protection none; even `munmap` + `MAP_FIXED` cannot take it). AnyPS5 v0.1.4's
   AGC driver commits a fixed 2 MiB "driver memory" block at `0xFE0000000` (inside the PS5 system-reserved range; it
   holds a stub shader and descriptors), so `sceVideoOutOpen` failed. `tools/ps5_mac_patch.py` rewrites the 18
   `movabs` immediates in `libSceAgcDriver.prx` to `0xFB0000000` (still system-reserved for the PS5, mappable on
   macOS); `tools/ps5_to_mac.sh` and `make ps5-draw` apply it. **Upstream suggestion for AnyPS5:** make the driver-memory
   address configurable (or pick it inside the arena at run time) so macOS hosts (Mach-O/Rosetta builds and ours) need
   no patching. Any title that maps fixed addresses in that range will need address translation on macOS.
2. **Windows created off the main thread.** AnyPS5's VideoOut creates its (SDL2, Win32) window on its presenter
   thread; Cocoa only allows windows on the main thread, so `user32.c` made no native window and
   `vkCreateWin32SurfaceKHR` failed. `runtime/ps5_gfx.m` gives such a window a detached `CAMetalLayer` (any thread may
   create one); MoltenVK renders and presents into it normally. When not headless the layer is put into an `NSWindow`
   by a block queued on the main dispatch queue, run the next time the main thread waits in a Win32 wait/sleep shim
   (`gfx_main_service` in `Sleep`, `SleepEx`, `WaitForSingleObject`, `NtDelayExecution`), which also pumps that
   window's events. Closing the window or Cmd-Q quits (Esc is the PS Options button in AnyPS5's mapping). Input: see below.
3. **`param.json`**: AnyPS5's libkernel throws if `app0/sce_sys/param.json` is missing (VideoOut asks for the title);
   the tests use `tests/ps5/app0/sce_sys/param.json`, `tools/ps5_to_mac.sh` writes one when the input has none.
   (Exceptions thrown inside the AnyPS5 libraries currently crash during unwinding; a separate fix is in progress.)
4. **`maskmovdqu`** (used by AnyPS5's upload path) was missing from the interpreter (`runtime/interp_sse.c`).

## Input in the window

AnyPS5's pad emulation (`scePad`) reads game controllers through SDL2 and maps keyboard/mouse (Return/Space = Cross,
Esc = Options, arrows = D-pad, WASD = left stick, ...; `anyps5-input.ini`, see AnyPS5's docs/user/INPUT_MAPPING.md)
from its window's SDL2 events, which come from Win32 messages.

- Keyboard, mouse, wheel and focus of the detached window: `ps5_gfx.m` turns the NSWindow's events into
  `user32_detached_key/mouse/focus` (`user32.c`), which post the same `WM_KEY*` (exact set-1 scancodes, shared with
  the SDL path: `deliver_key`), `WM_CHAR`, `WM_MOUSEMOVE`/buttons/`WM_MOUSEWHEEL` and `WM_SETFOCUS` messages an SDL
  window gets. Mouse positions are scaled to the window's client size. Showing a window without an SDL window now
  also sends `WM_SHOWWINDOW`/`WM_ACTIVATE`/`WM_SETFOCUS` and gives it the focus, as for SDL windows.
- Game controllers: SDL2 inside AnyPS5 uses XInput, which is our `xinput.c` on SDL3 gamepads, so they work whether
  or not the window exists.
- Headless check (`make ps5-draw PS5DRAW=pad_input`, `tests/ps5/pad_input.c`): `WINTOMAC_TEST_DETACHED_KEYS=1` types
  Return, Up and W into the window (Cross, D-pad up, left stick up must appear in `scePadReadState`), and
  `WINTOMAC_VIRTUAL_PAD=1` attaches a scripted SDL virtual gamepad. Result: Return -> `buttons=004000` (Cross),
  Up -> `000010`, W -> left stick `128,0`; the virtual pad -> `buttons=004810` (Cross|R1|Up), left stick `190,65`,
  right stick up. Its right trigger (held from the moment the pad is attached) reads 0: SDL takes trigger baselines
  when a pad is opened, so a trigger already pressed then is "rest"; a real trigger pressed later works.
- In a visible window, keyboard/mouse events come from AppKit (`ps5_gfx.m`: macOS key codes -> SDL scancodes ->
  the same path); that part cannot be checked headless.

## MoltenVK gaps

AnyPS5's `libSceAgcDriver` queries every optional feature before enabling it, so on MoltenVK 1.4 its device
creation succeeds unchanged: no mesh shaders, no `VK_KHR_draw_indirect_count`, no 64-bit image atomics, no
`nullDescriptor` (it falls back to placeholder images), no geometry shaders. Hard requirements it checks and MoltenVK
meets: `VK_KHR_shader_float_controls` with `shaderSignedZeroInfNanPreserveFloat32`, buffer device address,
8-bit storage, vertex/fragment stores and atomics, sampler anisotropy, BC textures.

Hardening in `runtime/vulkan.c` (`vkCreateDevice`) for layers that do not check: device extensions the host lacks are
dropped (logged), and the known extension feature structs in the `pNext` chain (Vulkan 1.1/1.2/1.3, robustness2,
descriptor indexing, mesh shader, int64 atomics, image int64 atomics, min LOD, maintenance8, ...) are ANDed with what
MoltenVK reports, like the core-feature clamp; the guest's structs are restored afterwards.
Not emulated (not needed so far): null descriptors (dummy 1x1 resources), `drawIndirectCount` (clamp to maxCount).

## Next for real titles

- Extend `gen_agc_draw.py`: vertex buffers / interpolated attributes, textures + samplers (T#/S# descriptors: this is
  where MoltenVK's missing `nullDescriptor` matters; AnyPS5 then uses placeholder images), depth, compute (ACB).
- Input to the detached window (keyboard/mouse -> AnyPS5's SDL2 window messages, pads via SDL3).
- Exception unwinding in the AnyPS5 libraries (in progress elsewhere).
- Fixed guest mappings in `[0xFC0000000, 0x7000000000)` for titles.
