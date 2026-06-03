// Copyright Contributors to the OpenVDB Project
// SPDX-License-Identifier: Apache-2.0

#include <gtest/gtest.h>

#include <nanovdb_editor/putil/Compiler.h>
#include <nanovdb_editor/putil/Compute.h>
#include <nanovdb_editor/putil/Editor.h>

#include "editor/PipelineTypes.h" // canonical pnanovdb_pipeline_type_* enum

#include <nanovdb/tools/CreatePrimitives.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <string>
#include <thread>
#include <vector>

namespace
{

struct SyntheticGaussian
{
    pnanovdb_compute_array_t* means = nullptr;
    pnanovdb_compute_array_t* opacities = nullptr;
    pnanovdb_compute_array_t* quaternions = nullptr;
    pnanovdb_compute_array_t* scales = nullptr;
    pnanovdb_compute_array_t* sh_0 = nullptr;
    pnanovdb_compute_array_t* sh_n = nullptr;

    void destroy(pnanovdb_compute_t& compute)
    {
        for (auto* arr : { means, opacities, quaternions, scales, sh_0, sh_n })
        {
            if (arr)
            {
                compute.destroy_array(arr);
            }
        }
        means = opacities = quaternions = scales = sh_0 = sh_n = nullptr;
    }
};

SyntheticGaussian make_gaussian(pnanovdb_compute_t& compute, std::uint32_t splat_count, float jitter)
{
    SyntheticGaussian out;

    std::vector<float> means(3u * splat_count);
    std::vector<float> opacities(splat_count, 1.0f);
    std::vector<float> quats(4u * splat_count);
    std::vector<float> scales(3u * splat_count, -2.0f);
    std::vector<float> sh0(3u * splat_count, 0.5f);
    std::vector<float> shN(45u * splat_count, 0.0f);

    for (std::uint32_t i = 0u; i < splat_count; ++i)
    {
        means[3u * i + 0u] = float(i & 1u) + jitter;
        means[3u * i + 1u] = float((i >> 1u) & 1u) + jitter;
        means[3u * i + 2u] = float((i >> 2u) & 1u) + jitter;

        quats[4u * i + 0u] = 1.0f;
        quats[4u * i + 1u] = 0.0f;
        quats[4u * i + 2u] = 0.0f;
        quats[4u * i + 3u] = 0.0f;
    }

    out.means = compute.create_array(sizeof(float), means.size(), means.data());
    out.opacities = compute.create_array(sizeof(float), opacities.size(), opacities.data());
    out.quaternions = compute.create_array(sizeof(float), quats.size(), quats.data());
    out.scales = compute.create_array(sizeof(float), scales.size(), scales.data());
    out.sh_0 = compute.create_array(sizeof(float), sh0.size(), sh0.data());
    out.sh_n = compute.create_array(sizeof(float), shN.size(), shN.data());
    return out;
}

pnanovdb_compute_array_t* make_nanovdb_sphere(pnanovdb_compute_t& compute, float radius)
{
    auto sphere = nanovdb::tools::createLevelSetSphere<float>(radius);
    return compute.create_array(4u, sphere.size() / 4u, sphere.data());
}

} // namespace

