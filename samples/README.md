# RmlUi sample backend overlay

The reusable renderer library intentionally does not fork RmlUi. A future sample harness should add a small `SDL_BGFX` backend layer on top of an upstream RmlUi checkout so the same RmlUi samples can be run against GL3 and bgfx for visual parity.

Expected overlay pieces:

- `RmlUi_Backend_SDL_BGFX.cpp/.h`, implementing the public sample backend API from upstream `Backends/RmlUi_Backend.h`.
- CMake glue that adds `SDL_BGFX` to the RmlUi backend option list without changing NovelTea.
- A shell integration branch for `RMLUI_RENDERER_BGFX`.
- A bgfx implementation of sample screenshot capture so `effects`, `transform`, `animation`, and other visual samples can be compared to GL3 output.

This belongs in this repository, not in NovelTea. NovelTea should consume only the packaged `rmlui_bgfx::rmlui_bgfx` target and provider interfaces.
