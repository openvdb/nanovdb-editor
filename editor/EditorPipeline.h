// Copyright Contributors to the OpenVDB Project
// SPDX-License-Identifier: Apache-2.0

/*!
    \file   nanovdb_editor/editor/EditorPipeline.h

    \author Andrew Reidmeyer

    \brief  Pipeline system for NanoVDB Editor - conversion and rendering pipelines.
*/

#ifndef NANOVDB_EDITOR_PIPELINE_H_HAS_BEEN_INCLUDED
#define NANOVDB_EDITOR_PIPELINE_H_HAS_BEEN_INCLUDED

#include "EditorToken.h"
#include "nanovdb_editor/putil/Editor.h"
#include "nanovdb_editor/putil/Raster.h"
#include "nanovdb_editor/putil/Shader.hpp"
#include "nanovdb_editor/putil/WorkerThread.hpp"
#include <vector>
#include <string>
#include <memory>
#include <functional>
#include <map>

namespace pnanovdb_editor
{

// Forward declarations
enum class SceneObjectType;
struct NamedComponent;

/*!
    \brief Pipeline types supported by the editor
*/
enum class PipelineType
{
    Null, ///< No-op pipeline (pass-through)
    Render, ///< Rendering pipeline (renderable data -> 2D image)
    Raster3D, ///< 3D rasterization (Gaussian data -> NanoVDB)
    FileImport, ///< File import pipeline (file -> Gaussian or NanoVDB)
};

/*!
    \brief Status of pipeline execution
*/
enum class PipelineStatus
{
    NotRun, ///< Pipeline has never been run
    Running, ///< Pipeline is currently executing
    Completed, ///< Pipeline execution completed successfully
    Failed, ///< Pipeline execution failed
    Dirty, ///< Input or parameters changed, needs re-run
};

/*!
    \brief Shader reference within a pipeline

    A pipeline can have multiple shaders that execute in sequence.
    Each shader can be overridden by providing a custom shader path.

    Shader parameters support reflection via ShaderParameter (from compiled shader JSON).
    This is the source of truth for what parameters exist in the shader.
    Default values are loaded from JSON, then can be overridden by the scene object.
*/
struct PipelineShader
{
    std::string shader_path; ///< Path to shader file (e.g., "raster/gaussian_rasterize_2d.slang")
    std::string shader_entry_point; ///< Entry point function name (e.g., "main")
    pnanovdb_editor_token_t* shader_name_token = nullptr; ///< Shader name token (for fast lookup)

    // Is this shader overridden from defaults?
    bool is_overridden = false;

    // Dynamic parameter backing storage for this shader
    pnanovdb_compute_array_t* params_array = nullptr; ///< Per-shader parameters (backing storage)
    void* params = nullptr; ///< Pointer to raw params data
    std::shared_ptr<pnanovdb_compute_array_t> params_array_owner;

    // Reflection: ShaderParameter from compiled shader JSON is the source of truth
    std::string params_json_name; ///< Shader name for loading params (empty = no params)
    std::vector<pnanovdb_shader::ShaderParameter> parameters; ///< Source of truth for reflection
    const pnanovdb_reflect_data_type_t* params_reflect_type = nullptr; ///< For backward-compatible copy

    /*!
        \brief Check if this shader has parameters
        \return true if parameters are defined
    */
    bool has_parameters() const
    {
        return !parameters.empty();
    }

    /*!
        \brief Create a shader with path and entry point
    */
    static PipelineShader create(const char* path, const char* entry_point = "main")
    {
        PipelineShader shader;
        shader.shader_path = path ? path : "";
        shader.shader_entry_point = entry_point ? entry_point : "main";
        shader.params_json_name = shader.shader_path; // Default: params JSON matches shader path
        return shader;
    }
};

/*!
    \brief Parameters for Raster3D pipeline (Gaussian -> NanoVDB conversion)
*/
struct Raster3DParams
{
    float voxel_size = 1.f / 128.f; ///< Size of voxels in the output NanoVDB grid
};

/*!
    \brief Base pipeline configuration

    Pipelines have:
    - Parameters to control behavior (via reflection, not fixed structs)
    - A list of shaders used in the pipeline (possible to override)
    - Parameters for those shaders

    Parameters support reflection and can be extended/changed without recompilation.
    Default values are loaded from JSON, then can be overridden by scene object values.

    For Gaussian splatting pipeline specifically, defaults come from JSON first,
    then are assigned from the scene object settings.
*/
struct PipelineConfig
{
    PipelineType type = PipelineType::Null; ///< Type of pipeline
    pnanovdb_editor_token_t* name_token = nullptr; ///< Optional name for multiple pipelines of same type

