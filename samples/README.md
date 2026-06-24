# RmlUi bgfx samples

This directory contains the SDL3/bgfx sample backend overlay used to build upstream RmlUi samples against `rmlui-bgfx`.

The overlay does not fork RmlUi. Configure `rmlui-bgfx` with `RMLUI_BGFX_BUILD_SAMPLES=ON` and point `RMLUI_BGFX_RMLUI_SOURCE_DIR` at an upstream RmlUi source checkout.

```sh
cmake --preset linux-samples \
  -DRMLUI_BGFX_RMLUI_SOURCE_DIR=/path/to/RmlUi
cmake --build --preset samples-all
```

The `samples-all` target compiles the reusable renderer, generated bgfx shader binaries, the SDL3/bgfx sample backend, upstream RmlUi libraries, and the upstream sample executables under `build/linux-samples/samples`.

Run samples from the upstream RmlUi source root so relative sample assets resolve correctly:

```sh
cd /path/to/RmlUi
/path/to/rmlui-bgfx/build/linux-samples/samples/basic/demo/rmlui_bgfx_sample_demo
```

For smoke testing interactive samples, a timeout exit is expected:

```sh
cd /path/to/RmlUi
timeout 4s /path/to/rmlui-bgfx/build/linux-samples/samples/basic/demo/rmlui_bgfx_sample_demo
```

Notes:

- The current backend uses SDL3 for window/input/system integration and bgfx with the OpenGL renderer on Linux.
- Texture loading is implemented through stb_image with static symbols to avoid collisions with static `rlottie` builds.
- The upstream tutorials keep their original executable names (`rmlui_tutorial_drag`, `rmlui_tutorial_template`) inside this build tree.
- This sample overlay is intended for renderer parity testing against upstream GL3 samples; it is not a replacement for an application's runtime integration.
