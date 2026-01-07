# Architecture Overview

Lucent is split into two major layers: the **editor app** (UI + tooling) and the **engine** (rendering, scene, and asset systems). The project is organized to keep editor concerns isolated while sharing engine modules across future tools.

## Top-Level Layout

```
app/        # Editor UI and interaction layer
engine/     # Core engine systems
shaders/    # GLSL + shader build scripts
assets/     # Runtime asset workspace (created at runtime)
docs/       # Documentation and milestones
```

## Editor App (app/editor)

**Primary responsibilities**
- Windowing + input (GLFW).
- Docking UI (ImGui + ImGuizmo).
- Scene editing workflow and user interactions.
- File dialogs, scene I/O, and content browsing.

**Key modules**
- `app/editor/src/Application.cpp`
  - Owns the main loop, Vulkan context, device, renderer, UI, and scene.
  - Initializes demo content, hooks editor camera, and passes render data.
- `app/editor/src/EditorUI.cpp`
  - Builds the dockspace, menus, panels, and modal dialogs.
  - Manages editor state (selection, gizmos, content browser, render settings UI).
- `app/editor/src/SceneIO.cpp`
  - Reads/writes `.lucent` scene files.
- `app/editor/src/MaterialGraphPanel.cpp`
  - Node editor UI for `.lmat` materials, compilation status, and editing.

## Engine (engine/)

The engine layer is organized into focused modules with minimal UI dependencies.

- `engine/gfx/`
  - Vulkan context/device management.
  - Swapchain, renderer, render settings, and render modes.
  - Final presentation path and feature capability detection.
- `engine/scene/`
  - ECS-style scene representation (entities + components).
  - Transform, camera, light, mesh renderer components.
- `engine/material/`
  - Material graph definition, compiler, asset management.
  - `.lmat` serialization and compilation into Vulkan pipelines.
- `engine/assets/`
  - Asset helpers and primitive mesh generation.
- `engine/core/`
  - Logging, assertions, and shared utilities.

## Data Flow (High Level)

1. **Editor UI** captures user intent (menus, gizmos, selection changes).
2. **Scene** is updated (entities/components/transform).
3. **Renderer** consumes the scene + render settings each frame.
4. **Material system** compiles graph changes into GPU pipelines.
5. **Output** is presented in the viewport and synchronized to UI panels.

## Extension Points

- **New panels:** Implement in `EditorUI.cpp`, add to the View menu and dock layout.
- **New components:** Add to `engine/scene`, then expose in the Inspector panel.
- **Render modes:** Extend `engine/gfx` and surface controls in Render Properties.
- **Material nodes:** Add node definitions and compiler rules in `engine/material`.
