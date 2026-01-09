# Editor Workflow & UX

This guide explains how the Lucent editor works today, including panels, workflows, and how the UI is structured.

## Core Panels

- **Viewport**
  - Primary scene view and interaction surface.
  - Orbit, pan, zoom, and select entities.
  - Drag `.lmat` materials from the Content Browser onto meshes.
- **Outliner**
  - Hierarchical view of all entities in the scene.
  - Context menu for creating primitives and lights.
- **Inspector**
  - Per-entity component editor.
  - Supports transforms, mesh renderer settings, light and camera settings.
- **Content Browser**
  - Asset management for the `Assets/` workspace.
  - Search, create folders, drag-and-drop assets.
- **Material Graph**
  - Node editor for `.lmat` materials.
  - Compile status, save state, and material assignment.
- **Render Properties**
  - Render mode selection and quality tuning (samples, bounces, denoise, film).
- **Console**
  - Diagnostics and system messages.

## Layout & Docking

The editor uses ImGui docking. Panels can be rearranged or reset via:

- **View → Reset Layout**
- **View → Save Layout**

## Scene Workflow (Recommended)

1. **Create a scene** via `File → New Scene`.
2. **Add entities** in the Create menu or Outliner context menu.
3. **Transform entities** with gizmos (`W`, `E`, `R`).
4. **Assign materials** by dragging `.lmat` files onto meshes in the Viewport.
5. **Tune rendering** in Render Properties.
6. **Save** to a `.lucent` file.

## Selection Model

- **Single select:** Left-click.
- **Toggle select:** `Ctrl` + click.
- **Add select:** `Shift` + click.
- **Select all:** `Ctrl+A`.

## Keyboard Shortcuts (Quick Reference)

| Action | Shortcut |
| --- | --- |
| New Scene | `Ctrl+N` |
| Open Scene | `Ctrl+O` |
| Save Scene | `Ctrl+S` |
| Save Scene As | `Ctrl+Shift+S` |
| Undo / Redo | `Ctrl+Z` / `Ctrl+Y` |
| Duplicate | `Ctrl+D` |
| Delete | `Delete` |
| Focus | `F` |
| Move / Rotate / Scale | `W` / `E` / `R` |
| Mesh Select Mode | `1` / `2` / `3` |
| Extrude Faces | `E` |
| Inset Faces | `I` |
| Bevel Edges | `Ctrl+B` |
| Loop Cut | `Ctrl+R` |
| Select Edge Loop | `Alt+L` |
| Select Edge Ring | `Alt+R` |
| Dissolve | `Alt+X` |
| Subdivide Faces | `Ctrl+Shift+R` |
| Shrink Selection | `Ctrl+-` |
