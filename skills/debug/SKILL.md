---
name: rmlui-bgfx-debug
description: >-
  Use this skill when diagnosing rmlui-bgfx renderer defects, especially optimized-path rendering
  bugs such as missing elements, flicker, clipping, distorted shadows, bad masks, wrong scroll
  segments, feedback-loop failures, pass exhaustion, or target/layer lifetime problems. It covers
  the project trace workflow, FPS throttling, effects probe comparison, good/bad frame narrowing,
  grep patterns, and renderer contracts to verify before making behavior changes.
---

# Debugging Skill

Use this skill when diagnosing `rmlui-bgfx` renderer defects, especially issues that only appear in
the optimized path: missing elements, flicker, clipping, distorted shadows, bad masks, wrong scroll
segments, feedback-loop failures, pass exhaustion, or target/layer lifetime bugs.

This is an evolving project-local skill. Update it whenever a better diagnostic workflow is proven.

## Core rule

Do not jump from a visual symptom directly to a rendering fix. First identify the exact pipeline
boundary where a good frame and bad frame diverge.

Prefer this sequence:

1. Reproduce the defect in the smallest useful probe.
2. Compare optimized output against GL3/reference output.
3. Capture a good frame and adjacent bad frame with trace output.
4. Locate the first divergent contract: source bounds, materialized layer bounds, filter work bounds,
   target allocation/reuse, destination rect, clip/stencil state, pass acquisition, or frame failure.
5. Make the smallest contract fix that explains the divergence.
6. Retest the minimal probe and the full probe that originally failed.

Avoid cycling between alternate symptoms by keeping confirmed fixes separate from experiments. If an
experiment does not improve the observed defect, revert it before continuing.

## Current local layout

Common paths:

```sh
/home/thomas/dev/nt/rmlui-bgfx
/home/thomas/dev/nt/refs/RmlUi
/home/thomas/dev/nt/refs/RmlUi/Backends/RmlUi_Renderer_GL3.cpp
```

Run sample binaries from the upstream RmlUi checkout so upstream sample assets resolve:

```sh
cd /home/thomas/dev/nt/refs/RmlUi
../../rmlui-bgfx/build/linux-samples/samples/rmlui_bgfx_sample_effects_probe 33
../../rmlui-bgfx/build/linux-samples/samples/rmlui_bgfx_sample_effects_probe_gl3 33
```

## Build and sanity checks

Preferred focused build:

```sh
cd /home/thomas/dev/nt/rmlui-bgfx
ninja -C build/linux-samples rmlui_bgfx_sample_effects_probe
```

Full sample build when CMake or shared backend code changed:

```sh
ninja -C build/linux-samples
```

Whitespace/check-only validation:

```sh
git diff --check
```

Unit tests when library contracts change:

```sh
ctest --test-dir build/linux-debug --output-on-failure
```

Do not stage files unless explicitly asked.

## Visual comparison workflow

Use the same probe case in all paths:

```sh
cd /home/thomas/dev/nt/refs/RmlUi

RMLUI_BGFX_RENDER_PATH=reference \
../../rmlui-bgfx/build/linux-samples/samples/rmlui_bgfx_sample_effects_probe 33

RMLUI_BGFX_RENDER_PATH=optimized \
RMLUI_BGFX_BOUNDED_TRANSFORM_LAYERS=1 \
../../rmlui-bgfx/build/linux-samples/samples/rmlui_bgfx_sample_effects_probe 33

../../rmlui-bgfx/build/linux-samples/samples/rmlui_bgfx_sample_effects_probe_gl3 33
```

Do not claim visual correctness unless the user or a manual visual check confirms it. A successful
build only proves the code compiles.

## Trace suite workflow

The renderer has a project-local trace suite controlled by sample environment variables. Start broad
enough to catch the failure, then narrow with frame and operation filters.

Useful aliases:

```sh
RMLUI_BGFX_TRACE=effects
RMLUI_BGFX_TRACE=scroll
RMLUI_BGFX_TRACE=targets
RMLUI_BGFX_TRACE=saved
```

Useful explicit categories:

