# Optimized Renderer Refactor Plan: GL3 Semantics, Bounded Layers, and Target Lifetime

## Purpose

The optimized bgfx renderer has accumulated correctness fixes, performance fixes, diagnostics, and partial bounded-layer work in ways that are now too brittle. The immediate goal is not to add another local patch. The goal is to refactor the optimized renderer around explicit contracts that match RmlUi's GL3 backend semantics first, and only then apply bounded-rendering and caching optimizations in controlled places.

The optimized renderer must keep the separate `reference` and `optimized` paths. This plan is for the optimized path. The reference path remains a correctness comparison path and must not be collapsed into the optimized renderer.

Correctness is the priority. Performance wins must come from real bounded work, explicit reuse, and validated cache lifetime rules, not from hiding full-frame work in counters or relying on incidental behavior.

## GL3 backend analysis

RmlUi's upstream `RmlUi_Renderer_GL3` has a comparatively simple and robust model. The optimized bgfx renderer should treat this model as the semantic baseline before applying optimizations.

### Frame and viewport contract

`SetViewport()` defines one viewport-sized coordinate system. `BeginFrame()` constructs an orthographic projection from `(0, 0)` to `(viewport_width, viewport_height)`, sets blend/stencil/scissor state, begins the render layer stack, binds the top layer, and clears it. `EndFrame()` resolves the active MSAA layer to a postprocess framebuffer, draws that postprocess texture to the backbuffer, then destroys no per-frame objects except by reducing the active layer count. Resize is handled by `RenderLayerStack::BeginFrame(new_width, new_height)`, which destroys all framebuffers if the size changed and starts fresh.

Important implication for bgfx: the optimized path should have a single frame contract that says when size changes are applied, when targets are invalidated, and when all coordinate transforms are updated. Resizes should never leave stale layer mappings or target sizes alive.

### Layer stack contract

GL3 uses `RenderLayerStack` as a true stack:

- `PushLayer()` allocates another full-viewport layer framebuffer only if the stack depth exceeds the existing pool.
- `PopLayer()` only decrements active depth.
- Layer handles are stack indices, valid for the current frame/layer stack sequence.
- Layer framebuffers are reused by stack depth, not by content bounds, scroll position, or historical layer handle identity.
- All layer framebuffers share one depth/stencil buffer where possible.
- Layer framebuffers are MSAA-enabled; postprocess framebuffers are not.

Important implication for bgfx: optimized virtual layers can be bounded, but their lifetime must still be frame-scoped unless a deliberate cache key and invalidation rule says otherwise. Caching by arbitrary RmlUi layer handle without clear per-frame ownership is risky because scrolling and re-recording can create many transient handles or dimensions.

### Postprocess contract

GL3 has exactly four postprocess framebuffers for the viewport size:

1. primary,
2. secondary,
3. tertiary,
4. blend mask.

They are lazily created, reused for the current viewport size, and destroyed on resize. Filters operate by copying/resolving the source layer into primary, ping-ponging through secondary/tertiary, and finally compositing primary into the destination layer. `MaskImage` samples both primary and the dedicated blend-mask framebuffer. The blend mask is not a generic scratch target selected by bounds; it is a named framebuffer in the layer stack.

Important implication for bgfx: postprocess targets need explicit roles. `Primary`, `Secondary`, `Tertiary`, and `BlendMask` should not be treated as anonymous interchangeable cache entries unless the cache key preserves the same ownership semantics. Bounded postprocess can use smaller textures, but the role/lifetime must remain explicit.

### Mask-image contract

GL3 `SaveLayerAsMaskImage()`:

- blits the full top layer into postprocess primary,
- draws primary into the dedicated blend-mask framebuffer,
- returns a `MaskImage` filter handle that implicitly references that blend-mask framebuffer.

It does not allocate a standalone texture handle for the mask and does not look up a blend mask later by incidental bounds. The mask resource is owned by the renderer's postprocess layer stack state.

Important implication for bgfx: the optimized path needs an explicit saved-mask resource model. A `MaskImage` filter should resolve to the exact mask target produced by `SaveLayerAsMaskImage()`, not to a fresh scratch target and not to whichever `BlendMask` target happens to match bounds. For bounded rendering, the mask record must carry its coordinate space and target identity or generation.

### Scissor and stencil contract

GL3 clip masks are broad and simple:

