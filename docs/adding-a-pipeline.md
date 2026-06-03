# Adding a new pipeline

This guide is the canonical recipe for adding a new pipeline type to the editor.
It is structured so both a human reviewer and an automated agent can follow it
end-to-end.

## Background

A "pipeline" is a registered `pnanovdb_pipeline_descriptor_t` indexed by a
`pnanovdb_pipeline_type_t` enum value. Each scene object pins one pipeline per
**stage** (`load`, `process`, `render`). The runtime invokes the descriptor's
`execute` function on the render thread; long-running tasks are handed off to
an `AsyncWorker` (one per kind of background job).

Two flavors of pipeline:

| Flavor                | What it does                            | Where it lives                              |
| --------------------- | --------------------------------------- | ------------------------------------------- |
| **Sync (execute)**    | Returns from `execute()` on the same frame; cheap GPU work or noop. | `execute_xxx()` in `editor/Pipeline.cpp`.   |
| **Async (worker)**    | Hands work to a background thread, polled per-frame by `pipeline_update()`. | `XxxWorker : AsyncWorker` in `PipelineRuntime.{h,cpp}` + descriptor in `Pipeline.cpp`. |

If your pipeline can finish in microseconds (e.g. swapping a render target), go
sync. If it touches disk, runs raster, or does multi-second GPU work, go async.

## Agent checklist

```
[ ] 1. Enum value
       file:  editor/PipelineTypes.h
       add:   pnanovdb_pipeline_type_<name> = <next-int>,
              (insert BEFORE pnanovdb_pipeline_type_count)

[ ] 2. Params struct (REQUIRED if pipeline has any tunable parameter)
       file:  editor/Pipeline.cpp  (process-stage params, this TU only)
         OR   editor/PipelineRuntime.h  (load-stage params shared with worker)
       add:   struct <Name>Params { ... }; with C-friendly POD fields
              PNANOVDB_REFLECT_STRUCT_OPAQUE_IMPL(<Name>Params)
              (immediately after the struct, in the same TU; this is the
              reflect tag the descriptor's params_data_type field references)

[ ] 3. Typed getters/setters for params (REQUIRED for callers outside this TU)
       file:  editor/PipelineRuntime.h
       add:   pipeline_params_get_<name>_<field>() / _set_<name>_<field>()
              These guard params->size and operate on the opaque blob.

[ ] 4. execute_<name> (REQUIRED for sync pipelines; set to nullptr for async-only)
       file:  editor/Pipeline.cpp
       sig:   static pnanovdb_pipeline_result_t execute_<name>(
                  pnanovdb_scene_object_t* obj, pnanovdb_pipeline_context_t* ctx);

[ ] 5. (async only) <Name>Worker
       file:  editor/PipelineRuntime.h  (declaration, inheriting AsyncWorker)
       file:  editor/PipelineRuntime.cpp  (implementation)
       Required overrides:
         - static constexpr pnanovdb_pipeline_type_t kPipelineType =
               pnanovdb_pipeline_type_<name>;
         - pnanovdb_pipeline_type_t pipeline_type() const override;
         - bool handle_completion() override;
       Required for load-stage workers (participate in pipeline_start_load):
         - bool start_from_request(const PipelineLoadRequest&,
                                   EditorSceneManager*,
                                   pnanovdb_editor_token_t*) override;
       Required for workers that need render-thread setup:
         - void init(const PipelineContext& ctx, EditorScene* editor_scene) override;
       Self-register at file scope in editor/PipelineRuntime.cpp:
         - PNANOVDB_REGISTER_WORKER(<Name>Worker);
       See Walkthrough B step 4 for what the macro does.

[ ] 6. Pipeline descriptor + auto-registration
       file:  editor/Pipeline.cpp
       template:
              static const pnanovdb_pipeline_descriptor_t s_<name>_descriptor = {
                  pnanovdb_pipeline_type_<name>,
                  pnanovdb_pipeline_stage_<load|process|render>,
                  "Human-readable name",
                  /*shaders*/ nullptr, /*shader_count*/ 0,
                  /*params_size*/ sizeof(<Name>Params),
                  /*params_data_type*/ PNANOVDB_REFLECT_DATA_TYPE(<Name>Params),
                  /*init_params*/ init_params_t<<Name>Params>,
                  /*execute*/ execute_<name>,   // or nullptr for async-only
                  /*get_render_method*/ get_render_method_<none|nanovdb|raster2d>,
                  /*map_params*/ map_params<&SceneObject::process_params>, // or render_params, or nullptr
                  /*param_fields*/ nullptr,        // (rare) array of Properties-panel fields
                  /*param_field_count*/ 0,
              };
              PNANOVDB_REGISTER_PIPELINE(s_<name>_descriptor);
       Central entry points (pipeline_init, pipeline_start_load,
       pipeline_update) iterate rt.workers() polymorphically -- no edits
       there for new pipelines.

[ ] 7. (optional public entry point) Add an import/start helper
       Example: mesh_import::mesh() in editor/MeshImport.cpp builds a
       PipelineLoadRequest{ load_pipeline = pnanovdb_pipeline_type_mesh_load }
       and calls pipeline_start_load.
```

