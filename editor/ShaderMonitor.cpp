// Copyright Contributors to the OpenVDB Project
// SPDX-License-Identifier: Apache-2.0

/*!
    \file   nanovdb_editor/editor/ShaderMonitor.cpp

    \author Petra Hapalova

    \brief
*/

#include "ShaderMonitor.h"
#include "nanovdb_editor/putil/Shader.hpp"

#include <filesystem>
#include <regex>
#include <chrono>
#include <thread>
#include <iostream>

namespace fs = std::filesystem;

namespace pnanovdb_editor
{
static const std::string shaderExtensions = ".*\\.(slang|slang\\.tmp)$";

void ShaderMonitor::addPath(const std::string& path, ShaderCallback callback)
{
    std::string resolvedPath = pnanovdb_shader::resolveSymlink(path).string();
    if (watchers.find(resolvedPath) == watchers.end())
    {
        std::basic_regex<char> regexPattern(shaderExtensions);
        watchers[resolvedPath] = std::make_unique<filewatch::FileWatch<std::string>>(
            path, regexPattern,
            [this, path, callback](const std::string& filename, const filewatch::Event changeType)
            {
                // runs on a worker thread
                fs::path filePath = fs::path(path) / fs::path(filename);
                std::string filePathStr = filePath.string();

#ifdef WIN32
                // filewatch::Event::renamed_new not working same as on Linux - monitors the old file
                if (changeType == filewatch::Event::modified && filePath.extension() == ".tmp" &&
                    filePath.stem().extension() == ".slang") {
                    // Remove .tmp extension
                    filePath.replace_extension(""); // removes .tmp, leaves .slang
                    filePathStr = filePath.string();
                }
#endif
                if (changeType == filewatch::Event::modified || changeType == filewatch::Event::renamed_new ||
                    changeType == filewatch::Event::added)
                {
                    auto now = std::chrono::steady_clock::now();
                    auto it = lastEventTime.find(filePathStr);
                    if (it != lastEventTime.end())
                    {
                        auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(now - it->second).count();
                        if (duration < 500)
                        {
                            // ignore events within 500 milliseconds
                            return;
                        }
                    }
                    lastEventTime[filePathStr] = now;

                    std::cout << "Shader to recompile: " << filePathStr << std::endl;
                    if (callback)
                    {
                        std::thread workerThread(callback, filePathStr);
                        workerThread.detach();
                    }
                }
            });
        std::cout << "Started monitoring: " << path << std::endl;
    }

    // recursively check for symlinks in the path
    for (const auto& entry : fs::recursive_directory_iterator(resolvedPath))
    {
        if (fs::is_directory(entry.path()) && pnanovdb_shader::isSymlink(entry.path()))
        {
            fs::path linkedPath = pnanovdb_shader::resolveSymlink(entry.path().string());
            if (fs::is_directory(linkedPath))
            {
                addPath(linkedPath.string(), callback);
            }
        }
    }
}

void ShaderMonitor::removePath(const std::string& path)
{
    auto it = watchers.find(path);
    if (it != watchers.end())
    {
        watchers.erase(it);
        std::cout << "Stopped monitoring: " << path << std::endl;
    }
}

std::vector<std::string> ShaderMonitor::getMonitoredPaths() const
{
    std::vector<std::string> paths;
    for (const auto& watcher : watchers)
    {
        paths.push_back(watcher.first);
    }
    return paths;
}

void monitor_shader_dir(const char* path, ShaderCallback callback)
{
    ShaderMonitor::getInstance().addPath(path, callback);
}
}
