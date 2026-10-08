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
    constexpr float tile = 8.f;
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
    draw->AddRect(minimum, maximum, ImGui::GetColorU32(ImGuiCol_Border));
}

} // namespace

bool renderColorRamp(const char* label, std::vector<ColorRampPoint>& points, size_t capacity)
{
    if (points.empty())
    {
        return false;
    }
    bool changed = false;
    ImGui::PushID(label);
    ImGui::BeginGroup();
    ImGui::TextUnformatted(label, std::strstr(label, "##"));
    auto* state = ImGui::GetStateStorage();
    const ImGuiID selected_id = ImGui::GetID("selected");
    const ImGuiID drag_start_id = ImGui::GetID("drag_start");
    int selected = std::clamp(state->GetInt(selected_id), 0, static_cast<int>(points.size()) - 1);
    constexpr float marker_radius = 6.f;
    constexpr float bar_height = 24.f;
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    const float width = std::max(ImGui::GetContentRegionAvail().x, 2.f * marker_radius + 1.f);
    const ImVec2 bar_min(origin.x + marker_radius, origin.y);
    const ImVec2 bar_max(origin.x + width - marker_radius, origin.y + bar_height);
    const float bar_width = bar_max.x - bar_min.x;
    ImGui::InvisibleButton("stops", ImVec2(width, bar_height + 2.f * marker_radius + 2.f));
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
        const ImVec2 a(x, bar_max.y + 1.f);
        const ImVec2 b(x - marker_radius, bar_max.y + 2.f * marker_radius + 1.f);
        const ImVec2 c(x + marker_radius, b.y);
        draw->AddTriangleFilled(a, b, c, displayColor({ points[i].color[0], points[i].color[1], points[i].color[2], 1.f }));
        draw->AddTriangle(a, b, c, ImGui::GetColorU32(static_cast<int>(i) == selected ? ImGuiCol_Text : ImGuiCol_Border),
                          static_cast<int>(i) == selected ? 2.f : 1.f);
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
    changed |= ImGui::ColorEdit4("Color", points[selected].color.data(),
                                ImGuiColorEditFlags_Float | ImGuiColorEditFlags_HDR | ImGuiColorEditFlags_DisplayRGB |
                                    ImGuiColorEditFlags_AlphaBar);
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
