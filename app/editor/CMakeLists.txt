# Configure resource file with icon path
configure_file(
    ${CMAKE_CURRENT_SOURCE_DIR}/editor.rc.in
    ${CMAKE_CURRENT_BINARY_DIR}/editor.rc
    @ONLY
)

add_executable(editor
    src/main.cpp
    src/Application.cpp
    src/EditorUI.cpp
    src/SceneIO.cpp
    src/Win32FileDialogs.cpp
    src/EditorSettings.cpp
    src/MaterialGraphPanel.cpp
    src/UndoStack.cpp
    ${CMAKE_CURRENT_BINARY_DIR}/editor.rc
)

# Fix sporadic MSVC PDB contention (C1041) during parallel builds.
if(MSVC)
    target_compile_options(editor PRIVATE /FS)
endif()

# Add manifest for comctl32 v6.0 (required for LoadIconMetric)
if(WIN32)
    target_sources(editor PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/editor.manifest)
endif()

target_include_directories(editor
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)

find_package(imguizmo CONFIG REQUIRED)
find_package(unofficial-imgui-node-editor CONFIG REQUIRED)

target_link_libraries(editor
    PRIVATE
        Lucent::Core
        Lucent::Gfx
        Lucent::Scene
        Lucent::Assets
        Lucent::Material
        imgui::imgui
        imguizmo::imguizmo
        unofficial::imgui-node-editor::imgui-node-editor
)

if(WIN32)
    target_link_libraries(editor PRIVATE comctl32)
endif()

# Ensure shaders are compiled before building editor
add_dependencies(editor shaders)

# -----------------------------------------------------------------------------
# Editor icon pack (Font Awesome) - auto-download and copy into runtime Assets
# -----------------------------------------------------------------------------
option(LUCENT_EDITOR_DOWNLOAD_FONTAWESOME "Download Font Awesome TTF at build time (requires internet)" ON)

if(LUCENT_EDITOR_DOWNLOAD_FONTAWESOME)
    set(LUCENT_EDITOR_FONTS_OUT_DIR "${CMAKE_SOURCE_DIR}/Assets/Fonts")
    set(LUCENT_FA_SOLID_TTF "${LUCENT_EDITOR_FONTS_OUT_DIR}/fa-solid-900.ttf")

    # Source of the font file (from the Font Awesome Sass repo).
    # Folder reference: https://github.com/FortAwesome/font-awesome-sass/tree/main/assets/fonts/font-awesome
    set(LUCENT_FA_SOLID_URL "https://raw.githubusercontent.com/FortAwesome/font-awesome-sass/main/assets/fonts/font-awesome/fa-solid-900.ttf")

    add_custom_command(TARGET editor POST_BUILD
        COMMAND ${CMAKE_COMMAND} -DURL="${LUCENT_FA_SOLID_URL}" -DOUT="${LUCENT_FA_SOLID_TTF}"
                -P "${CMAKE_SOURCE_DIR}/cmake/DownloadFile.cmake"
        COMMENT "Ensuring Font Awesome icon font is available (fa-solid-900.ttf)"
    )

    # Also copy into the editor output directory for packaging/distribution.
    add_custom_command(TARGET editor POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:editor>/Assets/Fonts"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${LUCENT_FA_SOLID_TTF}"
                "$<TARGET_FILE_DIR:editor>/Assets/Fonts/fa-solid-900.ttf"
        COMMENT "Copying Font Awesome icon font to output Assets/Fonts"
    )
endif()

# -----------------------------------------------------------------------------
# Editor UI font (Roboto) - copy into output folder for packaging/distribution
# -----------------------------------------------------------------------------
set(LUCENT_EDITOR_FONTS_SRC_DIR "${CMAKE_SOURCE_DIR}/Assets/Fonts")
set(LUCENT_EDITOR_ROBOTO_TTF "${LUCENT_EDITOR_FONTS_SRC_DIR}/Roboto.ttf")

if(EXISTS "${LUCENT_EDITOR_ROBOTO_TTF}")
    add_custom_command(TARGET editor POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:editor>/Assets/Fonts"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${LUCENT_EDITOR_ROBOTO_TTF}"
                "$<TARGET_FILE_DIR:editor>/Assets/Fonts/Roboto.ttf"
        COMMENT "Copying Roboto.ttf to output Assets/Fonts"
    )
else()
    message(STATUS "Roboto.ttf not found at ${LUCENT_EDITOR_ROBOTO_TTF} (skipping copy)")
endif()

# Copy shaders to output directory
add_custom_command(TARGET editor POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        ${CMAKE_SOURCE_DIR}/shaders/compiled
        $<TARGET_FILE_DIR:editor>/shaders
    COMMENT "Copying compiled shaders to output directory"
)

set_target_properties(editor PROPERTIES
    VS_DEBUGGER_WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
)

