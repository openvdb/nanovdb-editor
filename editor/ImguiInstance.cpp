// Copyright Contributors to the OpenVDB Project
// SPDX-License-Identifier: Apache-2.0

/*!
    \file   nanovdb_editor/editor/ImguiInstance.cpp

    \author Andrew Reidmeyer

    \brief
*/

#include "Editor.h"

PNANOVDB_CAST_PAIR(pnanovdb_editor_token_t, pnanovdb_compute_token_t)

static const pnanovdb_uint32_t s_default_width = 1440u;
static const pnanovdb_uint32_t s_default_height = 720u;

#ifndef IMGUI_DEFINE_MATH_OPERATORS
#    define IMGUI_DEFINE_MATH_OPERATORS
#endif // IMGUI_DEFINE_MATH_OPERATORS

#include "imgui.h"
#include <imgui_internal.h> // for the docking branch

// ImGui context TLS - configured in cmake/patch_imgui.cmake via imconfig.h
// GImGui is #defined to ImGuiTLS, so this must be defined for imgui to link
thread_local ImGuiContext* ImGuiTLS = nullptr;

#include <ImGuiFileDialog.h>

namespace imgui_instance_user
{

struct tree_state_t
{
    pnanovdb_uint64_t path;
    pnanovdb_uint64_t selected_path;
    pnanovdb_uint32_t level;
    pnanovdb_uint32_t selected_level;
};

struct Instance
{
    InstanceUserdata* instance_user_data = nullptr;
    pnanovdb_imgui_settings_render_t* settings = nullptr;
    bool is_docking_setup = false;
    bool show_render_settings = true;
    bool show_scene = true;
    bool show_properties = true;

    tree_state_t tree_state = {};
    int32_t property_selected_stage_idx = -1;
    int32_t property_selected_pipeline_idx = 0;
    int32_t property_selected_pipeline_type = -1;