- `Set` clears the stencil buffer and replaces with `1`.
- `SetInverse` clears to `1`, then writes `0` into the geometry.
- `Intersect` increments the stencil reference.
- Color writes are disabled during mask rendering.
- Scissor state is renderer-global and vertically flipped only at GL submission boundaries.

This broad behavior is part of why probe `23` is sensitive. Narrowing stencil clears to command geometry bounds can leave stale stencil values that leak through rounded/inset masks.

Important implication for bgfx: broad stencil clears are not a hack. They are closer to GL3 semantics. Any bounded version must first define what "broad" means within the active materialized target and then map that rectangle into target-local coordinates.

### Transform contract

GL3 applies transforms through a single projection-transform uniform and per-draw translation. Layers remain full viewport size, so transformed commands do not need coordinate rebasing when rendered into child layers. This is why GL3 avoids the bounded transformed-layer bug class entirely.

Important implication for bgfx: bounded transformed layers are a real optimization beyond GL3 semantics. They require a formal mapping plan. It is unsafe to remove the full-frame transform fallback until every draw, clip, filter, saved texture, saved mask, and composite operation agrees on the same global-to-target coordinate contract.

## Refactor design principles

1. Preserve GL3 semantics first. Bounded rendering must be an optimization layer on top of those semantics, not a replacement for them.
2. Separate semantic roles from physical allocations. A postprocess `BlendMask` role is not the same thing as a generic scratch framebuffer that happens to be the same size.
3. Make coordinate space explicit. Every materialized layer and every postprocess source/destination needs a documented mapping between global framebuffer space and target-local texture space.
4. Make lifetime explicit. A target must be frame-scoped, viewport-scoped, or persistent-cached. Anything else is a bug waiting to happen.
5. Keep full-frame fallbacks visible. A fallback should have a reason and metrics should report it as real work.
6. Do not optimize by hiding counters. Counters should describe the actual work performed.
7. Do not introduce cross-frame caching until per-frame correctness is stable and tested.

## Core contracts to introduce

### 1. `RenderTargetRole`

Define target roles independently from cache storage:

- `LayerColorMsaa`: active layer target with stencil/depth attachment.
- `LayerResolvedColor`: optional resolved layer texture if MSAA needs resolve before sampling.
- `PostprocessPrimary`.
- `PostprocessSecondary`.
- `PostprocessTertiary`.
- `SavedBlendMask`.
- `ScratchCopy`.
- `BackdropCopy`.

Each role must define whether it can be sampled, whether it can be written more than once per frame, whether it can be reused in the same frame, and whether it may alias another role.

### 2. `TargetLifetime`

Every target allocation should be one of:

- `Frame`: destroyed or returned to a per-frame pool at `begin_frame()`.
- `Viewport`: reused while framebuffer dimensions are unchanged, destroyed on resize.
- `PersistentCache`: retained across frames with an explicit bounded cache key and eviction policy.
- `External`: owned by RmlUi/user texture management.

Initial refactor should prefer `Frame` and `Viewport`. `PersistentCache` should be added only after metrics prove it is necessary and after a bounded eviction rule is defined.

### 3. `TargetDescriptor`

All target acquisition should use a single descriptor:

- role,
- lifetime,
- global bounds,
- target size,
- MSAA sample count,
- texture format,
- stencil/depth requirement,
- whether sampling is needed,
- debug label/reason.

Framebuffer creation should be centralized through this descriptor so allocation failures can report the same fields consistently.

### 4. `LayerSpaceMapping`

A materialized layer plan should contain:

- `global_bounds`: framebuffer-space rectangle covered by the target.
- `logical_bounds`: logical-space equivalent.
- `target_origin`: framebuffer-space origin of the target.
- `texture_width` / `texture_height`.
- `full_frame_fallback_reason` or `None`.
- helpers for `global_to_local`, `local_to_global`, `scissor_to_local`, and UV-region conversion.

The mapping is per materialization, per frame. It is not a cross-frame cache until proven safe.

### 5. `SavedMaskRecord` and `SavedTextureRecord`

Saved textures and saved masks need explicit records, not implicit target lookup by kind/bounds.

A saved mask record should store:

- filter handle,
- target role/handle/generation,
- global mask bounds,
- target-local mask rect,
- source layer generation,
- whether it was full-frame or bounded.

A saved texture record should store:

