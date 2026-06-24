# Effects Probe Sample

This sample is a deliberately small diagnostic harness for the RmlUi effects renderer path. It exists so renderer bugs can be isolated one feature at a time instead of debugging the full upstream `Samples/basic/effects` document all at once.

Run from the upstream RmlUi checkout root, just like the other sample binaries:

```bash
cd /path/to/RmlUi
/path/to/rmlui-bgfx/build/linux-samples/samples/rmlui_bgfx_sample_effects_probe 00
```

A GL3 reference twin is also built when the upstream `rmlui_backend_SDL_GL3` target is available:

```bash
cd /path/to/RmlUi
/path/to/rmlui-bgfx/build/linux-samples/samples/rmlui_bgfx_sample_effects_probe_gl3 00
```

Use the same case number with both binaries to compare the bgfx backend against RmlUi's official GL3 renderer.

The bgfx backend defaults to its GL3-compatible filtered-layer composite path. To force the older bounded optimized filtered-layer path for comparison, set:

```bash
RMLUI_BGFX_FILTER_LAYER_COMPOSITE=optimized /path/to/rmlui-bgfx/build/linux-samples/samples/rmlui_bgfx_sample_effects_probe 13
```

Set `RMLUI_BGFX_FILTER_TRACE=1` to print filter rectangles, texture handles, blur bounds, and output bounds. Set `RMLUI_BGFX_BLUR_SAMPLE_BOUNDS=full` to force blur sampling against the full postprocess texture instead of the calculated source bounds.

Cases:

- `00` baseline rounded document, fixed header, scrolling.
- `01` body and child border-radius clipping.
- `02` box-shadow trail plus `filter: opacity(1)` no-op filter layer.
- `03` blurred and inset box shadows.
- `04` regular filter chain: blur, drop-shadow, color matrix, opacity.
- `05` backdrop-filter over a striped background.
- `06` mask-image with a gradient mask.
- `07` smaller scroll stress case with mixed effects.
- `08` direct `RenderInterface` `SetInverse` clip-mask probe, bypassing RML/CSS and box-shadow generation.
- `09` `RenderManager` callback texture probe using `PushLayer`, `SetInverse`, and `SaveLayerAsTexture` without CSS box-shadow generation.
- `10` rounded `RenderBox` shadow-mesh callback texture probe using `MeshUtilities::GenerateBackground`, still without CSS box-shadow generation.
- `11` CSS box-shadow outer-only probe. This is the same outer shadow stack as `02`, but without the inset blurred shadow.
- `12` CSS inset box-shadow probe. This isolates the subtle inner glow/inset-shadow difference still visible in `02`.
- `13` manual `RenderManager` inset blur callback texture probe. This recreates the inset blur layer sequence without CSS box-shadow resolution.
- `14` manual `RenderManager` inset callback texture probe without blur. This verifies the inset clip/source layer before filter compositing is introduced.
- `15` manual `RenderManager` inset child-layer composite without blur. This verifies child-layer compositing before blur filtering is introduced.
- `16` manual `RenderManager` inset child-layer blur without clip masks. This verifies the blur pipeline before inset clipping is introduced.
- `17` manual `RenderManager` inset child-layer blur with the inverse clip only. This verifies the source-side inset clip before the final rounded clip is introduced.

Workflow:

1. Run `00`. Do not debug filters until baseline document clipping and scrolling are correct.
2. Run `01`. Fix clip masks and rounded overflow before touching box shadows.
3. Run `08` if `02` has missing shadows. This determines whether the low-level inverse stencil operation works independently of RmlUi's box-shadow callback texture generation.
4. Run `09` if `08` passes. This keeps the low-level mask but adds callback texture generation and `SaveLayerAsTexture`.
5. Run `10` if `09` passes. This adds rounded `RenderBox` meshes similar to `GeometryBoxShadow` while still bypassing CSS resolution.
6. Run `11` if `10` passes but `02` fails. This determines whether CSS box-shadow works without the inset blur entry.
7. Run `12` if `11` passes but `02` still lacks the inner glow. This isolates inset box-shadow rendering.
8. Run `13` if `12` differs from GL3. This removes CSS resolution and exercises the same inset clip, blur, composite, and save-texture sequence directly.
9. Run `14` if `13` has no visible inset. This distinguishes direct source inset clipping from child-layer/filter compositing.
10. Run `15` if `14` passes. This distinguishes child-layer compositing from blur filtering.
11. Run `16` if `15` passes but `13` has no visible inset. This verifies blur without clip masks.
12. Run `17` if `16` passes. This verifies the inverse inset clip without the final rounded clip.
13. Run `02` and `03`. Fix saved layer texture bounds and box-shadow generation.
14. Run `04`. Fix ordinary filter compositing.
5. Run `05` and `06`. Fix backdrop and mask-image paths.
6. Only then return to the full upstream effects sample.
