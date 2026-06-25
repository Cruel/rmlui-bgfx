# Shader Uniform Probe Sample

This sample is a focused diagnostic harness for `rmlui_bgfx::PrecompiledMaterialShaderProvider` and its standard material-shader ABI. Each case is a small RML document using `decorator: shader(...)` to validate the uniforms supplied by the reusable bgfx provider.

Run from the upstream RmlUi checkout root, just like the other sample binaries:

```bash
cd /path/to/RmlUi
/path/to/rmlui-bgfx/build/linux-samples/samples/rmlui_bgfx_sample_effects_probe 00
```

A GL3 reference twin is still built for renderer-shell comparison, but these cases are primarily for the bgfx material-provider ABI. Upstream GL3 does not implement these `abi_*` shader names by default.

```bash
cd /path/to/RmlUi
/path/to/rmlui-bgfx/build/linux-samples/samples/rmlui_bgfx_sample_effects_probe_gl3 00
```

Use `--help` to print the current case list.

The bgfx backend defaults to the correctness-focused reference render path. To force the bounded optimized render path for comparison, set:

```bash
RMLUI_BGFX_RENDER_PATH=optimized /path/to/rmlui-bgfx/build/linux-samples/samples/rmlui_bgfx_sample_effects_probe 00
```

Use `RMLUI_BGFX_RENDER_PATH=reference` to explicitly select the default correctness path. The aliases `gl3-compatible`, `gl3`, `compatible`, and `compat` are also accepted.

Reference diagnostics are enabled by default and print event-level information to stdout. Set `RMLUI_BGFX_FILTER_TRACE=0` to disable these diagnostics.

Cases:

- `00` tests `u_rmluiMaterialParams0.x` elapsed time animation.
- `01` tests `u_rmluiMaterialParams0.yz` decorator paint width and height.
- `02` tests `u_rmluiMaterialParams0.w` DPI/content scale.
- `03` tests `u_rmluiMaterialParams1.xy` mouse coordinates and `u_rmluiMaterialParams1.z` mouse validity.
- `04` tests time, dimensions, DPI/content scale, mouse coordinates, and mouse validity together.

Suggested workflow:

1. Run `00` first to confirm animated material uniform updates are reaching bgfx every frame.
2. Run `01` to confirm per-decorator paint dimensions are stored and submitted correctly.
3. Run `02` on displays or platforms with different content scales to validate DPI propagation.
4. Run `03` and move the mouse in and out of the window to validate host-driven pointer updates.
5. Run `04` as the broad material ABI integration check.