- texture handle,
- dimensions,
- source global bounds,
- target-local copy region,
- ownership.

This makes the radial sphere and scrolling `.mask` cases explicit instead of relying on accidental target reuse.

## Implementation phases

### Phase 0: freeze and restore a trustworthy baseline

Before any refactor:

1. Inspect `git status --short` and separate staged from unstaged changes.
2. Remove temporary diagnostics and experiments.
3. Restore the known-correct transform fallback behavior.
4. Keep only confirmed fixture/test additions that help reproduce bugs.
5. Keep broad stencil clear behavior required by probe `23`.
6. Keep optimized `MaskImage` routed through the general filter path until a correct fast path is designed.
7. Do not keep resource-cache patches unless they have a documented target lifetime model.

Validation:

```sh
cmake --build build/linux-samples
git diff --check
```

Manual checks:

```sh
cd /home/thomas/dev/nt/refs/RmlUi
RMLUI_BGFX_RENDER_PATH=reference ../../rmlui-bgfx/build/linux-samples/samples/rmlui_bgfx_sample_effects
RMLUI_BGFX_RENDER_PATH=optimized ../../rmlui-bgfx/build/linux-samples/samples/rmlui_bgfx_sample_effects
../../rmlui-bgfx/build/linux-samples/samples/rmlui_bgfx_sample_effects_probe 23
../../rmlui-bgfx/build/linux-samples/samples/rmlui_bgfx_sample_effects_probe_gl3 23
```

### Phase 1: document and test GL3 semantic invariants

Add comments and CPU tests for the intended bgfx equivalents of GL3 behavior:

- full-frame transform fallback means no target-origin rebase is needed;
- broad stencil clear for `Set` and `SetInverse`;
- postprocess roles are primary/secondary/tertiary/blend-mask, not anonymous scratch slots;
- save-mask produces a named saved blend-mask resource;
- resize invalidates viewport-scoped targets;
- a render target's coordinate mapping is immutable for its materialization.

No rendering behavior should change in this phase.

### Phase 2: centralize target allocation behind descriptors

Refactor `BgfxTargetCache` so all layer and postprocess allocation goes through `TargetDescriptor` and returns a `TargetLease` or `TargetHandle` with role, lifetime, size, and generation.

Rules:

- Layer targets are frame-scoped by default.
- Postprocess targets are viewport-scoped fixed-role by default, mirroring GL3.
- Bounded postprocess targets are allowed only through an explicit bounded descriptor.
- Persistent reuse of arbitrary sizes is disabled until an eviction policy is part of the descriptor model.
- Allocation failure logs come from the descriptor and include role, lifetime, bounds, size, MSAA, format, and reason.

This phase should remove ad hoc allocation paths before adding new optimizations.

### Phase 3: implement explicit saved mask and saved texture records

Replace implicit saved-mask resolution with `SavedMaskRecord`.

Required behavior:

- `SaveLayerAsMaskImage()` creates or updates a saved blend-mask target and records exactly which target/generation the returned filter handle references.
- `MaskImage` filter resolution uses that record, not `acquire_postprocess_target()` and not a bounds search.
- General filter-chain path remains the only optimized `MaskImage` path until equivalence is proven.
- Full-frame GL3-compatible saved mask mode remains available as the baseline.
- Bounded saved mask mode must carry mapping data and be behind validation/toggle until tests pass.

Validation cases:

- scrolling `.mask` item in the effects sample;
- readback radial masked sphere;
- transform plus mask-image probe;
- resize readback reference and optimized.

### Phase 4: introduce `LayerSpaceMapping` without changing output

Add pure mapping helpers and CPU tests:

- global-to-local and local-to-global round trips;
- scissor conversion;
- transformed geometry bounds;
- parent/scissor intersection;
- empty and fractional bounds;
- full-frame fallback mapping.

Wire plans into layer records but keep existing full-frame transformed fallback. This phase should not make web-smoke metrics better yet. It should only make internal state explicit.

### Phase 5: rebase non-transformed bounded layer replay

Apply `LayerSpaceMapping` to non-transformed commands first:

- geometry translation,
- gradient/material shader translation,
- scissor conversion,
- layer clears,
- saved texture copies,
- composite source/destination rectangles.

This is the first phase where bounded rendering should produce real performance improvement, but only for non-transformed content. Transform fallback remains.

Validation:

