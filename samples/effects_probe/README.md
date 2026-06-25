# Effects Probe Sample

This sample is a focused diagnostic harness for renderer effects and material-shader ABI behavior. Each case is a small RML document intended to isolate one renderer feature or one small interaction between features.

Run from the upstream RmlUi checkout root, just like the other sample binaries:

```bash
cd /path/to/RmlUi
/path/to/rmlui-bgfx/build/linux-samples/samples/rmlui_bgfx_sample_effects_probe 06
```

The bgfx backend defaults to the correctness-focused reference render path. To force the bounded optimized render path for comparison, set:

```bash
RMLUI_BGFX_RENDER_PATH=optimized /path/to/rmlui-bgfx/build/linux-samples/samples/rmlui_bgfx_sample_effects_probe 06
```

Use `RMLUI_BGFX_RENDER_PATH=reference` to explicitly select the default correctness path. Set `RMLUI_BGFX_FILTER_TRACE=1` to print filter/layer diagnostics while narrowing a failing case. Set it back to `0` for visual comparison.

Cases:

- `00` tests `u_rmluiMaterialParams0.x` elapsed time animation.
- `01` tests `u_rmluiMaterialParams0.yz` decorator paint width and height.
- `02` tests `u_rmluiMaterialParams0.w` DPI/content scale.
- `03` tests `u_rmluiMaterialParams1.xy` mouse coordinates and `u_rmluiMaterialParams1.z` mouse validity.
- `04` tests time, dimensions, DPI/content scale, mouse coordinates, and mouse validity together.
- `05` reproduces the top stack from the full effects sample: scrolling root, fixed header, transformed box-shadow, trail shadow, inset shadow, and a filtered wrapper.
- `06` isolates nested scrolling with no transforms or filters.
- `07` adds transforms only.
- `08` adds filters only.
- `09` adds box-shadow decorators only.
- `10` combines nested scrolling with transforms and box-shadow decorators.
- `11` combines nested scrolling with transforms and CSS filters.
- `12` combines nested scrolling, transforms, box-shadow decorators, and CSS filters.

Suggested workflow:

1. Run the same case with `RMLUI_BGFX_RENDER_PATH=reference` and `RMLUI_BGFX_RENDER_PATH=optimized`.
2. Start at `06` and move upward until optimized first diverges. That identifies the first operation combination that breaks bounded nested scrolling.
3. When optimized differs from reference, rerun that exact case with `RMLUI_BGFX_FILTER_TRACE=1` and capture the output.
