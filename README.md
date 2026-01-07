# Lucent

**Lucent** is an early-stage 3D editor built on Vulkan, with a modern docking UI, realtime shading, and a progressive ray/path tracing pipeline in development. It targets creators who want a fast, GPU-first workspace for scene layout, materials, and lighting.

> **Status:** Alpha (Windows-only). Expect rapid iteration and breaking changes.

## Highlights

- **Docking editor UI** with Viewport, Outliner, Inspector, Content Browser, Console, Render Properties, and Material Graph panels.
- **Scene authoring** with primitives, cameras, and lights, plus selection and gizmo-based transforms.
- **Material graph editor** with on-demand compilation and asset management.
- **Progressive rendering modes** (Simple, Traced, Ray Traced) with sampling, denoising, and film controls.
- **Asset pipeline** with an `Assets/` workspace, import tools, and `.lucent` scene files.

## Current Progress

**Ready today**
- Docking editor shell and core panels.
- Primitive creation (cube, sphere, plane, cylinder, cone) and basic lighting (point, directional, spot).
- Scene save/load to the `.lucent` format.
- Content Browser with search, folder tools, and drag/drop asset workflows.
- Material Graph with `.lmat` assets stored in `Assets/materials`.
- Render Properties for sampling, denoise, exposure, tonemapping, and gamma.

**In flight**
- Production-ready ray/path tracing workflows and higher-level asset import.
- Expanded node library, material authoring, and external format interoperability.

## Requirements

- **OS:** Windows 10/11 (64-bit)
- **GPU:** Vulkan 1.3 capable GPU (RTX 2000+ / RX 6000+ recommended for ray tracing)
- **Vulkan SDK:** 1.3.x or later
- **Build Tools:** CMake 3.25+, MSVC 2022, vcpkg

## Quick Start (Windows)

> Build in Windows PowerShell (not WSL).

```powershell
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

## App Documentation

### Workspace Overview

- **Viewport:** The realtime preview where you orbit, select, and manipulate entities.
- **Outliner:** Hierarchical list of scene entities. Right-click for create/duplicate/delete.
- **Inspector:** Edit transforms, mesh renderer, light, and camera components.
- **Content Browser:** Manage `Assets/` (search, folders, drag/drop).
- **Material Graph:** Node editor for `.lmat` materials with compile status.
- **Render Properties:** Configure render modes, samples, bounces, denoise, and film settings.
- **Console:** Diagnostics and status messages.

Open panels from **View → Panels** and reset the layout from **View → Reset Layout**.

### Navigation & Selection

- **Orbit:** Right-click + drag
- **Pan:** Middle-click + drag
- **Zoom:** Scroll wheel
- **Focus selection:** `F`
- **Select:** Left-click (use `Ctrl`/`Shift` for multi-select)

### Transform Tools

- **Move:** `W`
- **Rotate:** `E`
- **Scale:** `R`
- **Toggle snapping:** Hold `Ctrl`

### Scene Workflow

1. **Create entities:** Use **Create** menu or Outliner context menu.
2. **Edit transforms and components:** Use the Inspector panel.
3. **Assign materials:** Drag a `.lmat` material from the Content Browser onto a mesh in the Viewport.
4. **Save/Open scenes:** `Ctrl+S` / `Ctrl+O` to work with `.lucent` files.

### Assets & File Formats

- **Scenes:** `.lucent` files (Lucent scene format).
- **Materials:** `.lmat` files stored in `Assets/materials`.
- **Imported assets:** Use **File → Import** to copy images or `.obj` files into `Assets/`.

### Rendering Controls

Use **Render Properties** to switch modes and tune quality:

- **Simple:** Fast raster preview.
- **Traced / Ray Traced:** Progressive modes with sample accumulation (GPU dependent).
- **Sampling:** Viewport samples, final samples, half-resolution.
- **Denoise:** Box/Edge-aware and optional OptiX (when available).
- **Film:** Exposure, tonemapping, and gamma.

### Keyboard Shortcuts (Quick Reference)

| Action | Shortcut |
| --- | --- |
| New Scene | `Ctrl+N` |
| Open Scene | `Ctrl+O` |
| Save Scene | `Ctrl+S` |
| Save Scene As | `Ctrl+Shift+S` |
| Duplicate | `Ctrl+D` |
| Undo / Redo | `Ctrl+Z` / `Ctrl+Y` |
| Delete | `Delete` |
| Select All | `Ctrl+A` |

## Project Structure

```
app/            # Editor application
engine/         # Core engine (gfx, scene, material, assets)
shaders/        # GLSL shaders
docs/           # Developer documentation
```

## Documentation

- [Milestone A: Foundations](docs/milestones/A_foundations.md)

## License

TBD

## Acknowledgments

- Vulkan SDK by LunarG
- GLFW for windowing
- ImGui + ImGuizmo for UI
- glm for math
- spdlog for logging