    char object_rename_buf[256u] = {};
    char scene_rename_buf[256u] = {};
};
PNANOVDB_CAST_PAIR(Instance, pnanovdb_imgui_instance_t)

pnanovdb_imgui_instance_t* create(void* userdata,
                                  const void* user_settings,
                                  const pnanovdb_reflect_data_type_t* user_settings_data_type)
{
    auto ptr = new Instance();

    ptr->instance_user_data = (InstanceUserdata*)userdata;
    ptr->settings = (pnanovdb_imgui_settings_render_t*)user_settings;

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();

    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;

    // Set up clipboard callbacks to use GLFW/system clipboard
    io.ClipboardUserData = nullptr;
    io.SetClipboardTextFn = [](void*, const char* text) { pnanovdb_imgui_set_system_clipboard(text); };
    io.GetClipboardTextFn = [](void*) -> const char* { return pnanovdb_imgui_get_system_clipboard(); };

    // Register settings handlers
    ImGuiContext* context = ImGui::GetCurrentContext();

    return cast(ptr);
}

void destroy(pnanovdb_imgui_instance_t* instance)
{
    auto ptr = cast(instance);

    ImGui::DestroyContext();

    delete ptr;
}

void update_docking(pnanovdb_imgui_instance_t* instance);
void showRenderSettingsWindow(pnanovdb_imgui_instance_t* instance, pnanovdb_imgui_settings_render_t* settings);
void showSceneWindow(pnanovdb_imgui_instance_t* instance, InstanceUserdata* instance_user_data);
void showPropertiesWindow(pnanovdb_imgui_instance_t* instance, InstanceUserdata* instance_user_data);

void update(pnanovdb_imgui_instance_t* instance)
{
    auto ptr = cast(instance);

    ImGui::NewFrame();

    update_docking(instance);

    // bool show_demo_window = true;
    // ImGui::ShowDemoWindow(&show_demo_window);

    showSceneWindow(instance, ptr->instance_user_data);
    showRenderSettingsWindow(instance, ptr->settings);
    showPropertiesWindow(instance, ptr->instance_user_data);

    ImGui::Render();
}

ImGuiStyle* get_style(pnanovdb_imgui_instance_t* instance)
{
    ImGui::StyleColorsDark();
    ImGuiStyle& s = ImGui::GetStyle();

    return &s;
}

ImGuiIO* get_io(pnanovdb_imgui_instance_t* instance)
{
    ImGuiIO& io = ImGui::GetIO();

    return &io;
}

void get_tex_data_as_rgba32(pnanovdb_imgui_instance_t* instance, unsigned char** out_pixels, int* out_width, int* out_height)
{
    ImGuiIO& io = ImGui::GetIO();

    io.Fonts->GetTexDataAsRGBA32(out_pixels, out_width, out_height);
}

ImDrawData* get_draw_data(pnanovdb_imgui_instance_t* instance)
{
    return ImGui::GetDrawData();
}

#define IMGUI_CHECKBOX_SYNC(label, var)                                                                                \
    {                                                                                                                  \
        bool temp_bool = ((var) != PNANOVDB_FALSE);                                                                    \
        if (ImGui::Checkbox((label), &temp_bool))                                                                      \
        {                                                                                                              \
            (var) = temp_bool ? PNANOVDB_TRUE : PNANOVDB_FALSE;                                                        \
        }                                                                                                              \
    }

static inline bool showResolutionCombo(const char* label, pnanovdb_int32_t* width, pnanovdb_int32_t* height)
{
    bool changed = false;
    if (ImGui::BeginCombo(label, "Select..."))
    {
        const char* labels[4] = { "1440x720", "1920x1080", "2560x1440", "3840x2160" };
        const int widths[4] = { 1440, 1920, 2560, 3840 };
        const int heights[4] = { 720, 1080, 1440, 2160 };
        for (int i = 0; i < 4; i++)
        {
            if (ImGui::Selectable(labels[i]))
            {
                *width = widths[i];
                *height = heights[i];
                changed = true;
            }
        }
        ImGui::EndCombo();
    }
    return changed;
}

void update_docking(pnanovdb_imgui_instance_t* instance)
{
    auto ptr = cast(instance);

    // init docking
    ImGuiID dockspace_id = 1u;
    ImGui::DockSpaceOverViewport(dockspace_id, nullptr, ImGuiDockNodeFlags_PassthruCentralNode);

    if (ptr->is_docking_setup)
    {
        return;
    }
    ptr->is_docking_setup = true;

    pnanovdb_int32_t window_width = ptr->settings->window_width;
    pnanovdb_int32_t window_height = ptr->settings->window_height;

    float right_dock_width = window_width * 0.20f;

    // clear existing layout
    ImGui::DockBuilderRemoveNode(dockspace_id);
    ImGui::DockBuilderAddNode(dockspace_id, ImGuiDockNodeFlags_DockSpace);

    // create dock spaces for various windows, the order matters!
    ImGuiID dock_id_right = ImGui::DockBuilderSplitNode(dockspace_id, ImGuiDir_Right, 0.f, nullptr, &dockspace_id);
    ImGui::DockBuilderSetNodeSize(dock_id_right, ImVec2(right_dock_width, window_height));
    ImGuiID dock_id_right_top = 0;
    ImGuiID dock_id_right_bottom = 0;
    ImGui::DockBuilderSplitNode(dock_id_right, ImGuiDir_Down, 0.50f, &dock_id_right_bottom, &dock_id_right_top);
    ImGui::DockBuilderDockWindow("Settings", dock_id_right_top);
    ImGui::DockBuilderDockWindow("Scene", dock_id_right_top);
    ImGui::DockBuilderDockWindow("Properties", dock_id_right_bottom);

    ImGui::DockBuilderFinish(dockspace_id);
}

// needs to be called after camera.config is set
static inline void applyReverseZFarPlane(pnanovdb_camera_config_t* config)
{
    if (config->is_reverse_z && !config->is_orthographic)
    {
        config->far_plane = INFINITY;
    }
    else
    {
        config->far_plane = 10000.f;
    }
}

void showRenderSettingsWindow(pnanovdb_imgui_instance_t* instance, pnanovdb_imgui_settings_render_t* settings)
{
    auto ptr = cast(instance);
    if (!ptr->show_render_settings)
    {
        return;
    }

    if (ImGui::Begin("Settings", &ptr->show_render_settings))
    {
        ImGui::SeparatorText("Streaming");
        ImGui::InputText("Server Address", settings->server_address, 256u);
        ImGui::InputInt("Server Port", &settings->server_port);
        if (settings->enable_encoder)
        {
            ImGui::AlignTextToFramePadding();
            ImGui::Text("Streaming is running...");

            {
                const bool wasRecording = (settings->encode_to_file != 0);
                bool temp_bool = wasRecording;
                if (ImGui::Checkbox("Stream To File", &temp_bool))
                {
                    settings->encode_to_file = temp_bool ? PNANOVDB_TRUE : PNANOVDB_FALSE;
                    // logRecordingSavedOnStop(wasRecording, settings);
                }
            }
            ImGui::SameLine();
            ImGui::SetNextItemWidth(220.0f);
            ImGui::InputText("##StreamFileName", settings->encode_filename, IM_ARRAYSIZE(settings->encode_filename));

            {
                std::string baseName = settings->encode_filename[0] ? settings->encode_filename : "capture_stream";
                std::string inputFile = baseName + ".h264";
                std::string outputFile = baseName + ".mp4";
                std::string ffmpeg_cmd =
                    std::string("ffmpeg -i \"") + inputFile + "\" -c:v copy -f mp4 \"" + outputFile + "\"";
                ImGui::Indent(16.0f);
                ImGui::BeginGroup();

                const float h = ImGui::GetFrameHeight() * 1.1f;
                const ImVec2 size(h, h);
                if (ImGui::InvisibleButton("##copy_ffmpeg_cmd", size))
                {
                    ImGui::SetClipboardText(ffmpeg_cmd.c_str());
                }

                const bool hovered = ImGui::IsItemHovered();
                const bool held = ImGui::IsItemActive();
                if (hovered)
                {
                    ImGui::SetTooltip("Copy to clipboard");
                }

                ImDrawList* dl = ImGui::GetWindowDrawList();
                ImVec2 pMin = ImGui::GetItemRectMin();
                ImVec2 pMax = ImGui::GetItemRectMax();

                ImVec4 colV = hovered || held ? ImGui::GetStyleColorVec4(ImGuiCol_Text) :
                                                ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled);
                if (held)
                {
                    colV.x *= 0.9f;
                    colV.y *= 0.9f;
                    colV.z *= 0.9f;
                }
                const ImU32 col = ImGui::GetColorU32(colV);

                const float pad = h * 0.22f;
                const ImVec2 rectMin(pMin.x + pad, pMin.y + pad);
                const ImVec2 rectMax(pMax.x - pad, pMax.y - pad);
                const ImVec2 backOffset(-pad * 0.6f, -pad * 0.6f);

                // Draw copy icon (two overlapping rounded rectangles)
                dl->AddRect(rectMin + backOffset, rectMax + backOffset, col, 2.0f, 0, 1.5f);
                dl->AddRect(rectMin, rectMax, col, 2.0f, 0, 2.0f);

                ImGui::SameLine();
                ImGui::AlignTextToFramePadding();
                // ImGui::SetCursorPosY(ImGui::GetCursorPosY() + (h - ImGui::GetTextLineHeight()) * 0.20f);
                ImGui::TextDisabled("%s", ffmpeg_cmd.c_str());

                ImGui::EndGroup();
                ImGui::Unindent(16.0f);
            }
        }
        else
        {
            if (ImGui::Button("Start Streaming"))
            {
                settings->enable_encoder = PNANOVDB_TRUE;
                if (!settings->window_resize && !settings->encode_resize)
                {
                    settings->window_resize = PNANOVDB_TRUE;
                }
            }
        }

        ImGui::SeparatorText("Resolution");
        {
            // Resolution mode selection
            if (settings->enable_encoder)
            {
                // When streaming, show radio buttons for Fixed vs Fit Resolution
                bool isFixedResolution = settings->window_resize && !settings->encode_resize;
                bool isFitResolution = settings->encode_resize && !settings->window_resize;

                if (ImGui::RadioButton("Fixed", isFixedResolution))
                {
                    settings->window_resize = PNANOVDB_TRUE;
                    settings->encode_resize = PNANOVDB_FALSE;
                }
                ImGui::SameLine();
                if (ImGui::RadioButton("Fit Client", isFitResolution))
                {
                    settings->window_resize = PNANOVDB_FALSE;
                    settings->encode_resize = PNANOVDB_TRUE;
                }

                if (settings->window_resize)
                {
                    showResolutionCombo("Resolution", &settings->window_width, &settings->window_height);
                    ImGui::InputInt("Width", &settings->window_width);
                    ImGui::InputInt("Height", &settings->window_height);
                }
                else if (settings->encode_resize)
                {
                    ImGui::BeginDisabled();
                    ImGui::Text("Width: %d", settings->window_width);
                    ImGui::Text("Height: %d", settings->window_height);
                    ImGui::EndDisabled();
                }
            }
            else
            {
                // When not streaming, just show Fixed Resolution checkbox
                IMGUI_CHECKBOX_SYNC("Fixed Resolution", settings->window_resize);
                if (settings->window_resize)
                {
                    showResolutionCombo("Resolution", &settings->window_width, &settings->window_height);
                    ImGui::InputInt("Width", &settings->window_width);
                    ImGui::InputInt("Height", &settings->window_height);
                }
                else
                {
                    ImGui::BeginDisabled();
                    ImGui::Text("Width: %d", settings->window_width);
                    ImGui::Text("Height: %d", settings->window_height);
                    ImGui::EndDisabled();
                }
            }
        }

        ImGui::SeparatorText("Advanced");
        IMGUI_CHECKBOX_SYNC("VSync", settings->vsync);
        #if 0
        IMGUI_CHECKBOX_SYNC("Projection RH", settings->is_projection_rh);
        IMGUI_CHECKBOX_SYNC("Reverse Z Buffer", settings->is_reverse_z);
        #endif

#if 0
        {
            static bool s_device_list_built = false;
            static std::vector<std::string> s_device_labels;
            static std::vector<const char*> s_device_label_ptrs;

            if (!s_device_list_built && ptr->compute)
            {
                pnanovdb_compute_device_manager_t* device_manager =
                    ptr->compute->device_interface.create_device_manager(PNANOVDB_FALSE);
                if (device_manager)
                {
                    for (pnanovdb_uint32_t idx = 0u;; idx++)
                    {
                        pnanovdb_compute_physical_device_desc_t desc = {};
                        if (!ptr->compute->device_interface.enumerate_devices(device_manager, idx, &desc))
                        {
                            break;
                        }

                        const char* name = desc.device_name[0] ? desc.device_name : "Unknown";
                        std::string label = std::to_string(idx) + " - " + name;
                        s_device_labels.push_back(label);
                    }

                    ptr->compute->device_interface.destroy_device_manager(device_manager);
                }

                s_device_label_ptrs.clear();
                s_device_label_ptrs.reserve(s_device_labels.size());
                for (const auto& s : s_device_labels)
                {
                    s_device_label_ptrs.push_back(s.c_str());
                }
                s_device_list_built = true;
            }

            // Use preselected device index captured during initialization
            int device_index = ptr->device_index;
            if (!s_device_label_ptrs.empty())
            {
                ImGui::SeparatorText("Devices");
                ImGui::BeginDisabled(true);
                for (int i = 0; i < (int)s_device_label_ptrs.size(); ++i)
                {
                    bool selected = (i == device_index);
                    ImGui::Selectable(s_device_label_ptrs[i], selected);
                }
                ImGui::EndDisabled();
            }
            else
            {
                ImGui::TextDisabled("No devices found");
            }
        }
#endif
#if 0
        // UI Profile - only visible with default profile
        if (!ptr->is_viewer())
        {
            ImGui::SeparatorText("UI Profile");
            const char* profile_options[] = { "default", "viewer" };
            const char* current_profile =
                ptr->render_settings->ui_profile_name[0] != '\0' ? ptr->render_settings->ui_profile_name : "default";
            if (ImGui::BeginCombo("##ui_profile", current_profile))
            {
                for (int i = 0; i < IM_ARRAYSIZE(profile_options); i++)
                {
                    bool is_selected = (strcmp(current_profile, profile_options[i]) == 0);
                    if (ImGui::Selectable(profile_options[i], is_selected))
                    {
                        strcpy(ptr->render_settings->ui_profile_name, profile_options[i]);
                    }
                    if (is_selected)
                    {
                        ImGui::SetItemDefaultFocus();
                    }
                }
                ImGui::EndCombo();
            }
        }
#endif
    }
    ImGui::End();
}

void tree_node_begin(tree_state_t* tree)
{
    tree->path = 0llu;
    tree->level = 0u;
}