```sh
RMLUI_BGFX_TRACE=layer,filter,mask,texture,copy,composite,failure,fallback
RMLUI_BGFX_TRACE=pass,target,copy,composite,failure
RMLUI_BGFX_TRACE=clip,stencil,layer,composite,failure
```

Useful filters:

```sh
RMLUI_BGFX_TRACE_FRAME=120-135
RMLUI_BGFX_TRACE_EVERY=10
RMLUI_BGFX_TRACE_FIRST=5
RMLUI_BGFX_TRACE_OP=CompositeLayers
RMLUI_BGFX_TRACE_REASON=empty
RMLUI_BGFX_TRACE_RING=500
RMLUI_BGFX_TRACE_ON_FAILURE=1
```

Slow down fast samples when logging every frame:

```sh
RMLUI_BGFX_SAMPLE_FPS_LIMIT=5
```

Show the trace frame number and measured sample FPS on-screen so manual visual reports line up with
log frame numbers:

```sh
RMLUI_BGFX_DEBUG_OVERLAY=1
```

Typical current-defect command:

```sh
cd /home/thomas/dev/nt/refs/RmlUi

RMLUI_BGFX_RENDER_PATH=optimized \
RMLUI_BGFX_DEBUG_OVERLAY=1 \
RMLUI_BGFX_TRACE=effects \
RMLUI_BGFX_SAMPLE_FPS_LIMIT=5 \
../../rmlui-bgfx/build/linux-samples/samples/rmlui_bgfx_sample_effects_probe 33
```

After identifying a good frame and bad frame, rerun tightly:

```sh
RMLUI_BGFX_RENDER_PATH=optimized \
RMLUI_BGFX_DEBUG_OVERLAY=1 \
RMLUI_BGFX_TRACE=layer,filter,mask,composite,failure \
RMLUI_BGFX_TRACE_FRAME=<good>-<bad> \
RMLUI_BGFX_SAMPLE_FPS_LIMIT=5 \
../../rmlui-bgfx/build/linux-samples/samples/rmlui_bgfx_sample_effects_probe 33
```

Manual-report workflow:

1. Run with `RMLUI_BGFX_DEBUG_OVERLAY=1` and a low FPS limit, usually `2` to `5`.
2. When the visual defect appears, note the on-screen `frame=` value.
3. Report a small frame window around it, for example `410-415`.
4. Inspect only that frame range in the trace log.

`RMLUI_BGFX_BOUNDED_TRANSFORM_LAYERS` is enabled by default in the optimized path. Setting it to
`0` forces the old unbounded transform fallback, which is useful for A/B comparisons but is known to
clip the gold element in probe `33`.

## Trace line interpretation

Trace lines are structured for grep:

```text
[rmlui-bgfx][trace][frame=123][path=optimized][cat=layer][stage=materialize][op=MaterializeLayer] ...
```

Important fields:

- `source_required`: global framebuffer rect requested for source materialization.
- `source_valid_global`: global framebuffer rect considered valid source pixels.
- `filter_source_bounds`: semantic source bounds passed to filter expansion/allocation.
- `output_bounds`: global framebuffer bounds of filter output.
- `valid_output`: tracked valid output/ink bounds.
- `dst_local`: destination target-local composite rect.
- `mask_bounds`: saved mask semantic bounds.
- `destination`: target-local destination rect for copy/composite.
- `generation`: target generation, useful for stale mask/target lookup bugs.
- `full_frame`: whether a target/fallback is using full-frame semantics.

Compare good and bad frames in this order:

1. Frame/surface size and scale.
2. Pass acquisition or pass exhaustion.
3. Target allocation/reuse/generation.
4. Source layer materialization bounds.
5. Source valid bounds.
6. Filter work bounds and output validity.
7. Destination local rect and destination scissor.
8. Clip/stencil replay state.
9. Frame failure or early-return reason.

The first place that becomes empty, invalid, stale, or unexpectedly full-frame is usually the bug.

## Grep patterns

Use stable trace prefixes instead of scanning free-form logs:

```sh
grep '\[cat=failure\]'
grep '\[op=CompositeLayers\]'
grep 'source_required\|source_valid_global\|dst_local'
grep '\[cat=target\].*generation'
grep '\[cat=fallback\]'
```

