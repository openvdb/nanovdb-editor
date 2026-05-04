#  NanoVDB Editor

WIP

## Running in Docker
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

## Building and executing NanoVDB Editor

### Prerequisites
- git
- C++ compiler
- CMake at least 3.25
- Python 3.x
- Vulkan library `vulkan-1` (should be installed with graphics driver)

#### Streaming
- NVIDIA Driver Version at least 550.0 (https://developer.nvidia.com/vulkan/video/get-started)
- Vulkan video at least 1.3.275.0 (checked during editor startup, prints out a message if the upgrade is needed)

##### Upgrading Vulkan SDK on Linux
1. Download the latest Vulkan SDK from: https://vulkan.lunarg.com/sdk/home#linux
2. Follow the instructions for manual installation in section `Install the SDK`: https://vulkan.lunarg.com/doc/view/latest/linux/getting_started.html

```sh
VULKAN_SDK_VERSION="1.3.275.0"
cd ~
mkdir vulkan
cd vulkan
wget "https://sdk.lunarg.com/sdk/download/${VULKAN_SDK_VERSION}/linux/vulkansdk-linux-x86_64-${VULKAN_SDK_VERSION}.tar.xz"
tar xf "vulkansdk-linux-x86_64-${VULKAN_SDK_VERSION}.tar.xz"
echo "source ~/vulkan/${VULKAN_SDK_VERSION}/setup-env.sh" >> ~/.profile
. ~/.profile
# check the version
echo $VULKAN_SDK
```

#### Windows
- vcpkg (optional - recommended for e57 dependency)

### Dependencies

#### Python
```
pip install scikit-build wheel build numpy
```
#### Linux
By default, editor is built with enabled `NANOVDB_EDITOR_USE_GLFW` which requires:
```sh
sudo apt-get install libgl1-mesa-dev
```
In Conda environment:

`- mesalib`

The `NANOVDB_EDITOR_USE_GLFW` option can be disabled when using the editor in haedless and streaming mode only. In that case, `libvulkan.so.1` is built locally to ensure compatibility.

The `NANOVDB_EDITOR_USE_H264` is enabled by default, make sure you have:
```sh
sudo apt-get install make
```

#### macOS
Currently not actively used, might be stale.

```sh
# install Vulkan SDK from https://vulkan.lunarg.com/sdk/home#mac
# install homebrew from https://brew.sh
# brew install cmake
brew install glfw      # TODO: will be optional for streaming

```
#### Windows
Optionally, the project can use `vcpkg` for dependency management. If `NANOVDB_EDITOR_USE_VCPKG` is set, `vcpkg.json` automatically installs required dependencies.

To set up vcpkg:
```bat
git clone https://github.com/microsoft/vcpkg.git
cd vcpkg
bootstrap-vcpkg.bat
```

The following dependencies are automatically managed by `vcpkg.json`:
- blosc
- libe57format (and xerces-c dependency)

### Assets
Put any data files into the `data` folder, which is linked to the location next to the libraries.
Shaders are generated into the `shaders/_generated` folder next to the libraries.

### Build and Run
Run the build script with `-h` for available build options.

#### Build options

The Linux/macOS build script supports the following flags (combine as needed):

- **-x**: Perform a clean build (removes `build/`, also forces shader recompile)
- **-r**: Build in Release configuration
- **-d**: Build in Debug configuration
- **-v**: Enable verbose CMake build output
- **-s**: Compile Slang to ASM and clean shaders first
- **-a**: Build Debug with sanitizers enabled
- **-p**: Build and install the Python module (auto-installs `scikit-build-core` and `wheel`)
- **-e**: Install the Python module in editable mode (use with `-p`)
- **-t**: Run tests (ctest + pytest); honors `-r`/`-d` to pick configuration
- **-f**: Disable GLFW for a headless build

Notes:
- If neither `-r` nor `-d` is provided (and not using `-p` or `-t`), the script defaults to a Release build.
- For Python builds (`-p`), `-d` selects Debug wheels; otherwise Release is used.

Examples:

```sh
# Clean Release build
./build.sh -x -r

# Debug build with sanitizers
./build.sh -a

# Headless Release build (GLFW disabled)
./build.sh -f -r

# Build and install Python package (Release)
./build.sh -p

# Build and install Python package in Debug, editable mode
./build.sh -p -d -e

# Run tests (defaults to Release tests)
./build.sh -t

# Generate Slang ASM during build
./build.sh -s -r
```

#### Linux
```sh
./build.sh
```

#### Windows
Optionally, rename the config file `config` next to the build script to `config.ini` and fill in the environment variables:
```
MSVS_VERSION="Visual Studio 17 2022"
USE_VCPKG=ON
VCPKG_ROOT=path/to/vcpkg
```

To select a different profile for Slang compiler (https://github.com/shader-slang/slang/blob/master/source/slang/slang-profile-defs.h):
```
SLANG_PROFILE="sm_5_1"
```
Then, run the build script:
```bat
./build.bat
```

### Editor Application
After building, run the editor app:
```sh
./build/Release/pnanovdbeditorapp
```

### Python

The libraries can be bundled into a Python package with a wrapper for the C-type functions. The following script will automatically install scikit-build, wheel, and build dependencies:

Build with `-p` to build and install the `nanovdb_editor` package.

#### Python Test Apps

```sh
./build.sh -p
python3 test/test_editor.py
```

```sh
./build.sh -p
python3 test/test_streaming.py
```

#### Debugging
Add this line to the Python test script to print the PID of the process:
```python
import os
print(os.getpid())
```

1. Build the debug configuration
2. Run the test script; the console output will print the PID of the process
3. Attach to the process:

##### Linux

With GDB:
```sh
gdb -p <PID>
```

## fvdb.viz Integration Tests

We keep the Vulkan headless FVDB viewer validated in both CI and local development by sharing a cached Docker image and integration suite.

### Local workflow

`./scripts/run_fvdb_viz_integration.sh` mirrors the GitHub Actions job:

```sh
# Run release package tests (default)
./scripts/run_fvdb_viz_integration.sh

# Force rebuild the cached Docker image
./scripts/run_fvdb_viz_integration.sh --force-rebuild

# Validate the dev PyPI stream
./scripts/run_fvdb_viz_integration.sh --stream dev

# Smoke-test a locally built wheel
./scripts/run_fvdb_viz_integration.sh --local-wheel pymodule/dist/nanovdb_editor-*.whl
```

Highlights:
- Ensures (and caches) the `fvdb-test-image-<fvdb-core-version>` Docker image (for example, `fvdb-test-image-0.3.0`) with matching Torch/fvdb-core versions.
- Prints the installed `nanovdb_editor` version inside the container before running `pytests/test_fvdb_viz_integration.py -vv -s --full-trace`.
- Accepts `--force-rebuild` to bypass the local `.cache` tarball when you need a fresh base image.

### CI workflow

`.github/workflows/fvdb-viz-integration.yml` runs on `workflow_dispatch` or `workflow_call` and:
- Resolves the package stream (release/dev) plus optional wheel artifact.
- Restores a cached Docker image package with installed latest `fvdb-core` before building and executes the same pytest selector in Docker.

Use the workflow dispatch inputs in GitHub Actions to pick the stream or supply a wheel artifact.

## NanoVDB Editor GUI

### Shader Parameters
Shaders can have a defined struct with shader parameters that are intended to be shown in the editor's UI:

```hlsl
struct shader_params_t
{
    float4 color;
    bool use_color;
    bool3 _pad1;
    int _pad2;
};
```

The editor uses one JSON schema for both shader-backed parameters and editor-only custom scene parameters. Shader-backed parameters live under the top-level key `ShaderParams`; custom scene parameters live under `SceneParams`. The two payloads have an identical field schema and are routed by their top-level key.

Shader-backed parameters can define UI defaults and bounds in a JSON file:
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

Editor-only custom scene parameters use the same field schema under the top-level `SceneParams` key, but each field must also declare `type` because there is no shader reflection to infer it:
```json
{
    "SceneParams": {
        "exposure_bias": {
            "type": "float",
            "value": 0.0,
            "min": -4.0,
            "max": 4.0,
            "step": 0.1,
            "useSlider": true
        },
        "debug_slice": {
            "type": "bool",
            "value": false
        },
        "language_query": {
            "type": "string",
            "length": 128,
            "value": "a red chair"
        }
    }
}
```

Formal schema (applies under either `ShaderParams` or `SceneParams`):
- `ShaderParams` / `SceneParams`: object whose keys are field names mapped to field-definition objects. Under `ShaderParams` only, the value may instead be an array of shader paths to define a group file.
- `value`: scalar or array initial value shown in the UI. For `type: "string"` it must be a JSON string.
- `type`: required for custom scene params; ignored for shader-backed params because the type comes from shader reflection.
- `elementCount`: optional explicit array length for custom scene params when `value` is omitted or when zero-initialized storage is desired. Not valid for `type: "string"` (use `length` instead).
- `length`: required only for `type: "string"`; positive integer capacity of the `char[length]` buffer including the trailing `\0`. Capped at an editor-internal maximum.
- `min`: optional scalar or array lower bound for numeric widgets. Not valid for `type: "string"`.
- `max`: optional scalar or array upper bound for numeric widgets. Not valid for `type: "string"`.
- `step`: optional numeric increment for drag widgets. Defaults to `0.01`. Ignored for `type: "string"`.
- `useSlider`: optional boolean; renders a slider instead of a drag widget. Not valid for `type: "string"`.
- `isBool`: optional boolean; renders numeric `0`/`1` storage as a checkbox. Not valid for `type: "string"`.
- `hidden`: optional boolean; keeps the field mapped but hides it from the UI.

Supported scalar types: `bool`, `int`, `int32`, `uint`, `uint32`, `int64`, `uint64`, `float`, `double`, `string`.
Arrays are represented by using an array `value` or explicit `elementCount`; common reflected shader types such as float vectors and `float4x4` are exposed this way.
`type: "string"` maps to a fixed-capacity `char[length]` in the reflected data type. The widget writes directly into the buffer on every keystroke, and `map_params` clients always observe the current widget contents. Clients that need to throttle per-string-change work (e.g. a text encoder) should debounce on their side.
Variables with `_pad` in the name are not shown in the UI.
Shader-backed parameters are shown in the object properties/shader parameter UI. Custom scene params are editor-only, loaded per scene through `set_custom_scene_params(editor, scene, json_token, error_buf, error_buf_size)` where `json_token->str` contains the JSON payload; on failure the function returns `PNANOVDB_FALSE` and writes a human-readable message into `error_buf`. Custom scene params are rendered in the dedicated `Params` window. They do not currently change object `map_shader_params` output.

To display a group of shader parameters from different shaders, define a group JSON file with shader paths:
```json
{
    "ShaderParams": [
        "editor/editor.slang",
        "test/test.slang"
    ]
}
```

## Video Encoding To File

To convert output file to mp4:

```
ffmpeg -i input.h264 -c:v copy -f mp4 output.mp4
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
