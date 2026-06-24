# rmlui-bgfx

Reusable [RmlUi](https://github.com/mikke89/RmlUi) render interface implementation backed by [bgfx](https://github.com/bkaradzic/bgfx).

The library owns only generic RmlUi/bgfx rendering code. Applications provide platform policy through small interfaces:

- `rmlui_bgfx::ShaderProvider` loads the packaged bgfx programs required by the renderer.
- `rmlui_bgfx::TextureLoader` decodes textures into RGBA8 pixels.
- `rmlui_bgfx::Diagnostics` and `rmlui_bgfx::PerfLogger` receive warnings and optional performance lines.
- `rmlui_bgfx::MaterialShaderProvider` is an optional extension point for application-owned custom decorator shaders.

The renderer does not depend on NovelTea, SDL, Lua, ImGui, an asset manager, or an application-specific shader registry.

## Build

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

## RmlUi samples

The `samples/` directory documents the intended upstream RmlUi sample-backend overlay. It is deliberately separate from NovelTea so this renderer can be validated against RmlUi's own samples without making NovelTea part of the renderer package.
