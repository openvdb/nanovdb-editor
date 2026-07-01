// Copyright Contributors to the OpenVDB Project
// SPDX-License-Identifier: Apache-2.0

/*!
    \file   nanovdb_editor/editor/SceneObject.h

    \author Petra Hapalova

    \brief
*/

#ifndef NANOVDB_EDITOR_SCENE_OBJECT_H_HAS_BEEN_INCLUDED
#define NANOVDB_EDITOR_SCENE_OBJECT_H_HAS_BEEN_INCLUDED

#include "EditorToken.h"
#include "Renderer.h"
#include "PipelineTypes.h"
#include "nanovdb_editor/putil/Camera.h"
#include "nanovdb_editor/putil/Editor.h"
#include "nanovdb_editor/putil/Raster.h"
#include "nanovdb_editor/putil/Reflect.h"

#include <cstdlib>
#include <cstring>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#if defined(_WIN32)
#    if defined(pnanovdbeditor_EXPORTS)
#        define PNANOVDB_SCENE_MANAGER_EXPORT_CXX __declspec(dllexport)
#    else
#        define PNANOVDB_SCENE_MANAGER_EXPORT_CXX __declspec(dllimport)
#    endif
#else
#    define PNANOVDB_SCENE_MANAGER_EXPORT_CXX __attribute__((visibility("default")))
#endif

namespace pnanovdb_editor
{

/*!
    \brief Scene object types that can be managed
*/
enum class SceneObjectType
{
    Uninitialized, ///< Placeholder: configured but no resource attached yet
    NanoVDB, ///< Volume data
    GaussianData, ///< Gaussian splatting data
    Array, ///< Generic array data
    Camera ///< Camera view
};

/*!
    \brief Resource data for a scene object
*/
struct SceneObjectResources
{
    // Unnamed primary data (one active based on object type)
    pnanovdb_compute_array_t* nanovdb_array = nullptr;
    pnanovdb_raster_gaussian_data_t* gaussian_data = nullptr;
    pnanovdb_camera_view_t* camera_view = nullptr;

    // Named arrays - multiple arrays identified by name
    std::map<std::string, pnanovdb_compute_array_t*> named_arrays;

    // Snapshot that came directly from source-file loading
    std::map<std::string, pnanovdb_compute_array_t*> file_backed_named_arrays;

    // Converted/processed data (output of process pipeline)
    pnanovdb_compute_array_t* converted_nanovdb = nullptr;

    // Source file path (for re-conversion from file with different parameters)
    std::string source_filepath;

    // Ownership handles for automatic cleanup
    std::shared_ptr<pnanovdb_compute_array_t> nanovdb_array_owner;
    std::shared_ptr<pnanovdb_raster_gaussian_data_t> gaussian_data_owner;
    std::shared_ptr<pnanovdb_camera_view_t> camera_view_owner;
    std::shared_ptr<pnanovdb_compute_array_t> converted_nanovdb_owner;
    std::map<std::string, std::shared_ptr<pnanovdb_compute_array_t>> named_array_owners;
};

/*!
    \brief Mutable storage for a SceneObject's shader name
*/
struct ShaderNameStorage
{
    uint64_t object_key = 0;
    pnanovdb_editor_shader_name_t value = {};
};

/*!
    \brief Compile-time known parameters for a scene object
*/
struct SceneObjectParams
{
    // GPU-backed shader params storage
    pnanovdb_compute_array_t* shader_params_array = nullptr;
    std::shared_ptr<pnanovdb_compute_array_t> shader_params_array_owner;

    // Typed params pointer and reflection info
    void* shader_params = nullptr;
    const pnanovdb_reflect_data_type_t* shader_params_data_type = nullptr;

    // Associated shader name
    std::shared_ptr<ShaderNameStorage> shader_name_storage = std::make_shared<ShaderNameStorage>();
};

/*!
    \brief Per-shader parameter override
*/
struct ShaderOverride
{
    std::string shader_name; // Override shader name (empty = use pipeline default)

    // Dynamic parameter overrides: param_name -> serialized value
    std::map<std::string, std::vector<uint8_t>> param_overrides;

    bool has_shader_override() const
    {
        return !shader_name.empty();
    }
    bool has_param_overrides() const
    {
        return !param_overrides.empty();
    }
    bool is_empty() const
    {
        return !has_shader_override() && !has_param_overrides();
    }
};

inline constexpr const char* k_stage_output_nanovdb = "nanovdb";

/*!
    \brief Non-destructive snapshot of the resources a single stage produced
*/
struct StageOutput
{
    // Arrays this stage produced, keyed by resource name (e.g. k_stage_output_nanovdb).
    std::map<std::string, pnanovdb_compute_array_t*> arrays;
    std::map<std::string, std::shared_ptr<pnanovdb_compute_array_t>> array_owners;