    // Scene/object identification (for cleanup)
    pnanovdb_editor_token_t* scene_token = nullptr; ///< Scene this pipeline belongs to
    pnanovdb_editor_token_t* object_token = nullptr; ///< Object this pipeline belongs to

    // ========================================================================
    // SHADER PARAMETERS (reflection from compiled shader JSON)
    // ========================================================================

    // Shader name for loading parameters (e.g., "raster/gaussian_rasterize_2d.slang")
    std::string params_json_name; ///< Shader name for loading params

    // ShaderParameter from compiled shader JSON - source of truth for what params exist
    std::vector<pnanovdb_shader::ShaderParameter> parameters; ///< Parameter definitions from shader

    // Backing storage for parameter values (shared with scene object)
    pnanovdb_compute_array_t* params_array = nullptr; ///< Pipeline parameters (backing storage)
    void* params = nullptr; ///< Pointer to raw params data
    std::shared_ptr<pnanovdb_compute_array_t> params_array_owner; ///< Shared ownership with scene object

    // Reflection type for backward-compatible copy (tracks struct version)
    // Use pnanovdb_reflect_copy_by_name() when copying params across versions
    const pnanovdb_reflect_data_type_t* params_reflect_type = nullptr;

    // ========================================================================
    // SHADERS (can be overridden)
    // ========================================================================

    // List of shaders used in this pipeline, executed in order.
    // Default shaders are set by create_default_pipelines().
    // Users can override individual shaders via set_shader().
    std::vector<PipelineShader> shaders;

    /*!
        \brief Add a shader to the pipeline
        \param shader The shader to add
    */
    void add_shader(const PipelineShader& shader)
    {
        shaders.push_back(shader);
    }

    /*!
        \brief Override a shader at a specific index
        \param index Shader index (0-based)
        \param shader_path Path to the new shader
        \param entry_point Entry point function name
        \return true if successful, false if index out of range
    */
    bool set_shader(size_t index, const char* shader_path, const char* entry_point = "main")
    {
        if (index >= shaders.size())
        {
            return false;
        }
        shaders[index].shader_path = shader_path ? shader_path : "";
        shaders[index].shader_entry_point = entry_point ? entry_point : "main";
        shaders[index].params_json_name = shaders[index].shader_path;
        shaders[index].is_overridden = true;
        return true;
    }

    /*!
        \brief Get shader at index
        \param index Shader index (0-based)
        \return Pointer to shader, or nullptr if index out of range
    */
    PipelineShader* get_shader(size_t index)
    {
        return index < shaders.size() ? &shaders[index] : nullptr;
    }

    const PipelineShader* get_shader(size_t index) const
    {
        return index < shaders.size() ? &shaders[index] : nullptr;
    }

    /*!
        \brief Get number of shaders in this pipeline
    */
    size_t shader_count() const
    {
        return shaders.size();
    }

    // Execution state
    PipelineStatus status = PipelineStatus::NotRun;
    bool auto_execute = true; ///< Whether to auto-execute when inputs change
    bool needs_run = false; ///< Explicitly marked as needing execution

    // Input/output references (stored as component names)
    std::vector<pnanovdb_editor_token_t*> input_components; ///< Input component names
    std::vector<pnanovdb_editor_token_t*> output_components; ///< Output component names

    // ========================================================================
    // NAMED ARRAY REFERENCES (bonus arrays)
    // ========================================================================

