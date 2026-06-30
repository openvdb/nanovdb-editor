// Copyright Contributors to the OpenVDB Project
// SPDX-License-Identifier: Apache-2.0

#include <gtest/gtest.h>

#include "editor/EditorSceneManager.h"

namespace
{
using namespace pnanovdb_editor;

TEST(SceneObjectSourceTest, GaussianLoadPrecedesDerivedNanoVDBState)
{
    SceneObject obj;
    pnanovdb_compute_array_t derived{};
    obj.type = SceneObjectType::NanoVDB;
    obj.resources.nanovdb_array = &derived;
    obj.resources.source_filepath = "source.ply";
    obj.pipeline.load().type = pnanovdb_pipeline_type_gaussian_load;

    EXPECT_EQ(scene_object_source_kind(&obj), SceneObjectSourceKind::GaussianFile);
    EXPECT_EQ(scene_object_native_data_kind(&obj), pnanovdb_pipeline_data_kind_gaussian);
    EXPECT_EQ(scene_object_default_render_pipeline(&obj, pnanovdb_pipeline_data_kind_voxelbvh),
              pnanovdb_pipeline_type_voxelbvh_gaussians_render);
}

TEST(SceneObjectSourceTest, MeshTopologyComesFromIndexLayout)
{
    SceneObject obj;
    pnanovdb_compute_array_t positions{};
    pnanovdb_compute_array_t indices{};
    obj.resources.named_arrays["positions"] = &positions;
    obj.resources.named_arrays["indices"] = &indices;

    obj.pipeline.render().type = pnanovdb_pipeline_type_voxelbvh_triangles_render;
    indices.element_size = 2u * sizeof(uint32_t);
    EXPECT_EQ(scene_object_source_kind(&obj), SceneObjectSourceKind::MeshLines);
    EXPECT_EQ(scene_object_default_render_pipeline(&obj, pnanovdb_pipeline_data_kind_voxelbvh),
              pnanovdb_pipeline_type_voxelbvh_lines_render);

    obj.pipeline.render().type = pnanovdb_pipeline_type_voxelbvh_lines_render;
    indices.element_size = sizeof(uint32_t);
    EXPECT_EQ(scene_object_source_kind(&obj), SceneObjectSourceKind::MeshTriangles);
    EXPECT_EQ(scene_object_default_render_pipeline(&obj, pnanovdb_pipeline_data_kind_voxelbvh),
              pnanovdb_pipeline_type_voxelbvh_triangles_render);
}

TEST(SceneObjectSourceTest, GaussianArraysRemainAnIntrinsicGaussianSource)
{
    SceneObject obj;
    pnanovdb_compute_array_t means{};
    pnanovdb_compute_array_t scales{};
    obj.resources.named_arrays["means"] = &means;
    obj.resources.named_arrays["scales"] = &scales;
    obj.type = SceneObjectType::Array;

    EXPECT_EQ(scene_object_source_kind(&obj), SceneObjectSourceKind::GaussianArrays);
    EXPECT_EQ(scene_object_native_data_kind(&obj), pnanovdb_pipeline_data_kind_gaussian);
    EXPECT_EQ(scene_object_default_render_pipeline(&obj, pnanovdb_pipeline_data_kind_voxelbvh),
              pnanovdb_pipeline_type_voxelbvh_gaussians_render);
}

TEST(SceneObjectSourceTest, GaussianIdentityPrecedesInternalNamedArrays)
{
    SceneObject obj;
    pnanovdb_compute_array_t means{};
    pnanovdb_compute_array_t scales{};
    obj.type = SceneObjectType::GaussianData;
    obj.resources.named_arrays["means"] = &means;
    obj.resources.named_arrays["scales"] = &scales;

    EXPECT_EQ(scene_object_source_kind(&obj), SceneObjectSourceKind::GaussianData);

    obj.resources.source_filepath = "source.ply";
    obj.pipeline.load().type = pnanovdb_pipeline_type_gaussian_load;
    EXPECT_EQ(scene_object_source_kind(&obj), SceneObjectSourceKind::GaussianFile);
}

TEST(SceneObjectSourceTest, ProcessCompatibilityMatchesConcreteGaussianRepresentation)
{
    EXPECT_FALSE(
        process_pipeline_supports_source(SceneObjectSourceKind::GaussianData, pnanovdb_pipeline_type_voxelbvh_build));
    EXPECT_FALSE(process_pipeline_supports_source(
        SceneObjectSourceKind::GaussianData, pnanovdb_pipeline_type_gaussian_voxelize));

    EXPECT_TRUE(
        process_pipeline_supports_source(SceneObjectSourceKind::GaussianFile, pnanovdb_pipeline_type_voxelbvh_build));
    EXPECT_TRUE(process_pipeline_supports_source(
        SceneObjectSourceKind::GaussianFile, pnanovdb_pipeline_type_gaussian_voxelize));

    EXPECT_TRUE(
        process_pipeline_supports_source(SceneObjectSourceKind::GaussianArrays, pnanovdb_pipeline_type_voxelbvh_build));
    EXPECT_FALSE(process_pipeline_supports_source(
        SceneObjectSourceKind::GaussianArrays, pnanovdb_pipeline_type_gaussian_voxelize));
}

TEST(SceneObjectSourceTest, Rgba8ProcessCompatibilityRequiresTriangleSource)
{
    EXPECT_TRUE(
        process_pipeline_supports_source(SceneObjectSourceKind::MeshTriangles, pnanovdb_pipeline_type_voxelbvh_rgba8));
    EXPECT_FALSE(
        process_pipeline_supports_source(SceneObjectSourceKind::MeshLines, pnanovdb_pipeline_type_voxelbvh_rgba8));
    EXPECT_FALSE(
        process_pipeline_supports_source(SceneObjectSourceKind::GaussianFile, pnanovdb_pipeline_type_voxelbvh_rgba8));
    EXPECT_FALSE(process_pipeline_supports_source(SceneObjectSourceKind::NanoVDB, pnanovdb_pipeline_type_voxelbvh_rgba8));
}
} // namespace