TEST(NanoVDBEditor, ConcurrentPipelineRegistrationViaPublicApi)
{
    pnanovdb_compiler_t compiler = {};
    pnanovdb_compiler_load(&compiler);
    ASSERT_NE(compiler.module, nullptr) << "Compiler module not available";

    pnanovdb_compute_t compute = {};
    pnanovdb_compute_load(&compute, &compiler);
    ASSERT_NE(compute.module, nullptr) << "Failed to load compute module";

    pnanovdb_compute_device_desc_t device_desc = {};
    pnanovdb_compute_device_manager_t* device_manager = compute.device_interface.create_device_manager(PNANOVDB_FALSE);
    ASSERT_NE(device_manager, nullptr) << "Failed to create compute device manager";

    pnanovdb_compute_physical_device_desc_t phys_desc = {};
    if (!compute.device_interface.enumerate_devices(device_manager, 0u, &phys_desc))
    {
        compute.device_interface.destroy_device_manager(device_manager);
        pnanovdb_compute_free(&compute);
        pnanovdb_compiler_free(&compiler);
        GTEST_SKIP() << "No Vulkan-compatible device available on this machine";
    }

    pnanovdb_compute_device_t* device = compute.device_interface.create_device(device_manager, &device_desc);
    ASSERT_NE(device, nullptr) << "Failed to create compute device";

    pnanovdb_editor_t editor = {};
    pnanovdb_editor_load(&editor, &compute, &compiler);
    ASSERT_NE(editor.module, nullptr) << "Editor module failed to load";

    pnanovdb_editor_config_t cfg = {};
    cfg.ip_address = "127.0.0.1";
    cfg.port = 8092;
    cfg.headless = PNANOVDB_TRUE;
    cfg.streaming = PNANOVDB_FALSE;

    editor.start(&editor, device, &cfg);
    std::this_thread::sleep_for(std::chrono::milliseconds(500));

    constexpr std::uint32_t kPerThreadObjects = 16u;
    constexpr std::uint32_t kSplatCount = 8u;

    std::vector<SyntheticGaussian> gaussian_pool;
    std::vector<pnanovdb_compute_array_t*> nanovdb_pool;
    gaussian_pool.reserve(kPerThreadObjects);
    nanovdb_pool.reserve(kPerThreadObjects);
    for (std::uint32_t i = 0u; i < kPerThreadObjects; ++i)
    {
        gaussian_pool.push_back(make_gaussian(compute, kSplatCount, 0.1f * float(i)));
        nanovdb_pool.push_back(make_nanovdb_sphere(compute, 4.0f + float(i)));
        ASSERT_NE(nanovdb_pool.back(), nullptr);
    }

    std::atomic<int> gaussian_added{ 0 };
    std::atomic<int> nanovdb_added{ 0 };

    std::thread gaussian_thread(
        [&]()
        {
            pnanovdb_editor_token_t* scene = editor.get_token("scene_gaussian");
            for (std::uint32_t i = 0u; i < kPerThreadObjects; ++i)
            {
                std::string name = "g_" + std::to_string(i);
                pnanovdb_editor_token_t* name_tok = editor.get_token(name.c_str());

                pnanovdb_editor_gaussian_data_desc_t desc = {};
                desc.means = gaussian_pool[i].means;
                desc.opacities = gaussian_pool[i].opacities;
                desc.quaternions = gaussian_pool[i].quaternions;
                desc.scales = gaussian_pool[i].scales;
                desc.sh_0 = gaussian_pool[i].sh_0;
                desc.sh_n = gaussian_pool[i].sh_n;

                editor.add_gaussian_data_3(
                    &editor, scene, name_tok, &desc, pnanovdb_pipeline_type_noop, pnanovdb_pipeline_type_raster2d);

                editor.set_visible(&editor, scene, name_tok, PNANOVDB_TRUE);
                editor.mark_pipeline_dirty(&editor, scene, name_tok);

                gaussian_added.fetch_add(1, std::memory_order_relaxed);
            }
        });

    std::thread nanovdb_thread(
        [&]()
        {
            pnanovdb_editor_token_t* scene = editor.get_token("scene_nanovdb");
            for (std::uint32_t i = 0u; i < kPerThreadObjects; ++i)
            {
                std::string name = "n_" + std::to_string(i);
                pnanovdb_editor_token_t* name_tok = editor.get_token(name.c_str());

                editor.add_nanovdb_3(&editor, scene, name_tok, nanovdb_pool[i], pnanovdb_pipeline_type_noop,
                                     pnanovdb_pipeline_type_nanovdb_render);

                editor.set_visible(&editor, scene, name_tok, PNANOVDB_TRUE);
                editor.mark_pipeline_dirty(&editor, scene, name_tok);

                nanovdb_added.fetch_add(1, std::memory_order_relaxed);
            }
        });

    gaussian_thread.join();
    nanovdb_thread.join();

    EXPECT_EQ(gaussian_added.load(), int(kPerThreadObjects));
    EXPECT_EQ(nanovdb_added.load(), int(kPerThreadObjects));

    pnanovdb_editor_token_t* scene_g = editor.get_token("scene_gaussian");
    pnanovdb_editor_token_t* scene_n = editor.get_token("scene_nanovdb");

    auto last_g_tok = editor.get_token(("g_" + std::to_string(kPerThreadObjects - 1u)).c_str());
    auto last_n_tok = editor.get_token(("n_" + std::to_string(kPerThreadObjects - 1u)).c_str());

    bool settled = false;
    for (int i = 0; i < 200 && !settled; ++i)
    {
        settled = (editor.get_pipeline(&editor, scene_g, last_g_tok, pnanovdb_pipeline_stage_render) ==
                   pnanovdb_pipeline_type_raster2d) &&
                  (editor.get_pipeline(&editor, scene_n, last_n_tok, pnanovdb_pipeline_stage_render) ==
                   pnanovdb_pipeline_type_nanovdb_render);
        if (!settled)
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
    }
    EXPECT_TRUE(settled) << "Render thread never observed the final concurrent add within 2s";

    for (std::uint32_t i = 0u; i < kPerThreadObjects; ++i)
    {
        std::string g_name = "g_" + std::to_string(i);
        std::string n_name = "n_" + std::to_string(i);
        pnanovdb_editor_token_t* g_tok = editor.get_token(g_name.c_str());
        pnanovdb_editor_token_t* n_tok = editor.get_token(n_name.c_str());

        EXPECT_EQ(editor.get_pipeline(&editor, scene_g, g_tok, pnanovdb_pipeline_stage_render),
                  pnanovdb_pipeline_type_raster2d)
            << "Gaussian object " << i << " has wrong render pipeline";
        EXPECT_EQ(editor.get_pipeline(&editor, scene_n, n_tok, pnanovdb_pipeline_stage_render),
                  pnanovdb_pipeline_type_nanovdb_render)
            << "NanoVDB object " << i << " has wrong render pipeline";

        EXPECT_EQ(editor.get_visible(&editor, scene_g, g_tok), PNANOVDB_TRUE);
        EXPECT_EQ(editor.get_visible(&editor, scene_n, n_tok), PNANOVDB_TRUE);
    }

    for (auto& g : gaussian_pool)
    {
        g.destroy(compute);
    }
    for (auto* arr : nanovdb_pool)
    {
        compute.destroy_array(arr);
    }

    editor.stop(&editor);
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    pnanovdb_editor_free(&editor);

    compute.device_interface.destroy_device(device_manager, device);
    compute.device_interface.destroy_device_manager(device_manager);
    pnanovdb_compute_free(&compute);
    pnanovdb_compiler_free(&compiler);
}