    // Generic named array references for pipelines that need additional input arrays.
    // The scene object can have multiple named arrays (added via add_named_array()).
    // Pipelines can reference them by name through this map.
    //
    // Key: semantic name used by the pipeline (e.g., "selection", "weights")
    // Value: token for the actual array name in the scene object
    //
    // Example usage:
    //   config.named_array_refs["selection"] = get_token("my_selection_mask");
    //   // Pipeline will look up "my_selection_mask" from scene object's named_arrays
    std::map<std::string, pnanovdb_editor_token_t*> named_array_refs;

    // Callback when pipeline completes
    std::function<void(PipelineConfig*, PipelineStatus)> on_complete;

    /*!
        \brief Check if this pipeline uses dynamic reflected parameters

        \return true if using JSON-based dynamic params, false if using legacy struct
    */
    bool uses_dynamic_params() const
    {
        return !params_json_name.empty();
    }
};

/*!
    \brief Pipeline execution context

    Contains runtime information for executing a pipeline.
    Provides access to input data, named arrays, and output destinations.
*/
struct PipelineExecutionContext
{
    const pnanovdb_compute_t* compute = nullptr;
    const pnanovdb_raster_t* raster = nullptr;
    pnanovdb_compute_queue_t* queue = nullptr;

    // ========================================================================
    // Input Data (unnamed components from scene object)
    // ========================================================================

    pnanovdb_compute_array_t* input_nanovdb = nullptr; ///< Input NanoVDB array
    pnanovdb_raster_gaussian_data_t* input_gaussian = nullptr; ///< Input Gaussian data

    // Generic input pointers (resolved from scene object components)
    std::vector<void*> inputs;

    // ========================================================================
    // Named Arrays (bonus arrays from scene object)
    // ========================================================================

    // Pointer to scene object's named_arrays map for lookup by name
    // Pipelines can look up arrays by name token ID
    const std::map<uint64_t, NamedComponent>* named_arrays = nullptr;

    /*!
        \brief Get a named array by name token

        \param name_token Token for the array name
        \return Pointer to the compute array, or nullptr if not found
    */
    pnanovdb_compute_array_t* get_named_array(pnanovdb_editor_token_t* name_token) const;

    /*!
        \brief Get a named array by name string

        Looks up the token first, then finds the array.

        \param name Name string
        \return Pointer to the compute array, or nullptr if not found
    */
    pnanovdb_compute_array_t* get_named_array_by_name(const char* name) const;

    // ========================================================================
    // Output Data
    // ========================================================================

    // Output data pointers (where results should be stored)
    std::vector<void*> outputs;

    // Output NanoVDB array (for conversion pipelines)
    pnanovdb_compute_array_t** output_nanovdb = nullptr;
};

/*!
    \brief Pipeline execution interface

    Defines how a specific pipeline type executes.
*/
class IPipelineExecutor
{
public:
    virtual ~IPipelineExecutor() = default;

    /*!
        \brief Execute the pipeline

        \param config Pipeline configuration
        \param context Execution context with compute resources
        \return Status of execution
    */
    virtual PipelineStatus execute(PipelineConfig* config, PipelineExecutionContext* context) = 0;

    /*!
        \brief Check if pipeline can execute with current inputs

        \param config Pipeline configuration
        \param context Execution context
        \return true if can execute
    */
    virtual bool can_execute(const PipelineConfig* config, const PipelineExecutionContext* context) const = 0;
};

/*!
    \brief Null pipeline executor (pass-through, no-op)

    Used when input and output are the same (e.g., loading NanoVDB directly).
    Leverages ref counting to avoid copy cost.
*/
class NullPipelineExecutor : public IPipelineExecutor
{
public:
    PipelineStatus execute(PipelineConfig* config, PipelineExecutionContext* context) override
    {
        // No-op: input is output
        config->status = PipelineStatus::Completed;
        return PipelineStatus::Completed;
    }

