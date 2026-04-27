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
- Vulkan runtime/loader:
  - Windows: Vulkan Runtime (`vulkan-1`)
  - Linux: system Vulkan loader/driver packages
  - macOS: MoltenVK (or the macOS Vulkan SDK)

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
- vcpkg (recommended; required for Windows H.264 support)

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

The `NANOVDB_EDITOR_USE_GLFW` option can be disabled when using the editor in headless and streaming mode only. In that case, `libvulkan.so.1` is built locally to ensure compatibility.

The `NANOVDB_EDITOR_USE_H264` option is enabled by default. Make sure you have:
```sh
sudo apt-get install make
```

#### macOS

```sh
# install homebrew from https://brew.sh
brew install cmake molten-vk
```

Notes:
- If the Vulkan loader does not discover MoltenVK automatically on your machine, point `VK_ICD_FILENAMES` and `VK_DRIVER_FILES` at `MoltenVK_icd.json`.

#### Windows
The project can use `vcpkg` for dependency management. If `NANOVDB_EDITOR_USE_VCPKG` is set, `vcpkg.json` automatically installs required dependencies.

`NANOVDB_EDITOR_USE_H264` is supported on Windows only when `NANOVDB_EDITOR_USE_VCPKG=ON`. In that configuration, CMake consumes the `openh264` package from `vcpkg` instead of the Unix-only source build path used on Linux.

To set up vcpkg:
```bat
git clone https://github.com/microsoft/vcpkg.git
cd vcpkg
bootstrap-vcpkg.bat
```

The following dependencies are automatically managed by `vcpkg.json`:
- libe57format (and xerces-c dependency) when `NANOVDB_EDITOR_E57_FORMAT=ON`
- openh264

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
- **-c**: Force CMake reconfigure (needed when switching build options like GLFW on/off)

Notes:
- If neither `-r` nor `-d` is provided (and not using `-p` or `-t`), the script defaults to a Release build.
- For Python builds (`-p`), `-d` selects Debug wheels; otherwise Release is used.
- CMake configure is skipped automatically when the build directory already exists. Use `-c` to force reconfigure when changing options (e.g., switching between GLFW enabled/disabled).

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

# Rebuild after changing options
./build.sh -r -c
```

#### Linux
```sh
./build.sh
```

#### Windows
Optionally, rename the config file `config` next to the build script to `config.ini` and set the environment variables (use unquoted values; `build.bat` passes them to CMake with quotes):
```
MSVS_VERSION=Visual Studio 17 2022
USE_VCPKG=ON
NANOVDB_EDITOR_E57_FORMAT=ON
VCPKG_ROOT=path/to/vcpkg
```

When `USE_VCPKG=ON`, `NANOVDB_EDITOR_USE_H264` defaults to `ON` on Windows. Without `vcpkg`, H.264 remains disabled on Windows because the fallback OpenH264 source build depends on Unix command-line tools.

Set `NANOVDB_EDITOR_E57_FORMAT=ON` to enable E57 support and install the optional `vcpkg` `e57` feature on Windows.

To select a different profile for the Slang compiler (https://github.com/shader-slang/slang/blob/master/source/slang/slang-profile-defs.h):
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

We keep the Vulkan headless FVDB viewer validated in both CI and local development with a shared integration suite and a consistent Docker image recipe.

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

# Test against the latest fvdb-core nightly (pip install --pre)
./scripts/run_fvdb_viz_integration.sh --fvdb-nightly
```

Highlights:
- Ensures (and caches) the `nanovdb-editor_fvdb-<fvdb-core-version>` Docker image with matching Torch/fvdb-core versions.
- Prints the installed `nanovdb_editor` version inside the container before running `pytests/test_fvdb_viz_integration.py -vv -s --full-trace`.
- Prints the available Vulkan ICDs plus `vulkaninfo --summary`, then fails fast if `fvdb.viz` cannot initialize instead of reporting a skipped upstream suite.
- Accepts `--force-rebuild` to bypass the local `.cache` tarball when you need a fresh base image.

### CI workflow

`.github/workflows/fvdb-viz-integration.yml` runs on `workflow_dispatch` or `workflow_call` and:
- Resolves the package stream (release/dev) plus optional wheel artifact.
- Builds a lightweight `ubuntu:24.04`-based Docker image with prebuilt fvdb-core wheels (pinned release or nightly via `pip install --pre`).
- Runs the same pytest selector in Docker.

`build-wheels.yml` triggers two parallel integration jobs when `run_fvdb_viz_integration` is enabled: one against the pinned fvdb-core release and one against the latest nightly.

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

Default values for shader parameters can be defined in the JSON file:
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
These parameters can be interactively changed with the generated UI in the editor's Params tab.

To display a group of shader parameters from different shaders, define a JSON file with various shader paths:
```json
{
    "ShaderParams": [
        "editor/editor.slang",
        "test/test.slang"
    ]
}
```

## Video Encoding To File

To convert the output to mp4:

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
