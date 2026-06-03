# Editor pipelines

## What is a pipeline?

A **pipeline** is the unit of work the editor runs on a scene object to turn data
into something renderable. Concretely it is a registered
`pnanovdb_pipeline_descriptor_t` indexed by a `pnanovdb_pipeline_type_t` enum
value (see `editor/PipelineTypes.h`).

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
       editor/Pipeline.cpp        (process-stage params, this TU only)
         OR editor/PipelineRuntime.h (load-stage params shared with the worker)
       struct <Name>Params { ... };  // C-friendly POD
       PNANOVDB_REFLECT_STRUCT_OPAQUE_IMPL(<Name>Params)  // same TU, right after

[ ] 3. Typed getters/setters (if callers outside this TU touch the params)
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
       Wire into the descriptor's param_fields / param_field_count (step 5).
       Without this the params exist but the UI shows no widgets for them.

[ ] 5. Descriptor + registration
       editor/Pipeline.cpp:
       static const pnanovdb_pipeline_descriptor_t s_<name>_descriptor = {
           pnanovdb_pipeline_type_<name>,
           pnanovdb_pipeline_stage_<load|process|render>,
           "Human-readable name",
           /*shaders*/ nullptr, /*shader_count*/ 0,
           /*params_size*/ sizeof(<Name>Params),
           /*params_data_type*/ PNANOVDB_REFLECT_DATA_TYPE(<Name>Params),
           /*init_params*/ init_params_t<<Name>Params>,
           /*execute*/ execute_<name>,   // or nullptr for async-only
           /*get_render_method*/ get_render_method_<none|nanovdb|gaussian>,
           /*map_params*/ map_params<&SceneObject::process_params>, // or render_params / nullptr
           /*param_fields*/ s_<name>_param_fields, // or nullptr
           /*param_field_count*/ sizeof(s_<name>_param_fields) / sizeof(s_<name>_param_fields[0]), // or 0
       };
       PNANOVDB_REGISTER_PIPELINE(s_<name>_descriptor);
       pipeline_init / pipeline_load / pipeline_update iterate workers
       polymorphically -- no edits there.

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

An async pipeline offloads blocking work to a background worker. Set the
descriptor's `/*execute*/` slot to `nullptr` (step 5) and add a worker:

```
[ ] <Name>Worker : AsyncWorker
       editor/PipelineRuntime.h (declaration) + .cpp (impl)
       Static: kPipelineType = pnanovdb_pipeline_type_<name>;
       Overrides: pipeline_type(), handle_completion().
       Load-stage workers also: start_from_request().
       Render-thread setup: init() (capture compute/raster/EditorScene).
       Register at file scope: PNANOVDB_REGISTER_WORKER(<Name>Worker);
```

The `PipelineRuntime` (owned by the editor for the duration of `show()`) builds
one instance of every `PNANOVDB_REGISTER_WORKER`-ed worker, and `pipeline_load`
/ `pipeline_update` dispatch to them by `pipeline_type()` — no central switch to
edit.

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
`NanoVDBRenderParams` init/execute/map/render-method) needs no descriptor
aggregate. Enum + shader + shader.json + these 3 lines in `Pipeline.cpp`:

```cpp
PNANOVDB_DEFINE_PIPELINE_SHADERS(s_<name>_shaders,
    PNANOVDB_PIPELINE_SHADER("editor/<name>.slang", nullptr, PNANOVDB_TRUE));
PNANOVDB_DEFINE_NANOVDB_RENDER_PIPELINE(s_<name>_descriptor,
    pnanovdb_pipeline_type_<name>, "Human-readable name", s_<name>_shaders);
PNANOVDB_REGISTER_PIPELINE(s_<name>_descriptor);
```

## Reference

| Example                                 | Stage   | Sync/Async | Files                                                                                                        |
| --------------------------------------- | ------- | ---------- | ------------------------------------------------------------------------------------------------------------ |
| `pnanovdb_pipeline_type_noop`           | load    | sync       | `editor/Pipeline.cpp`                                                                                        |
| `pnanovdb_pipeline_type_nanovdb_render` | render  | sync       | `editor/Pipeline.cpp`                                                                                        |
| `pnanovdb_pipeline_type_voxelbvh_build` | process | sync+async | `editor/Pipeline.cpp`, `PipelineRuntime.{h,cpp}`                                                             |
| `pnanovdb_pipeline_type_mesh_load`      | load    | async-only | `PipelineRuntime.{h,cpp}`, `editor/Pipeline.cpp`, `editor/MeshImport.cpp`                                    |