    bool can_execute(const PipelineConfig* config, const PipelineExecutionContext* context) const override
    {
        return true; // Always can execute (it's a no-op)
    }
};

/*!
    \brief Render pipeline executor

    Converts renderable data (NanoVDB or Gaussian) into final 2D image.
    For NanoVDBs, often done in a single shader.
    For Gaussians, this is a many-step process.
*/
class RenderPipelineExecutor : public IPipelineExecutor
{
public:
    PipelineStatus execute(PipelineConfig* config, PipelineExecutionContext* context) override;
    bool can_execute(const PipelineConfig* config, const PipelineExecutionContext* context) const override;
};

/*!
    \brief 3D Rasterization pipeline executor

    Rasterizes Gaussian data to produce output NanoVDB.
*/
class Raster3DPipelineExecutor : public IPipelineExecutor
{
public:
    PipelineStatus execute(PipelineConfig* config, PipelineExecutionContext* context) override;
    bool can_execute(const PipelineConfig* config, const PipelineExecutionContext* context) const override;
};

/*!
    \brief Configuration for file import pipeline

    Holds the parameters for importing a file. Supports:
    - NanoVDB files (.nvdb) - direct load
    - Gaussian files (.ply/.npy/.npz) - can produce Gaussian data or NanoVDB via rasterization

    File type is auto-detected from extension.
*/
struct FileImportConfig
{
    std::string filepath; ///< Path to the file to import
    float voxels_per_unit = 128.f; ///< Voxels per unit (for Gaussian->NanoVDB rasterization)
    bool rasterize_to_nanovdb = false; ///< For Gaussian: false = Raster2D, true = Raster3D->NanoVDB

    // Results (set after execution)
    void* result_data = nullptr; ///< Pointer to resulting data (gaussian_data or nanovdb_array)
    bool is_nanovdb = false; ///< true if result_data is nanovdb_array

    /*!
        \brief Check if the file is a NanoVDB file based on extension
        \return true if filepath ends with .nvdb
    */
    bool is_nanovdb_file() const;
};

// Forward declarations for file import
class EditorScene;
class EditorSceneManager;

/*!
    \brief File import pipeline executor

    Handles file import operations including rasterization to Gaussian or NanoVDB.
    Manages its own worker thread for async file processing.
*/
class FileImportPipelineExecutor : public IPipelineExecutor
{
public:
    FileImportPipelineExecutor();
    ~FileImportPipelineExecutor() = default;

    /*!
        \brief Initialize the executor with required interfaces

        \param compute Compute interface
        \param raster Raster interface
        \param device_queue Device queue for Gaussian rasterization
        \param compute_queue Compute queue for NanoVDB rasterization
    */
    void init(const pnanovdb_compute_t* compute,
              pnanovdb_raster_t* raster,
              pnanovdb_compute_queue_t* device_queue,
              pnanovdb_compute_queue_t* compute_queue);

    /*!
        \brief Set the editor scene for data handling

        \param scene Pointer to the editor scene
    */
    void set_editor_scene(EditorScene* scene)
    {
        m_editor_scene = scene;
    }

    /*!
        \brief Set the scene manager for data storage

        \param manager Pointer to the scene manager
    */
    void set_scene_manager(EditorSceneManager* manager)
    {
        m_scene_manager = manager;
    }

    PipelineStatus execute(PipelineConfig* config, PipelineExecutionContext* context) override;
    bool can_execute(const PipelineConfig* config, const PipelineExecutionContext* context) const override;

    /*!
        \brief Check if file import is in progress

        \return true if a file import task is running
    */
    bool is_importing() const;

    /*!
        \brief Get import progress

        \param progress_text Output text describing progress
        \param progress_value Output progress value (0.0 to 1.0)
        \return true if import is in progress
    */
    bool get_progress(std::string& progress_text, float& progress_value);

    /*!
        \brief Check and handle import completion

        \param old_gaussian_data_ptr Output parameter for old gaussian data (for deferred destruction)
        \return true if a task completed this frame
    */
    bool handle_completion(std::shared_ptr<pnanovdb_raster_gaussian_data_t>& old_gaussian_data_ptr);

private:
    bool m_initialized = false;
    const pnanovdb_compute_t* m_compute = nullptr;
    pnanovdb_raster_t* m_raster = nullptr;
    pnanovdb_compute_queue_t* m_device_queue = nullptr;
    pnanovdb_compute_queue_t* m_compute_queue = nullptr;

