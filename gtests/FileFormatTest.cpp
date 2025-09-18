// Copyright Contributors to the OpenVDB Project
// SPDX-License-Identifier: Apache-2.0

#include <gtest/gtest.h>

#include <nanovdb_editor/putil/Compute.h>
#include <nanovdb_editor/putil/FileFormat.h>
#include <filesystem>
#include <cstring>
#include <cstdarg>

void pnanovdb_compute_log_print(pnanovdb_compute_log_level_t level, const char* format, ...)
{
    va_list args;
    va_start(args, format);

    const char* prefix = "Unknown";
    if (level == PNANOVDB_COMPUTE_LOG_LEVEL_ERROR)
    {
        prefix = "Error";
    }
    else if (level == PNANOVDB_COMPUTE_LOG_LEVEL_WARNING)
    {
        prefix = "Warning";
    }
    else if (level == PNANOVDB_COMPUTE_LOG_LEVEL_INFO)
    {
        prefix = "Info";
    }
    printf("%s: ", prefix);
    vprintf(format, args);
    printf("\n");

    va_end(args);
}

TEST(NanoVDBEditor, FileFormatLoadsIngpFile)
{
    const std::filesystem::path filename =
        std::filesystem::path(__FILE__).parent_path().parent_path() / "data" / "ficus-30k.ingp";

    // Skip test if test data file doesn't exist
    if (!std::filesystem::exists(filename))
    {
        GTEST_SKIP() << "Test data file not found: " << filename.string() << " - skipping INGP format test";
    }

    // Load compute interface
    pnanovdb_compute_t compute = {};
    pnanovdb_compute_load(&compute, nullptr); // No compiler needed for file loading

    // Load file format interface
    pnanovdb_fileformat_t fileformat = {};
    pnanovdb_fileformat_load(&fileformat, &compute);

    if (compute.module == nullptr)
    {
        FAIL() << "Failed to load compute module";
    }

    if (fileformat.module == nullptr)
    {
        FAIL() << "Failed to load file format module";
    }

    // Define expected array names for INGP format
    const char* array_names[] = { "mog_positions", "mog_densities", "mog_rotations", "mog_scales", "mog_features" };
    const int ARRAY_COUNT = 5;

    pnanovdb_compute_array_t* arrays[ARRAY_COUNT] = {};

    // Load the file
    pnanovdb_bool_t loaded = fileformat.load_file(filename.string().c_str(), ARRAY_COUNT, array_names, arrays);

    if (loaded == PNANOVDB_TRUE)
    {
        // Verify that arrays were loaded
        for (int i = 0; i < ARRAY_COUNT; i++)
        {
            if (arrays[i])
            {
                EXPECT_GT(arrays[i]->element_count, 0) << "Array " << array_names[i] << " has zero elements";
                EXPECT_GT(arrays[i]->element_size, 0) << "Array " << array_names[i] << " has zero element size";

                // Clean up array
                compute.destroy_array(arrays[i]);
            }
            else
            {
                // Some arrays might be optional, so this is not necessarily an error
                printf("Array %s was not loaded (this might be normal)\n", array_names[i]);
            }
        }
    }
    else
    {
        FAIL() << "Failed to load INGP file: " << filename.string();
    }

    // Clean up
    pnanovdb_fileformat_free(&fileformat);
    pnanovdb_compute_free(&compute);
}

TEST(NanoVDBEditor, FileFormatLoadsPlyFile)
{
    const std::filesystem::path filename =
        std::filesystem::path(__FILE__).parent_path().parent_path() / "data" / "ficus.ply";

    // Skip test if test data file doesn't exist
    if (!std::filesystem::exists(filename))
    {
        GTEST_SKIP() << "Test data file not found: " << filename.string() << " - skipping PLY format test";
    }

    // Load compute interface
    pnanovdb_compute_t compute = {};
    pnanovdb_compute_load(&compute, nullptr); // No compiler needed for file loading

    // Load file format interface
    pnanovdb_fileformat_t fileformat = {};
    pnanovdb_fileformat_load(&fileformat, &compute);

    if (compute.module == nullptr)
    {
        FAIL() << "Failed to load compute module";
    }

    if (fileformat.module == nullptr)
    {
        FAIL() << "Failed to load file format module";
    }

    // Define expected array names for PLY format
    const char* array_names[] = { "means", "opacities", "quaternions", "scales", "sh" };
    const int ARRAY_COUNT = 5;

    pnanovdb_compute_array_t* arrays[ARRAY_COUNT] = {};

    // Load the file
    pnanovdb_bool_t loaded = fileformat.load_file(filename.string().c_str(), ARRAY_COUNT, array_names, arrays);

    if (loaded == PNANOVDB_TRUE)
    {
        // Verify that arrays were loaded
        for (int i = 0; i < ARRAY_COUNT; i++)
        {
            if (arrays[i])
            {
                EXPECT_GT(arrays[i]->element_count, 0) << "Array " << array_names[i] << " has zero elements";
                EXPECT_GT(arrays[i]->element_size, 0) << "Array " << array_names[i] << " has zero element size";

                // Clean up array
                compute.destroy_array(arrays[i]);
            }
            else
            {
                // Some arrays might be optional, so this is not necessarily an error
                printf("Array %s was not loaded (this might be normal)\n", array_names[i]);
            }
        }
    }
    else
    {
        FAIL() << "Failed to load PLY file: " << filename.string();
    }

    // Clean up
    pnanovdb_fileformat_free(&fileformat);
    pnanovdb_compute_free(&compute);
}

TEST(NanoVDBEditor, FileFormatHandlesMissingFile)
{
    const std::filesystem::path filename =
        std::filesystem::path(__FILE__).parent_path() / "data" / "nonexistent_file.ingp";

    // Load compute interface
    pnanovdb_compute_t compute = {};
    pnanovdb_compute_load(&compute, nullptr);

    // Load file format interface
    pnanovdb_fileformat_t fileformat = {};
    pnanovdb_fileformat_load(&fileformat, &compute);

    if (compute.module == nullptr)
    {
        FAIL() << "Failed to load compute module";
    }

    if (fileformat.module == nullptr)
    {
        FAIL() << "Failed to load file format module";
    }

    // Try to load a non-existent file
    const char* array_names[] = { "test" };
    const int ARRAY_COUNT = 1;

    pnanovdb_compute_array_t* arrays[ARRAY_COUNT] = {};

    // This should fail gracefully
    pnanovdb_bool_t loaded = fileformat.load_file(filename.string().c_str(), ARRAY_COUNT, array_names, arrays);
    EXPECT_EQ(loaded, PNANOVDB_FALSE) << "Loading non-existent file should fail";

    // Clean up
    pnanovdb_fileformat_free(&fileformat);
    pnanovdb_compute_free(&compute);
}
