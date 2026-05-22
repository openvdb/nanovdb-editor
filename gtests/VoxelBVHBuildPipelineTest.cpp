// Copyright Contributors to the OpenVDB Project
// SPDX-License-Identifier: Apache-2.0

/*!
    \file   gtests/VoxelBVHBuildPipelineTest.cpp

    \brief
*/

#include <gtest/gtest.h>

#include <nanovdb_editor/putil/Compiler.h>
#include <nanovdb_editor/putil/Compute.h>
#include <nanovdb_editor/putil/FileFormat.h>
#include <nanovdb_editor/putil/VoxelBVH.h>

#include <nanovdb/PNanoVDB.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

namespace
{

pnanovdb_compute_array_t* synthesize_white_colors(const pnanovdb_compute_t& compute, uint64_t float_count)
{
    std::vector<float> white(float_count, 1.0f);
    return compute.create_array(sizeof(float), float_count, white.data());
}

// Buffer is parseable and has at least one root tile (i.e. BVH actually populated).
void expect_nanovdb_populated(pnanovdb_compute_array_t* nanovdb_array, uint64_t min_bytes)
{
    ASSERT_NE(nanovdb_array, nullptr) << "nanovdb_from_triangles_array returned null";
    EXPECT_GT(nanovdb_array->element_count, 0u);

    const uint64_t bytes = nanovdb_array->element_size * nanovdb_array->element_count;
    EXPECT_GE(bytes, min_bytes) << "augmented NanoVDB unexpectedly small: " << bytes << " bytes";

    pnanovdb_buf_t buf =
        pnanovdb_make_buf((uint32_t*)nanovdb_array->data,
                          static_cast<uint64_t>(nanovdb_array->element_size * nanovdb_array->element_count / 4u));
    pnanovdb_grid_handle_t grid = {};
    pnanovdb_tree_handle_t tree = pnanovdb_grid_get_tree(buf, grid);
    pnanovdb_root_handle_t root = pnanovdb_tree_get_root(buf, tree);
    pnanovdb_uint32_t root_tile_count = pnanovdb_root_get_tile_count(buf, root);
    EXPECT_GE(root_tile_count, 1u) << "root has no tiles -- BVH did not populate";
}

// Procedural heightfield mesh with the same array layout EditorScene::load_mesh_file
// produces (flat float3 positions, flat uint32 triangle indices, flat float3 colors).
bool build_heightfield_mesh(const pnanovdb_compute_t& compute,
                            uint32_t width,
                            uint32_t height,
                            pnanovdb_compute_array_t** out_indices,
                            pnanovdb_compute_array_t** out_positions,
                            pnanovdb_compute_array_t** out_colors)
{
    const uint32_t vert_per_quad = 4u;
    const uint32_t tri_per_quad = 2u;
    const uint32_t quad_count = width * height;
    const uint32_t vertex_count = vert_per_quad * quad_count;
    const uint32_t triangle_count = tri_per_quad * quad_count;
    const uint32_t index_count = 3u * triangle_count;
    const uint32_t float_count = 3u * vertex_count;

    std::vector<uint32_t> indices(index_count, 0u);
    std::vector<float> positions(float_count, 0.f);
    std::vector<float> colors(float_count, 1.f);

    for (uint32_t j = 0u; j < height; j++)
    {
        for (uint32_t i = 0u; i < width; i++)
        {
            const float x0 = 1000.f * float(i) / float(width);
            const float x1 = 1000.f * float(i + 1) / float(width);
            const float y0 = 1000.f * float(j) / float(height);
            const float y1 = 1000.f * float(j + 1) / float(height);
            const float z00 = 20.f * std::sin(0.0001f * (x0 * x0 + y0 * y0));
            const float z10 = 20.f * std::sin(0.0001f * (x1 * x1 + y0 * y0));
            const float z01 = 20.f * std::sin(0.0001f * (x0 * x0 + y1 * y1));
            const float z11 = 20.f * std::sin(0.0001f * (x1 * x1 + y1 * y1));

            const uint32_t quad_idx = j * width + i;
            const uint32_t base_vert = vert_per_quad * quad_idx;

            // Two triangles: (0,1,3) and (3,2,0)
            indices[6u * quad_idx + 0u] = base_vert + 0u;
            indices[6u * quad_idx + 1u] = base_vert + 1u;
            indices[6u * quad_idx + 2u] = base_vert + 3u;
            indices[6u * quad_idx + 3u] = base_vert + 3u;
            indices[6u * quad_idx + 4u] = base_vert + 2u;
            indices[6u * quad_idx + 5u] = base_vert + 0u;

            const uint32_t base_pos = 3u * base_vert;
            positions[base_pos + 0u] = x0;
            positions[base_pos + 1u] = y0;
            positions[base_pos + 2u] = z00;
            positions[base_pos + 3u] = x1;
            positions[base_pos + 4u] = y0;
            positions[base_pos + 5u] = z10;
            positions[base_pos + 6u] = x0;
            positions[base_pos + 7u] = y1;
            positions[base_pos + 8u] = z01;
            positions[base_pos + 9u] = x1;
            positions[base_pos + 10u] = y1;
            positions[base_pos + 11u] = z11;
        }
    }

    *out_indices = compute.create_array(sizeof(uint32_t), indices.size(), indices.data());
    *out_positions = compute.create_array(sizeof(float), positions.size(), positions.data());
    *out_colors = compute.create_array(sizeof(float), colors.size(), colors.data());
    return *out_indices && *out_positions && *out_colors;
}

bool dragon_ply_available()
{
    return std::filesystem::exists("./data/xyzrgb_dragon.ply");
}

bool is_software_renderer_name(const char* name)
{
    if (!name || name[0] == '\0')
    {
        return false;
    }
    std::string lowered(name);
    std::transform(lowered.begin(), lowered.end(), lowered.begin(), [](unsigned char c) { return std::tolower(c); });
    return lowered.find("lavapipe") != std::string::npos || lowered.find("llvmpipe") != std::string::npos ||
           lowered.find("swiftshader") != std::string::npos;
}

bool software_voxelbvh_tests_force_enabled()
{
    const char* env = std::getenv("PNANOVDB_ENABLE_SOFTWARE_VOXELBVH_TESTS");
    return env && env[0] != '\0' && std::strcmp(env, "0") != 0;
}

// TODO: make common function for a cpp test
bool save_wireframe_lines_ply(
    const std::string& path, uint32_t width, uint32_t height, uint64_t* out_vertex_count, uint64_t* out_edge_count)
{
    const uint32_t vert_per_quad = 4u;
    const uint32_t edge_per_quad = 4u;
    const uint32_t quad_count = width * height;
    const uint64_t vertex_count = uint64_t(vert_per_quad) * quad_count;
    const uint64_t edge_count = uint64_t(edge_per_quad) * quad_count;

    std::vector<float> positions(3u * vertex_count, 0.f);
    std::vector<uint32_t> edges(2u * edge_count, 0u);

    for (uint32_t j = 0u; j < height; ++j)
    {
        for (uint32_t i = 0u; i < width; ++i)
        {
            const float x0 = 1000.f * float(i) / float(width);
            const float x1 = 1000.f * float(i + 1) / float(width);
            const float y0 = 1000.f * float(j) / float(height);
            const float y1 = 1000.f * float(j + 1) / float(height);
            const float z00 = 20.f * std::sin(0.0001f * (x0 * x0 + y0 * y0));
            const float z10 = 20.f * std::sin(0.0001f * (x1 * x1 + y0 * y0));
            const float z01 = 20.f * std::sin(0.0001f * (x0 * x0 + y1 * y1));
            const float z11 = 20.f * std::sin(0.0001f * (x1 * x1 + y1 * y1));

            const uint32_t quad_idx = j * width + i;
            const uint32_t base_vert = vert_per_quad * quad_idx;
            const uint32_t base_pos = 3u * base_vert;

            positions[base_pos + 0u] = x0;
            positions[base_pos + 1u] = y0;
            positions[base_pos + 2u] = z00;
            positions[base_pos + 3u] = x1;
            positions[base_pos + 4u] = y0;
            positions[base_pos + 5u] = z10;
            positions[base_pos + 6u] = x0;
            positions[base_pos + 7u] = y1;
            positions[base_pos + 8u] = z01;
            positions[base_pos + 9u] = x1;
            positions[base_pos + 10u] = y1;
            positions[base_pos + 11u] = z11;

            const uint32_t base_edge = 2u * edge_per_quad * quad_idx;
            edges[base_edge + 0u] = base_vert + 0u;
            edges[base_edge + 1u] = base_vert + 1u;
            edges[base_edge + 2u] = base_vert + 1u;
            edges[base_edge + 3u] = base_vert + 3u;
            edges[base_edge + 4u] = base_vert + 3u;
            edges[base_edge + 5u] = base_vert + 2u;
            edges[base_edge + 6u] = base_vert + 2u;
            edges[base_edge + 7u] = base_vert + 0u;
        }
    }

    FILE* f = std::fopen(path.c_str(), "wb");
    if (!f)
    {
        return false;
    }
    std::fprintf(f, "ply\n");
    std::fprintf(f, "format binary_little_endian 1.0\n");
    std::fprintf(f, "element vertex %zu\n", static_cast<size_t>(vertex_count));
    std::fprintf(f, "property float x\n");
    std::fprintf(f, "property float y\n");
    std::fprintf(f, "property float z\n");
    std::fprintf(f, "element edge %zu\n", static_cast<size_t>(edge_count));
    std::fprintf(f, "property int vertex1\n");
    std::fprintf(f, "property int vertex2\n");
    std::fprintf(f, "end_header\n");
    std::fwrite(positions.data(), sizeof(float), positions.size(), f);
    std::fwrite(edges.data(), 2u * sizeof(uint32_t), static_cast<size_t>(edge_count), f);
    std::fclose(f);

    if (out_vertex_count)
        *out_vertex_count = vertex_count;
    if (out_edge_count)
        *out_edge_count = edge_count;
    return true;
}

// Compiler / compute / device / voxelbvh fixture. init() returns false
// when no Vulkan device is available so the caller can GTEST_SKIP.
struct VoxelBVHRuntime
{
    pnanovdb_compiler_t compiler{};
    pnanovdb_compute_t compute{};
    pnanovdb_compute_device_manager_t* device_manager = nullptr;
    pnanovdb_compute_device_t* device = nullptr;
    pnanovdb_compute_queue_t* queue = nullptr;
    pnanovdb_voxelbvh_t voxelbvh{};
    pnanovdb_voxelbvh_context_t* voxelbvh_ctx = nullptr;
    bool initialized = false;
    bool voxelbvh_unsupported = false;
    bool software_renderer = false;
    std::string device_name;

