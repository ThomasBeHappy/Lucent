# Lucent

A Blender-like 3D editor with a Cycles-style progressive Vulkan ray-traced path tracer.

## Features (Planned)

- **Modern 3D Editor**: Scene outliner, viewport, gizmos, selection, transforms
- **Dual Viewport Modes**: Fast raster preview + progressive path-traced viewport
- **Vulkan Ray Tracing**: KHR ray tracing extensions for hardware-accelerated path tracing
- **Material Node Editor**: Node-based material system compiling to GPU shaders
- **High Performance**: GPU-first rendering, stable frame pacing, incremental scene updates

## Requirements

- **OS**: Windows 10/11 (64-bit)
- **GPU**: NVIDIA RTX 2000+ or AMD RX 6000+ (Vulkan 1.3 required, RT optional)
- **Vulkan SDK**: 1.3.x or later
- **Build Tools**: CMake 3.25+, MSVC 2022, vcpkg

## Quick Start (Windows Only)

> **Note**: This project currently targets Windows only. Build in Windows PowerShell, not WSL.

### Prerequisites

1. Visual Studio 2022 with C++ workload
2. [Vulkan SDK 1.3+](https://vulkan.lunarg.com/sdk/home)
3. [vcpkg](https://github.com/microsoft/vcpkg) with `VCPKG_ROOT` environment variable set

### Build

```powershell
# In Windows PowerShell (NOT WSL)

# Verify vcpkg is configured
echo $env:VCPKG_ROOT  # Should print your vcpkg path

# Configure and build
cmake --preset debug
cmake --build build/debug --config Debug

# Compile shaders
cd shaders
./compile_shaders.ps1
cd ..

# Run
./build/debug/bin/Debug/editor.exe
```

## Project Structure

```
lucent/
├── app/
│   └── editor/          # Main editor application
├── engine/
│   ├── core/            # Logging, assertions, utilities
│   ├── gfx/             # Vulkan abstraction layer
│   ├── rt/              # Ray tracing (Milestone F+)
│   ├── scene/           # ECS / scene graph (Milestone C+)
│   ├── assets/          # Asset pipeline (Milestone D+)
│   └── material/        # Node material system (Milestone L+)
├── shaders/             # GLSL shaders
├── docs/
│   └── milestones/      # Per-milestone documentation
└── tests/               # Unit tests
```

## Milestone Progress

- [x] **Milestone A**: Foundations (Boot to Triangle)
- [ ] **Milestone B**: Editor Shell (ImGui docking)
- [ ] **Milestone C**: Scene System + Gizmos
- [ ] **Milestone D**: Asset Pipeline v1 (glTF)
- [ ] **Milestone E**: PBR Raster Viewport
- [ ] **Milestone F**: Vulkan RT Core
- [ ] **Milestone G**: RT Scene Sync (BLAS/TLAS)
- [ ] **Milestone H**: Path Tracer v1

## Documentation

- [Milestone A: Foundations](docs/milestones/A_foundations.md)

## License

TBD

## Acknowledgments

- Vulkan SDK by LunarG
- GLFW for windowing
- ImGui for UI (upcoming)
- glm for math
- spdlog for logging

