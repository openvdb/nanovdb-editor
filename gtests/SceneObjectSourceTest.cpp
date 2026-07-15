// Copyright Contributors to the OpenVDB Project
// SPDX-License-Identifier: Apache-2.0

#include <gtest/gtest.h>

#include "editor/EditorSceneManager.h"
#include "editor/Pipeline.h"

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

    EXPECT_EQ(obj.source_kind(), SceneObjectSourceKind::GaussianFile);
    EXPECT_EQ(obj.native_data_kind(), pnanovdb_pipeline_data_kind_gaussian);
    EXPECT_EQ(obj.default_render_pipeline(pnanovdb_pipeline_data_kind_voxelbvh),
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
    EXPECT_EQ(obj.source_kind(), SceneObjectSourceKind::MeshLines);
    EXPECT_EQ(obj.default_render_pipeline(pnanovdb_pipeline_data_kind_voxelbvh),
              pnanovdb_pipeline_type_voxelbvh_lines_render);

    obj.pipeline.render().type = pnanovdb_pipeline_type_voxelbvh_lines_render;
    indices.element_size = sizeof(uint32_t);
    EXPECT_EQ(obj.source_kind(), SceneObjectSourceKind::MeshTriangles);
    EXPECT_EQ(obj.default_render_pipeline(pnanovdb_pipeline_data_kind_voxelbvh),
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

    EXPECT_EQ(obj.source_kind(), SceneObjectSourceKind::GaussianArrays);
    EXPECT_EQ(obj.native_data_kind(), pnanovdb_pipeline_data_kind_gaussian);
    EXPECT_EQ(obj.default_render_pipeline(pnanovdb_pipeline_data_kind_voxelbvh),
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

    EXPECT_EQ(obj.source_kind(), SceneObjectSourceKind::GaussianData);

    obj.resources.source_filepath = "source.ply";
    obj.pipeline.load().type = pnanovdb_pipeline_type_gaussian_load;
    EXPECT_EQ(obj.source_kind(), SceneObjectSourceKind::GaussianFile);
}

TEST(SceneObjectSourceTest, ResettingGaussianFileForNanoVdbClearsLoadIdentity)
{
    SceneObject obj;
    obj.type = SceneObjectType::GaussianData;
    obj.resources.source_filepath = "source.ply";
    obj.pipeline.load().type = pnanovdb_pipeline_type_gaussian_load;
    obj.pipeline.load().configured = true;
    obj.pipeline.process().type = pnanovdb_pipeline_type_gaussian_voxelize;
    obj.pipeline.render().type = pnanovdb_pipeline_type_gaussian_splat;
    ASSERT_EQ(obj.source_kind(), SceneObjectSourceKind::GaussianFile);

    obj.reset_source();
    pnanovdb_compute_array_t nanovdb{};
    obj.type = SceneObjectType::NanoVDB;
    obj.resources.nanovdb_array = &nanovdb;

    EXPECT_EQ(obj.pipeline.load().type, pnanovdb_pipeline_type_noop);
    EXPECT_FALSE(obj.pipeline.load().configured);
    EXPECT_EQ(obj.pipeline.process().type, pnanovdb_pipeline_type_gaussian_voxelize);
    EXPECT_EQ(obj.pipeline.render().type, pnanovdb_pipeline_type_gaussian_splat);
    EXPECT_EQ(obj.source_kind(), SceneObjectSourceKind::NanoVDB);
}

TEST(SceneObjectSourceTest, ResettingMeshLoadForRawGaussianClearsLoadIdentity)
{
    SceneObject obj;
    obj.type = SceneObjectType::Array;
    obj.resources.source_filepath = "mesh.ply";
    obj.pipeline.load().type = pnanovdb_pipeline_type_mesh_load;
    ASSERT_EQ(obj.source_kind(), SceneObjectSourceKind::MeshTriangles);

    obj.reset_source();
    obj.type = SceneObjectType::GaussianData;
    obj.resources.gaussian_data = reinterpret_cast<pnanovdb_raster_gaussian_data_t*>(uintptr_t{ 1 });

    EXPECT_EQ(obj.pipeline.load().type, pnanovdb_pipeline_type_noop);
    EXPECT_EQ(obj.source_kind(), SceneObjectSourceKind::GaussianData);
}

TEST(SceneObjectSourceTest, ProcessCompatibilityRequiresCompleteGaussianResources)
{
    SceneObject gaussian_data;
    gaussian_data.type = SceneObjectType::GaussianData;
    EXPECT_FALSE(process_pipeline_supports_object(&gaussian_data, pnanovdb_pipeline_type_voxelbvh_build));
    EXPECT_FALSE(process_pipeline_supports_object(&gaussian_data, pnanovdb_pipeline_type_gaussian_voxelize));

    SceneObject gaussian_file;
    gaussian_file.pipeline.load().type = pnanovdb_pipeline_type_gaussian_load;
    gaussian_file.resources.source_filepath = "source.ply";
    EXPECT_TRUE(process_pipeline_supports_object(&gaussian_file, pnanovdb_pipeline_type_voxelbvh_build));
    EXPECT_TRUE(process_pipeline_supports_object(&gaussian_file, pnanovdb_pipeline_type_gaussian_voxelize));

    pnanovdb_compute_array_t array{};
    SceneObject gaussian_arrays;
    gaussian_arrays.type = SceneObjectType::Array;
    gaussian_arrays.resources.named_arrays["means"] = &array;
    gaussian_arrays.resources.named_arrays["scales"] = &array;
    EXPECT_EQ(gaussian_arrays.source_kind(), SceneObjectSourceKind::GaussianArrays);
    EXPECT_FALSE(process_pipeline_supports_object(&gaussian_arrays, pnanovdb_pipeline_type_voxelbvh_build));

    gaussian_arrays.resources.named_arrays["opacities"] = &array;
    gaussian_arrays.resources.named_arrays["quaternions"] = &array;
    gaussian_arrays.resources.named_arrays["sh_0"] = &array;
    gaussian_arrays.resources.named_arrays["sh_n"] = &array;
    EXPECT_TRUE(process_pipeline_supports_object(&gaussian_arrays, pnanovdb_pipeline_type_voxelbvh_build));
    EXPECT_FALSE(process_pipeline_supports_object(&gaussian_arrays, pnanovdb_pipeline_type_gaussian_voxelize));
}

TEST(SceneObjectSourceTest, MeshProcessCompatibilityRequiresNonNullArrays)
{
    pnanovdb_compute_array_t positions{};
    pnanovdb_compute_array_t indices{};
    SceneObject mesh;
    mesh.resources.named_arrays["positions"] = nullptr;
    mesh.resources.named_arrays["indices"] = &indices;
    EXPECT_EQ(mesh.source_kind(), SceneObjectSourceKind::MeshTriangles);
    EXPECT_FALSE(process_pipeline_supports_object(&mesh, pnanovdb_pipeline_type_voxelbvh_build));
    EXPECT_FALSE(process_pipeline_supports_object(&mesh, pnanovdb_pipeline_type_voxelbvh_rgba8));

    mesh.resources.named_arrays["positions"] = &positions;
    EXPECT_TRUE(process_pipeline_supports_object(&mesh, pnanovdb_pipeline_type_voxelbvh_build));
    EXPECT_TRUE(process_pipeline_supports_object(&mesh, pnanovdb_pipeline_type_voxelbvh_rgba8));

    indices.element_size = 2u * sizeof(uint32_t);
    EXPECT_TRUE(process_pipeline_supports_object(&mesh, pnanovdb_pipeline_type_voxelbvh_build));
    EXPECT_FALSE(process_pipeline_supports_object(&mesh, pnanovdb_pipeline_type_voxelbvh_rgba8));
}
} // namespace