    bool init()
    {
        pnanovdb_compiler_load(&compiler);
        if (!compiler.module)
        {
            ADD_FAILURE() << "Compiler module not available";
            return false;
        }
        pnanovdb_compute_load(&compute, &compiler);
        if (!compute.module)
        {
            ADD_FAILURE() << "Compute module not available";
            return false;
        }
        device_manager = compute.device_interface.create_device_manager(PNANOVDB_FALSE);
        if (!device_manager)
        {
            ADD_FAILURE() << "Failed to create device manager";
            return false;
        }
        pnanovdb_compute_physical_device_desc_t phys_desc{};
        if (!compute.device_interface.enumerate_devices(device_manager, 0u, &phys_desc))
        {
            return false;
        }
        device_name = phys_desc.device_name;
        if (is_software_renderer_name(phys_desc.device_name) && !software_voxelbvh_tests_force_enabled())
        {
            software_renderer = true;
            return false;
        }
        pnanovdb_compute_device_desc_t device_desc{};
        device = compute.device_interface.create_device(device_manager, &device_desc);
        if (!device)
        {
            ADD_FAILURE() << "Failed to create compute device";
            return false;
        }
        queue = compute.device_interface.get_compute_queue(device);
        if (!queue)
        {
            ADD_FAILURE() << "Failed to acquire compute queue";
            return false;
        }
        pnanovdb_voxelbvh_load(&voxelbvh, &compute);
        if (!voxelbvh.create_context || !voxelbvh.nanovdb_from_triangles_array)
        {
            ADD_FAILURE() << "voxelbvh interface not loaded";
            return false;
        }
        voxelbvh_ctx = voxelbvh.create_context(&compute, queue);
        if (!voxelbvh_ctx)
        {
            voxelbvh_unsupported = true;
            return false;
        }
        initialized = true;
        return true;
    }

