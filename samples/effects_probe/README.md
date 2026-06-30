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

Phase 8's experimental bounded transformed-layer path is disabled by default. To compare it against the default optimized transform fallback, run the same probe with `RMLUI_BGFX_BOUNDED_TRANSFORM_LAYERS=1 RMLUI_BGFX_RENDER_PATH=optimized`.

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
- `13` starts a stripped-down Effects sample layout with a fixed header, document scrolling,
  left/center/right rows, and explicit top/bottom markers.
- `14` adds the full-sample style no-op outer filter chain around the scrolling content.
- `15` adds transformed rows inside the no-op filtered wrapper.
- `16` adds the full-sample shadow overflow patterns inside the no-op filtered wrapper.
- `17` adds per-item real color/blur/drop-shadow filters while preserving scroll markers.
- `18` applies a real outer blur to the full scrolling wrapper.
- `19` applies a real outer drop-shadow to the full scrolling wrapper.
- `20` adds an absolute backdrop-filter window over the scrolling filtered wrapper.
- `21` uses large mid/lower/bottom markers to make scroll reveal failures obvious.
- `22` is the transformed-shadow scrolling control without the outer wrapper filter.
- `23` combines the stripped full Effects slice: no-op wrapper filter, transformed shadow,
  shadow overflow, real per-item filters, a large placeholder, and backdrop-filter.
- `24` isolates the full-sample rotated blur box-shadow inside the no-op filtered wrapper.
- `25` is the matching rotated blur box-shadow control without the outer wrapper filter.
- `26` isolates the full-sample trail box-shadow inside the no-op filtered wrapper.
- `27` is the matching trail box-shadow control without the outer wrapper filter.
- `28` isolates the full-sample blur box-shadow without transform inside the no-op filtered wrapper.
- `29` isolates a single inset box-shadow without outer shadows.
- `30` isolates a single inset box-shadow without blur.
- `31` mirrors the NovelTea readback-gallery effects stress scene for GL3, bgfx reference, and
  bgfx optimized comparison before readback expectations are captured.
- `32` duplicates the full NovelTea readback gallery document so compounded scene interactions can
  be compared in GL3, bgfx reference, and bgfx optimized without changing the readback harness.

Suggested workflow:

1. Run the same case with `RMLUI_BGFX_RENDER_PATH=reference` and `RMLUI_BGFX_RENDER_PATH=optimized`.
2. Start at `06` and move upward until optimized first diverges. That identifies the first operation combination that breaks bounded nested scrolling.
3. For the full Effects sample failure, start at `13` and move upward. The first case where
   optimized stops revealing rows or markers while scrolling identifies the missing interaction.
4. When optimized differs from reference, rerun that exact case with `RMLUI_BGFX_FILTER_TRACE=1`
   and capture the output.
