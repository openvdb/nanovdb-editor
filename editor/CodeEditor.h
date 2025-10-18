// Copyright Contributors to the OpenVDB Project
// SPDX-License-Identifier: Apache-2.0

/*!
    \file   nanovdb_editor/editor/CodeEditor.h

    \author Petra Hapalova

    \brief
*/

#pragma once

#include "nanovdb_editor/putil/Compute.h"
#include "nanovdb_editor/putil/Shader.hpp"

#ifndef IMGUI_DEFINE_MATH_OPERATORS
#    define IMGUI_DEFINE_MATH_OPERATORS
#endif // IMGUI_DEFINE_MATH_OPERATORS

#include <imgui/ImguiTLS.h>
#include <imgui.h>
#include <TextEditor.h>

#include <string>
#include <filesystem>
#include <atomic>

struct ImGuiSettingsHandler;
struct ImGuiTextBuffer;

namespace pnanovdb_editor
{
class CodeEditor
{
    static const char* defaultName;

    enum class ShowOption
    {
        ShaderOnly,
        Generated,
        ShaderParams,
        Last
    };

    struct EditorTab
    {
        TextEditor editor;
        TextEditor viewer; // viewer for generated HLSL code or shader params
        std::string title;
        std::string filepath;
        std::string shaderParamsFilepath;
        bool opened = false;
        int editorUndoIndex = -1;

        // serialized in the settings
        std::string shaderName;
        int firstVisibleLine = -1;
        int viewerFirstVisibleLine = -1;

        void rename(const char* name)
        {
            shaderName = name;
            std::filesystem::path fsPath(shaderName);
            title = fsPath.filename().string();
            filepath = pnanovdb_shader::getShaderFilePath(shaderName.c_str());
            shaderParamsFilepath = pnanovdb_shader::getShaderParamsFilePath(shaderName.c_str());
        }

        EditorTab() : EditorTab(defaultName)
        {
        }

        EditorTab(const char* shaderName)
        {
            rename(shaderName);

            opened = true;
            editorUndoIndex = 0;
            editor.SetLanguage(TextEditor::Language::Hlsl());
        }
    };

public:
    static CodeEditor& getInstance()
    {
        static CodeEditor instance;
        return instance;
    }

    bool render();
    void setup(std::string* shaderNamePtr,
               std::atomic<bool>* updateShaderPtr,
               ImVec2& dialogSize,
               pnanovdb_shader::run_shader_func_t runShader,
               bool restrictFileAccess);
    void setSelectedShader(const std::string& shaderName);
    void updateViewer();
    void saveShaderParams();
    void registerSettingsHandler(ImGuiContext* context);
    void saveTabsState();
    const std::string& getSelectedShader() const;

private:
    CodeEditor();
    ~CodeEditor() = default;

    CodeEditor(const CodeEditor&) = delete;
    CodeEditor& operator=(const CodeEditor&) = delete;
    CodeEditor(CodeEditor&&) = delete;
    CodeEditor& operator=(CodeEditor&&) = delete;

    static void ClearAll(ImGuiContext* ctx, ImGuiSettingsHandler* handler);
    static void* ReadOpen(ImGuiContext* ctx, ImGuiSettingsHandler* handler, const char* name);
    static void ReadLine(ImGuiContext* ctx, ImGuiSettingsHandler* handler, void* entry, const char* line);
    static void WriteAll(ImGuiContext* ctx, ImGuiSettingsHandler* handler, ImGuiTextBuffer* buf);
    static void ApplyAll(ImGuiContext* ctx, ImGuiSettingsHandler* handler);

    void setSelectedFile(const std::string& filepath);
    void addNewFile();
    void saveSelectedTabText();
    void openFileDialog(const char* dialogKey, const char* title, const char* filters);
    bool isPathWithinRoot(const std::string& path, const std::string& root) const;

    std::map<std::string, EditorTab> tabs_; // key is shader name
    std::string selectedTab_;
    ShowOption showOption_;
    ShowOption selectedOption_;
    std::string* editorShaderPtr_;
    std::atomic<bool>* updateShaderPtr_;
    pnanovdb_shader::run_shader_func_t runShader_;
    bool restricDirAccess_ = false;
    ImVec2 editorSize_;
    ImVec2 dialogSize_;
    bool isEditorLastClicked_ = false;
    int gridDims_[3] = { 1, 1, 1 };
    std::string rootPath_; // Root path for file browser restriction
};
}
