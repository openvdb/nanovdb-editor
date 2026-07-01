// Copyright Contributors to the OpenVDB Project
// SPDX-License-Identifier: Apache-2.0

/*!
    \file   nanovdb_editor/editor/SceneObject.cpp

    \brief  SceneObject pipeline and resource operations.
*/

#include "SceneObject.h"
#include "PipelineRegistry.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <utility>

namespace pnanovdb_editor
{

void SceneObject::reset_source()
{
    resources.nanovdb_array = nullptr;
    resources.gaussian_data = nullptr;
    resources.camera_view = nullptr;
    resources.converted_nanovdb = nullptr;
    resources.named_arrays.clear();
    resources.file_backed_named_arrays.clear();
    resources.source_filepath.clear();

    params.shader_params_array = nullptr;
    params.shader_params = nullptr;
    params.shader_params_data_type = nullptr;

    pipeline.load() = PipelineStage{};
    pipeline.render().output.clear();
    for (size_t i = 0; i < pipeline.process_count(); ++i)
    {
        pipeline.process_step(i).output.clear();
    }
    pipeline.process_run_snapshot.reset();
    pipeline.process_user_cancel_requested = false;
    pipeline.active_process_step = 0;

    resources.nanovdb_array_owner.reset();
    resources.gaussian_data_owner.reset();
    resources.camera_view_owner.reset();
    resources.converted_nanovdb_owner.reset();
    resources.named_array_owners.clear();
    params.shader_params_array_owner.reset();
}

void SceneObject::set_named_array_bindings(const std::map<std::string, pnanovdb_compute_array_t*>& arrays,
                                           const std::map<std::string, std::shared_ptr<pnanovdb_compute_array_t>>& owners)
{
    resources.named_arrays = arrays;
    resources.named_array_owners = owners;
}

void SceneObject::resolve_resources()
{
    pnanovdb_compute_array_t* nvdb = nullptr;
    std::shared_ptr<pnanovdb_compute_array_t> nvdb_owner;
    pnanovdb_compute_array_t* converted = nullptr;
    std::shared_ptr<pnanovdb_compute_array_t> converted_owner;
    pnanovdb_raster_gaussian_data_t* gauss = nullptr;
    std::shared_ptr<pnanovdb_raster_gaussian_data_t> gauss_owner;

    auto apply = [&](const PipelineStage& stage, bool is_process)
    {
        if (pnanovdb_compute_array_t* arr = stage.output.get_array(k_stage_output_nanovdb))
        {
            nvdb = arr;
            nvdb_owner = stage.output.get_array_owner(k_stage_output_nanovdb);
            if (is_process)
            {
                converted = arr;
                converted_owner = nvdb_owner;
            }
        }
        if (stage.output.gaussian)
        {
            gauss = stage.output.gaussian;
            gauss_owner = stage.output.gaussian_owner;
        }
    };

    apply(pipeline.load(), false);
    for (size_t i = 0; i < pipeline.process_count(); ++i)
    {
        apply(pipeline.process_step(i), true);
    }
    apply(pipeline.render(), false);

    resources.nanovdb_array = nvdb;
    resources.nanovdb_array_owner = nvdb_owner;
    resources.converted_nanovdb = converted;
    resources.converted_nanovdb_owner = converted_owner;
    resources.gaussian_data = gauss;
    resources.gaussian_data_owner = gauss_owner;
}

int SceneObject::next_dirty_process_step() const
{
    for (size_t i = 0; i < pipeline.process_count(); ++i)
    {
        const PipelineStage& stage = pipeline.process_step(i);
        if (stage.dirty && stage.type != pnanovdb_pipeline_type_noop)
        {
            return (int)i;
        }
    }
    return -1;
}

int SceneObject::process_rebuild_start(size_t requested_step) const
{
    const size_t count = pipeline.process_count();
    size_t first = std::min(requested_step, count);
    size_t consumer = first;
    while (consumer > 0)
    {
        size_t producer = consumer;
        do
        {
            --producer;
        } while (producer > 0 && pipeline.process_step(producer).type == pnanovdb_pipeline_type_noop);

        const PipelineStage& step = pipeline.process_step(producer);
        if (step.type == pnanovdb_pipeline_type_noop || !step.output.empty())
        {
            break;
        }
        first = producer;
        consumer = producer;
    }
    return (int)first;
}

SceneObjectSourceKind SceneObject::source_kind() const
{
    const auto& arrays = resources.named_arrays;
    const auto indices = arrays.find("indices");
    if (arrays.find("positions") != arrays.end() && indices != arrays.end() && indices->second)
    {
        return indices->second->element_size == 2u * sizeof(uint32_t) ? SceneObjectSourceKind::MeshLines :
                                                                        SceneObjectSourceKind::MeshTriangles;
    }
    if (pipeline.load().type == pnanovdb_pipeline_type_gaussian_load)
    {
        return resources.source_filepath.empty() ? SceneObjectSourceKind::GaussianData :
                                                   SceneObjectSourceKind::GaussianFile;
    }
    if (pipeline.load().type == pnanovdb_pipeline_type_mesh_load)
    {
        return SceneObjectSourceKind::MeshTriangles;
    }
    if (type == SceneObjectType::Array && arrays.find("means") != arrays.end() && arrays.find("scales") != arrays.end())
    {
        return SceneObjectSourceKind::GaussianArrays;
    }
    if (type == SceneObjectType::GaussianData)
    {
        return resources.source_filepath.empty() ? SceneObjectSourceKind::GaussianData :
                                                   SceneObjectSourceKind::GaussianFile;
    }
    if (type == SceneObjectType::NanoVDB)
    {
        return SceneObjectSourceKind::NanoVDB;
    }
    if (resources.nanovdb_array || resources.converted_nanovdb)
    {
        return SceneObjectSourceKind::NanoVDB;
    }
    return SceneObjectSourceKind::None;
}

pnanovdb_uint32_t SceneObject::native_data_kind()
{
    switch (source_kind())
    {
    case SceneObjectSourceKind::NanoVDB:
        return pnanovdb_pipeline_data_kind_nanovdb;
    case SceneObjectSourceKind::GaussianData:
    case SceneObjectSourceKind::GaussianFile:
    case SceneObjectSourceKind::GaussianArrays:
        return pnanovdb_pipeline_data_kind_gaussian;
    case SceneObjectSourceKind::MeshTriangles:
    case SceneObjectSourceKind::MeshLines:
        return pnanovdb_pipeline_data_kind_mesh;
    default:
        return pnanovdb_pipeline_data_kind_none;
    }
}

pnanovdb_uint32_t SceneObject::upstream_data_kind(size_t step)
{
    for (size_t i = step; i-- > 0;)
    {
        const pnanovdb_pipeline_type_t type = pipeline.process_step(i).type;
        if (type == pnanovdb_pipeline_type_noop)
        {
            continue;
        }
        const pnanovdb_pipeline_descriptor_t* desc = pnanovdb_pipeline_get_descriptor(type);
        if (desc && desc->outputs != pnanovdb_pipeline_data_kind_none)
        {
            return desc->outputs;
        }
    }
    return native_data_kind();
}

namespace
{

pnanovdb_pipeline_type_t default_render_for_data_kind(const SceneObject* obj, pnanovdb_uint32_t kind)
{
    if (kind & (pnanovdb_pipeline_data_kind_nanovdb | pnanovdb_pipeline_data_kind_nanovdb_rgba8))
    {
        return pnanovdb_pipeline_type_nanovdb_render;
    }
    if (kind == pnanovdb_pipeline_data_kind_gaussian)
    {
        return pnanovdb_pipeline_type_gaussian_splat;
    }
    if (kind == pnanovdb_pipeline_data_kind_voxelbvh && obj)
    {
        switch (obj->source_kind())
        {
        case SceneObjectSourceKind::GaussianData:
        case SceneObjectSourceKind::GaussianFile:
        case SceneObjectSourceKind::GaussianArrays:
            return pnanovdb_pipeline_type_voxelbvh_gaussians_render;
        case SceneObjectSourceKind::MeshLines:
            return pnanovdb_pipeline_type_voxelbvh_lines_render;
        default:
            return pnanovdb_pipeline_type_voxelbvh_triangles_render;
        }
    }

    for (int i = 0; i < pnanovdb_pipeline_type_count; ++i)
    {
        const auto type = static_cast<pnanovdb_pipeline_type_t>(i);
        const pnanovdb_pipeline_descriptor_t* desc = pnanovdb_pipeline_get_descriptor(type);
        if (!desc || desc->stage != pnanovdb_pipeline_stage_render || desc->chain_step_count > 0 ||
            type == pnanovdb_pipeline_type_noop)
        {
            continue;
        }
        if (desc->inputs == 0u || (desc->inputs & kind))
        {
            return type;
        }
    }
    return pnanovdb_pipeline_type_noop;
}

bool render_pipeline_accepts_kind(pnanovdb_pipeline_type_t render, pnanovdb_uint32_t kind)
{
    if (render == pnanovdb_pipeline_type_noop || kind == pnanovdb_pipeline_data_kind_none)
    {
        return true;
    }
    const pnanovdb_pipeline_descriptor_t* desc = pnanovdb_pipeline_get_descriptor(render);
    return desc && (desc->inputs == 0u || (desc->inputs & kind));
}

pnanovdb_uint32_t renderable_data_kind_from_outputs(SceneObject* obj)
{
    if (!obj)
    {
        return pnanovdb_pipeline_data_kind_none;
    }
    for (size_t i = obj->pipeline.process_count(); i-- > 0;)
    {
        const PipelineStage& stage = obj->pipeline.process_step(i);
        if (stage.type == pnanovdb_pipeline_type_noop)
        {
            continue;
        }
        const bool has_nanovdb = stage.output.get_array(k_stage_output_nanovdb) != nullptr;
        const bool has_gaussian = stage.output.gaussian != nullptr;
        if (!has_nanovdb && !has_gaussian)
        {
            continue;
        }
        const pnanovdb_pipeline_descriptor_t* desc = pnanovdb_pipeline_get_descriptor(stage.type);
        if (desc && desc->outputs != pnanovdb_pipeline_data_kind_none)
        {
            return desc->outputs;
        }
        if (has_nanovdb)
        {
            return pnanovdb_pipeline_data_kind_nanovdb;
        }
        if (has_gaussian)
        {
            return pnanovdb_pipeline_data_kind_gaussian;
        }
    }
    return obj->upstream_data_kind(0);
}

} // namespace

pnanovdb_pipeline_type_t SceneObject::default_render_pipeline(pnanovdb_uint32_t kind) const
{
    return default_render_for_data_kind(this, kind);
}

pnanovdb_uint32_t SceneObject::renderable_data_kind()
{
    return renderable_data_kind_from_outputs(this);
}

void SceneObject::sync_render_to_chain()
{
    const pnanovdb_uint32_t kind = renderable_data_kind_from_outputs(this);
    if (kind == pnanovdb_pipeline_data_kind_none || render_pipeline_accepts_kind(render_pipeline(), kind))
    {
        return;
    }

    const pnanovdb_pipeline_type_t new_render = default_render_pipeline(kind);
    if (new_render == pnanovdb_pipeline_type_noop || new_render == render_pipeline())
    {
        return;
    }

    render_pipeline() = new_render;
    pnanovdb_pipeline_get_default_params(new_render, &render_params());
    shader_name() = nullptr;
    shader_params() = nullptr;
    shader_params_data_type() = nullptr;
}

void SceneObject::clear_process_run_snapshot()
{
    pipeline.process_run_snapshot.reset();
}

void SceneObject::process_user_cancel()
{
    pipeline.process_user_cancel_requested = true;
}

void SceneObject::clear_process_cancel_state()
{
    pipeline.process_user_cancel_requested = false;
}

void SceneObject::restore_process_run_snapshot()
{
    if (!pipeline.process_run_snapshot)
    {
        return;
    }
    ProcessRunSnapshot snap = std::move(*pipeline.process_run_snapshot);
    pipeline.process_run_snapshot.reset();

    for (size_t j = 0; j < snap.step_outputs.size(); ++j)
    {
        const size_t i = (size_t)snap.from_step + j;
        if (i >= pipeline.process_count())
        {
            break;
        }
        PipelineStage& stage = pipeline.process_step(i);
        stage.output = snap.step_outputs[j];
        if (j < snap.step_dirty.size())
        {
            stage.dirty = snap.step_dirty[j];
        }
    }
    pipeline.active_process_step = snap.active_process_step;
    pipeline.render() = std::move(snap.render_stage);
    params.shader_name_storage = std::move(snap.shader_name_storage);
    ensure_shader_name_storage().value = snap.shader_name_value;
    params.shader_params_array = snap.shader_params_array;
    params.shader_params_array_owner = std::move(snap.shader_params_array_owner);
    shader_params() = snap.shader_params;
    shader_params_data_type() = snap.shader_params_data_type;
    resolve_resources();
}

void SceneObject::invalidate_process_from(int from)
{
    if (from < 0)
    {
        return;
    }

    const size_t count = pipeline.process_count();
    if ((size_t)from >= count)
    {
        return;
    }
    const size_t requested = (size_t)from;
    from = process_rebuild_start(requested);

    ProcessRunSnapshot snap;
    snap.from_step = from;
    snap.active_process_step = pipeline.active_process_step;
    snap.render_stage = pipeline.render();
    snap.shader_name_storage = params.shader_name_storage;
    if (snap.shader_name_storage)
    {
        snap.shader_name_value = snap.shader_name_storage->value;
    }
    snap.shader_params_array = params.shader_params_array;
    snap.shader_params_array_owner = params.shader_params_array_owner;
    snap.shader_params = shader_params();
    snap.shader_params_data_type = shader_params_data_type();
    snap.step_outputs.reserve(count - (size_t)from);
    snap.step_dirty.reserve(count - (size_t)from);
    for (size_t i = (size_t)from; i < count; ++i)
    {
        const PipelineStage& stage = pipeline.process_step(i);
        snap.step_outputs.push_back(stage.output);
        snap.step_dirty.push_back(stage.dirty);
    }
    pipeline.process_run_snapshot = std::move(snap);

    for (size_t i = (size_t)from; i < count; ++i)
    {
        PipelineStage& stage = pipeline.process_step(i);
        if (i < requested && stage.dirty)
        {
            continue;
        }
        stage.output.clear();
        stage.bump_revision();
        if (stage.type != pnanovdb_pipeline_type_noop)
        {
            stage.dirty = true;
        }
    }
    resolve_resources();
    sync_render_to_chain();
}

void SceneObject::invalidate_process_configuration_from(size_t from)
{
    const size_t count = pipeline.process_count();
    if (from > count)
    {
        return;
    }
    const size_t requested = from;
    const int first = process_rebuild_start(requested);
    if (first < 0)
    {
        return;
    }

    clear_process_run_snapshot();
    for (size_t i = (size_t)first; i < count; ++i)
    {
        PipelineStage& step = pipeline.process_step(i);
        if (i < requested && step.dirty)
        {
            continue;
        }
        step.output.clear();
        step.bump_revision();
        step.dirty = step.type != pnanovdb_pipeline_type_noop;
    }
    resolve_resources();
    sync_render_to_chain();
}

bool SceneObject::remove_process_step(size_t step)
{
    if (step == 0 || step >= pipeline.process_count())
    {
        return false;
    }

    clear_process_run_snapshot();
    pipeline.extra_process.erase(pipeline.extra_process.begin() + (std::ptrdiff_t)(step - 1));
    pipeline.active_process_step = 0;
    invalidate_process_configuration_from(std::min(step, pipeline.process_count()));
    return true;
}

void SceneObject::mark_process_dirty()
{
    bool armed = false;
    for (size_t i = 0; i < pipeline.process_count(); ++i)
    {
        PipelineStage& step = pipeline.process_step(i);
        step.bump_revision();
        if (!armed && step.type != pnanovdb_pipeline_type_noop)
        {
            step.dirty = true;
            armed = true;
        }
    }
}

void SceneObject::advance_process_chain(bool success)
{
    const int i = pipeline.active_process_step;
    if (i < 0 || (size_t)i >= pipeline.process_count())
    {
        resolve_resources();
        return;
    }

    pipeline.process_step((size_t)i).dirty = false;

    if (success)
    {
        if (pipeline.drop_intermediate && i >= 1)
        {
            pipeline.process_step((size_t)(i - 1)).output.clear();
        }
        if ((size_t)(i + 1) < pipeline.process_count())
        {
            PipelineStage& next = pipeline.process_step((size_t)(i + 1));
            next.output.clear();
            next.bump_revision();
            if (next.type != pnanovdb_pipeline_type_noop)
            {
                next.dirty = true;
            }
        }

        if (next_dirty_process_step() < 0)
        {
            clear_process_run_snapshot();
        }
    }
    else
    {
        for (size_t next = (size_t)i + 1; next < pipeline.process_count(); ++next)
        {
            pipeline.process_step(next).dirty = false;
        }
        clear_process_run_snapshot();
    }

    resolve_resources();
}

} // namespace pnanovdb_editor
