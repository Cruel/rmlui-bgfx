# Optimized Renderer Architecture and Guardrails

## Purpose

This document defines the architecture, goals, and non-negotiable constraints for the `optimized` bgfx renderer path. It is intended for implementation agents before they touch optimized rendering code.

The optimized path exists to reduce real GPU work and allocation churn while preserving RmlUi GL3 semantics. It is not a place for visual-bug patches that hide work behind counters, route everything through full-frame passes, or rely on accidental target reuse.

If a change fixes one probe by widening work to a full frame, the change is incomplete unless it also documents the fallback reason, proves that the fallback is semantically required, and keeps the fallback visible in metrics.

## Renderer paths

The renderer has two separate runtime paths:

- `reference`: correctness-first, GL3-compatible behavior. It is the comparison path and should remain simple.
- `optimized`: bounded/offscreen optimization path. It may do less physical work than GL3, but only when the reduced work has explicit bounds, target lifetime, and coordinate-space contracts.

Do not collapse the paths. Do not make the reference renderer depend on the optimized layer/filter system. Shared code is acceptable only for low-level helpers that do not impose optimized-path orchestration on the reference path.

## Semantic baseline: RmlUi GL3

The upstream GL3 backend is the behavioral baseline. It is simple because it uses full-viewport layers and fixed postprocess framebuffers.

Key GL3 semantics the optimized path must preserve:

- `SetViewport()` defines a single viewport-sized coordinate system.
- `BeginFrame()` establishes projection, blend, stencil, scissor, and layer-stack state for the viewport.
- `EndFrame()` resolves the active layer to a postprocess framebuffer and draws it to the backbuffer.
- `PushLayer()` and `PopLayer()` operate as a stack. GL3 reuses layer framebuffers by stack depth.
- GL3 postprocess roles are named: primary, secondary, tertiary, and blend mask.
- `SaveLayerAsMaskImage()` produces a mask-image filter that references the saved blend-mask resource produced by that call.
- `Set` and `SetInverse` clip-mask operations perform broad stencil clears.
- Transforms are applied through a projection/transform uniform plus per-draw translation; GL3 avoids bounded transformed-layer rebasing by keeping layer targets full viewport size.

The optimized path may use smaller physical targets, but it must not change the semantic model accidentally.

## Optimization goals

The optimized renderer should optimize by doing less real work, not by hiding work.

Primary goals:

- Bound child-layer render targets to the required work rectangle when safe.
- Bound filter/postprocess work to source, scissor, and filter-expansion bounds when safe.
- Reuse render targets through explicit lifetime rules instead of allocating/destroying every frame.
- Preserve mask-image, saved-texture, stencil, transform, and clip semantics.
- Keep full-frame fallbacks rare, intentional, and visible.
- Keep metrics honest: full-frame work must count as full-frame work.

Secondary goals:

- Keep the optimized path readable enough that future fixes are local and contract-driven.
- Make visual regressions diagnosable through focused probes and metric counters.
- Prefer small helper abstractions over broad resource-management frameworks.

## Architectural model

The optimized renderer has five major responsibilities.

### 1. Layer recording and materialization

Virtual child layers may be recorded first and materialized later. Recording captures draw commands, scissor state, transform state, clip-mask state, and conservative content bounds. Materialization chooses a physical target rectangle and replays recorded commands into it.

A materialized layer must have an explicit coordinate-space contract:

- global framebuffer bounds covered by the target;
- logical bounds equivalent to the framebuffer bounds;
- texture width and height;
- global-to-local conversion;
- local-to-global conversion;
- scissor-to-local conversion;
- fallback reason if it had to become full frame.

The existing `RenderBounds`, `GlobalFbRect`, `LocalFbRect`, `local_rect_for_layer()`, `global_rect_for_layer()`, and `clamp_scissor_local()` helpers are the current mapping contract. Extend these helpers before adding a larger `LayerSpaceMapping` type. Add a wrapper only when the helper set becomes too diffuse.

### 2. Target roles and lifetimes

Render targets must have explicit role/kind, lifetime, size, bounds, and generation. The current `PostprocessTargetKind` model is the starting point.

Minimum postprocess roles:

- `Primary`
- `Secondary`
- `Tertiary`
- `BlendMask`
- `Scratch`

Minimum lifetimes:

- `Frame`: reusable only inside the current frame; reset at `begin_frame()`.
- `Viewport`: reusable until viewport/framebuffer dimensions change; destroyed on resize.
- `External`: owned outside the target cache.

Do not add persistent cross-frame caches for arbitrary bounded sizes until correctness is stable and metrics prove they are needed. Persistent caches must have bounded keys, eviction, resize invalidation, and feedback-loop safety.

### 3. Saved masks and saved textures

Saved masks must have explicit ownership. A mask-image filter must resolve to the exact saved mask target/generation produced by `SaveLayerAsMaskImage()`. It must not rediscover a `BlendMask` target by kind and bounds.

A saved mask record should carry:

- filter handle;
- target role/kind;
- target handle or stable target id;
- target generation;
- global mask bounds;
- target-local mask rect;
- source layer generation or materialization id;
- whether it was full-frame or bounded.

Saved textures already flow through `TextureRecord` with `TextureOwnership::SavedLayer`. Do not add a parallel saved-texture registry unless a concrete correctness bug requires additional metadata.

### 4. Filter and composite pipeline

Filter planning should remain in global framebuffer space. Conversion to target-local rectangles must happen at draw, copy, postprocess, or composite submission boundaries.

The filter pipeline must track:

- source global bounds;
- source local rect;
- work/allocation bounds;
- valid-output bounds;
- conservative filter expansion;
- destination local rect;
- UV rect for sampled texture regions;
- whether the output is full-frame or bounded.