bool tree_node_ex(tree_state_t* tree, const char* label, ImGuiTreeNodeFlags flags = 0)
{
    if (tree->path == tree->selected_path &&
        tree->level == tree->selected_level)
    {
        flags |= ImGuiTreeNodeFlags_Selected;
    }
    if (ImGui::TreeNodeEx(label, flags))
    {
        if (ImGui::IsItemClicked())
        {
            tree->selected_path = tree->path;
            tree->selected_level = tree->level;
        }
        tree->level++;
        return true;
    }
    if (ImGui::IsItemClicked())
    {
        tree->selected_path = tree->path;
        tree->selected_level = tree->level;
    }
    tree->path += (1llu << (8u * tree->level));
    return false;
}

void tree_node_skip(tree_state_t* tree)
{
    tree->path += (1llu << (8u * tree->level));
}

void tree_node_pop(tree_state_t* tree)
{
    tree->path &= ~(255llu << (8u * tree->level));
    tree->level--;
    tree->path += (1llu << (8u * tree->level));
    ImGui::TreePop();
}

void showSceneWindow(pnanovdb_imgui_instance_t* instance, InstanceUserdata* instance_user_data)
{
    auto ptr = cast(instance);
    if (!ptr->show_scene)
    {
        return;
    }

    const pnanovdb_compute_t* compute = instance_user_data->compute;
    pnanovdb_pipeline_t* pipeline = instance_user_data->pipeline;
    pnanovdb_pipeline_registry_t* registry = instance_user_data->pipeline_registry;
    pnanovdb_pipeline_config_t* config = instance_user_data->pipeline_config;
    pnanovdb_compute_token_t* scene = pipeline->config_get_active_scene(config);
    if (!scene)
    {
        if (ImGui::Begin("Scene", &ptr->show_scene))
        {
            if (ImGui::Button("Add Scene"))
            {
                pnanovdb_compute_token_t* new_scene_name = compute->get_token("main");
                pipeline->config_add(config, new_scene_name, nullptr);
                pipeline->config_set_active_scene(config, new_scene_name);
            }
        }
        ImGui::End();
        return;
    }
    pnanovdb_pipeline_object_list_t* mapped_object_list = nullptr;
    if(!pipeline->config_map_params(config, scene, nullptr, PNANOVDB_REFLECT_DATA_TYPE(pnanovdb_pipeline_object_list_t), (void**)&mapped_object_list))
    {
        return;
    }

    tree_node_begin(&ptr->tree_state);

    // structural changes must be deferred to outside of map()/unmap()
    pnanovdb_compute_token_t* new_scene_name = nullptr;
    pnanovdb_compute_token_t* new_active_scene_name = nullptr;

    const ImGuiTreeNodeFlags parent_flags = ImGuiTreeNodeFlags_SpanAvailWidth;
    const ImGuiTreeNodeFlags leaf_flags = ImGuiTreeNodeFlags_Leaf | ImGuiTreeNodeFlags_SpanAvailWidth;
    if (ImGui::Begin("Scene", &ptr->show_scene))
    {
        pnanovdb_pipeline_scene_list_t* mapped_scene_list = nullptr;
        if(pipeline->config_map_params(config, scene, nullptr, PNANOVDB_REFLECT_DATA_TYPE(pnanovdb_pipeline_scene_list_t), (void**)&mapped_scene_list))
        {
            if (ImGui::BeginCombo("Scene", scene->str))
            {
                for (pnanovdb_uint64_t idx = 0u; idx < mapped_scene_list->scene_count; idx++)
                {
                    if (ImGui::Selectable(mapped_scene_list->scenes[idx]->str))
                    {
                        if (mapped_scene_list->scenes[idx]->id != scene->id)
                        {
                            new_active_scene_name = mapped_scene_list->scenes[idx];
                        }
                    }
                }
                if (ImGui::Selectable("+ New Scene"))
                {
                    pnanovdb_compute_token_t* scene_name = nullptr;
                    for (pnanovdb_uint32_t attempt = 0u; attempt < 1000u; attempt++)
                    {
                        char buf[40u] = {};
                        snprintf(buf, sizeof(buf), "scene_%d", attempt);
                        scene_name = compute->get_token(buf);
                        // check for conflict
                        for (pnanovdb_uint64_t scene_idx = 0u; scene_idx < mapped_scene_list->scene_count; scene_idx++)
                        {
                            pnanovdb_compute_token_t* scene_cmp = mapped_scene_list->scenes[scene_idx];
                            if (scene_name->id == scene_cmp->id)
                            {
                                scene_name = nullptr;
                                break;
                            }
                        }
                        if (scene_name)
                        {
                            break;
                        }
                    }
                    new_scene_name = scene_name;
                }
                ImGui::EndCombo();
            }
            pipeline->config_unmap_params(config, scene, nullptr, PNANOVDB_REFLECT_DATA_TYPE(pnanovdb_pipeline_scene_list_t));
        }

        if (tree_node_ex(&ptr->tree_state, scene->str, ImGuiTreeNodeFlags_DefaultOpen | parent_flags))
        {
            for (pnanovdb_uint64_t object_idx = 0u; object_idx < mapped_object_list->object_count; object_idx++)
            {
                pnanovdb_compute_token_t* object_name = mapped_object_list->objects[object_idx];
                pnanovdb_pipeline_object_mapped_t* mapped_object = nullptr;
                if(!pipeline->config_map_params(config, scene, object_name, PNANOVDB_REFLECT_DATA_TYPE(pnanovdb_pipeline_object_mapped_t), (void**)&mapped_object))
                {
                    continue;
                }
                if (tree_node_ex(&ptr->tree_state, object_name->str, ImGuiTreeNodeFlags_DefaultOpen | parent_flags))
                {
                    if (mapped_object->buffer_count == 0u)
                    {
                        tree_node_skip(&ptr->tree_state);
                    }
                    else if (tree_node_ex(&ptr->tree_state, "buffers", parent_flags))
                    {
                        for (pnanovdb_uint64_t buffer_idx = 0u; buffer_idx < mapped_object->buffer_count; buffer_idx++)
                        {
                            const char* buffer_name = mapped_object->buffer_names[buffer_idx]->str;
                            if (tree_node_ex(&ptr->tree_state, buffer_name, leaf_flags))
                            {
                                tree_node_pop(&ptr->tree_state);
                            }
                        }
                        tree_node_pop(&ptr->tree_state);
                    }
                    pnanovdb_uint64_t total_pipeline_count = 0llu;
                    for (pnanovdb_uint64_t stage_idx = 0u; stage_idx < mapped_object->stage_count; stage_idx++)
                    {
                        pnanovdb_pipeline_object_mapped_stage_t* stage = &mapped_object->stages[stage_idx];
                        total_pipeline_count += stage->pipeline_count;
                    }
                    if (total_pipeline_count == 0u)
                    {
                        tree_node_skip(&ptr->tree_state);
                    }
                    else if (tree_node_ex(&ptr->tree_state, "pipelines", parent_flags))
                    {
                        for (pnanovdb_uint64_t stage_idx = 0u; stage_idx < mapped_object->stage_count; stage_idx++)
                        {
                            pnanovdb_pipeline_object_mapped_stage_t* stage = &mapped_object->stages[stage_idx];
                            for (pnanovdb_uint64_t pipeline_idx = 0u; pipeline_idx < stage->pipeline_count; pipeline_idx++)
                            {
                                pnanovdb_uint32_t pipeline_type = stage->pipelines[pipeline_idx].pipeline_type;
                                pnanovdb_pipeline_metadesc_t pipe_desc = {};
                                const char* pipeline_name = "invalid";
                                if (pipeline->get_pipeline_metadesc(registry, pipeline_type, &pipe_desc))
                                {
                                    pipeline_name = pipe_desc.name->str;
                                }
                                if (tree_node_ex(&ptr->tree_state, pipeline_name, leaf_flags))
                                {
                                    tree_node_pop(&ptr->tree_state);
                                }
                            }
                        }
                        tree_node_pop(&ptr->tree_state);
                    }
                    if (!mapped_object->camera)
                    {
                        tree_node_skip(&ptr->tree_state);
                    }
                    else
                    {
                        if (tree_node_ex(&ptr->tree_state, "camera", leaf_flags))
                        {
                            tree_node_pop(&ptr->tree_state);
                        }
                    }
                    if (!mapped_object->camera_view)
                    {
                        tree_node_skip(&ptr->tree_state);
                    }
                    else
                    {
                        if (tree_node_ex(&ptr->tree_state, "camera_view", leaf_flags))
                        {
                            tree_node_pop(&ptr->tree_state);
                        }
                    }
                    tree_node_pop(&ptr->tree_state);
                }
                if (mapped_object)
                {
                    pipeline->config_unmap_params(config, scene, object_name, PNANOVDB_REFLECT_DATA_TYPE(pnanovdb_pipeline_object_mapped_t));
                }
            }
            tree_node_pop(&ptr->tree_state);
        }
    }
    ImGui::End();

    pipeline->config_unmap_params(config, scene, nullptr, PNANOVDB_REFLECT_DATA_TYPE(pnanovdb_pipeline_object_list_t));

    // structural changes must be deferred to outside of map()/unmap()
    if (new_active_scene_name)
    {
        pipeline->config_set_active_scene(config, new_active_scene_name);
    }
    if (new_scene_name)
    {
        pipeline->config_add(config, new_scene_name, nullptr);
        pipeline->config_set_active_scene(config, new_scene_name);
    }
}