    EditorScene* m_editor_scene = nullptr;
    EditorSceneManager* m_scene_manager = nullptr;

    // Worker thread for async file processing
    mutable pnanovdb_util::WorkerThread m_worker;
    pnanovdb_util::WorkerThread::TaskId m_task_id = pnanovdb_util::WorkerThread::invalidTaskId();

    // Import state
    std::string m_pending_filepath;
    float m_pending_voxel_size = 1.f / 128.f;
    bool m_is_nanovdb_import = false; ///< true if current import is a .nvdb file
    pnanovdb_raster_gaussian_data_t* m_pending_gaussian_data = nullptr;
    pnanovdb_raster_context_t* m_pending_raster_ctx = nullptr;
    pnanovdb_raster_shader_params_t* m_pending_raster_params = nullptr;
    pnanovdb_compute_array_t* m_pending_nanovdb_array = nullptr;
    pnanovdb_compute_array_t* m_pending_shader_params_arrays[pnanovdb_raster::shader_param_count];
    pnanovdb_raster_shader_params_t m_init_raster_shader_params;
    const pnanovdb_reflect_data_type_t* m_raster_shader_params_data_type = nullptr;
};

/*!
    \brief Pipeline manager for a scene

    Manages pipelines within a scene object, handles lazy execution,
    and tracks dependencies between pipelines.
*/
class PipelineManager
{
public:
    PipelineManager() = default;
    ~PipelineManager() = default;

    /*!
        \brief Add or update a conversion pipeline

        \param type Pipeline type
        \param config Pipeline configuration
        \param name Optional name for multiple pipelines of same type (nullptr for unnamed)
    */
    void set_conversion_pipeline(PipelineType type, const PipelineConfig& config, pnanovdb_editor_token_t* name = nullptr);

    /*!
        \brief Add or update a rendering pipeline

        \param config Pipeline configuration
        \param name Optional name for multiple pipelines (nullptr for unnamed)
    */
    void set_render_pipeline(const PipelineConfig& config, pnanovdb_editor_token_t* name = nullptr);

    /*!
        \brief Get a conversion pipeline

        \param type Pipeline type
        \param name Optional name (nullptr for unnamed)
        \return Pointer to pipeline config or nullptr if not found
    */
    PipelineConfig* get_conversion_pipeline(PipelineType type, pnanovdb_editor_token_t* name = nullptr);

    /*!
        \brief Get the rendering pipeline

        \param name Optional name (nullptr for unnamed)
        \return Pointer to pipeline config or nullptr if not found
    */
    PipelineConfig* get_render_pipeline(pnanovdb_editor_token_t* name = nullptr);

    /*!
        \brief Mark pipeline as dirty (needs re-run)

        Called when input data or parameters change.

        \param type Pipeline type
        \param name Optional name
    */
    void mark_dirty(PipelineType type, pnanovdb_editor_token_t* name = nullptr);

    /*!
        \brief Mark all pipelines as dirty
    */
    void mark_all_dirty();

    /*!
        \brief Execute a specific pipeline

        \param type Pipeline type
        \param context Execution context
        \param name Optional name
        \return Execution status
    */
    PipelineStatus execute_pipeline(PipelineType type,
                                    PipelineExecutionContext* context,
                                    pnanovdb_editor_token_t* name = nullptr);

    /*!
        \brief Execute all dirty pipelines that have auto_execute enabled

        This implements lazy execution for conversion pipelines.

        \param context Execution context
    */
    void execute_dirty_pipelines(PipelineExecutionContext* context);

    /*!
        \brief Check if any conversion pipelines need execution

        \return true if any pipeline is dirty or needs_run
    */
    bool has_pending_work() const;

    /*!
        \brief Manually run a specific conversion pipeline (for "Run" button UI)

        This bypasses the auto_execute/dirty check and forces execution.
        Uses internally stored compute/raster resources.

        \param type Pipeline type to run
        \param name Optional name token for named pipelines
        \param context Execution context with compute/raster resources
        \return Execution status
    */
    PipelineStatus run_pipeline_manual(PipelineType type,
                                       PipelineExecutionContext* context,
                                       pnanovdb_editor_token_t* name = nullptr);

