// Copyright Contributors to the OpenVDB Project
// SPDX-License-Identifier: Apache-2.0

#include <gtest/gtest.h>

#include <nanovdb_editor/putil/Compiler.h>
#include <nanovdb_editor/putil/Compute.h>
#include <filesystem>
#include <cstring>
#include <vector>
#include <iostream>

#ifdef _WIN32
#    include <windows.h>
#else
#    include <dlfcn.h>
#    include <libgen.h>
#    include <unistd.h>
#endif

#define SLANG_PRELUDE_NAMESPACE CPPPrelude
#include <slang-cpp-types.h>

struct constants_t
{
    int magic_number;
    int pad1;
    int pad2;
    int pad3;
};

TEST(NanoVDBEditor, ShaderCompilesViaCpuCompiler)
{
    const std::filesystem::path shader = std::filesystem::path(__FILE__).parent_path() / "shaders" / "test.slang";
    const std::string shader_path = shader.string();

    // Load the compiler
    pnanovdb_compiler_t compiler = {};
    pnanovdb_compiler_load(&compiler);

    if (compiler.module == nullptr)
    {
        FAIL() << "Compiler module not available";
    }

    char slangLlvmPath[1024] = { 0 };
#ifdef _WIN32
    HMODULE slangModule = GetModuleHandleA("slang.dll");
    if (slangModule)
    {
        GetModuleFileNameA(slangModule, slangLlvmPath, sizeof(slangLlvmPath));
        char* lastSlash = strrchr(slangLlvmPath, '\\');
        if (lastSlash)
        {
            *(lastSlash + 1) = '\0';
            strcat(slangLlvmPath, "slang-llvm.dll");
        }
    }
#else
    if (compiler.module != nullptr)
    {
        Dl_info info;
        void* getCompilerFunc = dlsym(compiler.module, "pnanovdb_get_compiler");
        if (getCompilerFunc && dladdr(getCompilerFunc, &info) && info.dli_fname)
        {
            char pathCopy[1024];
            strncpy(pathCopy, info.dli_fname, sizeof(pathCopy));
            pathCopy[sizeof(pathCopy) - 1] = 0;
            char* dir = dirname(pathCopy);
            snprintf(slangLlvmPath, sizeof(slangLlvmPath), "%s/libslang-llvm.so", dir);
        }
    }
#endif
    printf("Slang LLVM path: %s\n", slangLlvmPath);

    bool slangLlvmAvailable = false;
#ifdef _WIN32
    slangLlvmAvailable = GetFileAttributesA(slangLlvmPath) != INVALID_FILE_ATTRIBUTES;
#else
    slangLlvmAvailable = access(slangLlvmPath, F_OK) == 0;
#endif

    if (!slangLlvmAvailable)
    {
        GTEST_SKIP() << "Slang LLVM not found at: " << slangLlvmPath;
    }

    // Create compiler instance
    pnanovdb_compiler_instance_t* compiler_inst = compiler.create_instance();
    ASSERT_NE(compiler_inst, nullptr);

    // Set up compilation settings for CPU target
    pnanovdb_compiler_settings_t compile_settings = {};
    pnanovdb_compiler_settings_init(&compile_settings);
    compile_settings.compile_target = PNANOVDB_COMPILE_TARGET_CPU;
    std::strcpy(compile_settings.entry_point_name, "computeMain");

    // Compile the shader
    pnanovdb_bool_t result =
        compiler.compile_shader_from_file(compiler_inst, shader_path.c_str(), &compile_settings, nullptr);

    ASSERT_NE(result, PNANOVDB_FALSE) << "Compilation of CPU shader failed: " << shader_path;

    // compiler.destroy_instance(compiler_inst);
    // pnanovdb_compiler_free(&compiler);
}