    ~VoxelBVHRuntime()
    {
        if (voxelbvh_ctx)
            voxelbvh.destroy_context(&compute, queue, voxelbvh_ctx);
        if (initialized)
            pnanovdb_voxelbvh_free(&voxelbvh);
        if (device)
            compute.device_interface.destroy_device(device_manager, device);
        if (device_manager)
            compute.device_interface.destroy_device_manager(device_manager);
        if (compute.module)
            pnanovdb_compute_free(&compute);
        if (compiler.module)
            pnanovdb_compiler_free(&compiler);
    }
};

} // namespace

class VoxelBVHBuildPipelineTest : public ::testing::Test
{
protected:
    static VoxelBVHRuntime* s_rt;
    static bool s_device_unavailable;
    static bool s_voxelbvh_unsupported;
    static bool s_software_renderer;
    static std::string s_software_renderer_name;

    static void SetUpTestSuite()
    {
        s_rt = new VoxelBVHRuntime();
        if (!s_rt->init())
        {
            s_device_unavailable = (!s_rt->device_manager || !s_rt->device);
            s_voxelbvh_unsupported = s_rt->voxelbvh_unsupported;
            s_software_renderer = s_rt->software_renderer;
            if (s_software_renderer)
            {
                s_software_renderer_name = s_rt->device_name;
            }
        }
    }

