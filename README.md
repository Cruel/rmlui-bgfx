# rmlui-bgfx

Reusable [RmlUi](https://github.com/mikke89/RmlUi) render interface implementation backed by [bgfx](https://github.com/bkaradzic/bgfx).

The library owns only generic RmlUi/bgfx rendering code. Applications provide platform policy through small interfaces:

- `rmlui_bgfx::ShaderProvider` loads the packaged bgfx programs required by the renderer.
- `rmlui_bgfx::TextureLoader` decodes textures into RGBA8 pixels.
- `rmlui_bgfx::Diagnostics` and `rmlui_bgfx::PerfLogger` receive warnings and optional performance lines.
- `rmlui_bgfx::MaterialShaderProvider` is an optional extension point for application-owned custom decorator shaders.

The renderer core does not depend on SDL, Lua, ImGui, any application asset manager, or an application-specific shader registry.

## Build and test the renderer

```sh
cmake --preset linux-debug
cmake --build --preset linux-debug
ctest --preset linux-debug
```

Consumers should link against:

```cmake
target_link_libraries(my_app PRIVATE rmlui_bgfx::rmlui_bgfx)
```

## Shader sources

The reusable renderer's bgfx shader sources live under `shaders/bgfx`. CMake helper functions in `cmake/RmlUiBgfxShaders.cmake` can compile or stage the required programs into an application's runtime shader tree.

RmlUi's `decorator: shader("name")` syntax passes a shader name to the render interface, not runtime shader source. For bgfx, those names must resolve to precompiled programs. Applications can implement `MaterialShaderProvider` directly, or use `rmlui_bgfx::PrecompiledMaterialShaderProvider`:

```cpp
#include <rmlui_bgfx/precompiled_material_shader_provider.hpp>

rmlui_bgfx::PrecompiledMaterialShaderProviderConfig material_config;
material_config.root_directory = "assets/shaders/bgfx/glsl-120";

rmlui_bgfx::PrecompiledMaterialShaderProvider material_shaders(material_config);
material_shaders.register_shader("panel_noise", "material_panel_noise");

rmlui_bgfx::RendererConfig config;
config.material_shaders = &material_shaders;
```

With that configuration, `decorator: shader("panel_noise")` loads `material_panel_noise.vs.bin` and `material_panel_noise.fs.bin` from the configured directory and submits them with the same geometry, transform, scissor, texture, blend, and stencil state used by normal RmlUi decorator rendering.

The provider defines a small standard material ABI for precompiled bgfx decorator shaders:

```glsl
uniform vec4 u_rmluiMaterialParams0;
// x = elapsed time in seconds
// y = decorator paint width in RmlUi logical pixels
// z = decorator paint height in RmlUi logical pixels
// w = DPI/content scale

uniform vec4 u_rmluiMaterialParams1;
// x = mouse x in RmlUi logical coordinates, or -1 when unavailable
// y = mouse y in RmlUi logical coordinates, or -1 when unavailable
// z = 1 when the mouse position is valid, otherwise 0
// w = reserved
```

Mouse input remains application-owned. SDL, Android, Web, or an engine integration can update it without coupling the reusable renderer to any platform layer:

```cpp
material_shaders.set_mouse_position({mouse_x, mouse_y});
material_shaders.clear_mouse_position();
```

## Build the upstream RmlUi samples with bgfx

The `samples/` directory contains an SDL3/bgfx backend overlay for upstream RmlUi samples. This is useful for testing renderer behavior against RmlUi's own sample suite without forking RmlUi or embedding an application-specific runtime.

The sample build needs an upstream RmlUi source checkout. Pass it with `RMLUI_BGFX_RMLUI_SOURCE_DIR` unless your local checkout layout already matches the preset default.

```sh
cmake --preset linux-samples \
  -DRMLUI_BGFX_RMLUI_SOURCE_DIR=/path/to/RmlUi
cmake --build --preset samples-all
```

The build creates sample executables under:

```text
build/linux-samples/samples/
```

Examples include:

```text
build/linux-samples/samples/basic/demo/rmlui_bgfx_sample_demo
build/linux-samples/samples/basic/animation/rmlui_bgfx_sample_animation
build/linux-samples/samples/basic/transform/rmlui_bgfx_sample_transform
build/linux-samples/samples/basic/svg/rmlui_bgfx_sample_svg
build/linux-samples/samples/basic/lottie/rmlui_bgfx_sample_lottie
build/linux-samples/samples/invaders/rmlui_bgfx_sample_invaders
build/linux-samples/samples/lua_invaders/rmlui_bgfx_sample_lua_invaders
```

Run samples from the upstream RmlUi source root so their relative asset paths resolve correctly:

```sh
cd /path/to/RmlUi
/path/to/rmlui-bgfx/build/linux-samples/samples/basic/demo/rmlui_bgfx_sample_demo
```

For a quick interactive smoke test, use a timeout. Exit code `124` is expected because the sample remains open until the timeout kills it.

```sh
cd /path/to/RmlUi
timeout 4s /path/to/rmlui-bgfx/build/linux-samples/samples/basic/demo/rmlui_bgfx_sample_demo
```

The `linux-samples` preset builds the sample backend with SDL3 and uses bgfx's OpenGL renderer on Linux. Visual parity should still be checked manually against upstream GL3 samples.