void properties_scene(Instance*, const pnanovdb_compute_t*, pnanovdb_pipeline_t*, pnanovdb_pipeline_registry_t*,
    pnanovdb_pipeline_config_t*, pnanovdb_compute_token_t*);
void properties_object(Instance*, const pnanovdb_compute_t*, pnanovdb_pipeline_t*, pnanovdb_pipeline_registry_t*,
    pnanovdb_pipeline_config_t*, pnanovdb_compute_token_t*, int32_t);
void properties_pipelines(Instance*, const pnanovdb_compute_t*, pnanovdb_pipeline_t*, pnanovdb_pipeline_registry_t*,
    pnanovdb_pipeline_config_t*, pnanovdb_compute_token_t*, int32_t);
void properties_buffer(Instance*, const pnanovdb_compute_t*, pnanovdb_pipeline_t*, pnanovdb_pipeline_registry_t*,
    pnanovdb_pipeline_config_t*, pnanovdb_compute_token_t*, int32_t, int32_t);
void properties_pipeline(Instance*, const pnanovdb_compute_t*, pnanovdb_pipeline_t*, pnanovdb_pipeline_registry_t*,
    pnanovdb_pipeline_config_t*, pnanovdb_compute_token_t*, int32_t, int32_t);
void properties_camera(Instance*, const pnanovdb_compute_t*, pnanovdb_pipeline_t*, pnanovdb_pipeline_registry_t*,
    pnanovdb_pipeline_config_t*, pnanovdb_compute_token_t*, int32_t);

void showPropertiesWindow(pnanovdb_imgui_instance_t* instance, InstanceUserdata* instance_user_data)
{
    auto ptr = cast(instance);
    if (!ptr->show_properties)
    {
        return;
    }

    int32_t selected_level = ptr->tree_state.selected_level;
    int32_t selected_object_idx = (ptr->tree_state.selected_path >> 8) & 255;
    int32_t selected_component = (ptr->tree_state.selected_path >> 16) & 255;

    const pnanovdb_compute_t* compute = instance_user_data->compute;
    pnanovdb_pipeline_t* pipeline = instance_user_data->pipeline;
    pnanovdb_pipeline_registry_t* registry = instance_user_data->pipeline_registry;
    pnanovdb_pipeline_config_t* config = instance_user_data->pipeline_config;
    pnanovdb_compute_token_t* scene = pipeline->config_get_active_scene(config);
    if (!scene)
    {
        if (ImGui::Begin("Properties", &ptr->show_properties))
        {
        }
        ImGui::End();
        return;
    }

    const ImGuiTreeNodeFlags parent_flags = ImGuiTreeNodeFlags_DefaultOpen | ImGuiTreeNodeFlags_SpanAvailWidth;
    if (ImGui::Begin("Properties", &ptr->show_properties))
    {
        if (selected_level == 0u)
        {
            properties_scene(ptr, compute, pipeline, registry, config, scene);
        }
        if (selected_level == 1u)
        {
            properties_object(ptr, compute, pipeline, registry, config, scene, selected_object_idx);
        }
        if (((selected_level == 2u && selected_component == 1u) || selected_level == 1u))
        {
            properties_pipelines(ptr, compute, pipeline, registry, config, scene, selected_object_idx);
        }
        if (selected_level == 3u && selected_component == 0u)
        {
            int32_t selected_buffer_idx = (ptr->tree_state.selected_path >> 24) & 255;
            properties_buffer(ptr, compute, pipeline, registry, config, scene, selected_object_idx, selected_buffer_idx);
        }
        if (selected_level == 3u && selected_component == 1u)
        {
            int32_t selected_stage_pipeline_idx = (ptr->tree_state.selected_path >> 24) & 255;
            properties_pipeline(ptr, compute, pipeline, registry, config, scene, selected_object_idx, selected_stage_pipeline_idx);
        }
        if (selected_level == 2u && selected_component == 2u)
        {
            properties_camera(ptr, compute, pipeline, registry, config, scene, selected_object_idx);
        }
    }
    ImGui::End();
}

template <typename ValueT>
static ValueT get_metadata(void* mapped_metadata, const pnanovdb_reflect_data_t* reflect_metadata, const char* label)
{
    if (!mapped_metadata || !reflect_metadata)
    {
        return ValueT();
    }
    ValueT ret = ValueT();
    uint8_t* metadata = (uint8_t*)mapped_metadata + reflect_metadata->data_offset;
    for (pnanovdb_uint64_t idx = 0u; idx < reflect_metadata->data_type->child_reflect_data_count; idx++)
    {
        auto reflect_data = &reflect_metadata->data_type->child_reflect_datas[idx];
        if (pnanovdb_reflect_string_compare(label, reflect_data->name) == 0)
        {
            ret = *((ValueT*)(metadata + reflect_data->data_offset));
            break;
        }
    }
    return ret;
}

void showReflectParameter(
    const pnanovdb_compute_t* compute,
    void* mapped_data, const pnanovdb_reflect_data_t* reflect_data,
    void* mapped_metadata, const pnanovdb_reflect_data_t* reflect_metadata)
{
    uint8_t* data = (uint8_t*)mapped_data + reflect_data->data_offset;

    double stepd = get_metadata<double>(mapped_metadata, reflect_metadata, "step");
    pnanovdb_bool_t is_bool = get_metadata<pnanovdb_bool_t>(mapped_metadata, reflect_metadata, "is_bool");
    pnanovdb_bool_t hidden = get_metadata<pnanovdb_bool_t>(mapped_metadata, reflect_metadata, "hidden");
    pnanovdb_bool_t use_slider = get_metadata<pnanovdb_bool_t>(mapped_metadata, reflect_metadata, "use_slider");
    if (hidden)
    {
        return;
    }
    if (reflect_data->data_type->data_type == PNANOVDB_REFLECT_TYPE_BOOL32)
    {
        is_bool = PNANOVDB_TRUE;
    }

    void* minv = nullptr;
    void* maxv = nullptr;
    int64_t mini = 0, maxi = 0;
    float minf = 0.f, maxf = 0.f;
    double mind = 0.0, maxd = 0.0;
    if (reflect_data->data_type->data_type == PNANOVDB_REFLECT_TYPE_FLOAT)
    {
        minf = float(get_metadata<double>(mapped_metadata, reflect_metadata, "min_double"));
        maxf = float(get_metadata<double>(mapped_metadata, reflect_metadata, "max_double"));
        minv = &minf;
        maxv = &maxf;
    }
    else if (reflect_data->data_type->data_type == PNANOVDB_REFLECT_TYPE_DOUBLE)
    {
        mind = get_metadata<double>(mapped_metadata, reflect_metadata, "min_double");
        maxd = get_metadata<double>(mapped_metadata, reflect_metadata, "max_double");
        minv = &mind;
        maxv = &maxd;
    }
    else
    {
        mini = get_metadata<int64_t>(mapped_metadata, reflect_metadata, "min_int64");
        maxi = get_metadata<int64_t>(mapped_metadata, reflect_metadata, "max_int64");
        minv = &mind;
        maxv = &maxd;
    }

    ImGuiDataType type = ImGuiDataType_COUNT;
    switch(reflect_data->data_type->data_type)
    {
        case PNANOVDB_REFLECT_TYPE_INT32: type = ImGuiDataType_S32; break;
        case PNANOVDB_REFLECT_TYPE_UINT32: type = ImGuiDataType_U32; break;
        case PNANOVDB_REFLECT_TYPE_FLOAT: type = ImGuiDataType_Float; break;
        case PNANOVDB_REFLECT_TYPE_BOOL32: type = ImGuiDataType_U32; break;
        case PNANOVDB_REFLECT_TYPE_UINT8: type = ImGuiDataType_U8; break;
        case PNANOVDB_REFLECT_TYPE_UINT16: type = ImGuiDataType_U16; break;
        case PNANOVDB_REFLECT_TYPE_UINT64: type = ImGuiDataType_U64; break;
        case PNANOVDB_REFLECT_TYPE_DOUBLE: type = ImGuiDataType_Double; break;
        case PNANOVDB_REFLECT_TYPE_INT64: type = ImGuiDataType_S64; break;
    }

    if (reflect_data->reflect_mode == PNANOVDB_REFLECT_MODE_POINTER &&
        reflect_data->data_type->data_type == PNANOVDB_REFLECT_TYPE_CHAR)
    {
        char buf[4096u] = {};
        if (data)
        {
            strncpy(buf, *(const char**)data, sizeof(buf) - 1u);
        }
        pnanovdb_compute_token_t* str_token = nullptr;
        if (ImGui::InputText(reflect_data->name, buf, sizeof(buf), ImGuiInputTextFlags_EnterReturnsTrue))
        {
            pnanovdb_compute_token_t* str_token = compute->get_token(buf);
            *(const char**)data = str_token->str;
        }
    }
    else if (type == ImGuiDataType_COUNT)
    {
        ImGui::Text("%s", reflect_data->name);
    }
    else if (is_bool)
    {
        ImGui::Checkbox(reflect_data->name, (bool*)data);
    }
    else if (use_slider)
    {
        const char* format = type == ImGuiDataType_Float || type == ImGuiDataType_Double ? "%.3f" : nullptr;
        ImGui::SliderScalar(reflect_data->name, type, data, minv, maxv, format, ImGuiSliderFlags_AlwaysClamp);
    }
    else
    {
        ImGui::DragScalar(reflect_data->name, type, data, float(stepd), minv, maxv);
    }
}