    /*!
        \brief Create default pipelines for a given data type

        \param data_type Type of data (NanoVDB, Gaussian, etc.)
        \param compute Compute interface for creating params
    */
    void create_default_pipelines(SceneObjectType data_type, const pnanovdb_compute_t* compute);

    /*!
        \brief Initialize pipeline parameters from JSON defaults

        For pipelines using dynamic reflected parameters (params_json_name is set),
        this loads default values from the JSON file and creates the backing storage.

        For Gaussian splatting pipelines, this ensures defaults come from JSON,
        which can then be overridden by scene object values.

        \param config Pipeline configuration to initialize
        \param shader_params ShaderParams instance for JSON loading
        \param compute Compute interface for creating backing storage
    */
    static void initialize_params_from_json(PipelineConfig& config,
                                            class ShaderParams& shader_params,
                                            const pnanovdb_compute_t* compute);

    // ========================================================================
    // File Import Pipeline
    // ========================================================================

    /*!
        \brief Configure the file import executor with dependencies

        Must be called before import_file() to set up required components.

        \param compute Compute interface
        \param raster Raster interface
        \param device_queue Device queue for Gaussian rasterization
        \param compute_queue Compute queue for NanoVDB rasterization
        \param editor_scene EditorScene for data handling
        \param scene_manager EditorSceneManager for data storage
    */
    void configure_file_import(const pnanovdb_compute_t* compute,
                               pnanovdb_raster_t* raster,
                               pnanovdb_compute_queue_t* device_queue,
                               pnanovdb_compute_queue_t* compute_queue,
                               EditorScene* editor_scene,
                               EditorSceneManager* scene_manager);

    /*!
        \brief Import a file via the pipeline system

        Creates a FileImport pipeline and starts execution. The import runs
        asynchronously on a worker thread.

        Pipeline settings can include:
        - "voxel_size" param: Voxels per unit for Raster3D conversion (default: 128)
        - shader_path: Custom shader for the imported data

        \param filepath Path to the file to import
        \param rasterize_to_nanovdb true to convert to NanoVDB, false for Gaussian
        \param settings Optional pipeline settings (can be NULL for defaults)
        \return true if import was started successfully
    */
    bool import_file(const std::string& filepath,
                     bool rasterize_to_nanovdb,
                     const pnanovdb_pipeline_settings_t* settings = nullptr);

    /*!
        \brief Check if a file import is currently in progress

        \return true if an import is running
    */
    bool is_importing() const;

    /*!
        \brief Get the current file import configuration

        \return Pointer to the import config, or nullptr if no import is active
    */
    FileImportConfig* get_import_config();

    /*!
        \brief Get file import progress

        \param progress_text Output text describing progress
        \param progress_value Output progress value (0.0 to 1.0)
        \return true if import is in progress
    */
    bool get_import_progress(std::string& progress_text, float& progress_value);

    /*!
        \brief Check and handle import completion

        Should be called each frame to check for completed imports.

        \param old_gaussian_data_ptr Output parameter for old gaussian data (for deferred destruction)
        \return true if an import completed this frame
    */
    bool handle_import_completion(std::shared_ptr<pnanovdb_raster_gaussian_data_t>& old_gaussian_data_ptr);

private:
    // Map from (type, name) to pipeline config
    // Key is combined: (type << 32) | (name ? name->id : 0)
    std::map<uint64_t, PipelineConfig> m_conversion_pipelines;
    std::map<uint64_t, PipelineConfig> m_render_pipelines;

    // Pipeline executors (one per type)
    std::map<PipelineType, std::shared_ptr<IPipelineExecutor>> m_executors;

    // File import state
    std::unique_ptr<FileImportConfig> m_import_config;
    std::shared_ptr<FileImportPipelineExecutor> m_file_import_executor;