    static void TearDownTestSuite()
    {
        delete s_rt;
        s_rt = nullptr;
    }

    void SetUp() override
    {
        if (s_software_renderer)
        {
            GTEST_SKIP() << "Software Vulkan driver detected ('" << s_software_renderer_name
                         << "'); VoxelBVH shaders are too slow to test there. Set "
                            "PNANOVDB_ENABLE_SOFTWARE_VOXELBVH_TESTS=1 to override.";
        }
        if (s_device_unavailable)
        {
            GTEST_SKIP() << "No Vulkan-compatible device available on this machine";
        }
        if (s_voxelbvh_unsupported)
        {
            // MoltenVK does not support 64-bit indexing required by the voxelbvh shaders
            GTEST_SKIP() << "VoxelBVH shader pipeline is not supported on this Vulkan runtime";
        }
        ASSERT_NE(s_rt, nullptr) << "VoxelBVHRuntime failed to initialize";
    }

    VoxelBVHRuntime& rt()
    {
        return *s_rt;
    }
};

VoxelBVHRuntime* VoxelBVHBuildPipelineTest::s_rt = nullptr;
bool VoxelBVHBuildPipelineTest::s_device_unavailable = false;
bool VoxelBVHBuildPipelineTest::s_voxelbvh_unsupported = false;
bool VoxelBVHBuildPipelineTest::s_software_renderer = false;
std::string VoxelBVHBuildPipelineTest::s_software_renderer_name;

