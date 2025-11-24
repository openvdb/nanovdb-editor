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

editor, compute, compiler = nve.create_default(device_id=args.device)

config = nve.EditorConfig()

# Default values, set as needed
config.ip_address = b"127.0.0.1"
config.port = 8080
config.headless = 0
config.streaming = 0

editor.show(config)
# editor.wait_for_interrupt()   # uncomment when streaming is enabled
```

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
