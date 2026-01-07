## Icon pack (ImGui) for Lucent Editor

Lucent’s editor UI is built on ImGui. The cleanest way to add a modern icon pack is to **merge an icon font** into the main UI font.

### How it works
- `EditorUI::SetupFonts()` loads a base UI font (optional)
- Then it optionally loads an icon font **in merge mode**, so icons can be used inline in labels (e.g. `"\xEF\x82\x93 Import"`).

### Where to put the files
At runtime, place fonts in:

- `Assets/Fonts/Inter-Regular.ttf` (optional UI font)
- `Assets/Fonts/fa-solid-900.ttf` (optional icon font; Font Awesome solid)

If these files aren’t present, the editor falls back to ImGui default font and icons are simply not shown.

### Auto-download (CMake)
If `LUCENT_EDITOR_DOWNLOAD_FONTAWESOME=ON` (default), the build will download `fa-solid-900.ttf` from the Font Awesome Sass repo and copy it into:
- `${projectRoot}/Assets/Fonts/` (for VS debugger working dir)
- `${editorExeDir}/Assets/Fonts/` (for packaged builds)

Source reference: [Font Awesome Sass fonts folder](https://github.com/FortAwesome/font-awesome-sass/tree/main/assets/fonts/font-awesome)

### Using icons in the UI
See `app/editor/include/EditorIcons.h` for a few starter icons.

Example usage:
- Use icons only when the icon font loaded: `m_IconFontLoaded ? (LUCENT_ICON_IMPORT " Import") : "Import"`

### Adding more icons
If you use Font Awesome:
- Find the icon’s Unicode (e.g. `f002`) and convert it to UTF‑8 bytes.
- Add a `#define` in `EditorIcons.h`.

If you use a different icon font (Material Symbols, etc.):
- Update the codepoint ranges in `EditorUI::SetupFonts()` and add matching constants.


