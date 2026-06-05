# Editor pipelines

## What is a pipeline?

A **pipeline** is the unit of work the editor runs on a scene object to turn data
into something renderable. Concretely it is a registered
`pnanovdb_pipeline_descriptor_t` indexed by a `pnanovdb_pipeline_type_t` enum
value (see `editor/PipelineTypes.h`). The descriptors live in
`editor/Pipeline.cpp`.

Every scene object pins one pipeline per **stage**, and the three stages run in
order:

| Stage       | Purpose                                                              | Example |
| ----------- | -------------------------------------------------------------------- | ------- |
| **load**    | Bring data in from disk / another source into a scene object.        | `mesh_load` reads a PLY into compute arrays. |
| **process** | Transform the loaded data into a GPU-renderable representation.      | `voxelbvh_build` builds a BVH from a mesh. |
| **render**  | Draw the processed data each frame with a shader / render method.    | `nanovdb_render` ray-marches a grid. |

The runtime drives all of this on the render thread:

- `pipeline_init()` sets up per-editor runtime state and the workers.
- `pipeline_load()` kicks off a load (usually from a UI entry point that
  builds a `PipelineLoadRequest`).
- `pipeline_update()` is called once per frame to advance work and surface
  progress.

Each descriptor's `execute` runs on the render thread. Cheap work happens
inline; long-running work is handed off to an `AsyncWorker` (one per kind of
background job) and polled by `pipeline_update()`.

## Sync vs async

This is the first decision when adding a pipeline — it determines which steps
below you implement.

| Flavor             | What it does                                                      | Where it lives                                                                        |
| ------------------ | ----------------------------------------------------------------- | ------------------------------------------------------------------------------------- |
| **Sync (execute)** | Returns from `execute()` on the same frame; cheap GPU/noop.       | `execute_xxx()` in `editor/Pipeline.cpp`.                                             |
| **Async (worker)** | Hands work to a background thread, polled by `pipeline_update()`. | `XxxWorker : AsyncWorker` in `PipelineRuntime.{h,cpp}` + descriptor in `Pipeline.cpp`. |

Go **sync** for microsecond work (e.g. swapping a render target). Go **async**
for disk, raster, or multi-second GPU work that would otherwise stall the render
thread.

## Shared steps (every pipeline)

Do these regardless of sync vs async.

```
[ ] 1. Enum value
       editor/PipelineTypes.h: pnanovdb_pipeline_type_<name> = <next-int>,
       (before pnanovdb_pipeline_type_count)

[ ] 2. Params struct (if the pipeline has tunable parameters)
       editor/Pipeline.cpp        (process-stage params used only inside Pipeline.cpp)
         OR editor/PipelineRuntime.h (load-stage params, so the worker's .cpp sees them too)
       struct <Name>Params { ... };  // C-friendly POD
       PNANOVDB_REFLECT_STRUCT_OPAQUE_IMPL(<Name>Params)  // in the same file, right after

[ ] 3. Typed getters/setters (only if code in another .cpp reads/writes the params)
       editor/PipelineRuntime.h:
       pipeline_params_get_<name>_<field>() / _set_<name>_<field>()
       (guard params->size, operate on the opaque blob)

[ ] 4. Param field descriptors (optional; only to expose params in the
       Properties panel)
       editor/Pipeline.cpp:
       static const pnanovdb_pipeline_param_field_t s_<name>_param_fields[] = {
           { "Label", "tooltip", PNANOVDB_REFLECT_TYPE_FLOAT,
             offsetof(<Name>Params, <field>), default, min, max, step,
             /*enum_labels*/ nullptr, /*enum_count*/ 0 },
       };
       Pass it in step 5 via PNANOVDB_PIPELINE_FIELDS(s_<name>_param_fields).
       Without this the params exist but the UI shows no widgets for them.

[ ] 5. Descriptor + registration
       editor/Pipeline.cpp: pick the define-and-register macro for your stage.
       Each one builds a static pnanovdb_pipeline_descriptor_t and self-registers
       it (no separate PNANOVDB_REGISTER_PIPELINE line, no central switch). Fill
       the params trio with PNANOVDB_PIPELINE_PARAMS(<Name>Params) (or
       PNANOVDB_PIPELINE_NO_PARAMS) and the param-field pair with
       PNANOVDB_PIPELINE_FIELDS(s_<name>_param_fields) (or PNANOVDB_PIPELINE_NO_FIELDS).

       // load: no shaders, no render method, params never mapped to the object
       PNANOVDB_REGISTER_LOAD_PIPELINE(
           s_<name>_descriptor, pnanovdb_pipeline_type_<name>, "Human-readable name",
           PNANOVDB_PIPELINE_PARAMS(<Name>Params), // or PNANOVDB_PIPELINE_NO_PARAMS
           execute_<name>);                        // or nullptr for async-only

       // process: params always map to SceneObject::process_params
       PNANOVDB_REGISTER_PROCESS_PIPELINE(
           s_<name>_descriptor, pnanovdb_pipeline_type_<name>, "Human-readable name",
           s_<name>_shaders, /*shader_count*/ 1,   // or nullptr, 0 for no shaders
           PNANOVDB_PIPELINE_PARAMS(<Name>Params),
           execute_<name>,                         // or nullptr for async-only
           get_render_method_<nanovdb|gaussian>,
           PNANOVDB_PIPELINE_FIELDS(s_<name>_param_fields)); // or PNANOVDB_PIPELINE_NO_FIELDS

       // render: you pick the render method and how params map to the object
       PNANOVDB_REGISTER_RENDER_PIPELINE(
           s_<name>_descriptor, pnanovdb_pipeline_type_<name>, "Human-readable name",
           s_<name>_shaders, /*shader_count*/ 1,
           PNANOVDB_PIPELINE_PARAMS(<Name>Params), // or PNANOVDB_PIPELINE_NO_PARAMS
           execute_<name>, get_render_method_<none|nanovdb|gaussian>,
           map_params<&SceneObject::render_params>); // or nullptr

       (pipeline_init / pipeline_load / pipeline_update need no edits.)

[ ] 6. UI entry point (optional)
       e.g. mesh_import::mesh() in editor/MeshImport.cpp builds a
       PipelineLoadRequest and calls pipeline_load.
```