static const pnanovdb_reflect_data_t* find_metadata(const pnanovdb_reflect_data_type_t* metadata_type, const char* name)
{
    if (!metadata_type)
    {
        return nullptr;
    }
    for (pnanovdb_uint64_t idx = 0u; idx < metadata_type->child_reflect_data_count; idx++)
    {
        auto reflect_metadata = &metadata_type->child_reflect_datas[idx];
        if (pnanovdb_reflect_string_compare(name, reflect_metadata->name) == 0)
        {
            return reflect_metadata;
        }
    }
    return nullptr;
}

void showShaderParams(const pnanovdb_compute_t* compute, pnanovdb_pipeline_shader_t* shader, void* mapped_metadata, const pnanovdb_reflect_data_t* reflect_metadata)
{
    if (!shader->params_data || !shader->params_data_type)
    {
        return;
    }
    const char* current_name = shader->shader_path ? shader->shader_path : "Select...";
    const char** names = get_metadata<const char**>(mapped_metadata, reflect_metadata, "names");
    pnanovdb_uint64_t name_count = get_metadata<pnanovdb_uint64_t>(mapped_metadata, reflect_metadata, "name_count");
    if (names && name_count != 0u)
    {
        if (ImGui::BeginCombo("shader", current_name))
        {
            for (pnanovdb_uint64_t i = 0; i < name_count; i++)
            {
                if (ImGui::Selectable(names[i]))
                {
                    // make compute system own string
                    shader->shader_path = compute->get_token(names[i])->str;
                }
            }
            ImGui::EndCombo();
        }
    }
    for (pnanovdb_uint64_t idx = 0u; idx < shader->params_data_type->child_reflect_data_count; idx++)
    {
        auto reflect_data = &shader->params_data_type->child_reflect_datas[idx];
        auto reflect_metadata = find_metadata(shader->params_metadata_type, reflect_data->name);
        showReflectParameter(compute, shader->params_data, reflect_data, shader->params_metadata, reflect_metadata);
        if (reflect_data->data_type->data_type == PNANOVDB_REFLECT_TYPE_STRUCT)
        {
            ImGui::Indent();
            const pnanovdb_reflect_data_type_t* data_type2 = reflect_data->data_type;
            void* params_data2 = (uint8_t*)shader->params_data + reflect_data->data_offset;
            ImGui::PushID(idx);
            for (pnanovdb_uint64_t idx2 = 0u; idx2 < data_type2->child_reflect_data_count; idx2++)
            {
                auto reflect_data2 = &data_type2->child_reflect_datas[idx2];
                showReflectParameter(compute, params_data2, reflect_data2, shader->params_metadata, reflect_metadata);
            }
            ImGui::PopID();
            ImGui::Unindent();
        }
    }
}

static inline float getDialogDpiScale()
{
    ImGuiIO& io = ImGui::GetIO();
    const float sx = io.DisplayFramebufferScale.x > 0.f ? io.DisplayFramebufferScale.x : 1.f;
    const float sy = io.DisplayFramebufferScale.y > 0.f ? io.DisplayFramebufferScale.y : 1.f;
    return std::max(1.f, std::max(sx, sy));
}
static inline ImVec2 scaleDialogSize(const ImVec2& size)
{
    const float scale = getDialogDpiScale();
    return ImVec2(size.x * scale, size.y * scale);
}

void properties_scene(
    Instance* ptr,
    const pnanovdb_compute_t* compute,
    pnanovdb_pipeline_t* pipeline,
    pnanovdb_pipeline_registry_t* registry,
    pnanovdb_pipeline_config_t* config,
    pnanovdb_compute_token_t* scene)
{

    pnanovdb_pipeline_object_list_t* mapped_object_list = nullptr;
    if (!pipeline->config_map_params(config, scene, nullptr, PNANOVDB_REFLECT_DATA_TYPE(pnanovdb_pipeline_object_list_t), (void**)&mapped_object_list))
    {
        return;
    }

    pnanovdb_compute_token_t* scene_rename_name = nullptr;
    pnanovdb_compute_token_t* scene_remove_name = nullptr;
    pnanovdb_compute_token_t* new_object_name = nullptr;

    ImGui::SeparatorText("Scene");
    if (ImGui::Button("Remove"))
    {
        scene_remove_name = scene;
    }
    ImGui::InputText("##new_scene_name", ptr->scene_rename_buf, sizeof(ptr->scene_rename_buf));
    ImGui::SameLine();
    if (ImGui::Button("Rename"))
    {
        if (ptr->scene_rename_buf[0])
        {
            scene_rename_name = compute->get_token(ptr->scene_rename_buf);
        }
    }
    if (ImGui::Button("Add Object"))
    {
        pnanovdb_compute_token_t* object_name = nullptr;
        for (pnanovdb_uint32_t attempt = 0u; attempt < 1000u; attempt++)
        {
            char buf[40u] = {};
            snprintf(buf, sizeof(buf), "object_%d", attempt);
            object_name = compute->get_token(buf);
            // check for conflict
            for (pnanovdb_uint64_t object_idx = 0u; object_idx < mapped_object_list->object_count; object_idx++)
            {
                pnanovdb_compute_token_t* object_cmp = mapped_object_list->objects[object_idx];
                if (object_name->id == object_cmp->id)
                {
                    object_name = nullptr;
                    break;
                }
            }
            if (object_name)
            {
                break;
            }
        }
        new_object_name = object_name;
    }

    pipeline->config_unmap_params(config, scene, nullptr, PNANOVDB_REFLECT_DATA_TYPE(pnanovdb_pipeline_object_list_t));

    if (new_object_name)
    {
        pipeline->config_add(config, scene, new_object_name);
    }
    if (scene_rename_name)
    {
        pipeline->config_rename(config, nullptr, scene, scene_rename_name);
    }
    if (scene_remove_name)
    {
        pipeline->config_remove(config, scene_remove_name, nullptr);
    }
}