TEST_F(VoxelBVHBuildPipelineTest, LinesSynthetic)
{
    ASSERT_NE(rt().voxelbvh.nanovdb_from_lines_array, nullptr) << "nanovdb_from_lines_array not bound";

    // Two 100-unit squares stacked at z=0 and z=50: 8 segments, 16 line indices.
    pnanovdb_compute_array_t* positions = nullptr;
    pnanovdb_compute_array_t* indices = nullptr;
    pnanovdb_compute_array_t* colors = nullptr;
    {
        const float p[] = {
            0.f, 0.f, 0.f,  100.f, 0.f, 0.f,  100.f, 100.f, 0.f,  0.f, 100.f, 0.f,
            0.f, 0.f, 50.f, 100.f, 0.f, 50.f, 100.f, 100.f, 50.f, 0.f, 100.f, 50.f,
        };
        const uint32_t segs[] = {
            0u, 1u, 1u, 2u, 2u, 3u, 3u, 0u, 4u, 5u, 5u, 6u, 6u, 7u, 7u, 4u,
        };
        std::vector<float> cs(sizeof(p) / sizeof(float), 1.f);
        positions = rt().compute.create_array(sizeof(float), sizeof(p) / sizeof(float), p);
        indices = rt().compute.create_array(sizeof(uint32_t), sizeof(segs) / sizeof(uint32_t), segs);
        colors = rt().compute.create_array(sizeof(float), cs.size(), cs.data());
    }
    ASSERT_NE(positions, nullptr);
    ASSERT_NE(indices, nullptr);
    ASSERT_NE(colors, nullptr);

    const pnanovdb_uint32_t resolution = 127u;
    const float inflation_radius = 1.0f;

    pnanovdb_compute_array_t* nanovdb_array = rt().voxelbvh.nanovdb_from_lines_array(
        &rt().compute, rt().queue, rt().voxelbvh_ctx, indices, positions, colors, inflation_radius, resolution);

    expect_nanovdb_populated(nanovdb_array, /*min_bytes=*/4096u);

    rt().compute.destroy_array(nanovdb_array);
    rt().compute.destroy_array(colors);
    rt().compute.destroy_array(positions);
    rt().compute.destroy_array(indices);
}

TEST_F(VoxelBVHBuildPipelineTest, TrianglesSynthetic)
{

    pnanovdb_compute_array_t* indices = nullptr;
    pnanovdb_compute_array_t* positions = nullptr;
    pnanovdb_compute_array_t* colors = nullptr;
    ASSERT_TRUE(build_heightfield_mesh(rt().compute, 16u, 16u, &indices, &positions, &colors));

    const pnanovdb_uint32_t resolution = 127u;
    const float inflation_radius = 0.f;

    pnanovdb_compute_array_t* nanovdb_array = rt().voxelbvh.nanovdb_from_triangles_array(
        &rt().compute, rt().queue, rt().voxelbvh_ctx, indices, positions, colors, inflation_radius, resolution);

    expect_nanovdb_populated(nanovdb_array, /*min_bytes=*/4096u);

    rt().compute.destroy_array(nanovdb_array);
    rt().compute.destroy_array(colors);
    rt().compute.destroy_array(positions);
    rt().compute.destroy_array(indices);
}

// Skipped when the (~137 MB) Stanford dragon PLY isn't present locally.
TEST_F(VoxelBVHBuildPipelineTest, TrianglesFromDragonPly)
{
    if (!dragon_ply_available())
    {
        GTEST_SKIP() << "data/xyzrgb_dragon.ply not found; download via "
                        "graphics.stanford.edu/data/3Dscanrep/xyzrgb/xyzrgb_dragon.ply.gz "
                        "to run this test.";
    }

    pnanovdb_fileformat_t fileformat{};
    pnanovdb_fileformat_load(&fileformat, &rt().compute);
    ASSERT_NE(fileformat.load_file, nullptr) << "file format module unavailable";

    const char* array_names[] = { "positions", "indices" };
    pnanovdb_compute_array_t* mesh_arrays[2] = {};
    pnanovdb_bool_t loaded = fileformat.load_file("./data/xyzrgb_dragon.ply", 2u, array_names, mesh_arrays);
    pnanovdb_fileformat_free(&fileformat);

    ASSERT_TRUE(loaded);
    ASSERT_NE(mesh_arrays[0], nullptr) << "positions array missing";
    ASSERT_NE(mesh_arrays[1], nullptr) << "indices array missing";

    pnanovdb_compute_array_t* positions = mesh_arrays[0];
    pnanovdb_compute_array_t* indices = mesh_arrays[1];

    EXPECT_EQ(positions->element_count % 3u, 0u) << "positions should be a flat float3 stream";
    EXPECT_EQ(indices->element_count % 3u, 0u) << "indices should be a flat triangle index stream";

    pnanovdb_compute_array_t* colors = synthesize_white_colors(rt().compute, positions->element_count);
    ASSERT_NE(colors, nullptr);

    const pnanovdb_uint32_t resolution = 511u;
    const float inflation_radius = 0.f;

    pnanovdb_compute_array_t* nanovdb_array = rt().voxelbvh.nanovdb_from_triangles_array(
        &rt().compute, rt().queue, rt().voxelbvh_ctx, indices, positions, colors, inflation_radius, resolution);

    expect_nanovdb_populated(nanovdb_array, /*min_bytes=*/1u * 1024u * 1024u);

    rt().compute.destroy_array(nanovdb_array);
    rt().compute.destroy_array(colors);
    rt().compute.destroy_array(positions);
    rt().compute.destroy_array(indices);
}