    // Gaussian data this stage produced (load stage for imported splats).
    pnanovdb_raster_gaussian_data_t* gaussian = nullptr;
    std::shared_ptr<pnanovdb_raster_gaussian_data_t> gaussian_owner;

    bool empty() const
    {
        return arrays.empty() && gaussian == nullptr;
    }

    void clear()
    {
        arrays.clear();
        array_owners.clear();
        gaussian = nullptr;
        gaussian_owner.reset();
    }

    void set_array(const std::string& key, pnanovdb_compute_array_t* array, std::shared_ptr<pnanovdb_compute_array_t> owner)
    {
        if (array)
        {
            arrays[key] = array;
            array_owners[key] = std::move(owner);
        }
        else
        {
            arrays.erase(key);
            array_owners.erase(key);
        }
    }

    pnanovdb_compute_array_t* get_array(const std::string& key) const
    {
        auto it = arrays.find(key);
        return it != arrays.end() ? it->second : nullptr;
    }

    std::shared_ptr<pnanovdb_compute_array_t> get_array_owner(const std::string& key) const
    {
        auto it = array_owners.find(key);
        return it != array_owners.end() ? it->second : nullptr;
    }
};

/*!
    \brief Configuration for a single pipeline stage

    Owns the heap-allocated params.data via malloc/free.
*/
struct PipelineStage
{
    pnanovdb_pipeline_type_t type = pnanovdb_pipeline_type_noop;
    pnanovdb_pipeline_params_t params = {}; // Heap-allocated stage params (data owned by this struct)

    // Per-shader overrides (indexed by shader position in pipeline descriptor)
    std::vector<ShaderOverride> shader_overrides;

    // Non-destructive snapshot of resources this stage produced
    StageOutput output;

    bool dirty = true; // Needs re-execution

    // Changes whenever this stage is invalidated. Async workers capture this
    // value so results from an older configuration cannot replace newer work.
    uint64_t revision = 1;

    // Set once the user explicitly configures this stage via set_pipeline() or map_pipeline_params()
    bool configured = false;

    void bump_revision()
    {
        if (++revision == 0)
        {
            revision = 1;
        }
    }

    // Deep-copy the heap-allocated params blob. Returns a zero-initialized
    // params (data == nullptr) when there is nothing to copy or the allocation
    // fails, so callers can detect failure via (src had data && result has none).
    static pnanovdb_pipeline_params_t clone_params(const pnanovdb_pipeline_params_t& src)
    {
        pnanovdb_pipeline_params_t out = {};
        if (src.data && src.size > 0)
        {
            out.data = malloc((size_t)src.size);
            if (out.data)
            {
                memcpy(out.data, src.data, (size_t)src.size);
                out.size = src.size;
                out.type = src.type;
            }
        }
        return out;
    }

    PipelineStage() = default;

    ~PipelineStage()
    {
        free(params.data);
    }

    PipelineStage(const PipelineStage& other)
        : type(other.type),
          params(clone_params(other.params)),
          shader_overrides(other.shader_overrides),
          output(other.output),
          dirty(other.dirty),
          revision(other.revision),
          configured(other.configured)
    {
        if (other.params.data && other.params.size > 0 && !params.data)
        {
            // Keep object in a fully default/safe state if deep-copy allocation fails
            type = pnanovdb_pipeline_type_noop;
            shader_overrides.clear();
            output.clear();
            dirty = true;
            configured = false;
        }
    }

    PipelineStage& operator=(const PipelineStage& other)
    {
        if (this != &other)
        {
            pnanovdb_pipeline_params_t new_params = clone_params(other.params);
            if (other.params.data && other.params.size > 0 && !new_params.data)
            {
                // Allocation failed: keep current object unchanged
                return *this;
            }

            free(params.data);
            params = new_params;
            type = other.type;
            shader_overrides = other.shader_overrides;
            output = other.output;
            dirty = other.dirty;
            configured = other.configured;
            bump_revision();
        }
        return *this;
    }

    PipelineStage(PipelineStage&& other) noexcept
        : type(other.type),
          params(other.params),
          shader_overrides(std::move(other.shader_overrides)),
          output(std::move(other.output)),
          dirty(other.dirty),
          revision(other.revision),
          configured(other.configured)
    {
        other.params = {};
    }