- effects sample scrolling;
- readback gallery;
- probe `23`;
- no framebuffer spam;
- no metric suppression.

### Phase 6: rebase clip masks and stencil clears

This is high risk.

Rules:

- Preserve GL3 broad `Set` and `SetInverse` semantics.
- Convert broad clear regions into target-local coordinates only at the submission boundary.
- Do not clear only command geometry bounds unless a specific probe proves equivalence.
- Keep stencil reference overflow normalization explicit.

Validation must include rounded clips, inset shadows, inverse masks, and probe `23`.

### Phase 7: rebase filter chains and composites

Keep filter planning in global framebuffer space, but convert at draw/read/write boundaries:

- source global rect to source local rect;
- destination global rect to destination local rect;
- UV rect for sampled texture regions;
- blur/drop-shadow expansion;
- backdrop copy bounds;
- final composite bounds.

Do not optimize mask-image separately yet. Use the saved-mask record from Phase 3.

### Phase 8: bounded transformed layers behind a toggle

Only after Phases 2 through 7 are stable, add bounded transformed-layer support behind a toggle such as:

```sh
RMLUI_BGFX_BOUNDED_TRANSFORM_LAYERS=1
```

The transformed plan must account for:

- push-layer transform;
- per-command transform;
- translation in the same coordinate space as the transform;
- saved texture callback bounds;
- clips and stencil clears;
- filter expansion;
- final composite.

Fallback reasons remain visible:

- `None`;
- `SavedTextureTransformContract`;
- `UnsupportedTransformRebase`;
- `UnboundedClipMask`;
- `InverseClipConservativeFallback`;
- `SavedMaskMappingUnsupported`.

### Phase 9: make bounded transformed layers default only after validation

Promotion criteria:

- optimized and reference match in readback gallery except known intentional tolerances;
- GL3 and optimized match focused probes visually;
- probe `23` remains correct;
- scrolling `.mask` remains correct;
- no framebuffer spam or unbounded memory growth during effects sample scrolling;
- web smoke passes with real metrics;
- full-frame fallback counters are real and understandable;
- no temporary diagnostics or undocumented environment toggles remain.

## Validation matrix

Minimum command validation:

```sh
cmake --build build/linux-samples
git diff --check
```

From `/home/thomas/dev/nt`:

```sh
./scripts/run-tests.sh --reference -R noveltea_rmlui_readback --output-on-failure
./scripts/run-tests.sh --reference -R noveltea_rmlui_resize_readback --output-on-failure
./scripts/run-tests.sh -R noveltea_rmlui_readback --output-on-failure
./scripts/run-web-smoke.sh
```

Manual visual validation:

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

Manual stress validation:

- Scroll `rmlui_bgfx_sample_effects` slowly and quickly.
- Resize while the effects sample is visible.
- Compare reference, optimized, and GL3 for the focused probes.
- Watch for framebuffer creation spam and sustained memory growth.

## New probes/tests to add

1. Transform plus `mask-image`.
2. Transform plus blur/drop-shadow expansion.
3. Transform plus rounded clip/inset shadow.
4. Transform plus saved texture callback.
5. Scroll-coupled mask-image with changing scissor.
6. Bounded backdrop-filter in a scrolling container.
7. Resource churn test that scrolls through many slightly different filter bounds and asserts target counts stay bounded.
8. Resize test that verifies viewport-scoped target destruction and recreation.

## Acceptance criteria

The refactor is complete when:

- optimized renderer has explicit target roles and lifetimes;
- saved masks and saved textures have explicit ownership records;
- materialized layers have explicit coordinate mappings;
- transformed layers either use a documented fallback or a validated bounded mapping;
- probe `23` remains visually correct;
- radial masked sphere remains colored;
- scrolling `.mask` remains correct;
- effects sample scrolling does not produce framebuffer allocation spam;
- memory use stays bounded during scrolling and resize stress;
- web smoke passes without metric suppression;
- reference and optimized paths remain separate.

## Non-goals

- Do not refactor the reference renderer into the optimized layer system.
- Do not hide real full-frame work by changing counters only.
- Do not use anonymous target lookup as a saved-mask ownership model.
- Do not add unbounded cross-frame caches.
- Do not re-enable the optimized single-mask fast path until it is proven equivalent to the general path.
- Do not remove the full-frame transform fallback before bounded transformed rendering passes the validation matrix.
