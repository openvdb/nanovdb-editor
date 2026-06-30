# Editor pipelines

A **pipeline** is a reusable operation bound to one **stage**, shared by every
scene object that uses it (not owned per-object).

A scene object picks one pipeline per stage, ordered load → process → render (any
stage may be empty), turning raw data into something renderable. The **process**
stage may be a *chain* of several steps (see
[Chaining process steps](#chaining-process-steps)).

| Stage       | Purpose                                                  | Example |
| ----------- | -------------------------------------------------------- | ------- |
| **load**    | Bring data in from disk / another source.                | `mesh_load` reads a PLY into compute arrays. |
| **process** | Transform loaded data into a GPU-renderable form. May chain multiple steps. | `voxelbvh_build` builds a BVH from a mesh; `voxelbvh_rgba8` then converts it to an RGBA8 grid. |
| **render**  | Draw the processed data each frame.                      | `nanovdb_render` ray-marches a grid. |

The runtime drives this on the render thread: `pipeline_init()` sets up workers,
`pipeline_load()` starts a load (usually from a UI entry point that builds a
`PipelineLoadRequest`), and `pipeline_update()` runs once per frame to advance
work and report progress. Each descriptor's `execute` runs on the render thread;
long-running work is handed to an `AsyncWorker` (one per background job) and
polled by `pipeline_update()`.

## Adding a New Pipeline

First decide whether the pipeline is **sync** or **async** — it determines which
steps you implement.

| Flavor             | What it does                                                      | Where it lives |
| ------------------ | ----------------------------------------------------------------- | -------------- |
| **Sync (execute)** | Returns from `execute()` on the same frame; cheap GPU/noop.       | `execute_xxx()` in `editor/Pipeline.cpp`. |
| **Async (worker)** | Hands work to a background thread, polled by `pipeline_update()`. | `XxxWorker : AsyncWorker` in `PipelineRuntime.{h,cpp}` + descriptor in `Pipeline.cpp`. |

Use **sync** for microsecond work, **async** for disk, raster, or multi-second GPU
work that would stall the render thread.

## Shared steps (every pipeline)

1. **Enum value** — add `pnanovdb_pipeline_type_<name>` in `editor/PipelineTypes.h`,
   before `pnanovdb_pipeline_type_count`.

2. **Params struct** (if the pipeline has tunable parameters) — put it in
   `editor/Pipeline.cpp` if only that file uses it, or in `editor/PipelineRuntime.h`
   if a worker's `.cpp` also reads it. Declare a C-friendly POD and reflect each
   field right after (the Properties panel drives its widgets off this reflection):

```cpp
struct <Name>Params { float <field> = ...; };

#define PNANOVDB_REFLECT_TYPE <Name>Params
PNANOVDB_REFLECT_BEGIN()
PNANOVDB_REFLECT_VALUE(float, <field>, 0, 0)
PNANOVDB_REFLECT_END(0)
#undef PNANOVDB_REFLECT_TYPE
```

   (Use `PNANOVDB_REFLECT_STRUCT_OPAQUE_IMPL(<Name>Params)` only for params that are
   never shown in the Properties panel.)

3. **Typed getters/setters** (only if another `.cpp` reads/writes the params) — in
   `editor/PipelineRuntime.h`, declare `pipeline_params_get/set_<name>_<field>()`;
   guard `params->size` and operate on the opaque blob.

4. **UI hints JSON** (optional; refines how params show in the Properties panel) —
   add a `editor/shaders/<name>.slang.json` whose `PipelineParams` object keys match the
   struct field names. The panel renders one widget per reflected field; each entry can
   supply `label`, `tooltip`, `min`, `max`, `step`, `isBool`, `useSlider`, and `hidden`.
   Field entries use the same shape as `ShaderParams` in render-shader JSON; only the
   top-level key differs. Without a JSON file the fields still render, using their
   reflected names and default widgets.

```jsonc
// editor/shaders/<name>.slang.json
{
    "PipelineParams": {
        "<field>": { "label": "Label", "tooltip": "...", "min": 0, "max": 100, "step": 1 }
    }
}
```

5. **Descriptor + registration** — in `editor/Pipeline.cpp`, pick the macro for your
   stage. Each builds a static descriptor and self-registers it (no central switch).
   Pass params with `PNANOVDB_PIPELINE_PARAMS(<Name>Params)` or
   `PNANOVDB_PIPELINE_NO_PARAMS`. For process pipelines, the UI-hints argument is the
   shader/base name (e.g. `"editor/<name>.slang"`) or `nullptr` for none. Pass `nullptr`
   for `execute` when async-only. The trailing data-kind arguments declare the pipeline's typed
   data flow (see [Pipeline compatibility](#pipeline-compatibility-data-kinds)): each stage's
   `outputs` and/or `inputs`.

```cpp
// load: no shaders, no render method; declares what it outputs
PNANOVDB_REGISTER_LOAD_PIPELINE(
    s_<name>_descriptor, pnanovdb_pipeline_type_<name>, "Name",
    PNANOVDB_PIPELINE_PARAMS(<Name>Params),
    execute_<name>,
    pnanovdb_pipeline_data_kind_<kind>);   // outputs (or _none)

// process: params map to process_params; declares outputs + inputs
PNANOVDB_REGISTER_PROCESS_PIPELINE(
    s_<name>_descriptor, pnanovdb_pipeline_type_<name>, "Name",
    s_<name>_shaders, 1,                   // or nullptr, 0 for no shaders
    PNANOVDB_PIPELINE_PARAMS(<Name>Params),
    execute_<name>,
    get_render_method_<nanovdb|gaussian>,
    "editor/<name>.slang",                 // UI-hints JSON base name, or nullptr
    pnanovdb_pipeline_data_kind_<kind>,    // outputs (or _none)
    pnanovdb_pipeline_data_kind_<kind>);   // inputs (bitmask; 0 = anything)

// render: you pick method + param map; declares what it inputs
PNANOVDB_REGISTER_RENDER_PIPELINE(
    s_<name>_descriptor, pnanovdb_pipeline_type_<name>, "Name",
    s_<name>_shaders, 1,
    PNANOVDB_PIPELINE_PARAMS(<Name>Params),
    execute_<name>, get_render_method_<none|nanovdb|gaussian>,
    map_params<&SceneObject::render_params>, // or nullptr
    pnanovdb_pipeline_data_kind_<kind>);     // inputs (bitmask; 0 = anything)
```

6. **UI entry point** (optional) — e.g. `mesh_import::mesh()` in
   `editor/MeshImport.cpp` builds a `PipelineLoadRequest` and calls `pipeline_load`.

## Pipeline compatibility (data kinds)

The Properties panel only offers pipelines that can consume an object's current data.
Each descriptor tags its typed data flow with `pnanovdb_pipeline_data_kind_t` bits
(in `editor/PipelineTypes.h`): **`outputs`** (the kind a stage produces; `_none` for a
passthrough like `noop` or a terminal render) and **`inputs`** (a bitmask of accepted
kinds; `0` = anything).

## Sync pipeline only

All work happens inside `execute()`. Add one function in `editor/Pipeline.cpp`,
wire it into the descriptor's execute slot, and return one of
`pnanovdb_pipeline_result_{success,skipped,no_data,error,pending}`:

```cpp
static pnanovdb_pipeline_result_t execute_<name>(
    pnanovdb_scene_object_t* obj, pnanovdb_pipeline_context_t* ctx);
```

## Async pipeline only

Add a worker (declared in `editor/PipelineRuntime.h`, implemented in the `.cpp`):

- Static `kPipelineType = pnanovdb_pipeline_type_<name>;`
- Override `pipeline_type()` and `handle_completion()`.
- `init()` captures compute/raster/EditorScene (render thread).
- Register at file scope with `PNANOVDB_REGISTER_WORKER(<Name>Worker)`.

How the worker is launched depends on the stage:

- **Load stage** — pass `nullptr` for the step-5
  `execute` and override `start_from_request()` so `pipeline_load` starts it.
- **Process stage** — keep a normal
  `execute_<name>` that builds the request, launches the worker, and returns
  `pnanovdb_pipeline_result_pending`.

The runtime runs at most one worker at a time; new work is deferred, so a worker
never handles concurrent runs.

### Async worker contract

On the render thread unless noted:

- `start()` — validate inputs, copy params, enqueue a worker-thread lambda that
  does the blocking work and stores results in `m_pending_*`.
- `handle_completion()` — gated by `isTaskCompleted`; validate, transfer
  ownership to the scene, **null the transferred `m_pending_*` pointers**, then
  `finish_task()`.
- `start_from_request()` — check `request.load_pipeline`, unpack
  `request.load_params` via the typed getters, forward to `start()`; return
  `false` for other types so `pipeline_load` probes the next worker.

### Async quality gates

- **Destructor cleanup**: free any untransferred `m_pending_*`, after
  `cancel_and_join()` joins the worker thread.
- **Reset pending output pointers** in `start()` and the non-success branches of
  `handle_completion()` to avoid silent overwrite + leak.
- **No `current_runtime()` outside a `RuntimeScope`**; wrap in `with_runtime_or_warn`.

## Example: shader-only render pipeline

To render a NanoVDB grid with a custom shader (reusing `NanoVDBRenderParams`), add
the enum + shader + shader.json, then 2 lines in `Pipeline.cpp`:

```cpp
PNANOVDB_DEFINE_PIPELINE_SHADERS(s_<name>_shaders,
    PNANOVDB_PIPELINE_SHADER("editor/<name>.slang", nullptr, PNANOVDB_TRUE));
PNANOVDB_REGISTER_NANOVDB_RENDER_PIPELINE(s_<name>_descriptor,
    pnanovdb_pipeline_type_<name>, "Name", s_<name>_shaders,
    pnanovdb_pipeline_data_kind_nanovdb);   // inputs
```

## Chaining process steps

The process stage can run several steps in sequence, each feeding the next.

### Registering a chain as a single pipeline

```cpp
static const pnanovdb_pipeline_type_t s_voxelbvh_rgba8_chain_steps[] = {
    pnanovdb_pipeline_type_voxelbvh_build,   // step 0: build the VoxelBVH grid
    pnanovdb_pipeline_type_voxelbvh_rgba8,   // step 1: convert it to RGBA8
};
PNANOVDB_REGISTER_PROCESS_CHAIN_PIPELINE(s_voxelbvh_rgba8_chain_descriptor,
                                         pnanovdb_pipeline_type_voxelbvh_rgba8_chain,
                                         "VoxelBVH + RGBA8",
                                         PNANOVDB_PIPELINE_CHAIN(s_voxelbvh_rgba8_chain_steps),
                                         pnanovdb_pipeline_data_kind_nanovdb_rgba8,        // outputs (final step)
                                         pnanovdb_pipeline_data_kind_mesh);                 // inputs (first step)
```

## Reference

| Example                                 | Stage   | Sync/Async | Files |
| --------------------------------------- | ------- | ---------- | ----- |
| `pnanovdb_pipeline_type_noop`           | load    | sync       | `editor/Pipeline.cpp` |
| `pnanovdb_pipeline_type_nanovdb_render` | render  | sync       | `editor/Pipeline.cpp` |
| `pnanovdb_pipeline_type_voxelbvh_build` | process | async-only | `editor/Pipeline.cpp`, `PipelineRuntime.{h,cpp}` |
| `pnanovdb_pipeline_type_voxelbvh_rgba8` | process | async-only | `editor/Pipeline.cpp`, `PipelineRuntime.{h,cpp}` |
| `pnanovdb_pipeline_type_voxelbvh_rgba8_chain` | process | chain | `editor/Pipeline.cpp` (template; expands to `voxelbvh_build` + `voxelbvh_rgba8`) |
| `pnanovdb_pipeline_type_mesh_load`      | load    | async-only | `PipelineRuntime.{h,cpp}`, `editor/Pipeline.cpp`, `editor/MeshImport.cpp` |
| `pnanovdb_pipeline_type_gaussian_load`  | load    | async-only | `PipelineRuntime.{h,cpp}`, `editor/Pipeline.cpp`, `editor/GaussianImport.cpp` |