    PipelineStage& operator=(PipelineStage&& other) noexcept
    {
        if (this != &other)
        {
            free(params.data);
            type = other.type;
            params = other.params;
            shader_overrides = std::move(other.shader_overrides);
            output = std::move(other.output);
            dirty = other.dirty;
            configured = other.configured;
            other.params = {};
            bump_revision();
        }
        return *this;
    }
};

struct ProcessRunSnapshot
{
    int from_step = 0;
    int active_process_step = 0;

    PipelineStage render_stage;
    std::shared_ptr<ShaderNameStorage> shader_name_storage;
    pnanovdb_editor_shader_name_t shader_name_value = {};
    pnanovdb_compute_array_t* shader_params_array = nullptr;
    std::shared_ptr<pnanovdb_compute_array_t> shader_params_array_owner;
    void* shader_params = nullptr;
    const pnanovdb_reflect_data_type_t* shader_params_data_type = nullptr;
    std::vector<StageOutput> step_outputs;
    std::vector<bool> step_dirty;
};

/*!
    \brief Pipeline configuration for a scene object
*/
struct SceneObjectPipeline
{
    PipelineStage stages[pnanovdb_pipeline_stage_count];
    std::vector<PipelineStage> extra_process;
    int active_process_step = 0;
    bool drop_intermediate = false;
    std::optional<ProcessRunSnapshot> process_run_snapshot;
    bool process_user_cancel_requested = false;

    // Convenience accessors
    PipelineStage& load()
    {
        return stages[pnanovdb_pipeline_stage_load];
    }
    PipelineStage& process()
    {
        return stages[pnanovdb_pipeline_stage_process];
    }
    PipelineStage& render()
    {
        return stages[pnanovdb_pipeline_stage_render];
    }
    const PipelineStage& load() const
    {
        return stages[pnanovdb_pipeline_stage_load];
    }
    const PipelineStage& process() const
    {
        return stages[pnanovdb_pipeline_stage_process];
    }
    const PipelineStage& render() const
    {
        return stages[pnanovdb_pipeline_stage_render];
    }

    size_t process_count() const
    {
        return 1 + extra_process.size();
    }
    PipelineStage& process_step(size_t i)
    {
        return (i == 0) ? stages[pnanovdb_pipeline_stage_process] : extra_process[i - 1];
    }
    const PipelineStage& process_step(size_t i) const
    {
        return (i == 0) ? stages[pnanovdb_pipeline_stage_process] : extra_process[i - 1];
    }
    size_t append_process_step()
    {
        extra_process.emplace_back();
        return extra_process.size();
    }
};

enum class SceneObjectSourceKind
{
    None,
    NanoVDB,
    GaussianData,
    GaussianFile,
    GaussianArrays,
    MeshTriangles,
    MeshLines,
};

/*!
    \brief A single object in the scene

    Represents one object that can be tracked by the scene manager.

    \note The scene_object operation methods (resolve_resources(), source_kind(),
          invalidate_process_from(), ...) are defined in SceneObject.cpp and
          are NOT thread-safe; callers must hold the EditorSceneManager mutex (e.g.
          from inside with_object()/for_each_object()).
*/
struct PNANOVDB_SCENE_MANAGER_EXPORT_CXX SceneObject
{
    uint64_t lifetime_id = 0; ///< Unique identity for this object's current lifetime
    SceneObjectType type = SceneObjectType::Uninitialized; ///< Type of scene object
    pnanovdb_editor_token_t* scene_token; ///< Scene identifier token
    pnanovdb_editor_token_t* name_token; ///< Object name token

    SceneObjectResources resources; ///< Binary data (files)
    SceneObjectParams params; ///< Compile-time schemas (JSON)
    SceneObjectPipeline pipeline; ///< Dynamic overrides (JSON overrides)

    bool visible = true;

    // Resources
    pnanovdb_compute_array_t*& nanovdb_array()
    {
        return resources.nanovdb_array;
    }
    pnanovdb_raster_gaussian_data_t*& gaussian_data()
    {
        return resources.gaussian_data;
    }
    pnanovdb_camera_view_t*& camera_view()
    {
        return resources.camera_view;
    }
    std::map<std::string, pnanovdb_compute_array_t*>& named_arrays()
    {
        return resources.named_arrays;
    }
    pnanovdb_compute_array_t*& converted_nanovdb()
    {
        return resources.converted_nanovdb;
    }