Mask-image must use the saved-mask record. It should not have a separate fast path unless that path has proven equivalence with the general filter-chain path.

### 5. Clip-mask and stencil behavior

Stencil behavior must preserve GL3 semantics. Broad clears are not hacks; they are part of the semantic baseline.

Rules:

- `Set` and `SetInverse` clear broadly within the active materialized target's contract.
- Do not shrink stencil clears to command geometry bounds unless a focused probe proves equivalence.
- Convert broad clear regions into target-local coordinates only at the submission boundary.
- Use two-sided bgfx stencil state for geometry, gradient, composite, clip-mask, and decrement submissions.
- Keep stencil reference normalization explicit.

Probe `23` is a sensitive guard for this area. Treat a probe `23` regression as evidence that stencil or mask bounds were narrowed too aggressively.

## Full-frame fallback policy

Full-frame fallbacks are allowed only when they are semantically necessary or when a bounded implementation has not yet been validated.

Acceptable fallback reasons include:

- no valid scissor/work bounds exist;
- transformed-layer rebasing is unsupported for this command set;
- saved texture callback geometry depends on a coordinate contract not yet expressible in bounded space;
- inverse clip-mask bounds are too conservative to bound safely;
- saved mask mapping is unsupported;
- destination/root preservation is required to avoid WebGL feedback loops or API-invalid sampling.

Requirements for any full-frame fallback:

- record an explicit reason;
- increment the relevant full-frame fallback/perf counter;
- keep the fallback local to the unsupported case;
- add or reference a validation case that explains why the fallback exists;
- do not silently route an entire visual bug through full-frame postprocess as a permanent fix.

## What not to do

Do not make a probe pass by broadening every layer or filter to full frame without a documented semantic reason.

Do not use anonymous target lookup as a saved-mask ownership model.

Do not hide full-frame work by changing counters.

Do not add unbounded target caches keyed by scrolling bounds, historical layer handles, or arbitrary dimensions.

Do not make optimized renderer state depend on transient RmlUi handles across frames unless there is a documented generation/invalidation model.

Do not add a single-purpose mask-image fast path until it is proven equivalent to the general filter-chain behavior.

Do not remove the full-frame transform fallback until bounded transformed rendering validates every affected operation: geometry, shaders, clips, stencil clears, filters, saved textures, saved masks, and final composites.

Do not refactor the reference path into the optimized layer system.

## Recommended implementation order

1. Keep the current known-good transform fallback and broad stencil behavior stable.
2. Add target role/lifetime/generation metadata and centralize allocation failure diagnostics.
3. Implement explicit saved-mask records and route mask-image resolution through them.
4. Stop postprocess allocation churn with bounded frame/viewport lifetime rules.
5. Add mapping tests around the existing global/local helper model.
6. Audit non-transformed bounded replay against the mapping contract.
7. Rebase clip-mask, stencil-clear, filter, and composite boundaries only after mapping is stable.
8. Add bounded transformed layers behind a disabled-by-default toggle.
9. Promote bounded transformed layers only after probe, readback, scrolling, resize, and web-smoke validation.

## Validation expectations

Minimum command checks for meaningful optimized renderer changes:

```sh
cmake --build build/linux-samples
git diff --check
```

Use project-level readback/web checks when behavior changes affect NovelTea integration:

```sh
./scripts/run-tests.sh --reference -R noveltea_rmlui_readback --output-on-failure
./scripts/run-tests.sh --reference -R noveltea_rmlui_resize_readback --output-on-failure
./scripts/run-tests.sh -R noveltea_rmlui_readback --output-on-failure
./scripts/run-web-smoke.sh
```

Manual visual checks remain required for renderer correctness:

```sh
cd /home/thomas/dev/nt/refs/RmlUi
RMLUI_BGFX_RENDER_PATH=reference ../../rmlui-bgfx/build/linux-samples/samples/rmlui_bgfx_sample_effects
RMLUI_BGFX_RENDER_PATH=optimized ../../rmlui-bgfx/build/linux-samples/samples/rmlui_bgfx_sample_effects
../../rmlui-bgfx/build/linux-samples/samples/rmlui_bgfx_sample_effects_probe 23
../../rmlui-bgfx/build/linux-samples/samples/rmlui_bgfx_sample_effects_probe_gl3 23
../../rmlui-bgfx/build/linux-samples/samples/rmlui_bgfx_sample_effects_probe 31
../../rmlui-bgfx/build/linux-samples/samples/rmlui_bgfx_sample_effects_probe_gl3 31
../../rmlui-bgfx/build/linux-samples/samples/rmlui_bgfx_sample_effects_probe 32
../../rmlui-bgfx/build/linux-samples/samples/rmlui_bgfx_sample_effects_probe_gl3 32
```

Stress validation:

- scroll `rmlui_bgfx_sample_effects` slowly and quickly;
- resize while the effects sample is visible;
- compare reference, optimized, and GL3 for focused probes;
- watch allocation counters and memory use for framebuffer creation spam;
- confirm web smoke metrics reflect real full-frame and bounded work.

## Review checklist for agents

Before modifying optimized renderer code, answer these questions:

1. Which GL3 semantic is this code preserving?
2. What is the source coordinate space and destination coordinate space?
3. Is this target frame-scoped, viewport-scoped, or external?
4. What invalidates this target or record?
5. Is any full-frame fallback being introduced? If yes, what is the reason and metric?
6. Does this change affect saved textures, saved masks, clip masks, transforms, filters, or composites?
7. Which probe/readback/stress case validates it?
8. Could the same visual fix be achieved by correcting mapping/lifetime instead of widening work?

If these questions cannot be answered, do not patch the renderer yet. First add or tighten the relevant contract.