To audit trace call sites:

```sh
grep -R "RMLUI_BGFX_TRACE" src include samples
grep -R "trace_" src include samples
```

Renderer trace calls should go through shared helpers/macros. Do not add one-off local `printf`,
`fprintf`, or hand-built trace prefixes in renderer hot paths.

## Common renderer contracts to verify

Layer/materialization contracts:

- Root layers and virtual child layers have different semantics.
- A bounded physical target may still represent a larger semantic/global contract.
- Replaying recorded commands into a compact target requires exactly one global-to-local mapping.
- Transformed child layers often need conservative bounds until bounded transform replay is proven.

Filter contracts:

- Real filters must sample the filter work rectangle, including transparent margins.
- Do not shrink blur/drop-shadow/filter input to tracked ink bounds when transparent margin is part of
  the visual result.
- No-op filter chains may preserve the layer contract without running a postprocess pass.

Mask contracts:

- Saved mask image semantics are global/framebuffer-oriented.
- Distinguish semantic mask bounds from physical storage bounds.
- Track target generation to avoid stale saved mask records sampling a reused target.

Copy/texture contracts:

- Saved callback textures preserve requested save/scissor size.
- Compact source layers require explicit source local rect and destination offset.
- Origin-bottom-left correction belongs at the save/replay boundary, not as a random visual flip.

Pass/target contracts:

- Role identity matters. Primary, Secondary, Tertiary, Scratch, and BlendMask targets are not
  interchangeable just because dimensions match.
- Full-frame fallbacks must be explicit and traceable.
- View/pass reuse must not hide a needed clear, stencil state transition, or target switch.
- If a defect only appears above a specific FPS and the semantic trace is stable across good and
  bad frames, inspect `BeginFrameTargetGC` and `AcquirePostprocessTarget` for per-frame
  destroy/recreate churn. Bounded postprocess targets should be retained for a short idle window
  instead of being destroyed immediately on the next frame.
- Also inspect `EnsureLayerTarget` for per-frame layer target churn. When virtual child layer
  records are reset for a new frame, all target identity metadata must be preserved with the handles:
  generation, formats, MSAA state, and dimensions. Preserving only framebuffer/texture handles is
  not enough; reuse checks will fail and the target will be destroyed/reallocated every frame.

## Editing discipline

When debugging renderer defects:

- Keep confirmed fixes and experiments separated in the diff.
- Prefer diagnostic trace additions over speculative rendering changes.
- Revert experiments that do not improve the observed issue.
- Do not replace a bounded-rendering problem with an undocumented full-frame workaround.
- If a full-frame fallback is temporarily needed as a correctness anchor, document and trace it as a
  fallback, then plan the compact semantic/physical rect fix.
- Retest the reduced probe and the full probe after each behavioral fix.

## Files worth checking first

Core optimized path:

- `src/layers.cpp`
- `src/layers_optimized.cpp`
- `src/filters.cpp`
- `src/filters_optimized.cpp`
- `src/render_interface.cpp`
- `src/target_cache.cpp`
- `src/pass_scheduler.cpp`
- `src/passes.cpp`

Shared contracts:

- `src/private/rmlui_bgfx_types.hpp`
- `src/private/rmlui_bgfx_layers.hpp`
- `src/private/rmlui_bgfx_filters.hpp`
- `src/private/rmlui_bgfx_trace.hpp`
- `src/private/rmlui_bgfx_layer_composite_helpers.hpp`
- `src/private/rmlui_bgfx_mapping.hpp`

Reference behavior:

- `/home/thomas/dev/nt/refs/RmlUi/Backends/RmlUi_Renderer_GL3.cpp`
- `/home/thomas/dev/nt/refs/RmlUi/Source/Core/ElementEffects.cpp`
- `/home/thomas/dev/nt/refs/RmlUi/Source/Core/GeometryBoxShadow.cpp`

Documentation:

- `AGENTS.md`
- `docs/OPTIMIZED_RENDERER_ARCHITECTURE.md`
- `samples/effects_probe/README.md`
