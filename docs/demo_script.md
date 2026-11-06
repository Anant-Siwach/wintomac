# Demo video script (about 2 minutes)

Record with QuickTime (File → New Screen Recording). Keep Activity Monitor visible in one shot to show
"Apple" (native) in the Kind column, not "Intel".

1. **Hook (10 s).** "This is a PS5 program running natively on a Mac. No emulator, no Rosetta."
   Show the PS5 homebrew drawing (once graphics lands), with Activity Monitor showing Kind = Apple.
2. **The pipeline (20 s).** Terminal: `tools/ps5_to_mac.sh tests/ps5/<demo> --name "PS5 Demo"` → `dist/PS5 Demo.app`.
   Then `open "dist/PS5 Demo.app"`.
3. **Windows games too (30 s).** `open dist/vkQuake.app`: play for a few seconds and open the console to show fps.
   Mention: Vulkan → MoltenVK → Metal, recompiled ahead of time.
4. **D3D12 on Metal (15 s).** The HelloTriangle sample with `WINTOMAC_D3D12_BACKEND=metal`, next to the vkd3d path.
5. **Under the hood (25 s).** Show a generated `recomp_*.c` function next to its x86, the `make check` summary
   (all pass, diffed against native builds and Rosetta), and the JIT numbers from `docs/performance.md`.
6. **Close (10 s).** "Open source, credits to AnyPS5, DXVK, vkd3d, MoltenVK and many more: link in the description."

Do not show any commercial game files or dumps on screen.