## Sync pipeline only

A sync pipeline does all its work inside `execute()`. Add one more step on top
of the shared ones:

```
[ ] execute_<name>
       editor/Pipeline.cpp:
       static pnanovdb_pipeline_result_t execute_<name>(
           pnanovdb_scene_object_t* obj, pnanovdb_pipeline_context_t* ctx);
       Wire it into the descriptor's /*execute*/ slot (step 5). Return one of
       pnanovdb_pipeline_result_{success,skipped,no_data,error,pending}.
```

## Async pipeline only

An async pipeline offloads blocking work to a background worker. Pass `nullptr`
as the `execute` argument of the step 5 macro and add a worker:

```
[ ] <Name>Worker : AsyncWorker
       editor/PipelineRuntime.h (declaration) + .cpp (impl)
       Static: kPipelineType = pnanovdb_pipeline_type_<name>;
       Overrides: pipeline_type(), handle_completion().
       Load-stage workers also: start_from_request().
       Render-thread setup: init() (capture compute/raster/EditorScene).
       Register at file scope: PNANOVDB_REGISTER_WORKER(<Name>Worker);
```

The runtime runs at most one worker at a time; new work is deferred while a
worker is running, so your worker never has to handle concurrent runs.

### Async worker contract

For the worker, implement on the render thread unless noted:

- `start()` — validate inputs, copy params, enqueue a worker-thread lambda that
  does the blocking work and stores results in `m_pending_*`.
- `handle_completion()` — gated by `isTaskCompleted`; validate, transfer
  ownership to the scene, **null the transferred `m_pending_*` pointers**, then
  `finish_task()`.
- `start_from_request()` — adapter: check `request.load_pipeline`, unpack
  `request.load_params` via the typed getters, forward to `start()`; return
  `false` for other types so `pipeline_load` probes the next worker.

### Async quality gates

- **Destructor cleanup.** Free any `m_pending_*` resource allocated on the
  worker thread but not yet transferred. See `GaussianVoxelizeWorker`, `VoxelBVHWorker`.
- **`cancel_and_join()` before touching pending state.** Join the worker thread
  before the destructor reads/frees `m_pending_*`.
- **Reset pending output pointers in `start()` and the non-success branches of
  `handle_completion()`** to avoid silent overwrite + leak. See `GaussianSplatWorker`.
- **No `current_runtime()` outside an active `RuntimeScope`.** Wrap C-API shims
  in `with_runtime_or_warn`.
- **Prefer `init_params_t<MyParams>` and `map_params<&SceneObject::process_params>`**;
  hand-rolled `init_xxx_params` / `map_xxx_params` shims are dead code.

## Shortcut: shader-only render pipeline

A render-stage pipeline that renders a NanoVDB grid with a custom shader (reusing
`NanoVDBRenderParams` init/execute/map/render-method) skips the per-field macro.
Enum + shader + shader.json + these 2 lines in `Pipeline.cpp`:

```cpp
PNANOVDB_DEFINE_PIPELINE_SHADERS(s_<name>_shaders,
    PNANOVDB_PIPELINE_SHADER("editor/<name>.slang", nullptr, PNANOVDB_TRUE));
PNANOVDB_REGISTER_NANOVDB_RENDER_PIPELINE(s_<name>_descriptor,
    pnanovdb_pipeline_type_<name>, "Human-readable name", s_<name>_shaders);
```

## Reference

| Example                                 | Stage   | Sync/Async | Files                                                                                                        |
| --------------------------------------- | ------- | ---------- | ------------------------------------------------------------------------------------------------------------ |
| `pnanovdb_pipeline_type_noop`           | load    | sync       | `editor/Pipeline.cpp`                                                                                        |
| `pnanovdb_pipeline_type_nanovdb_render` | render  | sync       | `editor/Pipeline.cpp`                                                                                        |
| `pnanovdb_pipeline_type_voxelbvh_build` | process | sync+async | `editor/Pipeline.cpp`, `PipelineRuntime.{h,cpp}`                                                             |
| `pnanovdb_pipeline_type_mesh_load`      | load    | async-only | `PipelineRuntime.{h,cpp}`, `editor/Pipeline.cpp`, `editor/MeshImport.cpp`                                    |
| `pnanovdb_pipeline_type_gaussian_load`  | load    | async-only | `PipelineRuntime.{h,cpp}`, `editor/Pipeline.cpp`, `editor/GaussianImport.cpp`                                |