    // Params
    void*& shader_params()
    {
        return params.shader_params;
    }
    void* shader_params() const
    {
        return params.shader_params;
    }
    const pnanovdb_reflect_data_type_t*& shader_params_data_type()
    {
        return params.shader_params_data_type;
    }
    const pnanovdb_reflect_data_type_t* shader_params_data_type() const
    {
        return params.shader_params_data_type;
    }
    pnanovdb_editor_token_t*& shader_name()
    {
        return params.shader_name_storage->value.shader_name;
    }
    pnanovdb_editor_token_t* shader_name() const
    {
        return params.shader_name_storage->value.shader_name;
    }
    ShaderNameStorage& ensure_shader_name_storage()
    {
        if (!params.shader_name_storage)
        {
            params.shader_name_storage = std::make_shared<ShaderNameStorage>();
        }
        return *params.shader_name_storage;
    }

    // Pipeline shortcuts
    pnanovdb_pipeline_type_t& load_pipeline()
    {
        return pipeline.load().type;
    }
    pnanovdb_pipeline_type_t& process_pipeline()
    {
        return pipeline.process().type;
    }
    pnanovdb_pipeline_type_t& render_pipeline()
    {
        return pipeline.render().type;
    }
    pnanovdb_pipeline_params_t& load_params()
    {
        return pipeline.load().params;
    }
    pnanovdb_pipeline_params_t& process_params()
    {
        return pipeline.process().params;
    }
    pnanovdb_pipeline_params_t& render_params()
    {
        return pipeline.render().params;
    }
    bool& process_dirty()
    {
        return pipeline.process().dirty;
    }

    // --- Operations (defined in SceneObject.cpp) ---
    // These mutate/query the object's pipeline chain and derived resources.
    // Precondition: caller holds the EditorSceneManager mutex.

    // Recompute the object's resource pointers/owners from the configured
    // pipeline stage outputs (load -> process chain -> render).
    void resolve_resources();

    // Clear all source/derived data and ownership handles back to empty.
    void reset_source();

    // Replace the named-array bindings (and their ownership handles).
    void set_named_array_bindings(const std::map<std::string, pnanovdb_compute_array_t*>& arrays,
                                  const std::map<std::string, std::shared_ptr<pnanovdb_compute_array_t>>& owners);

    // Index of the first dirty, non-noop process step (or -1 if none).
    int next_dirty_process_step() const;

    // Earliest process step that must re-run so \p requested_step has valid input.
    int process_rebuild_start(size_t requested_step) const;

    // Concrete source representation of this object.
    SceneObjectSourceKind source_kind() const;

    // Intrinsic data kind retained by the object, independent of pipeline stages.
    pnanovdb_uint32_t native_data_kind();

    // Data kind feeding pipeline stage \p step (0 = before first process step).
    pnanovdb_uint32_t upstream_data_kind(size_t step);

    // Data kind of the newest process step with output (else load/native data).
    pnanovdb_uint32_t renderable_data_kind();

    // Default renderer for \p kind, refined by this object's intrinsic source.
    pnanovdb_pipeline_type_t default_render_pipeline(pnanovdb_uint32_t kind) const;

    // Switch the render pipeline to match renderable_data_kind() when the current
    // one is incompatible.
    void sync_render_to_chain();

    void clear_process_run_snapshot();
    void process_user_cancel();
    void clear_process_cancel_state();
    void restore_process_run_snapshot();

    // Snapshot then invalidate the process chain from \p from onward (re-runnable).
    void invalidate_process_from(int from);

    // Invalidate process configuration from \p from onward (no rollback snapshot).
    void invalidate_process_configuration_from(size_t from);

    // Remove extra process step \p step (>0); returns false if out of range.
    bool remove_process_step(size_t step);

    // Mark the first non-noop process step dirty and bump all step revisions.
    void mark_process_dirty();

    // Advance the active process step after a (successful or failed) run.
    void advance_process_chain(bool success);

    // Clear the dirty flag of a running step without taking a rollback snapshot.
    void cancel_running_process_step_without_snapshot(size_t step)
    {
        if (step < pipeline.process_count())
        {
            pipeline.process_step(step).dirty = false;
        }
    }
};

} // namespace pnanovdb_editor

PNANOVDB_CAST_PAIR(pnanovdb_scene_object_t, pnanovdb_editor::SceneObject)

#endif // NANOVDB_EDITOR_SCENE_OBJECT_H_HAS_BEEN_INCLUDED