## Walkthrough A: minimal sync pipeline (1 file touched)

Goal: a `process`-stage pipeline that has no params and just marks the scene
object processed.

1. `editor/PipelineTypes.h` — add `pnanovdb_pipeline_type_my_sync = N`.
2. `editor/Pipeline.cpp` — add:
   - `static pnanovdb_pipeline_result_t execute_my_sync(...)`.
   - `static const pnanovdb_pipeline_descriptor_t s_my_sync_descriptor = { ... };`
     with `init_params=nullptr`, `params_size=0`, `params_data_type=nullptr`.
   - `PNANOVDB_REGISTER_PIPELINE(s_my_sync_descriptor);` on the line below.

That's it — **2 files, 4 edit sites**. No worker, no PipelineRuntime touch, no
manual registration list.

## Walkthrough B: async load-stage pipeline (worker)

Use `MeshLoadWorker` (`pnanovdb_pipeline_type_mesh_load`) as the reference.
Touches **4 files** (enum, params header, worker impl, descriptor file), plus an
optional 5th if you add a public entry point.

1. Enum value in `PipelineTypes.h`.
2. Params struct + getters/setters in `PipelineRuntime.h`. Also add a
   `PNANOVDB_REFLECT_STRUCT_OPAQUE_IMPL(MeshLoadParams)` right after the
   struct so the descriptor can reference it by reflect data type rather
   than by typename string:
   ```cpp
   struct MeshLoadParams { float inflation_radius = 0.f; pnanovdb_uint32_t resolution = 511u; ... };
   PNANOVDB_REFLECT_STRUCT_OPAQUE_IMPL(MeshLoadParams)
   inline float pipeline_params_get_mesh_load_inflation_radius(const pnanovdb_pipeline_params_t*);
   inline bool  pipeline_params_set_mesh_load_inflation_radius(pnanovdb_pipeline_params_t*, float);
   // ... one pair per field
   ```
3. Worker declaration in `PipelineRuntime.h`:
   ```cpp
   class MeshLoadWorker : public AsyncWorker {
   public:
       static constexpr pnanovdb_pipeline_type_t kPipelineType = pnanovdb_pipeline_type_mesh_load;

       pnanovdb_pipeline_type_t pipeline_type() const override { return kPipelineType; }
       void init(const PipelineContext& ctx, EditorScene* editor_scene) override;
       bool start(const char* filepath, const MeshLoadParams& p, pnanovdb_editor_token_t* scene);
       bool handle_completion() override;
       bool start_from_request(const PipelineLoadRequest& request,
                               EditorSceneManager* scene_manager,
                               pnanovdb_editor_token_t* scene_token) override;
   protected:
       const char* progress_running_fallback_text() const override { return "Loading mesh..."; }
   private:
       // ...pending state nulled after transfer to scene to avoid double-free
   };
   ```
4. Self-register the worker in `PipelineRuntime.cpp` with **one line**:
   ```cpp
   PNANOVDB_REGISTER_WORKER(MeshLoadWorker);
   ```
   The macro pushes a factory into a static-init registry that
   `PipelineRuntime`'s constructor walks to build
   `std::vector<std::unique_ptr<AsyncWorker>> m_workers`. No edits to the
   constructor or destructor; `cancel_and_join()` runs via the generic
   shutdown loop. Process-stage entry points that need typed access do
   `rt.worker<MeshLoadWorker>()` instead of a per-pipeline accessor.
