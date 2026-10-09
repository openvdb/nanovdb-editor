// Copyright Contributors to the OpenVDB Project
// SPDX-License-Identifier: Apache-2.0

/*!
    \file   ColorRamp.cpp

    \author Petra Hapalova

    \brief
*/

#include "ColorRamp.h"

#include <imgui.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>

namespace pnanovdb_editor
{

std::array<float, 4> sampleColorRamp(const std::vector<ColorRampPoint>& points, float position)
{
    const ColorRampPoint* low = nullptr;
    const ColorRampPoint* high = nullptr;
    for (const auto& point : points)
    {
        // Later slots win when positions are equal.
        if (point.position <= position && (!low || point.position >= low->position))
        {
            low = &point;
        }
        if (point.position >= position && (!high || point.position <= high->position))
        {
            high = &point;
        }
    }
    if (!low)
    {
        return high ? high->color : std::array<float, 4>{};
    }
    if (!high || low->position == high->position)
    {
        return low->color;
    }
    const float weight = (position - low->position) / (high->position - low->position);
    std::array<float, 4> color;
    for (size_t channel = 0; channel < color.size(); ++channel)
    {
        color[channel] = low->color[channel] + weight * (high->color[channel] - low->color[channel]);
    }
    return color;
}

bool addColorRampPoint(std::vector<ColorRampPoint>& points, size_t capacity, float position)
{
    if (points.size() >= capacity || !std::isfinite(position))
    {
        return false;
    }
    position = std::clamp(position, 0.f, 1.f);
    points.push_back({ position, sampleColorRamp(points, position) });
    return true;
}

bool removeColorRampPoint(std::vector<ColorRampPoint>& points, size_t index)
{
    if (points.size() <= 1 || index >= points.size())
    {
        return false;
    }
    points.erase(points.begin() + index);
    return true;
}

namespace
{

float clampForDisplay(float position)
{
    return std::isfinite(position) ? std::clamp(position, 0.f, 1.f) : 0.f;
}

ImU32 displayColor(const std::array<float, 4>& color)
{
    return ImGui::ColorConvertFloat4ToU32(
        ImVec4(clampForDisplay(color[0]), clampForDisplay(color[1]), clampForDisplay(color[2]), clampForDisplay(color[3])));
}

std::vector<float> rampBoundaries(const std::vector<ColorRampPoint>& points)
{
    std::vector<float> boundaries{ 0.f, 1.f };
    for (const auto& point : points)
    {
        boundaries.push_back(clampForDisplay(point.position));
    }
    std::sort(boundaries.begin(), boundaries.end());
    return boundaries;
}

std::vector<float> displayBoundaries(const std::vector<ColorRampPoint>& points)
{
    const auto stops = rampBoundaries(points);
    auto boundaries = stops;
    for (size_t i = 1; i < stops.size(); ++i)
    {
        const auto left = sampleColorRamp(points, stops[i - 1]);
        const auto right = sampleColorRamp(points, stops[i]);
        for (size_t channel = 0; channel < left.size(); ++channel)
        {
            for (float limit : { 0.f, 1.f })
            {
                if ((left[channel] < limit && right[channel] > limit) ||
                    (right[channel] < limit && left[channel] > limit))
                {
                    const float weight = (limit - left[channel]) / (right[channel] - left[channel]);
                    boundaries.push_back(stops[i - 1] + weight * (stops[i] - stops[i - 1]));
                }
            }
        }
    }
    std::sort(boundaries.begin(), boundaries.end());
    return boundaries;
}

float newStopPosition(const std::vector<ColorRampPoint>& points)
{
    const auto boundaries = rampBoundaries(points);
    float width = -1.f;
    float position = 0.5f;
    for (size_t i = 1; i < boundaries.size(); ++i)
    {
        if (boundaries[i] - boundaries[i - 1] > width)
        {
            width = boundaries[i] - boundaries[i - 1];
            position = (boundaries[i] + boundaries[i - 1]) * 0.5f;
        }
    }
    return position;
}

void drawRamp(const ImVec2& minimum, const ImVec2& maximum, const std::vector<ColorRampPoint>& points)
{
    auto* draw = ImGui::GetWindowDrawList();
    const float tile = (maximum.y - minimum.y) / 3.f;
    for (int row = 0; minimum.y + row * tile < maximum.y; ++row)
    {
        for (int column = 0; minimum.x + column * tile < maximum.x; ++column)
        {
            const ImVec2 start(minimum.x + column * tile, minimum.y + row * tile);
            const ImVec2 end(std::min(start.x + tile, maximum.x), std::min(start.y + tile, maximum.y));
            draw->AddRectFilled(start, end, (row + column) % 2 ? IM_COL32(90, 90, 90, 255) : IM_COL32(160, 160, 160, 255));
        }
    }
    const auto boundaries = displayBoundaries(points);
    const float width = maximum.x - minimum.x;
    for (size_t i = 1; i < boundaries.size(); ++i)
    {
        const float start = boundaries[i - 1];
        const float end = boundaries[i];
        if (start == end)
        {
            continue;
        }
        const ImU32 left = displayColor(sampleColorRamp(points, start));
        const ImU32 right = displayColor(sampleColorRamp(points, end));
        draw->AddRectFilledMultiColor(ImVec2(minimum.x + start * width, minimum.y),
                                     ImVec2(minimum.x + end * width, maximum.y), left, right, right, left);
    }
    draw->AddRect(minimum, maximum, ImGui::GetColorU32(ImGuiCol_Border), 0.f, 0, ImGui::GetFrameHeight() / 20.f);
}

void includeColor(std::array<float, 4>& ranges, const std::array<float, 4>& color)
{
    for (size_t channel = 0; channel < color.size(); ++channel)
    {
        if (std::isfinite(color[channel]))
        {
            const size_t offset = channel < 3 ? 0 : 2;
            ranges[offset] = std::min(ranges[offset], color[channel]);
            ranges[offset + 1] = std::max(ranges[offset + 1], color[channel]);
        }
    }
}

void editPickerRange(const char* label, float* range, const float* color, size_t channels)
{
    float candidate[] = { range[0], range[1] };
    bool invalid = false;
    if (ImGui::DragFloat2(label, candidate, 0.01f, 0.f, 0.f, "%.3g"))
    {
        const bool valid = std::isfinite(candidate[0]) && std::isfinite(candidate[1]) && candidate[0] < candidate[1] &&
                           std::all_of(color, color + channels,
                                       [&](float value) { return value >= candidate[0] && value <= candidate[1]; });
        if (valid)
        {
            std::copy(candidate, candidate + 2, range);
        }
        else
        {
            invalid = true;
        }
    }
    if (invalid)
    {
        ImGui::SetTooltip("Use a finite, increasing range that contains the current values.");
    }
    else if (ImGui::IsItemHovered())
    {
        ImGui::SetTooltip(
            "Picker minimum and maximum. Changing the range keeps the color unchanged.\n"
            "The range must contain the current values.");
    }
}

bool acceptColorDrop(std::array<float, 4>& color)
{
    bool changed = false;
    if (ImGui::BeginDragDropTarget())
    {
        for (size_t channels : { 3u, 4u })
        {
            const char* type = channels == 3 ? IMGUI_PAYLOAD_TYPE_COLOR_3F : IMGUI_PAYLOAD_TYPE_COLOR_4F;
            if (const auto* payload = ImGui::AcceptDragDropPayload(type))
            {
                if (payload->DataSize == channels * sizeof(float))
                {
                    std::memcpy(color.data(), payload->Data, channels * sizeof(float));
                    changed = true;
                }
            }
        }
        ImGui::EndDragDropTarget();
    }
    return changed;
}

bool editRgba(const char* label, std::array<float, 4>& color)
{
    // ColorEdit4 converts through integers even in HDR mode. DragFloat keeps large values valid.
    const char* ids[] = { "##X", "##Y", "##Z", "##W" };
    const char* formats[] = { "R:%.3f", "G:%.3f", "B:%.3f", "A:%.3f" };
    const float spacing = ImGui::GetStyle().ItemInnerSpacing.x;
    const float width = std::max(1.f, (ImGui::CalcItemWidth() - 3.f * spacing) / 4.f);
    const bool show_prefix = width >= ImGui::CalcTextSize("M:0.000").x + 2.f * ImGui::GetStyle().FramePadding.x;
    bool changed = false;
    ImGui::PushID(label);
    for (size_t channel = 0; channel < color.size(); ++channel)
    {
        if (channel)
        {
            ImGui::SameLine(0.f, spacing);
        }
        ImGui::SetNextItemWidth(width);
        changed |= ImGui::DragFloat(
            ids[channel], &color[channel], 1.f / 255.f, 0.f, 0.f, show_prefix ? formats[channel] : "%.3f");
    }
    ImGui::PopID();
    return changed;
}

bool editHdrColor(std::array<float, 4>& color, int selected)
{
    bool open = false;
    const auto swatch = [&]()
    {
        auto preview = color;
        std::replace_if(preview.begin(), preview.end(), [](float value) { return !std::isfinite(value); }, 0.f);
        ImGui::PushID("Color");
        open = ImGui::ColorButton("##ColorButton", ImVec4(preview[0], preview[1], preview[2], preview[3]),
                                  ImGuiColorEditFlags_NoTooltip);
        ImGui::PopID();
    };
    const auto& style = ImGui::GetStyle();
    const float input_width = std::max(1.f, ImGui::CalcItemWidth() - ImGui::GetFrameHeight() - style.ItemInnerSpacing.x);
    ImGui::BeginGroup();
    if (style.ColorButtonPosition == ImGuiDir_Left)
    {
        swatch();
        ImGui::SameLine(0.f, style.ItemInnerSpacing.x);
    }
    ImGui::PushItemWidth(input_width);
    bool changed = editRgba("Color", color);
    ImGui::PopItemWidth();
    if (style.ColorButtonPosition == ImGuiDir_Right)
    {
        ImGui::SameLine(0.f, style.ItemInnerSpacing.x);
        swatch();
    }
    ImGui::SameLine(0.f, style.ItemInnerSpacing.x);
    ImGui::TextUnformatted("Color");
    ImGui::EndGroup();
    changed |= acceptColorDrop(color);
    if (ImGui::IsItemHovered())
    {
        ImGui::SetTooltip(
            "Drag or type HDR RGBA values, including values above one.\n"
            "Click the swatch to open the HDR picker.");
    }

    ImGui::PushID("Color");
    auto* state = ImGui::GetStateStorage();
    const ImGuiID stop_id = ImGui::GetID("picker_stop");
    const ImGuiID range_ids[] = { ImGui::GetID("rgb_min"), ImGui::GetID("rgb_max"), ImGui::GetID("alpha_min"),
                                  ImGui::GetID("alpha_max") };
    std::array<float, 4> ranges{ 0.f, 1.f, 0.f, 1.f };
    if (open)
    {
        includeColor(ranges, color);
        state->SetInt(stop_id, selected);
        ImGui::OpenPopup("picker");
    }
    else
    {
        for (size_t i = 0; i < ranges.size(); ++i)
        {
            ranges[i] = state->GetFloat(range_ids[i], ranges[i]);
        }
    }
    if (ImGui::BeginPopup("picker"))
    {
        if (state->GetInt(stop_id, -1) != selected)
        {
            ImGui::CloseCurrentPopup();
        }
        else
        {
            ImGui::PushItemWidth(ImGui::GetFontSize() * 18.f);
            if (std::all_of(color.begin(), color.end(), [](float value) { return std::isfinite(value); }))
            {
                // Keep the range fixed during drags. Expand it only for values outside the range.
                includeColor(ranges, color);
                std::array<float, 4> normalized;
                for (size_t channel = 0; channel < color.size(); ++channel)
                {
                    const size_t offset = channel < 3 ? 0 : 2;
                    const double minimum = ranges[offset];
                    const double width = double(ranges[offset + 1]) - minimum;
                    normalized[channel] = float((double(color[channel]) - minimum) / width);
                }
                const auto original = normalized;
                if (ImGui::ColorPicker4("##picker", normalized.data(),
                                        ImGuiColorEditFlags_NoInputs | ImGuiColorEditFlags_NoOptions |
                                            ImGuiColorEditFlags_NoSidePreview | ImGuiColorEditFlags_NoLabel |
                                            ImGuiColorEditFlags_InputRGB | ImGuiColorEditFlags_PickerHueBar |
                                            ImGuiColorEditFlags_AlphaBar | ImGuiColorEditFlags_NoDragDrop))
                {
                    for (size_t channel = 0; channel < color.size(); ++channel)
                    {
                        if (normalized[channel] != original[channel])
                        {
                            const size_t offset = channel < 3 ? 0 : 2;
                            const double weight = normalized[channel];
                            color[channel] =
                                float((1.0 - weight) * double(ranges[offset]) + weight * double(ranges[offset + 1]));
                            changed = true;
                        }
                    }
                }
            }
            ImGui::BeginGroup();
            const bool numeric_changed = editRgba("RGBA", color);
            ImGui::SameLine(0.f, style.ItemInnerSpacing.x);
            ImGui::TextUnformatted("RGBA");
            ImGui::EndGroup();
            if (acceptColorDrop(color) || numeric_changed)
            {
                includeColor(ranges, color);
                changed = true;
            }
            editPickerRange("RGB range", ranges.data(), color.data(), 3);
            editPickerRange("Alpha range", ranges.data() + 2, color.data() + 3, 1);
            ImGui::PopItemWidth();
            for (size_t i = 0; i < ranges.size(); ++i)
            {
                state->SetFloat(range_ids[i], ranges[i]);
            }
        }
        ImGui::EndPopup();
    }
    ImGui::PopID();
    return changed;
}

} // namespace

bool renderColorRamp(const char* label, std::vector<ColorRampPoint>& points, size_t capacity, const char* tooltip)
{
    if (points.empty())
    {
        return false;
    }
    bool changed = false;
    const auto show_hint = [&]()
    {
        if (tooltip && *tooltip && ImGui::IsItemHovered())
        {
            ImGui::SetTooltip("%s", tooltip);
        }
    };
    ImGui::PushID(label);
    ImGui::BeginGroup();
    ImGui::TextUnformatted(label, std::strstr(label, "##"));
    show_hint();
    auto* state = ImGui::GetStateStorage();
    const ImGuiID selected_id = ImGui::GetID("selected");
    const ImGuiID drag_start_id = ImGui::GetID("drag_start");
    int selected = std::clamp(state->GetInt(selected_id), 0, static_cast<int>(points.size()) - 1);
    const float bar_height = ImGui::GetFrameHeight() * 1.25f;
    const float marker_radius = bar_height * 0.25f;
    const float marker_gap = ImGui::GetStyle().ItemInnerSpacing.y * 0.25f;
    const float marker_border = ImGui::GetFrameHeight() / 20.f;
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    const float width = std::max(ImGui::GetContentRegionAvail().x, 2.f * marker_radius + 1.f);
    const ImVec2 bar_min(origin.x + marker_radius, origin.y);
    const ImVec2 bar_max(origin.x + width - marker_radius, origin.y + bar_height);
    const float bar_width = bar_max.x - bar_min.x;
    ImGui::InvisibleButton("stops", ImVec2(width, bar_height + 2.f * marker_radius + 2.f * marker_gap));
    show_hint();
    if (ImGui::IsItemActivated())
    {
        float distance = std::numeric_limits<float>::max();
        for (size_t i = 0; i < points.size(); ++i)
        {
            const float x = bar_min.x + clampForDisplay(points[i].position) * bar_width;
            const float candidate = std::abs(ImGui::GetIO().MousePos.x - x);
            if (candidate <= distance)
            {
                distance = candidate;
                selected = static_cast<int>(i);
            }
        }
        state->SetFloat(drag_start_id, clampForDisplay(points[selected].position));
    }
    if (ImGui::IsItemActive() && ImGui::IsMouseDragging(ImGuiMouseButton_Left))
    {
        const float position = std::clamp(
            state->GetFloat(drag_start_id) + ImGui::GetMouseDragDelta(ImGuiMouseButton_Left).x / bar_width, 0.f, 1.f);
        if (position != points[selected].position)
        {
            points[selected].position = position;
            changed = true;
        }
    }
    drawRamp(bar_min, bar_max, points);
    auto* draw = ImGui::GetWindowDrawList();
    for (size_t i = 0; i < points.size(); ++i)
    {
        const float x = bar_min.x + clampForDisplay(points[i].position) * bar_width;
        const ImVec2 a(x, bar_max.y + marker_gap);
        const ImVec2 b(x - marker_radius, bar_max.y + 2.f * marker_radius + marker_gap);
        const ImVec2 c(x + marker_radius, b.y);
        draw->AddTriangleFilled(a, b, c, displayColor({ points[i].color[0], points[i].color[1], points[i].color[2], 1.f }));
        draw->AddTriangle(a, b, c, ImGui::GetColorU32(static_cast<int>(i) == selected ? ImGuiCol_Text : ImGuiCol_Border),
                          marker_border * (static_cast<int>(i) == selected ? 2.f : 1.f));
    }
    if (ImGui::ArrowButton("previous", ImGuiDir_Left))
    {
        selected = (selected + static_cast<int>(points.size()) - 1) % static_cast<int>(points.size());
    }
    ImGui::SameLine();
    ImGui::Text("Stop %d / %zu", selected + 1, points.size());
    ImGui::SameLine();
    if (ImGui::ArrowButton("next", ImGuiDir_Right))
    {
        selected = (selected + 1) % static_cast<int>(points.size());
    }
    ImGui::PushItemWidth(std::max(1.f, ImGui::GetContentRegionAvail().x - ImGui::CalcTextSize("Position").x -
                                        ImGui::GetStyle().ItemInnerSpacing.x));
    changed |= ImGui::SliderFloat("Position", &points[selected].position, 0.f, 1.f, "%.3f", ImGuiSliderFlags_AlwaysClamp);
    changed |= editHdrColor(points[selected].color, selected);
    ImGui::PopItemWidth();
    ImGui::BeginDisabled(points.size() >= capacity);
    if (ImGui::Button("Add stop") && addColorRampPoint(points, capacity, newStopPosition(points)))
    {
        selected = static_cast<int>(points.size()) - 1;
        changed = true;
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::BeginDisabled(points.size() <= 1);
    if (ImGui::Button("Remove stop") && removeColorRampPoint(points, selected))
    {
        selected = std::min(selected, static_cast<int>(points.size()) - 1);
        changed = true;
    }
    ImGui::EndDisabled();
    state->SetInt(selected_id, selected);
    ImGui::EndGroup();
    ImGui::PopID();
    return changed;
}

} // namespace pnanovdb_editor
