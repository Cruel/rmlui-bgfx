# AGENTS.md

Guidance for future automated development work in this repository.

## Project scope

`rmlui-bgfx` is a reusable RmlUi render interface backed by bgfx. Keep it application-agnostic. Do not introduce dependencies on NovelTea, SDL outside the sample backend, ImGui, Lua, an application asset manager, or an application-specific shader registry.

Applications are expected to provide policy through the public extension points in `include/rmlui_bgfx`: shader loading, texture loading, diagnostics/perf logging, and optional material shader support.

## Local paths used by the main development workspace

Common local layout during development:

- Repository: `/home/thomas/dev/nt/rmlui-bgfx`
- Upstream RmlUi checkout: `/home/thomas/dev/nt/refs/RmlUi`
- Upstream GL3 reference renderer: `/home/thomas/dev/nt/refs/RmlUi/Backends/RmlUi_Renderer_GL3.cpp`

Run sample binaries from the upstream RmlUi checkout root so relative sample assets resolve:

```sh
cd /home/thomas/dev/nt/refs/RmlUi
../../rmlui-bgfx/build/linux-samples/samples/rmlui_bgfx_sample_effects_probe 12
../../rmlui-bgfx/build/linux-samples/samples/rmlui_bgfx_sample_effects_probe_gl3 12
```

## Build and validation commands

Before committing renderer changes, run at least:

```sh
cmake --build build/linux-samples
ctest --test-dir build/linux-debug --output-on-failure
git diff --check
```

For library-only changes, also use the preset flow when needed:

```sh
cmake --preset linux-debug
cmake --build --preset linux-debug
ctest --preset linux-debug
```

Visual renderer fixes require manual comparison against RmlUi GL3. Automated tests are useful but not sufficient for effects correctness.

## Render paths

The renderer has two runtime paths:

- `reference`: correctness-first, GL3-compatible behavior. This is the default.
- `optimized`: bounded/offscreen optimization path.

Environment aliases accepted by the sample backend include:

```sh
RMLUI_BGFX_RENDER_PATH=reference
RMLUI_BGFX_RENDER_PATH=optimized
```

The reference path must remain separate from the optimized path. Do not make the reference renderer depend on old optimized layer/filter orchestration.

Reference path must not use:

- `BgfxLayerSystem`
- `materialize_layer` / `materialized_layer_for_handle`
- old optimized content-bounds layer allocation
- old `CompositeLayers` dispatch through `layers.cpp`
- `BgfxFilterPipeline::apply`
- optimized filter/layer orchestration
- `RenderInterface::Impl::submit()` as its geometry renderer
- `RenderInterface::Impl::composite()` as its compositor
- old `save_texture_context()` / `save_mask_context()` implementations

Acceptable sharing includes public config/types, resource maps, shader/program/uniform handles, bgfx/RmlUi handles, low-level math helpers, rect/color/filter helpers, and low-level draw helpers that are not high-level optimized renderer orchestration.

## Reference renderer correctness notes

RmlUi GL3 is the primary behavioral reference. Important GL3 semantics to preserve in bgfx:

- Clip-mask and stencil behavior must match GL3, including inverse masks.
- The reference renderer uses full-frame layers for correctness isolation, but callback/save/filter work must still respect current scissor/save bounds.
- On origin-bottom-left renderers, saved callback textures copied out of full-frame layers need the source region converted before copy.
- GL3 render layers share stencil state. The bgfx reference implementation uses separate layer targets, so filtered child-layer composites may need the source layer's active clip mask replayed into the destination layer before compositing.
- Clip-mask draws must use the full layer viewport/projection and rely on scissor for bounds. Do not use the clipped work rect as the clip-mask pass viewport.
- Use two-sided bgfx stencil state (`bgfx::setStencil(front, back)`) for geometry, gradient, composite, clip-mask, and stencil-decrement submissions. GL3 behavior effectively applies the same state to both face directions unless configured otherwise.

## Effects probe workflow

The effects probe is the preferred minimal diagnostic harness for effects work. See `samples/effects_probe/README.md` for the full case list and workflow.

Typical commands:

```sh
cd /home/thomas/dev/nt/refs/RmlUi
RMLUI_BGFX_RENDER_PATH=reference ../../rmlui-bgfx/build/linux-samples/samples/rmlui_bgfx_sample_effects_probe 12
RMLUI_BGFX_RENDER_PATH=optimized ../../rmlui-bgfx/build/linux-samples/samples/rmlui_bgfx_sample_effects_probe 12
../../rmlui-bgfx/build/linux-samples/samples/rmlui_bgfx_sample_effects_probe_gl3 12
```

Use the same case number in bgfx and GL3. Do not claim visual correctness without a user/manual visual check.

Useful case order when debugging effects:

1. `00` baseline document clipping and scrolling.
2. `01` rounded overflow/clip masks.
3. `08` direct low-level `SetInverse` stencil probe.
4. `09` callback texture plus `SaveLayerAsTexture`.
5. `10` rounded `RenderBox` shadow meshes.
6. `11` outer CSS box-shadow.
7. `12` CSS inset box-shadow.
8. `13` manual inset blur callback texture.
9. `14` inset source clipping without blur.
10. `15` child-layer composite without blur.
11. `16` child-layer blur without clip masks.
12. `17` child-layer blur with inverse clip only.
13. `02`, `03`, `04`, `05`, `06` for broader effects/backdrop/mask coverage.

## Diagnostics policy

Diagnostics should be useful and event-level, not frame-spam. Avoid per-geometry logs by default.

Reference diagnostics should focus on:

- layer and target allocation
- push/pop
- clip-mask operations
- saved texture/mask handles and bounds
- filter chains and postprocess passes
- composite source/destination rects and stencil state

`RMLUI_BGFX_FILTER_TRACE=0` disables sample trace output. Do not add temporary diagnostic environment switches unless they are removed before commit or documented as permanent supported behavior.

## Commit hygiene

Before committing:

- Remove failed debug toggles and one-off diagnostic code.
- Keep old optimized files unless intentionally refactoring them; do not rename old renderer files merely to introduce a new path.
- Keep reference and optimized changes easy to audit separately.
- Run `git diff --check`.
- Run the relevant build/tests listed above.
- State clearly when visual correctness has not been manually verified.

Prefer separate commits for distinct concerns, for example one commit for renderer behavior and another for documentation or agent instructions.