    /*!
        \brief Make key for pipeline lookup (without object scope)

        \param type Pipeline type
        \param name Optional name token
        \return Combined key
    */
    static uint64_t make_pipeline_key(PipelineType type, pnanovdb_editor_token_t* name);

    /*!
        \brief Make key for object-scoped pipeline lookup

        \param scene Scene token
        \param object Object token
        \param type Pipeline type
        \param name Optional pipeline name token
        \return Combined key encoding all parameters
    */
    static uint64_t make_object_pipeline_key(pnanovdb_editor_token_t* scene,
                                             pnanovdb_editor_token_t* object,
                                             PipelineType type,
                                             pnanovdb_editor_token_t* name);

    /*!
        \brief Get or create executor for pipeline type

        \param type Pipeline type
        \return Executor instance
    */
    IPipelineExecutor* get_executor(PipelineType type);

public:
    // ========================================================================
    // Object-scoped pipeline methods (for central management)
    // ========================================================================

    /*!
        \brief Set conversion pipeline for a specific object

        \param scene Scene token
        \param object Object token
        \param type Pipeline type
        \param config Pipeline configuration
        \param name Optional pipeline name
    */
    void set_object_conversion_pipeline(pnanovdb_editor_token_t* scene,
                                        pnanovdb_editor_token_t* object,
                                        PipelineType type,
                                        const PipelineConfig& config,
                                        pnanovdb_editor_token_t* name = nullptr);

    /*!
        \brief Set render pipeline for a specific object

        \param scene Scene token
        \param object Object token
        \param config Pipeline configuration
        \param name Optional pipeline name
    */
    void set_object_render_pipeline(pnanovdb_editor_token_t* scene,
                                    pnanovdb_editor_token_t* object,
                                    const PipelineConfig& config,
                                    pnanovdb_editor_token_t* name = nullptr);

    /*!
        \brief Get pipeline for a specific object

        \param scene Scene token
        \param object Object token
        \param type Pipeline type
        \param name Optional pipeline name
        \return Pointer to pipeline config or nullptr
    */
    PipelineConfig* get_object_pipeline(pnanovdb_editor_token_t* scene,
                                        pnanovdb_editor_token_t* object,
                                        PipelineType type,
                                        pnanovdb_editor_token_t* name = nullptr);

    /*!
        \brief Mark object pipeline as dirty

        \param scene Scene token
        \param object Object token
        \param type Pipeline type
        \param name Optional pipeline name
    */
    void mark_object_dirty(pnanovdb_editor_token_t* scene,
                           pnanovdb_editor_token_t* object,
                           PipelineType type,
                           pnanovdb_editor_token_t* name = nullptr);

    /*!
        \brief Mark all pipelines for an object as dirty
    */
    void mark_object_all_dirty(pnanovdb_editor_token_t* scene, pnanovdb_editor_token_t* object);

    /*!
        \brief Execute pipeline for a specific object

        \param scene Scene token
        \param object Object token
        \param type Pipeline type
        \param context Execution context
        \param name Optional pipeline name
        \return Execution status
    */
    PipelineStatus execute_object_pipeline(pnanovdb_editor_token_t* scene,
                                           pnanovdb_editor_token_t* object,
                                           PipelineType type,
                                           PipelineExecutionContext* context,
                                           pnanovdb_editor_token_t* name = nullptr);

    /*!
        \brief Remove all pipelines for an object (called when object is removed)

        \param scene Scene token
        \param object Object token
    */
    void remove_object_pipelines(pnanovdb_editor_token_t* scene, pnanovdb_editor_token_t* object);

    /*!
        \brief Configure a pipeline for a scene object

        \param scene Scene token
        \param object Object token
        \param type Pipeline type
        \param settings Configuration settings
        \param name Optional pipeline name
    */
    void configure_pipeline(pnanovdb_editor_token_t* scene,
                            pnanovdb_editor_token_t* object,
                            PipelineType type,
                            const pnanovdb_pipeline_settings_t* settings,
                            pnanovdb_editor_token_t* name = nullptr);
};

} // namespace pnanovdb_editor

#endif // NANOVDB_EDITOR_PIPELINE_H_HAS_BEEN_INCLUDED
