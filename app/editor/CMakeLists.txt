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

