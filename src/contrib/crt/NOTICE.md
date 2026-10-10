# CRT filter — third-party components

The optional CRT presentation filter ([wcw crt], `src/render/rt64_crt_filter.*`) runs RetroArch
slang shader presets through librashader. Each component keeps its own license; nothing here
changes the license of RT64 itself.

| Component | Location | License | Source |
|---|---|---|---|
| librashader 0.12.0 (prebuilt `librashader.dll`, unmodified) | `contrib/librashader/bin/x64/` | MPL-2.0 (`contrib/librashader/LICENSE.md`) | https://github.com/SnowflakePowered/librashader/tree/librashader-v0.12.0 |
| librashader C headers (`librashader.h`, `librashader_ld.h`) | `contrib/librashader/include/` | MIT (see file headers) | same |
| crt-guest-advanced / crt-guest-advanced-ntsc (shaders + LUTs) | `contrib/crt/slang-shaders/crt/` | GPL-2.0-or-later (see file headers) — guest(r), Dr. Venom | https://github.com/libretro/slang-shaders (commit in `slang-shaders/UPSTREAM.txt`) |
| CRT Beam Simulator | `contrib/crt/slang-shaders/subframe-bfi/` | MIT — Mark Rejhon (Blur Busters) & Timothy Lottes | https://github.com/blurbusters/crt-beam-simulator |
| AKI presets (`contrib/crt/presets/*.slangp`) | `contrib/crt/presets/` | MIT (RT64 fork) | — |

The shader files are distributed in source form. librashader's corresponding source is the tag above.