void properties_object(
    Instance* ptr,
    const pnanovdb_compute_t* compute,
    pnanovdb_pipeline_t* pipeline,
    pnanovdb_pipeline_registry_t* registry,
    pnanovdb_pipeline_config_t* config,
    pnanovdb_compute_token_t* scene,
    int32_t selected_object_idx)
{
    pnanovdb_pipeline_object_list_t* mapped_object_list = nullptr;
    if (!pipeline->config_map_params(config, scene, nullptr, PNANOVDB_REFLECT_DATA_TYPE(pnanovdb_pipeline_object_list_t), (void**)&mapped_object_list))
    {
        return;
    }
    pnanovdb_compute_token_t* object_name = nullptr;
    pnanovdb_pipeline_object_mapped_t* mapped_object = nullptr;
    if (selected_object_idx >= 0 && selected_object_idx < mapped_object_list->object_count)
    {
        object_name = mapped_object_list->objects[selected_object_idx];
        pipeline->config_map_params(config, scene, object_name, PNANOVDB_REFLECT_DATA_TYPE(pnanovdb_pipeline_object_mapped_t), (void**)&mapped_object);
    }

    ImGui::SeparatorText("Object");

    pnanovdb_compute_token_t* rename_old_object_name = nullptr;
    pnanovdb_compute_token_t* rename_new_object_name = nullptr;
    pnanovdb_compute_token_t* remove_object_name = nullptr;
    pnanovdb_compute_token_t* dup_src_object_name = nullptr;
    pnanovdb_compute_token_t* dup_dst_object_name = nullptr;

    if (mapped_object)
    {
        if (ImGui::Button("Remove"))
        {
            remove_object_name = object_name;
        }
        ImGui::SameLine();
        if (ImGui::Button("Duplicate"))
        {
            pnanovdb_compute_token_t* dst_object_name = nullptr;
            for (pnanovdb_uint32_t attempt = 0u; attempt < 1000u; attempt++)
            {
                char buf[40u] = {};
                snprintf(buf, sizeof(buf), "object_%d", attempt);
                dst_object_name = compute->get_token(buf);
                // check for conflict
                for (pnanovdb_uint64_t object_idx = 0u; object_idx < mapped_object_list->object_count; object_idx++)
                {
                    pnanovdb_compute_token_t* object_cmp = mapped_object_list->objects[object_idx];
                    if (dst_object_name->id == object_cmp->id)
                    {
                        dst_object_name = nullptr;
                        break;
                    }
                }
                if (dst_object_name)
                {
                    break;
                }
            }
            dup_src_object_name = object_name;
            dup_dst_object_name = dst_object_name;
        }
        ImGui::InputText("##new_object_name", ptr->object_rename_buf, sizeof(ptr->object_rename_buf));
        ImGui::SameLine();
        if (ImGui::Button("Rename"))
        {
            if (ptr->object_rename_buf[0])
            {
                pnanovdb_compute_token_t* rename_token = compute->get_token(ptr->object_rename_buf);
                rename_old_object_name = object_name;
                rename_new_object_name = rename_token;
            }
        }
        if (mapped_object->is_visible)
        {
            ImGui::Checkbox("is_visible", (bool*)mapped_object->is_visible);
        }
    }

    if (mapped_object)
    {
        pipeline->config_unmap_params(config, scene, object_name, PNANOVDB_REFLECT_DATA_TYPE(pnanovdb_pipeline_object_mapped_t));
    }
    pipeline->config_unmap_params(config, scene, nullptr, PNANOVDB_REFLECT_DATA_TYPE(pnanovdb_pipeline_object_list_t));

    if (rename_new_object_name)
    {
        pipeline->config_rename(config, scene, rename_old_object_name, rename_new_object_name);
    }
    if (remove_object_name)
    {
        pipeline->config_remove(config, scene, remove_object_name);
    }
    if (dup_dst_object_name)
    {
        pipeline->config_duplicate(config, scene, dup_src_object_name, scene, dup_dst_object_name);
    }
}

void properties_pipelines(
    Instance* ptr,
    const pnanovdb_compute_t* compute,
    pnanovdb_pipeline_t* pipeline,
    pnanovdb_pipeline_registry_t* registry,
    pnanovdb_pipeline_config_t* config,
    pnanovdb_compute_token_t* scene,
    int32_t selected_object_idx)
{
    pnanovdb_pipeline_object_list_t* mapped_object_list = nullptr;
    if (!pipeline->config_map_params(config, scene, nullptr, PNANOVDB_REFLECT_DATA_TYPE(pnanovdb_pipeline_object_list_t), (void**)&mapped_object_list))
    {
        return;
    }
    pnanovdb_compute_token_t* object_name = nullptr;
    pnanovdb_pipeline_object_mapped_t* mapped_object = nullptr;
    if (selected_object_idx >= 0 && selected_object_idx < mapped_object_list->object_count)
    {
        object_name = mapped_object_list->objects[selected_object_idx];
        pipeline->config_map_params(config, scene, object_name, PNANOVDB_REFLECT_DATA_TYPE(pnanovdb_pipeline_object_mapped_t), (void**)&mapped_object);
    }

    ImGui::SeparatorText("Set Pipeline");

    pnanovdb_compute_token_t* set_pipe_object_name = nullptr;
    pnanovdb_uint32_t set_pipe_stage_idx = 0u;
    pnanovdb_uint32_t set_pipe_pipe_idx = 0u;
    pnanovdb_pipeline_type_t set_pipe_type = 0u;

    if (mapped_object)
    {
        const char* stage_name = "Select...";
        if (ptr->property_selected_stage_idx == pnanovdb_pipeline_stage_load) {stage_name = "load";}
        if (ptr->property_selected_stage_idx == pnanovdb_pipeline_stage_process) {stage_name = "process";}
        if (ptr->property_selected_stage_idx == pnanovdb_pipeline_stage_render) {stage_name = "render";}
        if (ImGui::BeginCombo("Stage", stage_name))
        {
            for (pnanovdb_uint64_t stage_idx = 0u; stage_idx < mapped_object->stage_count; stage_idx++)
            {
                pnanovdb_pipeline_object_mapped_stage_t* stage = &mapped_object->stages[stage_idx];
                // TODO: query this somehow
                stage_name = "unknown";
                if (stage_idx == pnanovdb_pipeline_stage_load) {stage_name = "load";}
                if (stage_idx == pnanovdb_pipeline_stage_process) {stage_name = "process";}
                if (stage_idx == pnanovdb_pipeline_stage_render) {stage_name = "render";}
                if (ImGui::Selectable(stage_name))
                {
                    ptr->property_selected_stage_idx = (int)stage_idx;
                }
            }
            ImGui::EndCombo();
        }
        const char* pipeline_name = "Select...";
        pnanovdb_pipeline_metadesc_t current_pipe = {};
        if (pipeline->get_pipeline_metadesc(registry, ptr->property_selected_pipeline_type, &current_pipe))
        {
            pipeline_name = current_pipe.name->str;
        }
        if (ImGui::BeginCombo("Pipeline", pipeline_name))
        {
            for (pnanovdb_uint32_t pipeline_type = 0; pipeline_type < ~0u; pipeline_type++)
            {
                pnanovdb_pipeline_metadesc_t pipe_desc = {};
                if (!pipeline->get_pipeline_metadesc(registry, pipeline_type, &pipe_desc))
                {
                    break;
                }
                if ((pipe_desc.stage_mask & (1llu << ptr->property_selected_stage_idx)) == 0llu)
                {
                    continue;
                }
                if (ImGui::Selectable(pipe_desc.name->str))
                {
                    ptr->property_selected_pipeline_type = pipeline_type;
                }
            }
            ImGui::EndCombo();
        }
        ImGui::DragInt("Pipeline Index", &ptr->property_selected_pipeline_idx, 1.f, 0, 10);
        if (ImGui::Button("Apply"))
        {
            set_pipe_object_name = object_name;
            set_pipe_stage_idx = ptr->property_selected_stage_idx;
            set_pipe_pipe_idx = ptr->property_selected_pipeline_idx;
            set_pipe_type = ptr->property_selected_pipeline_type;
        }
    }

    if (mapped_object)
    {
        pipeline->config_unmap_params(config, scene, object_name, PNANOVDB_REFLECT_DATA_TYPE(pnanovdb_pipeline_object_mapped_t));
    }
    pipeline->config_unmap_params(config, scene, nullptr, PNANOVDB_REFLECT_DATA_TYPE(pnanovdb_pipeline_object_list_t));

    if (set_pipe_object_name)
    {
        pipeline->config_set_pipeline(
        config,
        scene,
        set_pipe_object_name,
        (pnanovdb_pipeline_stage_t)set_pipe_stage_idx,
        set_pipe_pipe_idx,
        set_pipe_type);
    }
}

