# Effects Probe Sample

This sample is a deliberately small diagnostic harness for the RmlUi effects renderer path. It exists so renderer bugs can be isolated one feature at a time instead of debugging the full upstream `Samples/basic/effects` document all at once.

Run from the upstream RmlUi checkout root, just like the other sample binaries:

```bash
cd /path/to/RmlUi
/path/to/rmlui-bgfx/build/linux-samples/samples/rmlui_bgfx_sample_effects_probe 00
```

Cases:

- `00` baseline rounded document, fixed header, scrolling.
- `01` body and child border-radius clipping.
- `02` box-shadow trail plus `filter: opacity(1)` no-op filter layer.
- `03` blurred and inset box shadows.
- `04` regular filter chain: blur, drop-shadow, color matrix, opacity.
- `05` backdrop-filter over a striped background.
- `06` mask-image with a gradient mask.
- `07` smaller scroll stress case with mixed effects.

Workflow:

1. Run `00`. Do not debug filters until baseline document clipping and scrolling are correct.
2. Run `01`. Fix clip masks and rounded overflow before touching box shadows.
3. Run `02` and `03`. Fix saved layer texture bounds and box-shadow generation.
4. Run `04`. Fix ordinary filter compositing.
5. Run `05` and `06`. Fix backdrop and mask-image paths.
6. Only then return to the full upstream effects sample.
