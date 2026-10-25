# Third-party tool patches

The third-party tools are git submodules of this repository, pinned to the upstream commits
below. `setup.bat` fetches them and applies these patches automatically.

| Tool | Upstream | Commit | Patch |
|---|---|---|---|
| decaf-emu (GX2, libgpu, snd_core; GPL-3.0) | https://github.com/decaf-emu/decaf-emu | `e6c528a20a41c34e0f9eb91dd3da40f119db2dee` | `decaf-emu.patch` |
| XenonRecomp (PPC → C++ recompiler) | https://github.com/hedge-dev/XenonRecomp | `ddd128bcca99fe8bfbb99bea583c972351fa6ace` | `XenonRecomp.patch` |
| Vulkan-Headers | https://github.com/KhronosGroup/Vulkan-Headers | tag `v1.3.268` | none |

To apply them by hand:

```bat
git submodule update --init decompile/tools/decaf-emu decompile/tools/XenonRecomp decompile/tools/Vulkan-Headers
git -C decompile/tools/decaf-emu apply --ignore-whitespace ../../patches/decaf-emu.patch
git -C decompile/tools/XenonRecomp apply --ignore-whitespace ../../patches/XenonRecomp.patch
```

After changing a tool, regenerate its patch, e.g.
`git -C decompile/tools/decaf-emu diff > decompile/patches/decaf-emu.patch`.

What the patches do:
- **XenonRecomp**: Wii U (PPC32 / Espresso) support - paired-single instructions, `lmw`/`stmw`,
  Green Hills jump tables, Cafe OS imports, volatile `lwarx`, scalar single-precision operations
  also writing ps1. Details (in French) in `recomp/README.md`.
- **decaf-emu**: builds as libraries inside the recompilation (`DECAF_RECOMP`), per-thread GX2
  display lists and locked cache, R5G5B5A1/R4G4B4A4 render targets, read-back of linear surfaces
  rendered by the GPU, surface aliasing fixes, swapchain recreation on resize, snd_core frame
  stepping driven by the runtime. Each fix is described in `recomp/README.md`.