void properties_buffer(
    Instance* ptr,
    const pnanovdb_compute_t* compute,
    pnanovdb_pipeline_t* pipeline,
    pnanovdb_pipeline_registry_t* registry,
    pnanovdb_pipeline_config_t* config,
    pnanovdb_compute_token_t* scene,
    int32_t selected_object_idx,
    int32_t selected_buffer_idx)
{
    pnanovdb_pipeline_object_list_t* mapped_object_list = nullptr;
    if (!pipeline->config_map_params(config, scene, nullptr, PNANOVDB_REFLECT_DATA_TYPE(pnanovdb_pipeline_object_list_t), (void**)&mapped_object_list))
    {
        return;
    }
    pnanovdb_compute_token_t* object_name = nullptr;
    pnanovdb_pipeline_object_mapped_t* mapped_object = nullptr;
    if (selected_object_idx >= 0 && selected_object_idx < mapped_object_list->object_count)
    {
        object_name = mapped_object_list->objects[selected_object_idx];
        pipeline->config_map_params(config, scene, object_name, PNANOVDB_REFLECT_DATA_TYPE(pnanovdb_pipeline_object_mapped_t), (void**)&mapped_object);
    }

    if (mapped_object && selected_buffer_idx >= 0 && selected_buffer_idx < mapped_object->buffer_count)
    {
        pnanovdb_compute_array_t* res_array = mapped_object->buffer_arrays[selected_buffer_idx];
        pnanovdb_uint64_t res_version = mapped_object->buffer_array_versions[selected_buffer_idx];
        pnanovdb_compute_token_t* res_path = mapped_object->buffer_paths[selected_buffer_idx];

        pnanovdb_uint64_t tmp_element_count = res_array ? res_array->element_count : 0u;
        pnanovdb_uint64_t tmp_element_size = res_array ? res_array->element_size : 0u;
        pnanovdb_uint64_t tmp_array_version = res_version;
        ImGui::DragScalar("Array Element Count", ImGuiDataType_U64, &tmp_element_count, 1.f, nullptr, nullptr, nullptr, ImGuiSliderFlags_ReadOnly);
        ImGui::DragScalar("Array Element Size", ImGuiDataType_U64, &tmp_element_size, 1.f, nullptr, nullptr, nullptr, ImGuiSliderFlags_ReadOnly);
        ImGui::DragScalar("Array Version", ImGuiDataType_U64, &tmp_array_version, 1.f, nullptr, nullptr, nullptr, ImGuiSliderFlags_ReadOnly);
        char buf[4096u] = {};
        if (res_path)
        {
            strncpy(buf, res_path->str, sizeof(buf) - 1u);
        }
        pnanovdb_compute_token_t* path_token = nullptr;
        if (ImGui::InputText("Path", buf, sizeof(buf), ImGuiInputTextFlags_EnterReturnsTrue))
        {
            path_token = compute->get_token(buf);
        }

        const char* desc_str = nullptr;
        const char* filter_str = nullptr;
        if (pnanovdb_reflect_string_compare(mapped_object->buffer_names[selected_buffer_idx]->str, "nanovdb") == 0)
        {
            desc_str = "Open NanoVDB File";
            filter_str = "NanoVDB Files (*.nvdb){.nvdb}";
        }
        else if (pnanovdb_reflect_string_compare(mapped_object->buffer_names[selected_buffer_idx]->str, "ply") == 0)
        {
            desc_str = "Open PLY File";
            filter_str = "PLY Files (*.ply){.ply}";
        }
        if (desc_str)
        {
            if (ImGui::Button("Open File"))
            {
                IGFD::FileDialogConfig config;
                config.path = ".";

                ImGuiFileDialog::Instance()->OpenDialog(
                    "OpenFileDlgKey", desc_str, filter_str, config);
            }
        }
        if (ImGuiFileDialog::Instance()->IsOpened())
        {
            ImVec2 dialog_size = {768.f, 512.f};
            ImGui::SetNextWindowSize(scaleDialogSize(dialog_size), ImGuiCond_Appearing);
            if (ImGuiFileDialog::Instance()->Display("OpenFileDlgKey"))
            {
                if (ImGuiFileDialog::Instance()->IsOk())
                {
                    std::string file_path = ImGuiFileDialog::Instance()->GetFilePathName();
                    path_token = compute->get_token(file_path.c_str());
                }
                ImGuiFileDialog::Instance()->Close();
            }
        }
        if (path_token)
        {
            mapped_object->buffer_paths[selected_buffer_idx] = path_token;
        }
    }

    if (mapped_object)
    {
        pipeline->config_unmap_params(config, scene, object_name, PNANOVDB_REFLECT_DATA_TYPE(pnanovdb_pipeline_object_mapped_t));
    }
    pipeline->config_unmap_params(config, scene, nullptr, PNANOVDB_REFLECT_DATA_TYPE(pnanovdb_pipeline_object_list_t));
}

void properties_pipeline(
    Instance* ptr,
    const pnanovdb_compute_t* compute,
    pnanovdb_pipeline_t* pipeline,
    pnanovdb_pipeline_registry_t* registry,
    pnanovdb_pipeline_config_t* config,
    pnanovdb_compute_token_t* scene,
    int32_t selected_object_idx,
    int32_t selected_stage_pipeline_idx)
{
    pnanovdb_pipeline_object_list_t* mapped_object_list = nullptr;
    if (!pipeline->config_map_params(config, scene, nullptr, PNANOVDB_REFLECT_DATA_TYPE(pnanovdb_pipeline_object_list_t), (void**)&mapped_object_list))
    {
        return;
    }
    pnanovdb_compute_token_t* object_name = nullptr;
    pnanovdb_pipeline_object_mapped_t* mapped_object = nullptr;
    if (selected_object_idx >= 0 && selected_object_idx < mapped_object_list->object_count)
    {
        object_name = mapped_object_list->objects[selected_object_idx];
        pipeline->config_map_params(config, scene, object_name, PNANOVDB_REFLECT_DATA_TYPE(pnanovdb_pipeline_object_mapped_t), (void**)&mapped_object);
    }

    pnanovdb_compute_token_t* set_pipe_object_name = nullptr;
    pnanovdb_uint32_t set_pipe_stage_idx = 0u;
    pnanovdb_uint32_t set_pipe_pipe_idx = 0u;
    pnanovdb_pipeline_type_t set_pipe_type = 0u;

    if (mapped_object)
    {
        pnanovdb_uint32_t cmp_stage_pipeline_idx = 0;
        pnanovdb_uint32_t selected_stage_idx = 0u;
        pnanovdb_uint32_t selected_pipeline_idx = 0u;
        for (pnanovdb_uint32_t stage_idx = 0u; stage_idx < mapped_object->stage_count; stage_idx++)
        {
            pnanovdb_pipeline_object_mapped_stage_t* stage = &mapped_object->stages[stage_idx];
            for(pnanovdb_uint32_t pipeline_idx = 0u; pipeline_idx < stage->pipeline_count; pipeline_idx++)
            {
                if (cmp_stage_pipeline_idx == selected_stage_pipeline_idx)
                {
                    selected_stage_idx = stage_idx;
                    selected_pipeline_idx = pipeline_idx;
                }
                cmp_stage_pipeline_idx++;
            }
        }

        if (selected_stage_idx >= 0 && selected_stage_idx < mapped_object->stage_count)
        {
            pnanovdb_pipeline_object_mapped_stage_t* stage = &mapped_object->stages[selected_stage_idx];

            const char* stage_name = "Select...";
            if (selected_stage_idx == pnanovdb_pipeline_stage_load) {stage_name = "load";}
            if (selected_stage_idx == pnanovdb_pipeline_stage_process) {stage_name = "process";}
            if (selected_stage_idx == pnanovdb_pipeline_stage_render) {stage_name = "render";}
            ImGui::Text("Stage: %s", stage_name);

            pnanovdb_pipeline_type_t current_pipeline_type = ~0u;
            if (selected_pipeline_idx >= 0 && selected_pipeline_idx < stage->pipeline_count)
            {
                pnanovdb_pipeline_object_mapped_pipeline_t* pipe = &stage->pipelines[selected_pipeline_idx];
                current_pipeline_type = pipe->pipeline_type;
            }

            const char* pipeline_name = "Select...";
            pnanovdb_pipeline_metadesc_t current_pipe = {};
            if (pipeline->get_pipeline_metadesc(registry, current_pipeline_type, &current_pipe))
            {
                pipeline_name = current_pipe.name->str;
            }
            if (ImGui::BeginCombo("Pipeline", pipeline_name))
            {
                for (pnanovdb_uint32_t pipeline_type = 0; pipeline_type < ~0u; pipeline_type++)
                {
                    pnanovdb_pipeline_metadesc_t pipe_desc = {};
                    if (!pipeline->get_pipeline_metadesc(registry, pipeline_type, &pipe_desc))
                    {
                        break;
                    }
                    if ((pipe_desc.stage_mask & (1llu << selected_stage_idx)) == 0llu)
                    {
                        continue;
                    }
                    if (ImGui::Selectable(pipe_desc.name->str))
                    {
                        set_pipe_object_name = object_name;
                        set_pipe_stage_idx = selected_stage_idx;
                        set_pipe_pipe_idx = selected_pipeline_idx;
                        set_pipe_type = pipeline_type;
                    }
                }
                ImGui::EndCombo();
            }
            ImGui::Separator();

            //pnanovdb_pipeline_object_mapped_stage_t* stage = &mapped_object->stages[ptr->selected_stage_idx];
            if (selected_pipeline_idx >= 0 && selected_pipeline_idx < stage->pipeline_count)
            {
                pnanovdb_pipeline_object_mapped_pipeline_t* pipe = &stage->pipelines[selected_pipeline_idx];
                if (pipe->params_data && pipe->params_data_type)
                {
                    for (pnanovdb_uint64_t idx = 0u; idx < pipe->params_data_type->child_reflect_data_count; idx++)
                    {
                        auto reflect_data = &pipe->params_data_type->child_reflect_datas[idx];
                        auto reflect_metadata = find_metadata(pipe->params_metadata_type, reflect_data->name);
                        showReflectParameter(compute, pipe->params_data, reflect_data, pipe->params_metadata, reflect_metadata);
                        if (pnanovdb_reflect_layout_compare(
                                reflect_data->data_type,
                                PNANOVDB_REFLECT_DATA_TYPE(pnanovdb_pipeline_shader_t)))
                        {
                            ImGui::Indent();
                            pnanovdb_pipeline_shader_t* shader = (pnanovdb_pipeline_shader_t*)(((uint8_t*)pipe->params_data) + reflect_data->data_offset);
                            showShaderParams(compute, shader, pipe->params_metadata, reflect_metadata);
                            ImGui::Unindent();
                        }
                        else if (reflect_data->data_type->data_type == PNANOVDB_REFLECT_TYPE_STRUCT)
                        {
                            auto reflect_type2 = reflect_data->data_type;
                            uint8_t* data2 = (uint8_t*)pipe->params_data + reflect_data->data_offset;
                            for (pnanovdb_uint64_t idx2 = 0u; idx2 < reflect_type2->child_reflect_data_count; idx2++)
                            {
                                auto reflect_data2 = &reflect_type2->child_reflect_datas[idx2];
                                showReflectParameter(compute, data2, reflect_data2, nullptr, nullptr);
                            }
                        }
                    }
                }
            }
        }
    }

    if (mapped_object)
    {
        pipeline->config_unmap_params(config, scene, object_name, PNANOVDB_REFLECT_DATA_TYPE(pnanovdb_pipeline_object_mapped_t));
    }
    pipeline->config_unmap_params(config, scene, nullptr, PNANOVDB_REFLECT_DATA_TYPE(pnanovdb_pipeline_object_list_t));

    if (set_pipe_object_name)
    {
        pipeline->config_set_pipeline(
        config,
        scene,
        set_pipe_object_name,
        (pnanovdb_pipeline_stage_t)set_pipe_stage_idx,
        set_pipe_pipe_idx,
        set_pipe_type);
    }
}