TEST_F(VoxelBVHBuildPipelineTest, LinesFromEdgePly)
{
    ASSERT_NE(rt().voxelbvh.nanovdb_from_lines_array, nullptr) << "nanovdb_from_lines_array not bound";

    const std::filesystem::path edge_ply_path = std::filesystem::temp_directory_path() / "nvdb_editor_test_lines.ply";

    constexpr uint32_t kWidth = 16u;
    constexpr uint32_t kHeight = 16u;
    uint64_t expected_vert_count = 0u;
    uint64_t expected_edge_count = 0u;
    ASSERT_TRUE(
        save_wireframe_lines_ply(edge_ply_path.string(), kWidth, kHeight, &expected_vert_count, &expected_edge_count))
        << "failed to write " << edge_ply_path;

    pnanovdb_fileformat_t fileformat{};
    pnanovdb_fileformat_load(&fileformat, &rt().compute);
    ASSERT_NE(fileformat.load_file, nullptr) << "file format module unavailable";

    // Loader falls back to edge data when no triangle faces are present.
    const char* array_names[] = { "positions", "indices" };
    pnanovdb_compute_array_t* arrays[2] = {};
    pnanovdb_bool_t loaded = fileformat.load_file(edge_ply_path.string().c_str(), 2u, array_names, arrays);
    pnanovdb_fileformat_free(&fileformat);

    ASSERT_TRUE(loaded);
    ASSERT_NE(arrays[0], nullptr) << "positions array missing";
    ASSERT_NE(arrays[1], nullptr) << "indices array missing";

    pnanovdb_compute_array_t* positions = arrays[0];
    pnanovdb_compute_array_t* indices = arrays[1];

    EXPECT_EQ(positions->element_size, sizeof(float));
    EXPECT_EQ(positions->element_count, 3u * expected_vert_count) << "expected 3 floats per vertex";
    EXPECT_EQ(indices->element_size, 2u * sizeof(uint32_t)) << "indices must be uint2-packed for the lines pipeline";
    EXPECT_EQ(indices->element_count, expected_edge_count) << "expected one uint2 per edge";

    pnanovdb_compute_array_t* colors = synthesize_white_colors(rt().compute, positions->element_count);
    ASSERT_NE(colors, nullptr);

    const pnanovdb_uint32_t resolution = 127u;
    const float inflation_radius = 2.f;

    pnanovdb_compute_array_t* nanovdb_array = rt().voxelbvh.nanovdb_from_lines_array(
        &rt().compute, rt().queue, rt().voxelbvh_ctx, indices, positions, colors, inflation_radius, resolution);

    expect_nanovdb_populated(nanovdb_array, /*min_bytes=*/4096u);

    rt().compute.destroy_array(nanovdb_array);
    rt().compute.destroy_array(colors);
    rt().compute.destroy_array(positions);
    rt().compute.destroy_array(indices);

    std::error_code ec;
    std::filesystem::remove(edge_ply_path, ec);
}
