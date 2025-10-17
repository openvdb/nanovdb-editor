// Copyright Contributors to the OpenVDB Project
// SPDX-License-Identifier: Apache-2.0

/*!
    \file   nanovdb_editor/editor/ImguiIni.h

    \author Petra Hapalova

    \brief  Default ini settings for UI profiles
*/

#pragma once

#include <string>

// TODO: this will be in json

const std::string viewer_ini = R"(
[Window][Settings]
Pos=1080,21
Size=360,699
Collapsed=0
DockId=0x00000003,1

[Window][Viewport]
Pos=1080,21
Size=360,699
Collapsed=0
DockId=0x00000003,0

[Window][Compiler]
Collapsed=0
DockId=0x00000003

[Window][Shader Editor]
Collapsed=0
DockId=0x00000003

[Window][Profiler]
Collapsed=0
DockId=0x00000003

[Window][File Header]
Collapsed=0
DockId=0x00000003

[Window][Log]
Pos=0,576
Size=1078,144
Collapsed=0
DockId=0x00000009,0

[Window][Scene]
Pos=0,21
Size=288,275
Collapsed=0
DockId=0x0000000C,0

[Window][Properties]
Pos=0,298
Size=288,276
Collapsed=0
DockId=0x0000000D,1

[Window][Params]
Pos=0,298
Size=288,276
Collapsed=0
DockId=0x0000000D,0

[Window][Benchmark]
Collapsed=0
DockId=0x0000000D

[Window][WindowOverViewport_11111111]
Pos=0,21
Size=1440,699
Collapsed=0

[Window][Debug##Default]
Pos=60,60
Size=400,400
Collapsed=0

[Docking][Data]
DockSpace           ID=0x00000001 Window=0x1BBC0F80 Pos=0,21 Size=1440,699 Split=X
  DockNode          ID=0x00000002 Parent=0x00000001 SizeRef=64,0 Split=X
    DockNode        ID=0x00000004 Parent=0x00000002 SizeRef=64,0 Split=Y
      DockNode      ID=0x00000008 Parent=0x00000004 SizeRef=0,64 Split=X
        DockNode    ID=0x0000000A Parent=0x00000008 SizeRef=288,720 Split=Y
          DockNode  ID=0x0000000C Parent=0x0000000A SizeRef=288,718 Selected=0xE601B12F
          DockNode  ID=0x0000000D Parent=0x0000000A SizeRef=288,720 Selected=0x3F5D5ECA
        DockNode    ID=0x0000000B Parent=0x00000008 SizeRef=64,0 CentralNode=1
      DockNode      ID=0x00000009 Parent=0x00000004 SizeRef=1440,144 Selected=0x139FDA3F
    DockNode        ID=0x00000005 Parent=0x00000002 SizeRef=288,720 Split=Y
      DockNode      ID=0x00000006 Parent=0x00000005 SizeRef=288,287
      DockNode      ID=0x00000007 Parent=0x00000005 SizeRef=288,431
  DockNode          ID=0x00000003 Parent=0x00000001 SizeRef=360,720 Selected=0x4746B4B8

[RenderSettings][default]
vsync=1
is_projection_rh=1
is_orthographic=0
is_reverse_z=1
is_y_up=0
is_upside_down=0
camera_speed_multiplier=1.000000
ui_profile_name=viewer

[CameraState][default]
position=0.000000,0.000000,0.000000
eye_direction=0.000000,0.700000,0.700000
eye_up=0.000000,0.000000,1.000000
eye_distance_from_position=10.000000
orthographic_scale=1.000000

[InstanceSettings][Settings]
GroupName=
SelectedRenderSettingsName=default
WindowWidth=1440
WindowHeight=720
ShowProfiler=0
ShowCodeEditor=0
ShowConsole=0
ShowViewportSettings=0
ShowRenderSettings=0
ShowCompilerSettings=0
ShowShaderParams=0
ShowBenchmark=0
ShowFileHeader=0
ShowScene=1
ShowSceneProperties=1
ShowAbout=0

[CodeEditorTabs][Settings]
SelectedTab=editor/editor.slang
SelectedOption=0

[CodeEditorTabs][Tab_0]
ShaderName=editor/editor.slang
FirstVisibleLine=-1
ViewerFirstVisibleLine=-1

)";