void properties_camera(
    Instance* ptr,
    const pnanovdb_compute_t* compute,
    pnanovdb_pipeline_t* pipeline,
    pnanovdb_pipeline_registry_t* registry,
    pnanovdb_pipeline_config_t* config,
    pnanovdb_compute_token_t* scene,
    int32_t selected_object_idx)
{
    pnanovdb_pipeline_object_list_t* mapped_object_list = nullptr;
    if (!pipeline->config_map_params(config, scene, nullptr, PNANOVDB_REFLECT_DATA_TYPE(pnanovdb_pipeline_object_list_t), (void**)&mapped_object_list))
    {
        return;
    }
    pnanovdb_compute_token_t* object_name = nullptr;
    pnanovdb_pipeline_object_mapped_t* mapped_object = nullptr;
    if (selected_object_idx >= 0 && selected_object_idx < mapped_object_list->object_count)
    {
        object_name = mapped_object_list->objects[selected_object_idx];
        pipeline->config_map_params(config, scene, object_name, PNANOVDB_REFLECT_DATA_TYPE(pnanovdb_pipeline_object_mapped_t), (void**)&mapped_object);
    }

    if (mapped_object && mapped_object->camera)
    {
        auto camera = mapped_object->camera;
        auto& state = camera->state;

        ImGui::SeparatorText("Camera");
        if (ImGui::Button("Reset"))
        {
            if (mapped_object->camera_default)
            {
                *mapped_object->camera = *mapped_object->camera_default;
            }
        }

        if (ImGui::RadioButton("Perspective", camera->config.is_orthographic == PNANOVDB_FALSE))
        {
            camera->config.is_orthographic = PNANOVDB_FALSE;
            applyReverseZFarPlane(&camera->config);
        }
        ImGui::SameLine();
        if (ImGui::RadioButton("Orthographic", camera->config.is_orthographic == PNANOVDB_TRUE))
        {
            camera->config.is_orthographic = PNANOVDB_TRUE;
            applyReverseZFarPlane(&camera->config);
        }
        pnanovdb_vec3_t eyePos = pnanovdb_camera_get_eye_position_from_state(&state);
        if (ImGui::DragFloat3("Origin", &eyePos.x, 0.1f))
        {
            // Keep look-at point fixed, recalculate direction and distance from new eye position
            pnanovdb_vec3_t delta = { state.position.x - eyePos.x, state.position.y - eyePos.y,
                                    state.position.z - eyePos.z };
            float len2 = delta.x * delta.x + delta.y * delta.y + delta.z * delta.z;
            if (len2 > 0.f)
            {
                float len = pnanovdb_camera_sqrt(len2);
                state.eye_direction.x = delta.x / len;
                state.eye_direction.y = delta.y / len;
                state.eye_direction.z = delta.z / len;
                state.eye_distance_from_position = len;
            }
            else
            {
                state.eye_distance_from_position = 0.f;
            }
        }
        pnanovdb_vec3_t lookAt = state.position;
        if (ImGui::DragFloat3("Look At", &lookAt.x, 0.1f))
        {
            pnanovdb_vec3_t eye = pnanovdb_camera_get_eye_position_from_state(&state);
            pnanovdb_vec3_t delta = { lookAt.x - eye.x, lookAt.y - eye.y, lookAt.z - eye.z };
            float len2 = delta.x * delta.x + delta.y * delta.y + delta.z * delta.z;
            if (len2 > 0.f)
            {
                float len = pnanovdb_camera_sqrt(len2);
                pnanovdb_vec3_t dir = { delta.x / len, delta.y / len, delta.z / len };
                state.position = lookAt;
                state.eye_direction = dir;
                state.eye_distance_from_position = len;
            }
            else
            {
                state.position = lookAt;
                state.eye_distance_from_position = 0.f;
            }
        }
        pnanovdb_vec3_t upVec = state.eye_up;
        if (ImGui::DragFloat3("Up", &upVec.x, 0.1f))
        {
            float len =
                sqrtf(upVec.x * upVec.x + upVec.y * upVec.y + upVec.z * upVec.z);
            const float EPSILON = 1e-6f;
            if (len > EPSILON)
            {
                upVec.x /= len;
                upVec.y /= len;
                upVec.z /= len;
            }
            state.eye_up = upVec;
        }

        ImGui::DragFloat("FOV", &camera->config.fov_angle_y, 0.01f, 0.f, M_PI_2);
        ImGui::DragFloat("Near Plane", &camera->config.near_plane, 0.1f, 0.01f, 10000.f);
        ImGui::DragFloat("Far Plane", &camera->config.far_plane, 10.f, 1.f, 100000.f);
        if (camera->config.is_orthographic)
        {
            ImGui::DragFloat("Orthographic Y", &camera->config.orthographic_y, 0.1f, 0.f, 100000.f);
        }

        ImGui::SeparatorText("Camera Default");
        if (ImGui::Button("Set as Default"))
        {
            if (mapped_object->camera_default)
            {
                *mapped_object->camera_default = *mapped_object->camera;
            }
        }
    }

    if (mapped_object)
    {
        pipeline->config_unmap_params(config, scene, object_name, PNANOVDB_REFLECT_DATA_TYPE(pnanovdb_pipeline_object_mapped_t));
    }
    pipeline->config_unmap_params(config, scene, nullptr, PNANOVDB_REFLECT_DATA_TYPE(pnanovdb_pipeline_object_list_t));
}

}

pnanovdb_imgui_instance_interface_t* get_user_imgui_instance_interface()
{
    using namespace imgui_instance_user;
    static pnanovdb_imgui_instance_interface_t iface = { PNANOVDB_REFLECT_INTERFACE_INIT(
        pnanovdb_imgui_instance_interface_t) };
    iface.create = create;
    iface.destroy = destroy;
    iface.update = update;
    iface.get_style = get_style;
    iface.get_io = get_io;
    iface.get_tex_data_as_rgba32 = get_tex_data_as_rgba32;
    iface.get_draw_data = get_draw_data;
    return &iface;
}