5. Implement `start()`, `handle_completion()`, and `start_from_request()` in
   `PipelineRuntime.cpp`.
   - `start()` runs on the render thread: validate inputs, copy params,
     enqueue a worker-thread lambda that performs the blocking work and stores
     results into `m_pending_*` members.
   - `handle_completion()` runs on the render thread: gated by
     `isTaskCompleted`, validates results, transfers ownership to the editor
     scene, **nulls the raw `m_pending_*` pointers**, then calls
     `finish_task()`. Forgetting to null transferred pointers causes a
     double-free in the destructor.
   - `start_from_request()` is a thin adapter -- checks
     `request.load_pipeline == pnanovdb_pipeline_type_<your_type>`, unpacks
     `request.load_params` via the typed getters, and forwards to `start()`.
     Returning `false` for other types lets `pipeline_start_load` probe the
     next worker.
6. `editor/Pipeline.cpp`:
   - `s_mesh_load_descriptor` with stage `pnanovdb_pipeline_stage_load`,
     `execute=nullptr` (async-only), `params_size=sizeof(MeshLoadParams)`,
     `init_params=init_params_t<MeshLoadParams>`.
   - `PNANOVDB_REGISTER_PIPELINE(s_mesh_load_descriptor);` directly below.
   - **No edit to `pipeline_init`, `pipeline_start_load`, or `pipeline_update`**
     -- they iterate `rt.workers()` polymorphically. Dispatch happens via the
     virtual `init` (one-time render-thread setup), `start_from_request`
     (load-stage entry) and `handle_completion` (per-frame polling).
7. (optional) public entry point — see `editor/MeshImport.cpp` for the canonical
   pattern: build `pnanovdb_pipeline_params_t` via the typed setters, build a
   `PipelineLoadRequest`, call `pipeline_start_load`, log on failure.

## Quality gates

The checklist tells you what to wire up. These gates cover the subtleties --
mostly threading and ownership -- that mechanical wiring won't catch:

- **Destructor cleanup.** Free any `m_pending_*` GPU resource you allocated on
  the worker thread but did not yet transfer to the scene. Pattern:
  `Raster3DWorker::~Raster3DWorker` and `VoxelBVHWorker::~VoxelBVHWorker`.
- **`cancel_and_join()` before touching pending state.** Always join the
  worker thread *before* the destructor reads or frees `m_pending_*`; the
  worker may still be writing to them.
- **Reset pending output pointers in both `start()` and the non-success
  branches of `handle_completion()`.** Otherwise a failed task plus a
  successful next task silently overwrites and permanently leaks the previous
  allocation. See `Raster2DWorker` for the canonical defensive pattern.
- **No `current_runtime()` outside an active `RuntimeScope`.** Public C-API
  shims must wrap their use of `current_runtime()` in `with_runtime_or_warn`
  so missing-scope bugs surface as a single readable log line.
- **Prefer `init_params_t<MyParams>` and
  `map_params<&SceneObject::process_params>`** in the descriptor. Hand-rolled
  `init_xxx_params` / `map_xxx_params` shims are dead code now.

## Reference

Canonical examples in this repo:

| Example                                  | Stage   | Sync/Async | Files to look at                                                       |
| ---------------------------------------- | ------- | ---------- | ---------------------------------------------------------------------- |
| `pnanovdb_pipeline_type_noop`            | load    | sync       | `editor/Pipeline.cpp` (descriptor + execute)                           |
| `pnanovdb_pipeline_type_nanovdb_render`  | render  | sync       | `editor/Pipeline.cpp`                                                  |
| `pnanovdb_pipeline_type_voxelbvh_build`  | process | sync+async | `editor/Pipeline.cpp` (execute), `PipelineRuntime.{h,cpp}` (worker)    |
| `pnanovdb_pipeline_type_mesh_load`       | load    | async-only | `PipelineRuntime.{h,cpp}` (worker), `editor/Pipeline.cpp` (descriptor), `editor/MeshImport.cpp` (entry point) |

