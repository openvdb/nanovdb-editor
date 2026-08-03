# Pipelines

Pipelines turn your data — Gaussian splats, triangle meshes, line sets, or an
existing `.nvdb` file — into a NanoVDB grid you can view in the editor and save
to disk. This guide shows how to use them from Python.

- [Quick start](#quick-start)
- [Converting your data](#converting-your-data)
- [Choosing a process](#choosing-a-process)
- [Common options](#common-options)
- [Input reference](#input-reference)
- [Baking colors to RGBA8](#baking-colors-to-rgba8)
- [Controlling how objects are drawn](#controlling-how-objects-are-drawn)
- [Multi-step process chains](#multi-step-process-chains)
- [Tuning pipeline parameters](#tuning-pipeline-parameters)
- [Discovering pipelines](#discovering-pipelines)
- [Extending the pipelines](#extending-the-pipelines)

---

## Quick start

```python
import nanovdb_editor as nve

with nve.create_default() as app:
    # Build a NanoVDB grid from Gaussian splats and add it to a scene
    nvdb = app.scene("main").nanovdb_from_gaussians(
        means=means, quats=quats, scales=scales,
        sh_0=sh_0, sh_n=sh_n, opacities=opacities,
        process="voxelbvh", resolution=512,
    )
    app.show()
# Session closes here: worker stopped, editor shut down, compiler destroyed
```

`nve.create_default()` returns a `Session` — a small bundle of `(editor, compute,
compiler)` that also unpacks as a tuple. Prefer the `with` form above so native
resources are released cleanly on exit; `session.close()` does the same thing
explicitly. `app.scene(name)` returns a `Scene` with helpers that build a grid
and register it for you. Every helper returns the resulting NanoVDB grid and, by
default, adds it to the scene so it shows up in the editor.

---

## Converting your data

All helpers accept plain NumPy arrays. Reasonable defaults are chosen for you,
so you only need to pass what you have.

### Gaussian splats

```python
scene = editor.scene("main")

nvdb = scene.nanovdb_from_gaussians(
    means=means, quats=quats, scales=scales,
    sh_0=sh_0, sh_n=sh_n, opacities=opacities,
    process="voxelbvh",
    resolution=512,
)
```

Pass your raw splat parameters directly — normalization of quaternions, scale
exponentiation, color-from-SH and the opacity sigmoid are all handled for you.
If you have no higher-order spherical harmonics, pass `sh_n=None`.

### Triangle meshes

```python
nvdb = scene.nanovdb_from_mesh(
    indices=indices,         # uint32, 3 per triangle
    positions=positions,     # (V, 3) float32
    colors=colors,           # optional; defaults to white
    resolution=256,
    inflation_radius=0.0,    # optionally thicken each triangle
)
```

### Line sets

```python
nvdb = scene.nanovdb_from_lines(
    indices=indices,         # uint32, 2 per segment
    positions=positions,     # (V, 3) float32
    colors=colors,           # optional; defaults to white
    resolution=256,
)
```

### An existing NanoVDB file

Already have a `.nvdb` grid on disk? Load it straight into the scene — no
conversion step required:

```python
nvdb = scene.nanovdb_from_file("bunny.nvdb")
```

The object name defaults to the file's base name (`bunny.nvdb` → `"bunny"`).

---

## Choosing a process

Use `process="voxelbvh"` to build a grid. The main option is `resolution`
(grid size, `1`–`4096`, default `512`). The same process is used for Gaussians,
meshes, and line sets.

---

## Common options

Every `nanovdb_from_*` helper shares these:

- **`name`** — the object name to register the grid under in the scene. Defaults
  to something sensible (`"gaussians"`, `"mesh"`, `"lines"`, or the file name).
- **`add`** — set to `False` if you just want the grid back without adding it to
  the scene.

You can also pass pre-built `Grid` or `pnanovdb_ComputeArray` objects instead of
NumPy arrays anywhere an input is expected.

### Working with the returned grid

Every helper returns a `Grid` — a small handle around the NanoVDB data that you
can save, copy to NumPy, or free. It's also a context manager, so the underlying
GPU array is released automatically:

```python
with scene.nanovdb_from_mesh(indices=indices, positions=positions, add=False) as grid:
    grid.save("mesh.nvdb")          # write to disk
    data = grid.to_numpy()          # copy into a NumPy array
    print(grid.element_count)
# grid is freed here
```

If you don't use a `with` block, call `grid.close()` when you're done (grids
added to the scene stay valid for the editor regardless).

Need to read or edit the grid's bytes without copying? `grid.map(dtype)` is a
context manager yielding a **live, writable** NumPy view backed by mapped
memory. It's unmapped for you on exit, so the view is only valid inside the
block — use `to_numpy()` when you want an owned copy that outlives it:

```python
import numpy as np

with grid.map(np.uint32) as view:   # zero-copy view; writes go back to the grid
    header = int(view[0])
    view[1] = 0
# unmapped here
```

The same map/unmap-in-a-block pattern is available for any raw compute array via
`compute.mapped_array(array, np.dtype(...))`.

---

## Input reference

**Gaussians** — one array per attribute, all with `N` rows:

| Argument | Shape | Meaning |
|----------|-------|---------|
| `means` | `(N, 3)` | positions |
| `quats` | `(N, 4)` | rotation quaternions `(w, x, y, z)` |
| `scales` | `(N, 3)` | log-space scales |
| `sh_0` | `(N, 3)` | base color (order-0 spherical harmonics) |
| `sh_n` | higher-order SH, or `None` | view-dependent color |
| `opacities` | `(N,)` | logit-space opacities |

**Meshes / lines**:

| Argument | Type | Meaning |
|----------|------|---------|
| `indices` | `uint32` | 3 per triangle / 2 per line segment |
| `positions` | `float32` `(V, 3)` | vertex positions |
| `colors` | `float32`, optional | per-vertex RGB; white if omitted |

---

## Baking colors to RGBA8

A VoxelBVH grid can be turned into an **RGBA8 color image grid** — a NanoVDB
grid whose voxels hold packed RGBA colors, ready for the RGBA8 render pipeline.
Pass any VoxelBVH grid you built (Gaussians via `process="voxelbvh"`, a mesh, or
a line set) to `nanovdb_to_rgba8`:

```python
scene = editor.scene("main")

# Build a VoxelBVH grid, then bake it to RGBA8
grid  = scene.nanovdb_from_mesh(indices=indices, positions=positions, add=False)
rgba8 = scene.nanovdb_to_rgba8(grid, name="mesh_rgba8")
```

Options:

- **`ray_direction`** — index-space direction `(x, y, z)` used to bake colors
  (default `(0, 0, -1)`); a zero/invalid direction falls back to `(0, 0, -1)`.
- **`upsample_factor`** — topology upsampling, `1`–`4` (default `2`).
- **`name`** / **`add`** — as for the other helpers.

### Directional bake (one grid per ray direction)

To bake several view directions at once — matching the editor's
`bake_all_directions` mode — use `nanovdb_to_rgba8_directions`. It returns a
list of RGBA8 grids, one per direction:

```python
# Default: the same 8 directions the editor uses
grids = scene.nanovdb_to_rgba8_directions(grid, name="mesh_rgba8", add=False)

# Or pass your own index-space directions
grids = scene.nanovdb_to_rgba8_directions(
    grid,
    directions=[(0, 0, -1), (0, 0, 1), (1, 0, 0)],
    name="mesh_rgba8",
    add=False,
)
for g in grids:
    g.close()
```

`DEFAULT_RGBA8_DIRECTIONS` exposes the default 8-direction set.

---

## Controlling how objects are drawn

Each scene object has a **render** pipeline that decides how it's displayed. Set
it with `set_render_pipeline`, naming any render pipeline (see
[Discovering pipelines](#discovering-pipelines)):

```python
scene = editor.scene("main")

scene.set_render_pipeline("mesh", "voxelbvh_triangles_render")
scene.set_render_pipeline("splats", nve.PipelineType.voxelbvh_rgba8_render)

current = scene.get_render_pipeline("mesh")   # -> PipelineInfo
print(current.type_id)                         # "voxelbvh_triangles_render"
```

A pipeline can be given as its `process`/enum-name string, a
`PipelineType`/integer enum value, or a `PipelineInfo`. The same call works for
other stages via `scene.set_pipeline(name, stage, pipeline)` with a
`PipelineStage` (e.g. `PipelineStage.RENDER`). Call
`scene.mark_pipeline_dirty(name)` to force an object to re-run its pipelines on
the next editor update.

---

## Multi-step process chains

Some objects run more than one process pipeline in sequence (for example:
VoxelBVH build → RGBA8 conversion). `scene.process_steps(name)` returns a
sequence-like :class:`~nanovdb_editor.ProcessChain` you can index, assign,
iterate and append to:

```python
steps = scene.process_steps("mesh")
steps[0] = "voxelbvh"
steps.append("voxelbvh_rgba8")

print(len(steps))                       # 2
print([s.type_id for s in steps])       # ['voxelbvh_build', 'voxelbvh_rgba8']

# Per-step parameters (flushed + marked dirty on exit)
with steps[1].params() as p:
    if p is not None and p.data and p.size >= 4:
        ...  # p.data points at the step's parameter struct
```

Each `steps[i]` is a :class:`~nanovdb_editor.ProcessStep` with `.pipeline` /
`.type_id` and `.params()`. Only process-stage pipelines are accepted (not
chain templates like `voxelbvh_rgba8_chain` — use `set_pipeline` for those).

---

## Tuning pipeline parameters

Some pipelines expose parameters you can read and write per object. Typed helpers
cover the common fields:

```python
scene = editor.scene("main")

density = scene.get_voxels_per_unit("gaussians")   # defaults to 128
scene.set_voxels_per_unit("gaussians", 256)         # clamped to [1, 512]

scene.set_resolution("mesh", 256)
scene.set_inflation_radius("mesh", 0.01)
scene.set_rgba8_bake_params("rgba8", bake_all_directions=True, upsample_factor=2)
```

Each helper only applies to the process pipeline that owns the field:
`voxels_per_unit` to `gaussian_voxelize`, `resolution` and `inflation_radius` to
`voxelbvh_build`, and the bake params to `voxelbvh_rgba8`. Setters raise
`PipelineError` when the object runs a different process pipeline or has no
writable process params; getters return the documented default instead.
For other fields, `scene.pipeline_params(name, stage)` is a context manager
that hands you the mapped parameter block and flushes your writes (and marks the
stage dirty) on exit:

```python
from nanovdb_editor import PipelineStage

with scene.pipeline_params("gaussians", PipelineStage.PROCESS) as p:
    if p is not None and p.size >= 4:
        ...  # p.data points at the stage's parameter struct
```

### Showing the editor

- `app.show(...)` — blocking GUI (default for interactive use)
- `app.start(...)` — non-blocking worker (typically `headless=True` / streaming)
- `app.run()` — alias for `show`; `app.run(gui=False, headless=True, ...)` does
  `start` then `wait_for_interrupt`

Config accepts Python kwargs (`ip`, `port`, `headless`, `streaming`,
`stream_to_file`, `ui_profile`) instead of ctypes `EditorConfig` fields.

---

## Discovering pipelines

You can browse the available pipelines from Python without a running editor:

```python
import nanovdb_editor as nve

nve.list_pipelines("process")            # process pipelines that build a grid
nve.get_pipeline_info("voxelbvh")        # look up by the process alias
nve.get_pipeline_info(9)                 # ... or by enum value
```

Each result is a `PipelineInfo` describing the pipeline's stage, name, and
`process` alias (the string you pass as `process=`). For type-safe references
there are also `PipelineType` (one member per pipeline, e.g.
`PipelineType.voxelbvh_build`) and `PipelineStage` (`LOAD`/`PROCESS`/`RENDER`)
enums; both are plain `int` subclasses, so they interoperate with the raw enum
values. Prefer `PipelineStage` over the legacy `PIPELINE_STAGE_*` aliases.

Errors raised by this package derive from `NanoVDBError` (`PipelineError` for
native/conversion failures, `InvalidArgumentError` for bad arguments,
`DeviceError` for Vulkan setup, `SessionClosedError` after `Session.close`), so
you can catch them broadly with `except NanoVDBError`.

---

## Extending the pipelines

Adding a new binding is a developer task rather than day-to-day usage. The
Python package is a thin ctypes layer over the native `pnanovdbcompute` and
`pnanovdbeditor` libraries:

- Low-level interface wrappers live in
  [`raster.py`](nanovdb_editor/raster.py) and
  [`voxelbvh.py`](nanovdb_editor/voxelbvh.py).
- High-level scene helpers live in [`scene.py`](nanovdb_editor/scene.py).
- The pipeline registry (kept in sync with the C++
  [`editor/PipelineTypes.h`](../editor/PipelineTypes.h)) lives in
  [`pipelines.py`](nanovdb_editor/pipelines.py).

To expose another native function: mirror its interface struct field-for-field
as a `ctypes.Structure` (field order is load-bearing — use `c_void_p` for
anything you skip), wrap the call in a Pythonic method, then add a
`nanovdb_from_*` helper on `Scene`. Reuse `Scene._Arrays` for array
lifetime/cleanup and `Scene._finalize` to register the grid, and acquire the
native interface lazily via `Editor._get_raster()` / `Editor._get_voxelbvh()`.
Register any new `process` alias in `PIPELINE_REGISTRY` and export new public
symbols from [`__init__.py`](nanovdb_editor/__init__.py).
