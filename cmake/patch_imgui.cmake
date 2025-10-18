# CMake script to patch imgui imconfig.h to use thread-local storage for ImGui context
file(READ "imconfig.h" IMGUI_CONFIG_CONTENT)

# Add thread-local storage configuration after #pragma once
string(REPLACE
    "#pragma once\n\n//---- Define assertion handler."
    "#pragma once\n\n//---- Use thread_local storage for ImGui context\nstruct ImGuiContext;\nextern thread_local ImGuiContext* ImGuiTLS;\n#define GImGui ImGuiTLS\n\n//---- Define assertion handler."
    IMGUI_CONFIG_CONTENT
    "${IMGUI_CONFIG_CONTENT}"
)

# Write the modified content back
file(WRITE "imconfig.h" "${IMGUI_CONFIG_CONTENT}")

message(STATUS "Successfully patched imgui imconfig.h")

