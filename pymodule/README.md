##  NanoVDB Editor

Prerequisities:
- `numpy`

### Running in Docker
To run the editor in the docker container, the Dockerfile needs to contain:
```dockerfile
EXPOSE 8080

ENV NVIDIA_DRIVER_CAPABILITIES compute,graphics,utility

RUN apt-get update \
    && apt-get install -y \
    libxext6 \
    libegl1
```
Then run with the NVIDIA runtime selected (https://docs.nvidia.com/datacenter/cloud-native/container-toolkit/latest/install-guide.html):
```sh
docker run --runtime=nvidia --net=host --gpus=all ...
```

### Hello World

```py
import nanovdb_editor as nve

with nve.create_default() as app:
    # Python-friendly kwargs (or pass make_editor_config(...))
    app.show(ip="127.0.0.1", port=8080)
    # Headless/streaming: app.run(gui=False, headless=True, streaming=True, ...)
# Session closes here: worker stopped, editor shut down, compiler destroyed
```

### Pipelines

The editor's process pipelines can build NanoVDB grids from higher-level inputs
directly in Python:

```py
with nve.create_default() as app:
    scene = app.scene("main")

    nvdb = scene.nanovdb_from_gaussians(
        means=means, quats=quats, scales=scales,
        sh_0=sh_0, sh_n=sh_n, opacities=opacities,
        process="voxelbvh", resolution=512,
    )

    # Bake colors to an RGBA8 image grid, and choose how it's drawn
    rgba8 = scene.nanovdb_to_rgba8(nvdb, name="splats_rgba8", register=False)
    scene.set_render_pipeline("gaussians", "voxelbvh_rgba8_render")
```

You can also load an existing grid (`scene.nanovdb_from_file("grid.nvdb")`),
tune per-object parameters (`scene.set_voxels_per_unit("gaussians", 256)`,
`scene.set_resolution(...)`, `scene.set_inflation_radius(...)`),
build multi-step process chains (`scene.process_steps("mesh").append(...)`), bake
RGBA8 for several ray directions at once
(`scene.nanovdb_to_rgba8_directions(...)`), and manage scene objects with
`scene.remove` / `scene.add_grid` / `scene.update_camera` / `scene.add_image2d`.

The returned `Grid` is a context manager, and `grid.map(dtype)` hands you a
live, zero-copy NumPy view of its bytes that is unmapped on block exit:

```py
import numpy as np

with scene.nanovdb_from_mesh(indices=indices, positions=positions, register=False) as grid:
    with grid.map(np.uint32) as view:   # writes go back to the grid
        print(int(view[0]))
    grid.save("mesh.nvdb")
```

See [PIPELINES.md](PIPELINES.md) for the available pipelines, options, and a
guide to exposing (pythonizing) more of them.

### Shader Parameters
Shaders can have defined struct with shader parameters which are intended to be shown in the editor's UI:
```hlsl
struct shader_params_t
{
    float4 color;
    bool use_color;
    bool3 _pad1;
    int _pad2;
};
ConstantBuffer<shader_params_t> shader_params;
```

Shader parameters can have defined default values in the json file:
```json
{
    "ShaderParams": {
        "color": {
            "value": [1.0, 0.0, 1.0, 1.0],
            "min": 0.0,
            "max": 1.0,
            "step": 0.01
        }
    }
}
```
Supported types: `bool`, `int`, `uint`, `int64`, `uint64`, `float` and its vectors and 4x4 matrix.
Variables with `_pad` in the name are not shown in the UI.
Those parameters can be interactively changed with generated UI in the editor's Params tab.

To display a group of shader parameters from different shaders define a json file with various shader paths:
```json
{
    "ShaderParams": [
        "editor/editor.slang",
        "test/test.slang"
    ]
}
```

## Acknowledgements

This project makes use of the following libraries:

- [zlib](https://github.com/madler/zlib) – Compression library
- [c-blosc](https://github.com/Blosc/c-blosc) – High-performance compressor optimized for binary data
- [Vulkan-Headers](https://github.com/KhronosGroup/Vulkan-Headers) – Vulkan API headers
- [Vulkan-Loader](https://github.com/KhronosGroup/Vulkan-Loader) – Vulkan ICD loader
- [GLFW](https://github.com/glfw/glfw) – Windowing, context, and input (optional)
- [Dear ImGui](https://github.com/ocornut/imgui) – Immediate-mode GUI
- [ImGuiFileDialog](https://github.com/aiekick/ImGuiFileDialog) – File dialog for Dear ImGui
- [ImGuiColorTextEdit](https://github.com/goossens/ImGuiColorTextEdit) – Syntax-highlighted text/code editor widget
- [Slang](https://github.com/shader-slang/slang) – Shading language and compiler
- [filewatch](https://github.com/ThomasMonkman/filewatch) – Cross-platform file watching
- [JSON for Modern C++](https://github.com/nlohmann/json) – JSON serialization for C++
- [cnpy](https://github.com/rogersce/cnpy) – Read/write NumPy .npy/.npz files from C++
- [zstr](https://github.com/mateidavid/zstr) – Transparent zlib iostream wrappers
- [llhttp](https://github.com/nodejs/llhttp) – High-performance HTTP parser
- [Asio](https://github.com/chriskohlhoff/asio) – Asynchronous networking and concurrency primitives
- [RESTinio](https://github.com/Stiffstream/restinio) – Lightweight HTTP server framework
- [fmt](https://github.com/fmtlib/fmt) – Modern formatting library
- [argparse](https://github.com/morrisfranken/argparse) – Header-only argument parser for C++17
- [expected-lite](https://github.com/martinmoene/expected-lite) – std::expected-like type for C++11/14/17
- [libE57Format](https://github.com/asmaloney/libE57Format) – E57 point cloud IO (optional)
- [OpenH264](https://github.com/cisco/openh264) – H.264 encoder (optional)
- [GoogleTest](https://github.com/google/googletest) – C++ testing framework

Many thanks to the authors and contributors of these projects.
