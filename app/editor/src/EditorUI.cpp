#include "EditorUI.h"
#include "SceneIO.h"
#include "Win32FileDialogs.h"
#include "EditorSettings.h"
#include "UndoStack.h"
#include "lucent/gfx/VulkanContext.h"
#include "lucent/gfx/Device.h"
#include "lucent/gfx/Renderer.h"
#include "lucent/scene/Components.h"
#include "lucent/material/MaterialAsset.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include <shellapi.h>

#include <imgui.h>
#include <imgui_internal.h>
#include <imgui_impl_glfw.h>
#include <imgui_impl_vulkan.h>
#include <ImGuizmo.h>
#include <GLFW/glfw3.h>

#include <glm/gtc/type_ptr.hpp>
#include <glm/gtc/matrix_transform.hpp>

#include <filesystem>
#include <algorithm>
#include <cmath>

namespace lucent {

namespace {

static bool NearlyEqualVec3(const glm::vec3& a, const glm::vec3& b, float eps) {
    glm::vec3 d = a - b;
    return (d.x * d.x + d.y * d.y + d.z * d.z) <= (eps * eps);
}

static bool NearlyEqualTransform(const glm::vec3& posA, const glm::vec3& rotA, const glm::vec3& scaleA,
                                 const glm::vec3& posB, const glm::vec3& rotB, const glm::vec3& scaleB) {
    // Translation/scale are in world/local units. Rotation is in degrees.
    // Use slightly looser epsilon for rotation to avoid constant dirty from decomposition jitter.
    const float posEps = 1e-4f;
    const float rotEps = 1e-3f;
    const float scaleEps = 1e-4f;

    if (!NearlyEqualVec3(posA, posB, posEps)) return false;
    if (!NearlyEqualVec3(rotA, rotB, rotEps)) return false;
    if (!NearlyEqualVec3(scaleA, scaleB, scaleEps)) return false;
    return true;
}

} // namespace

EditorUI::~EditorUI() {
    Shutdown();
}

bool EditorUI::Init(GLFWwindow* window, gfx::VulkanContext* context, gfx::Device* device, gfx::Renderer* renderer) {
    m_Window = window;
    m_Context = context;
    m_Device = device;
    m_Renderer = renderer;
    
    // Create descriptor pool for ImGui
    VkDescriptorPoolSize poolSizes[] = {
        { VK_DESCRIPTOR_TYPE_SAMPLER, 1000 },
        { VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1000 },
        { VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1000 },
        { VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1000 },
        { VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER, 1000 },
        { VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER, 1000 },
        { VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1000 },
        { VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1000 },
        { VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, 1000 },
        { VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC, 1000 },
        { VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT, 1000 }
    };
    
    VkDescriptorPoolCreateInfo poolInfo{};
    poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    poolInfo.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
    poolInfo.maxSets = 1000;
    poolInfo.poolSizeCount = static_cast<uint32_t>(std::size(poolSizes));
    poolInfo.pPoolSizes = poolSizes;
    
    if (vkCreateDescriptorPool(context->GetDevice(), &poolInfo, nullptr, &m_ImGuiPool) != VK_SUCCESS) {
        LUCENT_CORE_ERROR("Failed to create ImGui descriptor pool");
        return false;
    }
    
    // Initialize ImGui
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;
    
    // Disable the ID conflict popup in debug builds (we log instead)
    // This is available in ImGui 1.91+
    io.ConfigDebugHighlightIdConflicts = true;
    
    // Set ini file path
    io.IniFilename = nullptr; // We'll handle saving manually
    
    SetupStyle();
    
    // Initialize platform/renderer backends
    ImGui_ImplGlfw_InitForVulkan(window, true);
    
    ImGui_ImplVulkan_InitInfo initInfo{};
    initInfo.Instance = context->GetInstance();
    initInfo.PhysicalDevice = context->GetPhysicalDevice();
    initInfo.Device = context->GetDevice();
    initInfo.QueueFamily = context->GetQueueFamilies().graphics;
    initInfo.Queue = context->GetGraphicsQueue();
    initInfo.PipelineCache = VK_NULL_HANDLE;
    initInfo.DescriptorPool = m_ImGuiPool;
    initInfo.Subpass = 0;
    initInfo.MinImageCount = 2;
    initInfo.ImageCount = renderer->GetSwapchain()->GetImageCount();
    initInfo.MSAASamples = VK_SAMPLE_COUNT_1_BIT;
    
    // Check if we should use dynamic rendering (Vulkan 1.3) or legacy render pass
    bool useDynamicRendering = renderer->UseDynamicRendering();
    VkFormat swapchainFormat = renderer->GetSwapchain()->GetFormat();
    
    if (useDynamicRendering) {
    initInfo.UseDynamicRendering = true;
    initInfo.PipelineRenderingCreateInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO;
    initInfo.PipelineRenderingCreateInfo.colorAttachmentCount = 1;
    initInfo.PipelineRenderingCreateInfo.pColorAttachmentFormats = &swapchainFormat;
    } else {
        initInfo.UseDynamicRendering = false;
        initInfo.RenderPass = renderer->GetSwapchainRenderPass();
    }
    
    ImGui_ImplVulkan_Init(&initInfo);
    
    // Load layout if exists
    LoadLayout();
    
    // Initialize material graph panel
    m_MaterialGraphPanel.Init(device);
    
    // Set up callback for navigating to assets from material graph
    m_MaterialGraphPanel.SetNavigateToAssetCallback([this](const std::string& path) {
        NavigateToAsset(path);
    });
    
    LUCENT_CORE_INFO("ImGui initialized with docking support");
    return true;
}

void EditorUI::Shutdown() {
    if (!m_Context) return;
    
    m_Context->WaitIdle();
    
    // Shutdown material graph panel
    m_MaterialGraphPanel.Shutdown();
    
    SaveLayout();
    
    ImGui_ImplVulkan_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    ImGui::DestroyContext();
    
    if (m_ImGuiPool != VK_NULL_HANDLE) {
        vkDestroyDescriptorPool(m_Context->GetDevice(), m_ImGuiPool, nullptr);
        m_ImGuiPool = VK_NULL_HANDLE;
    }
    
    m_Context = nullptr;
}

void EditorUI::SetupStyle() {
    ImGuiStyle& style = ImGui::GetStyle();
    ImVec4* colors = style.Colors;
    
    // ========================================================================
    // Modern Dark Theme - Inspired by VS Code Dark+ / Figma
    // ========================================================================
    
    // Base colors
    const ImVec4 bg_dark      = ImVec4(0.067f, 0.067f, 0.078f, 1.0f);  // #111114
    const ImVec4 bg_main      = ImVec4(0.098f, 0.098f, 0.118f, 1.0f);  // #19191E
    const ImVec4 bg_light     = ImVec4(0.137f, 0.137f, 0.157f, 1.0f);  // #232328
    const ImVec4 bg_lighter   = ImVec4(0.176f, 0.176f, 0.196f, 1.0f);  // #2D2D32
    const ImVec4 border       = ImVec4(0.216f, 0.216f, 0.235f, 1.0f);  // #37373C
    
    // Text colors
    const ImVec4 text_bright  = ImVec4(0.95f, 0.95f, 0.95f, 1.0f);
    const ImVec4 text_normal  = ImVec4(0.78f, 0.78f, 0.80f, 1.0f);
    const ImVec4 text_dim     = ImVec4(0.50f, 0.50f, 0.55f, 1.0f);
    
    // Accent color - Electric Cyan/Teal
    const ImVec4 accent       = ImVec4(0.25f, 0.78f, 0.85f, 1.0f);     // #40C7D9
    const ImVec4 accent_hover = ImVec4(0.35f, 0.85f, 0.92f, 1.0f);
    const ImVec4 accent_dim   = ImVec4(0.18f, 0.55f, 0.60f, 1.0f);
    
    // Secondary accent - Warm coral for warnings/selection
    const ImVec4 highlight    = ImVec4(0.94f, 0.42f, 0.42f, 1.0f);     // #F06B6B
    
    // Success/positive
    const ImVec4 success      = ImVec4(0.35f, 0.78f, 0.47f, 1.0f);     // #5AC778
    
    // Backgrounds
    colors[ImGuiCol_WindowBg]             = bg_main;
    colors[ImGuiCol_ChildBg]              = ImVec4(0.0f, 0.0f, 0.0f, 0.0f);
    colors[ImGuiCol_PopupBg]              = ImVec4(bg_light.x, bg_light.y, bg_light.z, 0.98f);
    colors[ImGuiCol_Border]               = border;
    colors[ImGuiCol_BorderShadow]         = ImVec4(0.0f, 0.0f, 0.0f, 0.0f);
    
    // Text
    colors[ImGuiCol_Text]                 = text_normal;
    colors[ImGuiCol_TextDisabled]         = text_dim;
    
    // Headers (collapsing headers, tree nodes)
    colors[ImGuiCol_Header]               = bg_lighter;
    colors[ImGuiCol_HeaderHovered]        = ImVec4(accent.x, accent.y, accent.z, 0.25f);
    colors[ImGuiCol_HeaderActive]         = ImVec4(accent.x, accent.y, accent.z, 0.35f);
    
    // Buttons
    colors[ImGuiCol_Button]               = bg_lighter;
    colors[ImGuiCol_ButtonHovered]        = ImVec4(accent.x, accent.y, accent.z, 0.65f);
    colors[ImGuiCol_ButtonActive]         = accent;
    
    // Frame backgrounds (input fields, checkboxes)
    colors[ImGuiCol_FrameBg]              = bg_dark;
    colors[ImGuiCol_FrameBgHovered]       = bg_light;
    colors[ImGuiCol_FrameBgActive]        = bg_lighter;
    
    // Tabs
    colors[ImGuiCol_Tab]                  = bg_light;
    colors[ImGuiCol_TabHovered]           = ImVec4(accent.x, accent.y, accent.z, 0.5f);
    colors[ImGuiCol_TabActive]            = ImVec4(accent.x, accent.y, accent.z, 0.3f);
    colors[ImGuiCol_TabUnfocused]         = bg_dark;
    colors[ImGuiCol_TabUnfocusedActive]   = bg_light;
    
    // Title bars
    colors[ImGuiCol_TitleBg]              = bg_dark;
    colors[ImGuiCol_TitleBgActive]        = bg_light;
    colors[ImGuiCol_TitleBgCollapsed]     = bg_dark;
    
    // Scrollbar
    colors[ImGuiCol_ScrollbarBg]          = bg_dark;
    colors[ImGuiCol_ScrollbarGrab]        = bg_lighter;
    colors[ImGuiCol_ScrollbarGrabHovered] = ImVec4(accent.x, accent.y, accent.z, 0.5f);
    colors[ImGuiCol_ScrollbarGrabActive]  = accent;
    
    // Slider
    colors[ImGuiCol_SliderGrab]           = accent;
    colors[ImGuiCol_SliderGrabActive]     = accent_hover;
    
    // Checkmarks and selection
    colors[ImGuiCol_CheckMark]            = accent;
    
    // Separators
    colors[ImGuiCol_Separator]            = border;
    colors[ImGuiCol_SeparatorHovered]     = accent;
    colors[ImGuiCol_SeparatorActive]      = accent_hover;
    
    // Resize grips
    colors[ImGuiCol_ResizeGrip]           = ImVec4(accent.x, accent.y, accent.z, 0.15f);
    colors[ImGuiCol_ResizeGripHovered]    = ImVec4(accent.x, accent.y, accent.z, 0.5f);
    colors[ImGuiCol_ResizeGripActive]     = accent;
    
    // Docking
    colors[ImGuiCol_DockingPreview]       = ImVec4(accent.x, accent.y, accent.z, 0.5f);
    colors[ImGuiCol_DockingEmptyBg]       = bg_dark;
    
    // Menu bar
    colors[ImGuiCol_MenuBarBg]            = bg_dark;
    
    // Tables
    colors[ImGuiCol_TableHeaderBg]        = bg_lighter;
    colors[ImGuiCol_TableBorderStrong]    = border;
    colors[ImGuiCol_TableBorderLight]     = ImVec4(border.x, border.y, border.z, 0.5f);
    colors[ImGuiCol_TableRowBg]           = ImVec4(0.0f, 0.0f, 0.0f, 0.0f);
    colors[ImGuiCol_TableRowBgAlt]        = ImVec4(1.0f, 1.0f, 1.0f, 0.02f);
    
    // Text selection
    colors[ImGuiCol_TextSelectedBg]       = ImVec4(accent.x, accent.y, accent.z, 0.35f);
    
    // Drag/drop
    colors[ImGuiCol_DragDropTarget]       = accent;
    
    // Nav highlight
    colors[ImGuiCol_NavHighlight]         = accent;
    colors[ImGuiCol_NavWindowingHighlight]= ImVec4(1.0f, 1.0f, 1.0f, 0.7f);
    colors[ImGuiCol_NavWindowingDimBg]    = ImVec4(0.8f, 0.8f, 0.8f, 0.2f);
    
    // Modal dim
    colors[ImGuiCol_ModalWindowDimBg]     = ImVec4(0.0f, 0.0f, 0.0f, 0.6f);
    
    // Plot
    colors[ImGuiCol_PlotLines]            = accent;
    colors[ImGuiCol_PlotLinesHovered]     = highlight;
    colors[ImGuiCol_PlotHistogram]        = accent;
    colors[ImGuiCol_PlotHistogramHovered] = highlight;
    
    // ========================================================================
    // Style Settings - Modern, rounded, spacious
    // ========================================================================
    
    // Rounding - more rounded for modern feel
    style.WindowRounding    = 8.0f;
    style.ChildRounding     = 6.0f;
    style.FrameRounding     = 6.0f;
    style.PopupRounding     = 6.0f;
    style.ScrollbarRounding = 12.0f;
    style.GrabRounding      = 6.0f;
    style.TabRounding       = 6.0f;
    
    // Padding and spacing - more spacious
    style.WindowPadding     = ImVec2(12, 12);
    style.FramePadding      = ImVec2(10, 6);
    style.CellPadding       = ImVec2(8, 4);
    style.ItemSpacing       = ImVec2(10, 6);
    style.ItemInnerSpacing  = ImVec2(6, 4);
    style.TouchExtraPadding = ImVec2(0, 0);
    style.IndentSpacing     = 20.0f;
    style.ScrollbarSize     = 14.0f;
    style.GrabMinSize       = 12.0f;
    
    // Borders
    style.WindowBorderSize  = 1.0f;
    style.ChildBorderSize   = 1.0f;
    style.PopupBorderSize   = 1.0f;
    style.FrameBorderSize   = 0.0f;
    style.TabBorderSize     = 0.0f;
    
    // Alignment
    style.WindowTitleAlign  = ImVec2(0.5f, 0.5f); // Center titles
    style.WindowMenuButtonPosition = ImGuiDir_None; // Hide menu button
    style.ColorButtonPosition = ImGuiDir_Right;
    style.ButtonTextAlign   = ImVec2(0.5f, 0.5f);
    style.SelectableTextAlign = ImVec2(0.0f, 0.5f);
    
    // Anti-aliasing
    style.AntiAliasedLines  = true;
    style.AntiAliasedFill   = true;
    
    // Misc
    style.WindowMinSize     = ImVec2(100, 100);
    style.DisplaySafeAreaPadding = ImVec2(3, 3);
}

void EditorUI::BeginFrame() {
    ImGui_ImplVulkan_NewFrame();
    ImGui_ImplGlfw_NewFrame();
    ImGui::NewFrame();
    ImGuizmo::BeginFrame();
    
    // Handle global keyboard shortcuts
    HandleGlobalShortcuts();
    
    DrawDockspace();
    
    if (m_ShowViewport) DrawViewportPanel();
    if (m_ShowOutliner) DrawOutlinerPanel();
    if (m_ShowInspector) DrawInspectorPanel();
    if (m_ShowContentBrowser) DrawContentBrowserPanel();
    if (m_ShowConsole) DrawConsolePanel();
    if (m_ShowRenderProperties) DrawRenderPropertiesPanel();
    
    // Draw material graph panel
    m_MaterialGraphPanel.Draw();
    
    // Draw modals
    DrawModals();
}

void EditorUI::EndFrame() {
    ImGui::Render();
    
    // Handle multi-viewport if enabled
    ImGuiIO& io = ImGui::GetIO();
    if (io.ConfigFlags & ImGuiConfigFlags_ViewportsEnable) {
        ImGui::UpdatePlatformWindows();
        ImGui::RenderPlatformWindowsDefault();
    }
}

void EditorUI::Render(VkCommandBuffer cmd) {
    ImGui_ImplVulkan_RenderDrawData(ImGui::GetDrawData(), cmd);
}

void EditorUI::SetViewportTexture(VkImageView view, VkSampler sampler) {
    // Remove old descriptor if exists
    if (m_ViewportDescriptor != VK_NULL_HANDLE) {
        // ImGui will handle cleanup
    }
    
    // Create new descriptor for the viewport texture
    m_ViewportDescriptor = ImGui_ImplVulkan_AddTexture(sampler, view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
}

void EditorUI::SaveLayout() {
    ImGui::SaveIniSettingsToDisk(m_LayoutPath.c_str());
    LUCENT_CORE_DEBUG("Layout saved to {}", m_LayoutPath);
}

void EditorUI::LoadLayout() {
    if (std::filesystem::exists(m_LayoutPath)) {
        ImGui::LoadIniSettingsFromDisk(m_LayoutPath.c_str());
        LUCENT_CORE_DEBUG("Layout loaded from {}", m_LayoutPath);
    }
}

void EditorUI::DrawDockspace() {
    // Fullscreen dockspace
    ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(viewport->WorkPos);
    ImGui::SetNextWindowSize(viewport->WorkSize);
    ImGui::SetNextWindowViewport(viewport->ID);
    
    ImGuiWindowFlags windowFlags = 
        ImGuiWindowFlags_NoDocking |
        ImGuiWindowFlags_NoTitleBar |
        ImGuiWindowFlags_NoCollapse |
        ImGuiWindowFlags_NoResize |
        ImGuiWindowFlags_NoMove |
        ImGuiWindowFlags_NoBringToFrontOnFocus |
        ImGuiWindowFlags_NoNavFocus |
        ImGuiWindowFlags_MenuBar;
    
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
    
    ImGui::Begin("DockSpace", nullptr, windowFlags);
    ImGui::PopStyleVar(3);
    
    // Menu bar
    if (ImGui::BeginMenuBar()) {
        // Logo / Brand
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.25f, 0.78f, 0.85f, 1.0f));
        ImGui::Text("LUCENT");
        ImGui::PopStyleColor();
        ImGui::Separator();
        
        if (ImGui::BeginMenu("File")) {
            if (ImGui::MenuItem("New Scene", "Ctrl+N")) {
                // Check for unsaved changes
                bool proceed = true;
                if (m_SceneDirty) {
                    auto result = Win32FileDialogs::ShowYesNoCancel(L"Unsaved Changes", 
                        L"Save changes before creating a new scene?");
                    if (result == Win32FileDialogs::MsgBoxResult::Yes) {
                        // Save first
                        if (m_CurrentScenePath.empty()) {
                            std::string path = Win32FileDialogs::SaveFile(L"Save Scene", 
                                {{L"Lucent Scene", L"*.lucent"}}, L"lucent");
                            if (!path.empty()) {
                                SceneIO::SaveScene(m_Scene, path);
                                m_CurrentScenePath = path;
                            }
                        } else {
                            SceneIO::SaveScene(m_Scene, m_CurrentScenePath);
                        }
                    } else if (result == Win32FileDialogs::MsgBoxResult::Cancel) {
                        proceed = false;
                    }
                }
                if (proceed && m_Scene) {
                    m_Scene->Clear();
                    m_Scene->SetName("New Scene");
                    // Create default camera and light
                    auto camera = m_Scene->CreateEntity("Main Camera");
                    camera.AddComponent<scene::CameraComponent>();
                    auto light = m_Scene->CreateEntity("Directional Light");
                    auto& l = light.AddComponent<scene::LightComponent>();
                    l.type = scene::LightType::Directional;
                    
                    ClearSelection();
                    m_CurrentScenePath.clear();
                    m_SceneDirty = false;
                }
            }
            if (ImGui::MenuItem("Open Scene...", "Ctrl+O")) {
                bool proceed = true;
                if (m_SceneDirty) {
                    auto result = Win32FileDialogs::ShowYesNoCancel(L"Unsaved Changes", 
                        L"Save changes before opening another scene?");
                    if (result == Win32FileDialogs::MsgBoxResult::Yes) {
                        if (!m_CurrentScenePath.empty()) {
                            SceneIO::SaveScene(m_Scene, m_CurrentScenePath);
                        } else {
                            std::string path = Win32FileDialogs::SaveFile(L"Save Scene", 
                                {{L"Lucent Scene", L"*.lucent"}}, L"lucent");
                            if (!path.empty()) {
                                SceneIO::SaveScene(m_Scene, path);
                            }
                        }
                    } else if (result == Win32FileDialogs::MsgBoxResult::Cancel) {
                        proceed = false;
                    }
                }
                if (proceed) {
                    std::string path = Win32FileDialogs::OpenFile(L"Open Scene", 
                        {{L"Lucent Scene", L"*.lucent"}, {L"All Files", L"*.*"}}, L"lucent");
                    if (!path.empty() && m_Scene) {
                        if (SceneIO::LoadScene(m_Scene, path)) {
                            m_CurrentScenePath = path;
                            m_SceneDirty = false;
                            ClearSelection();
                        } else {
                            Win32FileDialogs::ShowError(L"Error", L"Failed to load scene file.");
                        }
                    }
                }
            }
            ImGui::Separator();
            if (ImGui::MenuItem("Save Scene", "Ctrl+S")) {
                if (m_Scene) {
                    if (m_CurrentScenePath.empty()) {
                        std::string path = Win32FileDialogs::SaveFile(L"Save Scene", 
                            {{L"Lucent Scene", L"*.lucent"}}, L"lucent");
                        if (!path.empty()) {
                            SceneIO::SaveScene(m_Scene, path);
                            m_CurrentScenePath = path;
                            m_SceneDirty = false;
                        }
                    } else {
                        SceneIO::SaveScene(m_Scene, m_CurrentScenePath);
                        m_SceneDirty = false;
                    }
                }
            }
            if (ImGui::MenuItem("Save Scene As...", "Ctrl+Shift+S")) {
                if (m_Scene) {
                    std::string path = Win32FileDialogs::SaveFile(L"Save Scene As", 
                        {{L"Lucent Scene", L"*.lucent"}}, L"lucent");
                    if (!path.empty()) {
                        SceneIO::SaveScene(m_Scene, path);
                        m_CurrentScenePath = path;
                        m_SceneDirty = false;
                    }
                }
            }
            ImGui::Separator();
            if (ImGui::MenuItem("Import...")) {
                std::string path = Win32FileDialogs::OpenFile(L"Import Asset", 
                    {{L"All Supported", L"*.png;*.jpg;*.hdr;*.obj"}, 
                     {L"Images", L"*.png;*.jpg;*.hdr"},
                     {L"Models", L"*.obj"},
                     {L"All Files", L"*.*"}});
                if (!path.empty()) {
                    // Copy to Assets folder
                    std::filesystem::path src(path);
                    std::filesystem::path dest = std::filesystem::current_path() / "Assets" / src.filename();
                    std::filesystem::create_directories(dest.parent_path());
                    try {
                        std::filesystem::copy_file(src, dest, std::filesystem::copy_options::overwrite_existing);
                        LUCENT_CORE_INFO("Imported asset to: {}", dest.string());
                    } catch (const std::exception& e) {
                        LUCENT_CORE_ERROR("Failed to import: {}", e.what());
                    }
                }
            }
            if (ImGui::BeginMenu("Export")) {
                if (ImGui::MenuItem("Scene (.lucent)...")) {
                    if (m_Scene) {
                        std::string path = Win32FileDialogs::SaveFile(L"Export Scene", 
                            {{L"Lucent Scene", L"*.lucent"}}, L"lucent");
                        if (!path.empty()) {
                            SceneIO::SaveScene(m_Scene, path);
                        }
                    }
                }
                ImGui::EndMenu();
            }
            ImGui::Separator();
            if (ImGui::MenuItem("Exit", "Alt+F4")) {
                bool proceed = true;
                if (m_SceneDirty) {
                    auto result = Win32FileDialogs::ShowYesNoCancel(L"Unsaved Changes", 
                        L"Save changes before exiting?");
                    if (result == Win32FileDialogs::MsgBoxResult::Yes) {
                        if (!m_CurrentScenePath.empty()) {
                            SceneIO::SaveScene(m_Scene, m_CurrentScenePath);
                        } else {
                            std::string path = Win32FileDialogs::SaveFile(L"Save Scene", 
                                {{L"Lucent Scene", L"*.lucent"}}, L"lucent");
                            if (!path.empty()) {
                                SceneIO::SaveScene(m_Scene, path);
                            }
                        }
                    } else if (result == Win32FileDialogs::MsgBoxResult::Cancel) {
                        proceed = false;
                    }
                }
                if (proceed) {
                glfwSetWindowShouldClose(m_Window, GLFW_TRUE);
                }
            }
            ImGui::EndMenu();
        }
        
        if (ImGui::BeginMenu("Edit")) {
            auto& undoStack = UndoStack::Get();
            std::string undoLabel = undoStack.CanUndo() ? 
                "Undo " + undoStack.GetUndoDescription() : "Undo";
            std::string redoLabel = undoStack.CanRedo() ? 
                "Redo " + undoStack.GetRedoDescription() : "Redo";
            
            if (ImGui::MenuItem(undoLabel.c_str(), "Ctrl+Z", false, undoStack.CanUndo())) {
                undoStack.Undo();
            }
            if (ImGui::MenuItem(redoLabel.c_str(), "Ctrl+Y", false, undoStack.CanRedo())) {
                undoStack.Redo();
            }
            ImGui::Separator();
            if (ImGui::MenuItem("Cut", "Ctrl+X", false, !m_SelectedEntities.empty())) {
                // Copy then delete
                // TODO: Implement clipboard
                for (auto id : m_SelectedEntities) {
                    if (m_Scene) {
                        m_Scene->DestroyEntity(m_Scene->GetEntity(id));
                    }
                }
                ClearSelection();
                m_SceneDirty = true;
            }
            if (ImGui::MenuItem("Copy", "Ctrl+C", false, !m_SelectedEntities.empty())) {
                // TODO: Implement clipboard
            }
            if (ImGui::MenuItem("Paste", "Ctrl+V", false, false)) {
                // TODO: Implement paste from clipboard
            }
            if (ImGui::MenuItem("Duplicate", "Ctrl+D", false, !m_SelectedEntities.empty())) {
                if (m_Scene) {
                    std::vector<scene::Entity> newEntities;
                    for (auto id : m_SelectedEntities) {
                        scene::Entity src = m_Scene->GetEntity(id);
                        if (!src.IsValid()) continue;
                        
                        auto* tag = src.GetComponent<scene::TagComponent>();
                        scene::Entity dup = m_Scene->CreateEntity(tag ? tag->name + " Copy" : "Entity Copy");
                        
                        // Copy transform with offset
                        if (auto* t = src.GetComponent<scene::TransformComponent>()) {
                            auto* dt = dup.GetComponent<scene::TransformComponent>();
                            if (dt) {
                                *dt = *t;
                                dt->position += glm::vec3(1.0f, 0.0f, 0.0f); // Offset
                            }
                        }
                        
                        // Copy other components
                        if (auto* c = src.GetComponent<scene::CameraComponent>()) {
                            dup.AddComponent<scene::CameraComponent>() = *c;
                        }
                        if (auto* l = src.GetComponent<scene::LightComponent>()) {
                            dup.AddComponent<scene::LightComponent>() = *l;
                        }
                        if (auto* m = src.GetComponent<scene::MeshRendererComponent>()) {
                            dup.AddComponent<scene::MeshRendererComponent>() = *m;
                        }
                        
                        newEntities.push_back(dup);
                    }
                    ClearSelection();
                    for (auto& e : newEntities) {
                        AddToSelection(e);
                    }
                    m_SceneDirty = true;
                }
            }
            if (ImGui::MenuItem("Delete", "Del", false, !m_SelectedEntities.empty())) {
                if (m_Scene) {
                    for (auto id : m_SelectedEntities) {
                        m_Scene->DestroyEntity(m_Scene->GetEntity(id));
                    }
                    ClearSelection();
                    m_SceneDirty = true;
                }
            }
            ImGui::Separator();
            if (ImGui::MenuItem("Select All", "Ctrl+A")) {
                SelectAll();
            }
            if (ImGui::MenuItem("Deselect All", "Ctrl+Shift+A")) {
                ClearSelection();
            }
            ImGui::Separator();
            if (ImGui::MenuItem("Preferences...")) {
                m_ShowPreferencesModal = true;
            }
            ImGui::EndMenu();
        }
        
        if (ImGui::BeginMenu("Create")) {
            ImGui::TextDisabled("Primitives");
            if (ImGui::MenuItem("Cube")) {
                auto e = m_Scene->CreateEntity("Cube");
                auto& r = e.AddComponent<scene::MeshRendererComponent>();
                r.primitiveType = scene::MeshRendererComponent::PrimitiveType::Cube;
            }
            if (ImGui::MenuItem("Sphere")) {
                auto e = m_Scene->CreateEntity("Sphere");
                auto& r = e.AddComponent<scene::MeshRendererComponent>();
                r.primitiveType = scene::MeshRendererComponent::PrimitiveType::Sphere;
            }
            if (ImGui::MenuItem("Plane")) {
                auto e = m_Scene->CreateEntity("Plane");
                auto& r = e.AddComponent<scene::MeshRendererComponent>();
                r.primitiveType = scene::MeshRendererComponent::PrimitiveType::Plane;
            }
            if (ImGui::MenuItem("Cylinder")) {
                auto e = m_Scene->CreateEntity("Cylinder");
                auto& r = e.AddComponent<scene::MeshRendererComponent>();
                r.primitiveType = scene::MeshRendererComponent::PrimitiveType::Cylinder;
            }
            if (ImGui::MenuItem("Cone")) {
                auto e = m_Scene->CreateEntity("Cone");
                auto& r = e.AddComponent<scene::MeshRendererComponent>();
                r.primitiveType = scene::MeshRendererComponent::PrimitiveType::Cone;
            }
            ImGui::Separator();
            ImGui::TextDisabled("Lighting");
            if (ImGui::MenuItem("Point Light")) {
                auto e = m_Scene->CreateEntity("Point Light");
                auto& l = e.AddComponent<scene::LightComponent>();
                l.type = scene::LightType::Point;
            }
            if (ImGui::MenuItem("Directional Light")) {
                auto e = m_Scene->CreateEntity("Directional Light");
                auto& l = e.AddComponent<scene::LightComponent>();
                l.type = scene::LightType::Directional;
            }
            if (ImGui::MenuItem("Spot Light")) {
                auto e = m_Scene->CreateEntity("Spot Light");
                auto& l = e.AddComponent<scene::LightComponent>();
                l.type = scene::LightType::Spot;
            }
            ImGui::Separator();
            if (ImGui::MenuItem("Camera")) {
                auto e = m_Scene->CreateEntity("Camera");
                e.AddComponent<scene::CameraComponent>();
            }
            if (ImGui::MenuItem("Empty Entity")) {
                m_Scene->CreateEntity("Empty");
            }
            ImGui::EndMenu();
        }
        
        if (ImGui::BeginMenu("View")) {
            ImGui::TextDisabled("Panels");
            ImGui::MenuItem("Viewport", nullptr, &m_ShowViewport);
            ImGui::MenuItem("Outliner", nullptr, &m_ShowOutliner);
            ImGui::MenuItem("Inspector", nullptr, &m_ShowInspector);
            ImGui::MenuItem("Content Browser", nullptr, &m_ShowContentBrowser);
            ImGui::MenuItem("Console", nullptr, &m_ShowConsole);
            ImGui::MenuItem("Render Properties", nullptr, &m_ShowRenderProperties);
            
            bool matPanelVisible = m_MaterialGraphPanel.IsVisible();
            if (ImGui::MenuItem("Material Graph", nullptr, &matPanelVisible)) {
                m_MaterialGraphPanel.SetVisible(matPanelVisible);
            }
            
            ImGui::Separator();
            ImGui::TextDisabled("Layout");
            if (ImGui::MenuItem("Reset Layout")) {
                m_FirstFrame = true;
            }
            if (ImGui::MenuItem("Save Layout")) {
                SaveLayout();
            }
            ImGui::EndMenu();
        }
        
        if (ImGui::BeginMenu("Render")) {
            ImGui::TextDisabled("Viewport Mode");
            if (ImGui::MenuItem("Shaded", nullptr, m_RenderMode == RenderMode::Shaded)) {
                m_RenderMode = RenderMode::Shaded;
            }
            if (ImGui::MenuItem("Solid", nullptr, m_RenderMode == RenderMode::Solid)) {
                m_RenderMode = RenderMode::Solid;
            }
            if (ImGui::MenuItem("Wireframe", nullptr, m_RenderMode == RenderMode::Wireframe)) {
                m_RenderMode = RenderMode::Wireframe;
            }
            ImGui::Separator();
            
            // Check if RT is supported
            bool rtSupported = m_Context ? m_Context->IsRayTracingSupported() : false;
            if (ImGui::MenuItem("Path Tracer", nullptr, false, rtSupported)) {
                // TODO: Switch to path traced rendering
            }
            if (!rtSupported) {
                ImGui::TextDisabled("(Ray tracing not supported on this GPU)");
            }
            
            ImGui::Separator();
            ImGui::TextDisabled("Post Processing");
            
            // Exposure slider
            ImGui::SetNextItemWidth(120);
            ImGui::SliderFloat("Exposure", &m_Exposure, 0.1f, 5.0f, "%.2f");
            
            // Tonemapping options
            const char* tonemapModes[] = { "None", "Reinhard", "ACES", "Uncharted 2", "AgX" };
            ImGui::SetNextItemWidth(120);
            ImGui::Combo("Tonemap", &m_TonemapMode, tonemapModes, IM_ARRAYSIZE(tonemapModes));
            
            // Gamma
            ImGui::SetNextItemWidth(120);
            ImGui::SliderFloat("Gamma", &m_Gamma, 1.0f, 3.0f, "%.2f");
            
            ImGui::EndMenu();
        }
        
        if (ImGui::BeginMenu("Help")) {
            if (ImGui::MenuItem("Documentation")) {
                // Open docs folder in explorer
                ShellExecuteW(NULL, L"explore", L"docs", NULL, NULL, SW_SHOWNORMAL);
            }
            if (ImGui::MenuItem("Keyboard Shortcuts")) {
                m_ShowShortcutsModal = true;
            }
            ImGui::Separator();
            if (ImGui::MenuItem("About Lucent")) {
                m_ShowAboutModal = true;
            }
            ImGui::EndMenu();
        }
        
        // Right-align FPS counter
        float windowWidth = ImGui::GetWindowWidth();
        float fpsWidth = 120.0f;
        ImGui::SetCursorPosX(windowWidth - fpsWidth);
        ImGui::TextDisabled("%.1f FPS", ImGui::GetIO().Framerate);
        
        ImGui::EndMenuBar();
    }
    
    // Create the dockspace
    ImGuiID dockspaceId = ImGui::GetID("MainDockspace");
    ImGui::DockSpace(dockspaceId, ImVec2(0.0f, 0.0f), ImGuiDockNodeFlags_None);
    
    // Setup default layout on first frame
    if (m_FirstFrame) {
        m_FirstFrame = false;
        
        ImGui::DockBuilderRemoveNode(dockspaceId);
        ImGui::DockBuilderAddNode(dockspaceId, ImGuiDockNodeFlags_DockSpace);
        ImGui::DockBuilderSetNodeSize(dockspaceId, viewport->WorkSize);
        
        // Split the dockspace
        ImGuiID dockMain = dockspaceId;
        ImGuiID dockLeft = ImGui::DockBuilderSplitNode(dockMain, ImGuiDir_Left, 0.2f, nullptr, &dockMain);
        ImGuiID dockRight = ImGui::DockBuilderSplitNode(dockMain, ImGuiDir_Right, 0.25f, nullptr, &dockMain);
        ImGuiID dockBottom = ImGui::DockBuilderSplitNode(dockMain, ImGuiDir_Down, 0.25f, nullptr, &dockMain);
        
        // Dock windows
        ImGui::DockBuilderDockWindow("Viewport", dockMain);
        ImGui::DockBuilderDockWindow("Outliner", dockLeft);
        ImGui::DockBuilderDockWindow("Inspector", dockRight);
        ImGui::DockBuilderDockWindow("Content Browser", dockBottom);
        ImGui::DockBuilderDockWindow("Console", dockBottom);
        
        ImGui::DockBuilderFinish(dockspaceId);
    }
    
    ImGui::End();
}

void EditorUI::DrawViewportPanel() {
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    ImGui::Begin("Viewport", &m_ShowViewport);
    
    m_ViewportHovered = ImGui::IsWindowHovered();
    m_ViewportFocused = ImGui::IsWindowFocused();
    
    // Get viewport bounds
    ImVec2 viewportPanelPos = ImGui::GetCursorScreenPos();
    ImVec2 size = ImGui::GetContentRegionAvail();
    m_ViewportSize = size;
    m_ViewportPosition = viewportPanelPos;
    
    // Display the offscreen render result
    if (m_ViewportDescriptor != VK_NULL_HANDLE && size.x > 0 && size.y > 0) {
        ImGui::Image((ImTextureID)m_ViewportDescriptor, size);
        
        // Handle material drag-drop onto meshes
        if (ImGui::BeginDragDropTarget()) {
            if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("MATERIAL_PATH")) {
                std::string materialPath(static_cast<const char*>(payload->Data));
                HandleMaterialDrop(materialPath);
            }
            ImGui::EndDragDropTarget();
        }
    } else {
        ImGui::Text("Viewport not available");
    }
    
    // Draw gizmo if entity selected
    DrawGizmo();
    
    // Handle viewport click for selection (after gizmo so gizmo takes priority)
    HandleViewportClick();
    
    // Gizmo toolbar overlay
    ImGui::SetCursorPos(ImVec2(10, 30));
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(4, 4));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(2, 2));
    
    // Gizmo operation buttons
    if (ImGui::Button(m_GizmoOperation == GizmoOperation::Translate ? "[W] Move" : "W Move")) {
        m_GizmoOperation = GizmoOperation::Translate;
    }
    ImGui::SameLine();
    if (ImGui::Button(m_GizmoOperation == GizmoOperation::Rotate ? "[E] Rotate" : "E Rotate")) {
        m_GizmoOperation = GizmoOperation::Rotate;
    }
    ImGui::SameLine();
    if (ImGui::Button(m_GizmoOperation == GizmoOperation::Scale ? "[R] Scale" : "R Scale")) {
        m_GizmoOperation = GizmoOperation::Scale;
    }
    ImGui::SameLine();
    ImGui::Text(" | ");
    ImGui::SameLine();
    if (ImGui::Button(m_GizmoMode == GizmoMode::Local ? "[L]ocal" : "Local")) {
        m_GizmoMode = GizmoMode::Local;
    }
    ImGui::SameLine();
    if (ImGui::Button(m_GizmoMode == GizmoMode::World ? "[W]orld" : "World")) {
        m_GizmoMode = GizmoMode::World;
    }
    ImGui::SameLine();
    ImGui::Text(" | ");
    ImGui::SameLine();
    
    // Snapping toggle and settings
    if (ImGui::Button(m_SnapEnabled ? "[Snap]" : "Snap")) {
        m_SnapEnabled = !m_SnapEnabled;
    }
    if (ImGui::IsItemHovered()) {
        ImGui::BeginTooltip();
        ImGui::Text("Toggle snapping (hold Ctrl)");
        ImGui::EndTooltip();
    }
    
    // Snap settings popup
    if (m_SnapEnabled) {
        ImGui::SameLine();
        if (ImGui::Button("...##snap")) {
            ImGui::OpenPopup("SnapSettings");
        }
        
        if (ImGui::BeginPopup("SnapSettings")) {
            ImGui::Text("Snap Settings");
            ImGui::Separator();
            ImGui::SetNextItemWidth(80);
            ImGui::DragFloat("Move", &m_TranslateSnap, 0.1f, 0.1f, 10.0f, "%.1f");
            ImGui::SetNextItemWidth(80);
            ImGui::DragFloat("Rotate", &m_RotateSnap, 1.0f, 1.0f, 90.0f, "%.0f deg");
            ImGui::SetNextItemWidth(80);
            ImGui::DragFloat("Scale", &m_ScaleSnap, 0.01f, 0.01f, 1.0f, "%.2f");
            ImGui::EndPopup();
        }
    }
    
    ImGui::PopStyleVar(2);
    
    ImGui::End();
    ImGui::PopStyleVar();
}

void EditorUI::DrawGizmo() {
    scene::Entity selected = GetSelectedEntity();
    if (!selected.IsValid() || !m_EditorCamera || !m_Scene) {
        m_UsingGizmo = false;
        return;
    }
    
    auto* transform = selected.GetComponent<scene::TransformComponent>();
    if (!transform) {
        m_UsingGizmo = false;
        return;
    }
    
    // Get viewport bounds for ImGuizmo
    ImGuizmo::SetOrthographic(false);
    ImGuizmo::SetDrawlist();
    ImGuizmo::SetRect(m_ViewportPosition.x, m_ViewportPosition.y, m_ViewportSize.x, m_ViewportSize.y);
    
    // Get camera matrices
    if (m_ViewportSize.y <= 0.0f) {
        m_UsingGizmo = false;
        return;
    }

    // IMPORTANT: Do NOT mutate the shared editor camera here.
    // The viewport renderer owns camera aspect based on the render target; changing it here (only when selected)
    // can cause constant accumulation resets. We only need matrices for ImGuizmo, so build a local projection.
    const float aspectRatio = m_ViewportSize.x / m_ViewportSize.y;
    glm::mat4 view = m_EditorCamera->GetViewMatrix();
    glm::mat4 projection = glm::perspective(
        glm::radians(m_EditorCamera->GetFOV()),
        aspectRatio,
        m_EditorCamera->GetNearClip(),
        m_EditorCamera->GetFarClip()
    );
    
    // ImGuizmo expects OpenGL-style projection (Y-up), but Vulkan is Y-down
    // Flip the Y axis in the projection matrix for ImGuizmo
    projection[1][1] *= -1.0f;
    
    // Get transform matrix
    glm::mat4 transformMatrix = transform->GetLocalMatrix();

    // Snapshot current component values so we can detect actual changes robustly.
    const glm::vec3 beforePos = transform->position;
    const glm::vec3 beforeRot = transform->rotation;
    const glm::vec3 beforeScale = transform->scale;
    
    // Determine operation
    ImGuizmo::OPERATION operation = ImGuizmo::TRANSLATE;
    switch (m_GizmoOperation) {
        case GizmoOperation::Translate: operation = ImGuizmo::TRANSLATE; break;
        case GizmoOperation::Rotate: operation = ImGuizmo::ROTATE; break;
        case GizmoOperation::Scale: operation = ImGuizmo::SCALE; break;
        default: break;
    }
    
    // Determine mode
    ImGuizmo::MODE mode = (m_GizmoMode == GizmoMode::Local) ? ImGuizmo::LOCAL : ImGuizmo::WORLD;
    
    // Snapping
    float snapValue = 0.0f;
    float snapValues[3] = { 0.0f, 0.0f, 0.0f };
    
    if (m_SnapEnabled) {
        switch (m_GizmoOperation) {
            case GizmoOperation::Translate: snapValue = m_TranslateSnap; break;
            case GizmoOperation::Rotate: snapValue = m_RotateSnap; break;
            case GizmoOperation::Scale: snapValue = m_ScaleSnap; break;
        }
        snapValues[0] = snapValues[1] = snapValues[2] = snapValue;
    }
    
    // Manipulate
    bool manipulated = ImGuizmo::Manipulate(
        glm::value_ptr(view),
        glm::value_ptr(projection),
        operation,
        mode,
        glm::value_ptr(transformMatrix),
        nullptr,
        m_SnapEnabled ? snapValues : nullptr
    );
    
    bool currentlyUsing = ImGuizmo::IsUsing();
    
    // Detect gizmo start - capture initial state for undo
    if (currentlyUsing && !m_UsingGizmo) {
        m_GizmoStartPosition = transform->position;
        m_GizmoStartRotation = transform->rotation;
        m_GizmoStartScale = transform->scale;
        UndoStack::Get().BeginMergeWindow();
    }
    
    // Apply changes back to transform
    if (manipulated) {
        glm::vec3 translation, rotation, scale;
        ImGuizmo::DecomposeMatrixToComponents(
            glm::value_ptr(transformMatrix),
            glm::value_ptr(translation),
            glm::value_ptr(rotation),
            glm::value_ptr(scale)
        );

        // Only commit + mark dirty if the transform actually changed (avoid constant dirty when merely selected).
        if (!NearlyEqualTransform(beforePos, beforeRot, beforeScale, translation, rotation, scale)) {
            transform->position = translation;
            transform->rotation = rotation;
            transform->scale = scale;

            // Reset accumulation for traced modes when objects move
            m_Renderer->GetSettings().MarkDirty();
            m_SceneDirty = true;
        }
    }
    
    // Detect gizmo end - create undo command
    if (!currentlyUsing && m_UsingGizmo) {
        UndoStack::Get().EndMergeWindow();
        
        // Only create command if transform actually changed
        if (!NearlyEqualTransform(m_GizmoStartPosition, m_GizmoStartRotation, m_GizmoStartScale,
                                  transform->position, transform->rotation, transform->scale)) {
            
            TransformCommand::TransformState before{
                m_GizmoStartPosition,
                m_GizmoStartRotation,
                m_GizmoStartScale
            };
            TransformCommand::TransformState after{
                transform->position,
                transform->rotation,
                transform->scale
            };
            
            // Push without executing (state already applied during drag)
            auto cmd = std::make_unique<TransformCommand>(m_Scene, selected.GetID(), before, after);
            UndoStack::Get().Push(std::move(cmd));
            
            m_SceneDirty = true;
        }
    }
    
    m_UsingGizmo = currentlyUsing;
}

void EditorUI::DrawOutlinerPanel() {
    ImGui::Begin("Outliner", &m_ShowOutliner);
    
    // Header with scene name
    if (m_Scene) {
        ImGui::TextColored(ImVec4(0.5f, 0.8f, 0.9f, 1.0f), "Scene: %s", m_Scene->GetName().c_str());
    }
    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();
    
    if (m_Scene) {
        const auto& entities = m_Scene->GetEntities();
        for (scene::EntityID id : entities) {
            scene::Entity entity = m_Scene->GetEntity(id);
            DrawEntityNode(entity);
        }
        
        ImGui::Spacing();
        ImGui::Spacing();
        
        // Add entity button (more prominent)
        float buttonWidth = ImGui::GetContentRegionAvail().x;
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.2f, 0.5f, 0.6f, 0.5f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.25f, 0.78f, 0.85f, 0.7f));
        if (ImGui::Button("+ Add Entity", ImVec2(buttonWidth, 28))) {
            ImGui::OpenPopup("AddEntityPopup");
        }
        ImGui::PopStyleColor(2);
        
        // Add entity popup
        if (ImGui::BeginPopup("AddEntityPopup")) {
            ImGui::TextDisabled("Create New Entity");
            ImGui::Separator();
            
            if (ImGui::MenuItem("Empty")) {
                m_Scene->CreateEntity("New Entity");
            }
            
            ImGui::Separator();
            ImGui::TextDisabled("Primitives");
            
            if (ImGui::MenuItem("Cube")) {
                auto cube = m_Scene->CreateEntity("Cube");
                auto& renderer = cube.AddComponent<scene::MeshRendererComponent>();
                renderer.primitiveType = scene::MeshRendererComponent::PrimitiveType::Cube;
            }
            if (ImGui::MenuItem("Sphere")) {
                auto sphere = m_Scene->CreateEntity("Sphere");
                auto& renderer = sphere.AddComponent<scene::MeshRendererComponent>();
                renderer.primitiveType = scene::MeshRendererComponent::PrimitiveType::Sphere;
            }
            if (ImGui::MenuItem("Plane")) {
                auto plane = m_Scene->CreateEntity("Plane");
                auto& renderer = plane.AddComponent<scene::MeshRendererComponent>();
                renderer.primitiveType = scene::MeshRendererComponent::PrimitiveType::Plane;
            }
            if (ImGui::MenuItem("Cylinder")) {
                auto cyl = m_Scene->CreateEntity("Cylinder");
                auto& renderer = cyl.AddComponent<scene::MeshRendererComponent>();
                renderer.primitiveType = scene::MeshRendererComponent::PrimitiveType::Cylinder;
            }
            if (ImGui::MenuItem("Cone")) {
                auto cone = m_Scene->CreateEntity("Cone");
                auto& renderer = cone.AddComponent<scene::MeshRendererComponent>();
                renderer.primitiveType = scene::MeshRendererComponent::PrimitiveType::Cone;
            }
            
            ImGui::Separator();
            ImGui::TextDisabled("Lights & Cameras");
            
            if (ImGui::MenuItem("Point Light")) {
                auto light = m_Scene->CreateEntity("Point Light");
                auto& l = light.AddComponent<scene::LightComponent>();
                l.type = scene::LightType::Point;
            }
            if (ImGui::MenuItem("Directional Light")) {
                auto light = m_Scene->CreateEntity("Directional Light");
                auto& l = light.AddComponent<scene::LightComponent>();
                l.type = scene::LightType::Directional;
            }
            if (ImGui::MenuItem("Camera")) {
                auto camera = m_Scene->CreateEntity("Camera");
                auto& cam = camera.AddComponent<scene::CameraComponent>();
                cam.primary = false;
            }
            ImGui::EndPopup();
        }
        
        // Right-click context menu for empty area
        if (ImGui::BeginPopupContextWindow("OutlinerContextMenu", ImGuiPopupFlags_NoOpenOverItems | ImGuiPopupFlags_MouseButtonRight)) {
            if (ImGui::MenuItem("Paste")) {}
            ImGui::EndPopup();
        }
    } else {
        ImGui::TextDisabled("No scene loaded");
    }
    
    ImGui::End();
}

void EditorUI::DrawEntityNode(scene::Entity entity) {
    auto* tag = entity.GetComponent<scene::TagComponent>();
    if (!tag) return;
    
    bool isSelected = IsSelected(entity);
    
    ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_OpenOnArrow | 
                               ImGuiTreeNodeFlags_SpanAvailWidth |
                               ImGuiTreeNodeFlags_Leaf | // No children for now
                               ImGuiTreeNodeFlags_FramePadding;
    
    if (isSelected) {
        flags |= ImGuiTreeNodeFlags_Selected;
    }
    
    // Determine icon based on components
    const char* icon = "";  // Default: empty entity
    
    if (entity.HasComponent<scene::CameraComponent>()) {
        icon = "[CAM]";
    } else if (entity.HasComponent<scene::LightComponent>()) {
        auto* light = entity.GetComponent<scene::LightComponent>();
        if (light->type == scene::LightType::Directional) {
            icon = "[SUN]";
        } else {
            icon = "[LIT]";
        }
    } else if (entity.HasComponent<scene::MeshRendererComponent>()) {
        icon = "[MESH]";
    }
    
    // Push colors for selection
    if (isSelected) {
        ImGui::PushStyleColor(ImGuiCol_Header, ImVec4(0.25f, 0.78f, 0.85f, 0.4f));
        ImGui::PushStyleColor(ImGuiCol_HeaderHovered, ImVec4(0.25f, 0.78f, 0.85f, 0.5f));
    }
    
    // Format label with icon
    char label[512];
    snprintf(label, sizeof(label), "%s  %s", icon, tag->name.c_str());
    
    // Draw tree node with icon in label
    bool opened = ImGui::TreeNodeEx((void*)(intptr_t)entity.GetID(), flags, "%s", label);
    
    if (isSelected) {
        ImGui::PopStyleColor(2);
    }
    
    // Handle click with modifiers
    if (ImGui::IsItemClicked()) {
        bool ctrl = ImGui::GetIO().KeyCtrl;
        bool shift = ImGui::GetIO().KeyShift;
        
        if (ctrl) {
            ToggleSelection(entity);
        } else if (shift) {
            AddToSelection(entity);
        } else {
            SetSelectedEntity(entity);
        }
    }
    
    // Right-click context menu for entity
    if (ImGui::BeginPopupContextItem()) {
        if (ImGui::MenuItem("Delete")) {
            RemoveFromSelection(entity);
            m_Scene->DestroyEntity(entity);
        }
        if (ImGui::MenuItem("Duplicate")) {
            // TODO: Implement entity duplication
        }
        ImGui::EndPopup();
    }
    
    if (opened) {
        ImGui::TreePop();
    }
}

void EditorUI::DrawInspectorPanel() {
    ImGui::Begin("Inspector", &m_ShowInspector);
    
    scene::Entity selected = GetSelectedEntity();
    if (selected.IsValid()) {
        DrawComponentsPanel(selected);
    } else if (m_SelectedEntities.size() > 1) {
        ImGui::Text("%zu entities selected", m_SelectedEntities.size());
    } else {
        ImGui::TextDisabled("No entity selected");
    }
    
    ImGui::End();
}

void EditorUI::DrawComponentsPanel(scene::Entity entity) {
    // Tag component - editable name
    auto* tag = entity.GetComponent<scene::TagComponent>();
    if (tag) {
        char buffer[256];
        strncpy_s(buffer, tag->name.c_str(), sizeof(buffer) - 1);
        if (ImGui::InputText("##Name", buffer, sizeof(buffer))) {
            tag->name = buffer;
        }
    }
    
    ImGui::Separator();
    
    // Transform component
    auto* transform = entity.GetComponent<scene::TransformComponent>();
    if (transform) {
    if (ImGui::CollapsingHeader("Transform", ImGuiTreeNodeFlags_DefaultOpen)) {
            bool transformChanged = false;
            transformChanged |= ImGui::DragFloat3("Position", &transform->position.x, 0.1f);
            transformChanged |= ImGui::DragFloat3("Rotation", &transform->rotation.x, 1.0f);
            transformChanged |= ImGui::DragFloat3("Scale", &transform->scale.x, 0.1f);
            
            // Reset accumulation for traced modes when objects move
            if (transformChanged) {
                m_Renderer->GetSettings().MarkDirty();
                m_SceneDirty = true;
            }
        }
    }
    
    // Camera component
    auto* camera = entity.GetComponent<scene::CameraComponent>();
    if (camera) {
        if (ImGui::CollapsingHeader("Camera", ImGuiTreeNodeFlags_DefaultOpen)) {
            const char* projTypes[] = { "Perspective", "Orthographic" };
            int projType = static_cast<int>(camera->projectionType);
            if (ImGui::Combo("Projection", &projType, projTypes, 2)) {
                camera->projectionType = static_cast<scene::CameraComponent::ProjectionType>(projType);
            }
            
            if (camera->projectionType == scene::CameraComponent::ProjectionType::Perspective) {
                ImGui::DragFloat("FOV", &camera->fov, 1.0f, 1.0f, 179.0f);
            } else {
                ImGui::DragFloat("Size", &camera->orthoSize, 0.1f, 0.1f, 100.0f);
            }
            
            ImGui::DragFloat("Near", &camera->nearClip, 0.01f, 0.001f, camera->farClip - 0.01f);
            ImGui::DragFloat("Far", &camera->farClip, 1.0f, camera->nearClip + 0.01f, 10000.0f);
            ImGui::Checkbox("Primary", &camera->primary);
        }
    }
    
    // Light component
    auto* light = entity.GetComponent<scene::LightComponent>();
    if (light) {
        if (ImGui::CollapsingHeader("Light", ImGuiTreeNodeFlags_DefaultOpen)) {
            const char* lightTypes[] = { "Directional", "Point", "Spot", "Area" };
            int lightType = static_cast<int>(light->type);
            if (ImGui::Combo("Type", &lightType, lightTypes, 4)) {
                light->type = static_cast<scene::LightType>(lightType);
            }
            
            ImGui::ColorEdit3("Color", &light->color.x);
            ImGui::DragFloat("Intensity", &light->intensity, 0.1f, 0.0f, 100.0f);
            
            if (light->type == scene::LightType::Point || light->type == scene::LightType::Spot) {
                ImGui::DragFloat("Range", &light->range, 0.1f, 0.1f, 1000.0f);
            }
            
            if (light->type == scene::LightType::Spot) {
                ImGui::DragFloat("Inner Angle", &light->innerAngle, 1.0f, 0.0f, light->outerAngle);
                ImGui::DragFloat("Outer Angle", &light->outerAngle, 1.0f, light->innerAngle, 179.0f);
            }
            
            if (light->type == scene::LightType::Area) {
                ImGui::DragFloat2("Size", &light->areaSize.x, 0.1f, 0.01f, 100.0f);
            }
            
            ImGui::Checkbox("Cast Shadows", &light->castShadows);
        }
    }
    
    // Mesh Renderer component
    auto* meshRenderer = entity.GetComponent<scene::MeshRendererComponent>();
    if (meshRenderer) {
        if (ImGui::CollapsingHeader("Mesh Renderer", ImGuiTreeNodeFlags_DefaultOpen)) {
            const char* primitiveTypes[] = { "None", "Cube", "Sphere", "Plane", "Cylinder", "Cone" };
            int primType = static_cast<int>(meshRenderer->primitiveType);
            if (ImGui::Combo("Primitive", &primType, primitiveTypes, 6)) {
                meshRenderer->primitiveType = static_cast<scene::MeshRendererComponent::PrimitiveType>(primType);
            }
            
            ImGui::Spacing();
            ImGui::Checkbox("Visible", &meshRenderer->visible);
            ImGui::SameLine();
            ImGui::Checkbox("Cast Shadows", &meshRenderer->castShadows);
            ImGui::SameLine();
            ImGui::Checkbox("Receive Shadows", &meshRenderer->receiveShadows);
        }
        
        // Material properties section
        if (ImGui::CollapsingHeader("Material", ImGuiTreeNodeFlags_DefaultOpen)) {
            ImGui::Spacing();
            
            // Material asset assignment
            ImGui::Text("Material Asset");
            
            // Display current material path
            char matPathBuf[256];
            strncpy_s(matPathBuf, meshRenderer->materialPath.c_str(), sizeof(matPathBuf) - 1);
            matPathBuf[sizeof(matPathBuf) - 1] = '\0';
            
            ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - 120.0f);
            if (ImGui::InputText("##MaterialPath", matPathBuf, sizeof(matPathBuf))) {
                meshRenderer->materialPath = matPathBuf;
            }
            
            ImGui::SameLine();
            if (ImGui::Button("Edit Graph", ImVec2(115.0f, 0.0f))) {
                // Open material graph panel with this material
                if (!meshRenderer->materialPath.empty()) {
                    auto* mat = material::MaterialAssetManager::Get().GetMaterial(meshRenderer->materialPath);
                    if (mat) {
                        m_MaterialGraphPanel.SetMaterial(mat);
                        m_MaterialGraphPanel.SetVisible(true);
                    }
                } else {
                    // Create new material and open panel
                    auto* mat = m_MaterialGraphPanel.CreateNewMaterial();
                    if (mat) {
                        m_MaterialGraphPanel.SetVisible(true);
                    }
                }
            }
            
            // If no material assigned, show inline properties
            if (!meshRenderer->UsesMaterialAsset()) {
                ImGui::Spacing();
                ImGui::TextDisabled("Inline Properties (no material asset)");
                ImGui::Spacing();
                
                // Base Color with color picker
                ImGui::ColorEdit3("Base Color", &meshRenderer->baseColor.x, ImGuiColorEditFlags_Float);
                
                ImGui::Spacing();
                ImGui::Separator();
                ImGui::Spacing();
                
                // PBR sliders with better formatting
                ImGui::Text("Surface");
                ImGui::SliderFloat("Metallic", &meshRenderer->metallic, 0.0f, 1.0f, "%.2f");
                ImGui::SliderFloat("Roughness", &meshRenderer->roughness, 0.0f, 1.0f, "%.2f");
                
                ImGui::Spacing();
                ImGui::Separator();
                ImGui::Spacing();
                
                // Emission
                ImGui::Text("Emission");
                ImGui::ColorEdit3("Color##Emissive", &meshRenderer->emissive.x, ImGuiColorEditFlags_Float | ImGuiColorEditFlags_HDR);
                ImGui::SliderFloat("Intensity##Emissive", &meshRenderer->emissiveIntensity, 0.0f, 10.0f, "%.2f");
            } else {
                ImGui::Spacing();
                auto* mat = material::MaterialAssetManager::Get().GetMaterial(meshRenderer->materialPath);
                if (mat) {
                    ImGui::TextDisabled("Using material: %s", mat->GetGraph().GetName().c_str());
                    if (mat->IsValid()) {
                        ImGui::SameLine();
                        ImGui::TextColored(ImVec4(0.2f, 0.8f, 0.2f, 1.0f), "[OK]");
                    } else {
                        ImGui::SameLine();
                        ImGui::TextColored(ImVec4(0.9f, 0.2f, 0.2f, 1.0f), "[ERROR]");
                    }
                }
            }
        }
    }
    
    ImGui::Separator();
    
    // Add component button
    if (ImGui::Button("Add Component")) {
        ImGui::OpenPopup("AddComponentPopup");
    }
    
    if (ImGui::BeginPopup("AddComponentPopup")) {
        if (!entity.HasComponent<scene::CameraComponent>() && ImGui::MenuItem("Camera")) {
            entity.AddComponent<scene::CameraComponent>();
        }
        if (!entity.HasComponent<scene::LightComponent>() && ImGui::MenuItem("Light")) {
            entity.AddComponent<scene::LightComponent>();
        }
        if (!entity.HasComponent<scene::MeshRendererComponent>() && ImGui::MenuItem("Mesh Renderer")) {
            entity.AddComponent<scene::MeshRendererComponent>();
        }
        ImGui::EndPopup();
    }
}

void EditorUI::DrawContentBrowserPanel() {
    ImGui::Begin("Content Browser", &m_ShowContentBrowser);
    
    // Initialize path if needed
    static std::filesystem::path assetsPath = std::filesystem::current_path() / "Assets";
    if (m_ContentBrowserPath.empty()) {
        m_ContentBrowserPath = assetsPath;
    }
    
    // Create Assets folder if it doesn't exist
    if (!std::filesystem::exists(assetsPath)) {
        std::filesystem::create_directories(assetsPath);
    }
    
    // Toolbar
    static char searchBuffer[256] = "";
    ImGui::SetNextItemWidth(200);
    if (ImGui::InputTextWithHint("##search", "Search assets...", searchBuffer, sizeof(searchBuffer))) {
        m_ContentBrowserSearch = searchBuffer;
    }
    ImGui::SameLine();
    
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.2f, 0.5f, 0.6f, 0.5f));
    if (ImGui::Button("Import")) {
        std::string path = Win32FileDialogs::OpenFile(L"Import Asset", 
            {{L"All Supported", L"*.png;*.jpg;*.hdr;*.obj;*.lucent"}, 
             {L"Images", L"*.png;*.jpg;*.hdr"},
             {L"All Files", L"*.*"}});
        if (!path.empty()) {
            std::filesystem::path src(path);
            std::filesystem::path dest = m_ContentBrowserPath / src.filename();
            try {
                std::filesystem::copy_file(src, dest, std::filesystem::copy_options::overwrite_existing);
                LUCENT_CORE_INFO("Imported: {}", dest.string());
            } catch (const std::exception& e) {
                LUCENT_CORE_ERROR("Failed to import: {}", e.what());
            }
        }
    }
    ImGui::PopStyleColor();
    
    ImGui::SameLine();
    if (ImGui::Button("New Folder")) {
        // Create new folder with unique name
        int counter = 1;
        std::filesystem::path newPath = m_ContentBrowserPath / "New Folder";
        while (std::filesystem::exists(newPath)) {
            newPath = m_ContentBrowserPath / ("New Folder " + std::to_string(counter++));
        }
        std::filesystem::create_directory(newPath);
    }
    
    ImGui::SameLine();
    if (ImGui::Button("Reveal")) {
        // Open in Windows Explorer
        ShellExecuteW(NULL, L"explore", m_ContentBrowserPath.wstring().c_str(), NULL, NULL, SW_SHOWNORMAL);
    }
    
    ImGui::Separator();
    
    // Breadcrumb navigation
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.0f, 0.0f, 0.0f, 0.0f));
    
    // Build path segments
    std::vector<std::filesystem::path> segments;
    std::filesystem::path temp = m_ContentBrowserPath;
    while (temp != temp.root_path() && temp.has_parent_path()) {
        segments.push_back(temp);
        temp = temp.parent_path();
        // Stop at Assets folder
        if (temp == assetsPath.parent_path()) break;
    }
    std::reverse(segments.begin(), segments.end());
    
    if (ImGui::Button("Assets")) {
        m_ContentBrowserPath = assetsPath;
    }
    
    for (size_t i = 0; i < segments.size(); ++i) {
        if (segments[i] == assetsPath) continue;
        
        ImGui::SameLine();
        ImGui::TextDisabled(">");
        ImGui::SameLine();
        
        std::string name = segments[i].filename().string();
        if (ImGui::Button(name.c_str())) {
            m_ContentBrowserPath = segments[i];
        }
    }
    
    ImGui::PopStyleColor();
    
    // Back button
    ImGui::SameLine();
    if (m_ContentBrowserPath != assetsPath) {
        if (ImGui::Button("..")) {
            m_ContentBrowserPath = m_ContentBrowserPath.parent_path();
        }
    }
    
    ImGui::Separator();
    ImGui::Spacing();
    
    // Asset grid
    float padding = 12.0f;
    float thumbnailSize = 80.0f;
    float cellSize = thumbnailSize + padding * 2;
    float panelWidth = ImGui::GetContentRegionAvail().x;
    int columns = static_cast<int>(panelWidth / cellSize);
    if (columns < 1) columns = 1;
    
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(padding, padding));
    
    int itemIndex = 0;
    
    // List directory contents
    if (std::filesystem::exists(m_ContentBrowserPath)) {
        for (const auto& entry : std::filesystem::directory_iterator(m_ContentBrowserPath)) {
            std::string name = entry.path().filename().string();
            
            // Filter by search
            if (!m_ContentBrowserSearch.empty()) {
                std::string lowerName = name;
                std::string lowerSearch = m_ContentBrowserSearch;
                std::transform(lowerName.begin(), lowerName.end(), lowerName.begin(), ::tolower);
                std::transform(lowerSearch.begin(), lowerSearch.end(), lowerSearch.begin(), ::tolower);
                if (lowerName.find(lowerSearch) == std::string::npos) {
                    continue;
                }
            }
            
            ImGui::PushID(itemIndex);
            
            // Determine type and color
            bool isDirectory = entry.is_directory();
            std::string ext = entry.path().extension().string();
            std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
            
            ImVec4 color;
            const char* icon;
            
            if (isDirectory) {
                color = ImVec4(0.9f, 0.75f, 0.4f, 1.0f);
                icon = "[DIR]";
            } else if (ext == ".png" || ext == ".jpg" || ext == ".jpeg" || ext == ".hdr") {
                color = ImVec4(0.4f, 0.7f, 0.9f, 1.0f);
                icon = "[TEX]";
            } else if (ext == ".obj" || ext == ".fbx" || ext == ".gltf" || ext == ".glb") {
                color = ImVec4(0.4f, 0.8f, 0.5f, 1.0f);
                icon = "[OBJ]";
            } else if (ext == ".lucent") {
                color = ImVec4(0.25f, 0.78f, 0.85f, 1.0f);
                icon = "[SCN]";
            } else if (ext == ".mat") {
                color = ImVec4(0.8f, 0.5f, 0.9f, 1.0f);
                icon = "[MAT]";
            } else {
                color = ImVec4(0.6f, 0.6f, 0.6f, 1.0f);
                icon = "[???]";
            }
            
            ImGui::BeginGroup();
            
            // Thumbnail button
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(color.x * 0.3f, color.y * 0.3f, color.z * 0.3f, 0.8f));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(color.x * 0.5f, color.y * 0.5f, color.z * 0.5f, 0.9f));
            ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(color.x * 0.6f, color.y * 0.6f, color.z * 0.6f, 1.0f));
            
            if (ImGui::Button("##thumb", ImVec2(thumbnailSize, thumbnailSize))) {
                if (isDirectory) {
                    m_ContentBrowserPath = entry.path();
                }
            }
            
            // Drag source for compatible files (textures, materials)
            if (!isDirectory && ImGui::BeginDragDropSource(ImGuiDragDropFlags_SourceAllowNullID)) {
                std::string pathStr = entry.path().string();
                
                // Determine payload type based on extension
                const char* payloadType = "ASSET_PATH";
                if (ext == ".png" || ext == ".jpg" || ext == ".jpeg" || ext == ".hdr") {
                    payloadType = "TEXTURE_PATH";
                } else if (ext == ".lmat") {
                    payloadType = "MATERIAL_PATH";
                }
                
                ImGui::SetDragDropPayload(payloadType, pathStr.c_str(), pathStr.size() + 1);
                ImGui::Text("%s %s", icon, name.c_str());
                ImGui::EndDragDropSource();
            }
            
            // Double-click to open
            if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
                if (!isDirectory) {
                    // Handle special file types
                    if (ext == ".lmat") {
                        // Open material in Material Graph editor
                        OpenMaterialInEditor(entry.path().string());
                    } else {
                        // Open file with default application
                        ShellExecuteW(NULL, L"open", entry.path().wstring().c_str(), NULL, NULL, SW_SHOWNORMAL);
                    }
                }
            }
            
            ImGui::PopStyleColor(3);
            
            // Right-click context menu
            if (ImGui::BeginPopupContextItem()) {
                if (ImGui::MenuItem("Open")) {
                    if (isDirectory) {
                        m_ContentBrowserPath = entry.path();
                    } else {
                        ShellExecuteW(NULL, L"open", entry.path().wstring().c_str(), NULL, NULL, SW_SHOWNORMAL);
                    }
                }
                if (ImGui::MenuItem("Reveal in Explorer")) {
                    ShellExecuteW(NULL, L"open", L"explorer.exe", 
                        (L"/select,\"" + entry.path().wstring() + L"\"").c_str(), NULL, SW_SHOWNORMAL);
                }
                ImGui::Separator();
                if (ImGui::MenuItem("Delete")) {
                    try {
                        std::filesystem::remove_all(entry.path());
                    } catch (const std::exception& e) {
                        LUCENT_CORE_ERROR("Failed to delete: {}", e.what());
                    }
                }
                ImGui::EndPopup();
            }
            
            // Type icon
            ImGui::SetCursorPosY(ImGui::GetCursorPosY() - thumbnailSize - 5);
            ImGui::SetCursorPosX(ImGui::GetCursorPosX() + thumbnailSize / 2 - 15);
            ImGui::TextColored(color, "%s", icon);
            ImGui::SetCursorPosY(ImGui::GetCursorPosY() + thumbnailSize / 2 + 10);
            
            // File name (truncated)
            ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + thumbnailSize);
            if (name.length() > 12) {
                ImGui::TextWrapped("%s...", name.substr(0, 9).c_str());
            } else {
                ImGui::TextWrapped("%s", name.c_str());
            }
            ImGui::PopTextWrapPos();
            
            ImGui::EndGroup();
            
            // Tooltip with full name
            if (ImGui::IsItemHovered()) {
                ImGui::BeginTooltip();
                ImGui::Text("%s", name.c_str());
                if (!isDirectory) {
                    auto size = entry.file_size();
                    if (size < 1024) {
                        ImGui::Text("%llu bytes", size);
                    } else if (size < 1024 * 1024) {
                        ImGui::Text("%.1f KB", size / 1024.0f);
                    } else {
                        ImGui::Text("%.1f MB", size / (1024.0f * 1024.0f));
                    }
                }
                ImGui::EndTooltip();
            }
            
            // Column wrapping
            if ((itemIndex + 1) % columns != 0) {
                ImGui::SameLine();
            }
            
        ImGui::PopID();
            itemIndex++;
        }
    }
    
    // Show empty state
    if (itemIndex == 0) {
        ImGui::TextDisabled("This folder is empty");
        ImGui::TextDisabled("Drag files here or click Import");
    }
    
    ImGui::PopStyleVar();
    
    ImGui::End();
}

void EditorUI::NavigateToAsset(const std::string& path) {
    // Convert to filesystem path and navigate content browser
    std::filesystem::path assetPath(path);
    
    if (std::filesystem::exists(assetPath)) {
        // Navigate to the parent directory
        m_ContentBrowserPath = assetPath.parent_path();
        
        // Make sure content browser is visible
        m_ShowContentBrowser = true;
        
        LUCENT_CORE_INFO("Navigated to: {}", m_ContentBrowserPath.string());
    } else {
        LUCENT_CORE_WARN("Asset not found: {}", path);
    }
}

void EditorUI::OpenMaterialInEditor(const std::string& path) {
    // Load the material from the file
    auto* material = material::MaterialAssetManager::Get().LoadMaterial(path);
    
    if (material) {
        // Compile it if needed
        if (!material->IsValid()) {
            material->Recompile();
        }
        
        // Set it in the material graph panel
        m_MaterialGraphPanel.SetMaterial(material);
        
        // Make the panel visible
        m_MaterialGraphPanel.SetVisible(true);
        
        LUCENT_CORE_INFO("Opened material: {}", path);
    } else {
        LUCENT_CORE_ERROR("Failed to load material: {}", path);
    }
}

void EditorUI::HandleMaterialDrop(const std::string& materialPath) {
    if (!m_Scene || !m_EditorCamera) return;
    
    // Get mouse position relative to viewport
    ImVec2 mousePos = ImGui::GetMousePos();
    glm::vec2 relativePos(mousePos.x - m_ViewportPosition.x, mousePos.y - m_ViewportPosition.y);
    
    // Check if within viewport bounds
    if (relativePos.x < 0 || relativePos.y < 0 || 
        relativePos.x >= m_ViewportSize.x || relativePos.y >= m_ViewportSize.y) {
        return;
    }
    
    // Pick entity under mouse
    scene::Entity hitEntity = PickEntity(relativePos);
    
    if (hitEntity.IsValid()) {
        // Check if entity has a mesh renderer
        auto* meshRenderer = hitEntity.GetComponent<scene::MeshRendererComponent>();
        if (meshRenderer) {
            // Load the material to make sure it's valid
            auto* material = material::MaterialAssetManager::Get().LoadMaterial(materialPath);
            if (material) {
                if (!material->IsValid()) {
                    material->Recompile();
                }
                
                // Assign the material path to the mesh renderer
                meshRenderer->materialPath = materialPath;
                
                auto* tag = hitEntity.GetComponent<scene::TagComponent>();
                std::string entityName = tag ? tag->name : "Entity";
                
                LUCENT_CORE_INFO("Assigned material '{}' to '{}'", 
                    std::filesystem::path(materialPath).filename().string(), 
                    entityName);
            } else {
                LUCENT_CORE_WARN("Failed to load material: {}", materialPath);
            }
        } else {
            LUCENT_CORE_WARN("Entity doesn't have a MeshRenderer component");
        }
    } else {
        LUCENT_CORE_DEBUG("No entity under drop position");
    }
}

void EditorUI::DrawConsolePanel() {
    ImGui::Begin("Console", &m_ShowConsole);
    
    // Toolbar
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.3f, 0.3f, 0.35f, 0.5f));
    if (ImGui::Button("Clear")) {
        // Clear console (would clear log buffer)
    }
    ImGui::SameLine();
    if (ImGui::Button("Copy")) {
        // Copy to clipboard
    }
    ImGui::PopStyleColor();
    
    ImGui::SameLine();
    ImGui::Spacing();
    ImGui::SameLine();
    
    // Filter buttons
    static bool showInfo = true, showWarn = true, showError = true;
    
    ImGui::PushStyleColor(ImGuiCol_Button, showInfo ? ImVec4(0.3f, 0.7f, 0.5f, 0.6f) : ImVec4(0.2f, 0.2f, 0.2f, 0.5f));
    if (ImGui::Button("Info")) showInfo = !showInfo;
    ImGui::PopStyleColor();
    
    ImGui::SameLine();
    ImGui::PushStyleColor(ImGuiCol_Button, showWarn ? ImVec4(0.85f, 0.7f, 0.3f, 0.6f) : ImVec4(0.2f, 0.2f, 0.2f, 0.5f));
    if (ImGui::Button("Warn")) showWarn = !showWarn;
    ImGui::PopStyleColor();
    
    ImGui::SameLine();
    ImGui::PushStyleColor(ImGuiCol_Button, showError ? ImVec4(0.85f, 0.35f, 0.35f, 0.6f) : ImVec4(0.2f, 0.2f, 0.2f, 0.5f));
    if (ImGui::Button("Error")) showError = !showError;
    ImGui::PopStyleColor();
    
    ImGui::SameLine();
    static bool autoScroll = true;
    ImGui::Checkbox("Auto-scroll", &autoScroll);
    
    ImGui::Separator();
    
    // Log output area with colored background
    ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.05f, 0.05f, 0.06f, 1.0f));
    ImGui::BeginChild("ScrollingRegion", ImVec2(0, 0), true, ImGuiWindowFlags_HorizontalScrollbar);
    
    // Demo log messages with timestamps
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.5f, 0.5f, 0.55f, 1.0f));
    ImGui::TextUnformatted("[11:00:00]");
    ImGui::PopStyleColor();
    ImGui::SameLine();
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.4f, 0.8f, 0.5f, 1.0f));
    ImGui::TextUnformatted("[INFO]");
    ImGui::PopStyleColor();
    ImGui::SameLine();
    ImGui::TextUnformatted("Lucent Engine initialized");
    
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.5f, 0.5f, 0.55f, 1.0f));
    ImGui::TextUnformatted("[11:00:00]");
    ImGui::PopStyleColor();
    ImGui::SameLine();
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.4f, 0.8f, 0.5f, 1.0f));
    ImGui::TextUnformatted("[INFO]");
    ImGui::PopStyleColor();
    ImGui::SameLine();
    ImGui::TextUnformatted("Vulkan context initialized successfully");
    
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.5f, 0.5f, 0.55f, 1.0f));
    ImGui::TextUnformatted("[11:00:00]");
    ImGui::PopStyleColor();
    ImGui::SameLine();
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.4f, 0.8f, 0.5f, 1.0f));
    ImGui::TextUnformatted("[INFO]");
    ImGui::PopStyleColor();
    ImGui::SameLine();
    ImGui::TextUnformatted("Renderer initialized");
    
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.5f, 0.5f, 0.55f, 1.0f));
    ImGui::TextUnformatted("[11:00:01]");
    ImGui::PopStyleColor();
    ImGui::SameLine();
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.4f, 0.8f, 0.5f, 1.0f));
    ImGui::TextUnformatted("[INFO]");
    ImGui::PopStyleColor();
    ImGui::SameLine();
    ImGui::TextUnformatted("Scene initialized with 7 entities");
    
    if (autoScroll && ImGui::GetScrollY() >= ImGui::GetScrollMaxY()) {
        ImGui::SetScrollHereY(1.0f);
    }
    
    ImGui::EndChild();
    ImGui::PopStyleColor();
    
    ImGui::End();
}

void EditorUI::DrawRenderPropertiesPanel() {
    ImGui::Begin("Render Properties", &m_ShowRenderProperties);
    
    gfx::RenderSettings& settings = m_Renderer->GetSettings();
    const gfx::RenderCapabilities& caps = m_Renderer->GetCapabilities();
    bool settingsChanged = false;
    
    // === Render Mode ===
    ImGui::TextDisabled("Render Mode");
    ImGui::Spacing();
    
    gfx::RenderMode currentMode = m_Renderer->GetRenderMode();
    
    // Mode dropdown
    const char* modeNames[] = { "Simple", "Traced", "Ray Traced" };
    int currentModeIdx = static_cast<int>(currentMode);
    
    if (ImGui::BeginCombo("Mode", modeNames[currentModeIdx])) {
        for (int i = 0; i < 3; i++) {
            gfx::RenderMode mode = static_cast<gfx::RenderMode>(i);
            bool available = caps.IsModeAvailable(mode);
            
            if (!available) {
                ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.4f, 0.4f, 0.4f, 1.0f));
            }
            
            if (ImGui::Selectable(modeNames[i], i == currentModeIdx, available ? 0 : ImGuiSelectableFlags_Disabled)) {
                if (available && mode != currentMode) {
                    m_Renderer->SetRenderMode(mode);
                    settings.activeMode = mode;
                    settings.MarkDirty();
                }
            }
            
            if (!available) {
                ImGui::PopStyleColor();
            }
        }
        ImGui::EndCombo();
    }
    
    // Show mode status
    if (currentMode != gfx::RenderMode::Simple) {
        ImGui::Text("Samples: %u / %u", settings.accumulatedSamples, settings.viewportSamples);
        if (settings.IsConverged()) {
            ImGui::SameLine();
            ImGui::TextColored(ImVec4(0.4f, 0.8f, 0.4f, 1.0f), "(Converged)");
        }
    }
    
    ImGui::Separator();
    
    // === Sampling ===
    if (ImGui::CollapsingHeader("Sampling", ImGuiTreeNodeFlags_DefaultOpen)) {
        if (ImGui::DragInt("Viewport Samples", (int*)&settings.viewportSamples, 1.0f, 1, 4096)) {
            settingsChanged = true;
        }
        if (ImGui::DragInt("Final Samples", (int*)&settings.finalSamples, 1.0f, 1, 65536)) {
            // No reset needed, final render uses this
        }
        if (ImGui::DragFloat("Max Frame Time (ms)", &settings.maxFrameTimeMs, 0.1f, 1.0f, 100.0f, "%.1f")) {
            // Progressive time budget
        }
        if (ImGui::Checkbox("Half Resolution", &settings.useHalfRes)) {
            settingsChanged = true;
        }
    }
    
    // === Bounces (for Traced/RayTraced modes) ===
    if (currentMode != gfx::RenderMode::Simple) {
        if (ImGui::CollapsingHeader("Light Paths", ImGuiTreeNodeFlags_DefaultOpen)) {
            if (ImGui::DragInt("Max Bounces", (int*)&settings.maxBounces, 0.1f, 0, 32)) {
                settingsChanged = true;
            }
            if (ImGui::DragInt("Diffuse", (int*)&settings.diffuseBounces, 0.1f, 0, 32)) {
                settingsChanged = true;
            }
            if (ImGui::DragInt("Specular", (int*)&settings.specularBounces, 0.1f, 0, 32)) {
                settingsChanged = true;
            }
            if (ImGui::DragInt("Transmission", (int*)&settings.transmissionBounces, 0.1f, 0, 32)) {
                settingsChanged = true;
            }
        }
    }
    
    // === Clamping ===
    if (currentMode != gfx::RenderMode::Simple) {
        if (ImGui::CollapsingHeader("Clamping")) {
            if (ImGui::DragFloat("Clamp Direct", &settings.clampDirect, 0.1f, 0.0f, 100.0f, settings.clampDirect == 0 ? "Off" : "%.1f")) {
                settingsChanged = true;
            }
            if (ImGui::DragFloat("Clamp Indirect", &settings.clampIndirect, 0.1f, 0.0f, 100.0f, settings.clampIndirect == 0 ? "Off" : "%.1f")) {
                settingsChanged = true;
            }
        }
    }
    
    // === Film / Color ===
    if (ImGui::CollapsingHeader("Film", ImGuiTreeNodeFlags_DefaultOpen)) {
        if (ImGui::DragFloat("Exposure", &settings.exposure, 0.01f, 0.01f, 10.0f, "%.2f")) {
            m_Exposure = settings.exposure;  // Keep in sync
        }
        
        const char* tonemapNames[] = { "None", "Reinhard", "ACES", "Uncharted 2", "AgX" };
        int tonemapIdx = static_cast<int>(settings.tonemapOperator);
        if (ImGui::Combo("Tonemap", &tonemapIdx, tonemapNames, 5)) {
            settings.tonemapOperator = static_cast<gfx::TonemapOperator>(tonemapIdx);
            m_TonemapMode = tonemapIdx;  // Keep in sync
        }
        
        if (ImGui::DragFloat("Gamma", &settings.gamma, 0.01f, 1.0f, 3.0f, "%.2f")) {
            m_Gamma = settings.gamma;  // Keep in sync
        }
    }
    
    // === Denoise ===
    if (currentMode != gfx::RenderMode::Simple) {
        if (ImGui::CollapsingHeader("Denoise")) {
            const char* denoiserNames[] = {
                "None",
                "Box Blur",
                "Edge-Aware",
                "OpenImageDenoise",
                "OptiX",
                "NRD"
            };
            int denoiserIdx = static_cast<int>(settings.denoiser);
            if (ImGui::BeginCombo("Denoiser", denoiserNames[denoiserIdx])) {
                for (int i = 0; i < 6; i++) {
                    gfx::DenoiserType type = static_cast<gfx::DenoiserType>(i);
                    bool supported = (type == gfx::DenoiserType::None ||
                                      type == gfx::DenoiserType::Box ||
                                      type == gfx::DenoiserType::EdgeAware);
                    
                    // OptiX is supported if available
                    if (type == gfx::DenoiserType::OptiX && m_Renderer->IsOptiXDenoiserAvailable()) {
                        supported = true;
                    }
                    
                    if (!supported) {
                        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.45f, 0.45f, 0.45f, 1.0f));
                    }

                    if (ImGui::Selectable(denoiserNames[i], i == denoiserIdx,
                            supported ? 0 : ImGuiSelectableFlags_Disabled)) {
                        settings.denoiser = static_cast<gfx::DenoiserType>(i);
                        settingsChanged = true;
                    }

                    if (!supported) {
                        ImGui::PopStyleColor();
                    }

                    if (!supported && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
                        ImGui::BeginTooltip();
                        if (type == gfx::DenoiserType::OptiX) {
                            ImGui::TextUnformatted("OptiX SDK not found or initialization failed");
                        } else {
                            ImGui::TextUnformatted("External integration required");
                        }
                        ImGui::EndTooltip();
                    }
                }
                ImGui::EndCombo();
            }

            if (settings.denoiser != gfx::DenoiserType::None) {
                if (ImGui::DragFloat("Strength", &settings.denoiseStrength, 0.01f, 0.0f, 1.0f, "%.2f")) {
                    settingsChanged = true;
                }
                if (ImGui::DragInt("Radius", (int*)&settings.denoiseRadius, 1.0f, 1, 8)) {
                    settingsChanged = true;
                }

                bool supported = (settings.denoiser == gfx::DenoiserType::Box ||
                                  settings.denoiser == gfx::DenoiserType::EdgeAware);
                bool isOptiX = (settings.denoiser == gfx::DenoiserType::OptiX && m_Renderer->IsOptiXDenoiserAvailable());
                if (isOptiX) {
                    ImGui::TextDisabled("OptiX AI Denoiser with albedo + normal guides.");
                } else if (!supported) {
                    ImGui::TextDisabled("Selected denoiser not available in this build.");
                } else {
                    ImGui::TextDisabled("Edge-aware and box denoisers are CPU-only for final renders.");
                }
            }
        }
    }
    
    // === Shadows (Simple mode) ===
    if (currentMode == gfx::RenderMode::Simple) {
        if (ImGui::CollapsingHeader("Shadows", ImGuiTreeNodeFlags_DefaultOpen)) {
            ImGui::Checkbox("Enable Shadows", &settings.enableShadows);
            if (settings.enableShadows) {
                ImGui::DragFloat("Shadow Bias", &settings.shadowBias, 0.0001f, 0.0f, 0.1f, "%.4f");
            }
        }
    }
    
    // Mark dirty if settings changed
    if (settingsChanged) {
        settings.MarkDirty();
    }
    
    ImGui::End();
}

// ============================================================================
// Selection Management
// ============================================================================

void EditorUI::SetSelectedEntity(scene::Entity entity) {
    ClearSelection();
    if (entity.IsValid()) {
        m_SelectedEntities.push_back(entity.GetID());
    }
}

scene::Entity EditorUI::GetSelectedEntity() const {
    if (m_SelectedEntities.empty() || !m_Scene) {
        return scene::Entity();
    }
    return m_Scene->GetEntity(m_SelectedEntities[0]);
}

bool EditorUI::IsSelected(scene::Entity entity) const {
    if (!entity.IsValid()) return false;
    scene::EntityID id = entity.GetID();
    for (auto selId : m_SelectedEntities) {
        if (selId == id) return true;
    }
    return false;
}

void EditorUI::AddToSelection(scene::Entity entity) {
    if (!entity.IsValid()) return;
    if (!IsSelected(entity)) {
        m_SelectedEntities.push_back(entity.GetID());
    }
}

void EditorUI::RemoveFromSelection(scene::Entity entity) {
    if (!entity.IsValid()) return;
    scene::EntityID id = entity.GetID();
    m_SelectedEntities.erase(
        std::remove(m_SelectedEntities.begin(), m_SelectedEntities.end(), id),
        m_SelectedEntities.end()
    );
}

void EditorUI::ToggleSelection(scene::Entity entity) {
    if (IsSelected(entity)) {
        RemoveFromSelection(entity);
    } else {
        AddToSelection(entity);
    }
}

void EditorUI::ClearSelection() {
    m_SelectedEntities.clear();
}

void EditorUI::SelectAll() {
    if (!m_Scene) return;
    ClearSelection();
    for (scene::EntityID id : m_Scene->GetEntities()) {
        m_SelectedEntities.push_back(id);
    }
}

// ============================================================================
// Picking
// ============================================================================

void EditorUI::HandleViewportClick() {
    if (!m_Scene || !m_EditorCamera) return;
    if (!m_ViewportHovered) return;
    if (m_UsingGizmo) return; // Gizmo takes priority
    
    // Check for left click
    if (!ImGui::IsMouseClicked(ImGuiMouseButton_Left)) return;
    
    // Get mouse position relative to viewport
    ImVec2 mousePos = ImGui::GetMousePos();
    glm::vec2 relativePos(mousePos.x - m_ViewportPosition.x, mousePos.y - m_ViewportPosition.y);
    
    // Check if within viewport bounds
    if (relativePos.x < 0 || relativePos.y < 0 || 
        relativePos.x >= m_ViewportSize.x || relativePos.y >= m_ViewportSize.y) {
        return;
    }
    
    // Pick entity
    scene::Entity hitEntity = PickEntity(relativePos);
    
    // Handle selection based on modifiers
    bool ctrl = ImGui::GetIO().KeyCtrl;
    bool shift = ImGui::GetIO().KeyShift;
    
    if (hitEntity.IsValid()) {
        if (ctrl) {
            ToggleSelection(hitEntity);
        } else if (shift) {
            AddToSelection(hitEntity);
        } else {
            SetSelectedEntity(hitEntity);
        }
    } else {
        // Clicked on empty space
        if (!ctrl && !shift) {
            ClearSelection();
        }
    }
}

scene::Entity EditorUI::PickEntity(const glm::vec2& mousePos) {
    if (!m_Scene || !m_EditorCamera) return scene::Entity();
    
    // Convert mouse position to normalized device coordinates [-1, 1]
    float ndcX = (2.0f * mousePos.x / m_ViewportSize.x) - 1.0f;
    float ndcY = (2.0f * mousePos.y / m_ViewportSize.y) - 1.0f; // Y is already flipped in Vulkan
    
    // Get camera matrices
    glm::mat4 view = m_EditorCamera->GetViewMatrix();
    glm::mat4 proj = m_EditorCamera->GetProjectionMatrix();
    glm::mat4 invViewProj = glm::inverse(proj * view);
    
    // Unproject near and far points
    glm::vec4 nearPoint = invViewProj * glm::vec4(ndcX, ndcY, 0.0f, 1.0f);
    glm::vec4 farPoint = invViewProj * glm::vec4(ndcX, ndcY, 1.0f, 1.0f);
    
    nearPoint /= nearPoint.w;
    farPoint /= farPoint.w;
    
    glm::vec3 rayOrigin = glm::vec3(nearPoint);
    glm::vec3 rayDir = glm::normalize(glm::vec3(farPoint) - rayOrigin);
    
    // Find closest hit
    scene::Entity closestEntity;
    float closestT = FLT_MAX;
    
    auto view_iter = m_Scene->GetView<scene::MeshRendererComponent, scene::TransformComponent>();
    view_iter.Each([&](scene::Entity entity, scene::MeshRendererComponent& renderer, scene::TransformComponent& transform) {
        if (!renderer.visible) return;
        
        glm::mat4 modelMatrix = transform.GetLocalMatrix();
        glm::mat4 invModel = glm::inverse(modelMatrix);
        
        // Transform ray to local space
        glm::vec3 localOrigin = glm::vec3(invModel * glm::vec4(rayOrigin, 1.0f));
        glm::vec3 localDir = glm::normalize(glm::vec3(invModel * glm::vec4(rayDir, 0.0f)));
        
        float t = FLT_MAX;
        bool hit = false;
        
        switch (renderer.primitiveType) {
            case scene::MeshRendererComponent::PrimitiveType::Sphere: {
                hit = RayIntersectsSphere(localOrigin, localDir, glm::vec3(0.0f), 0.5f, t);
                break;
            }
            case scene::MeshRendererComponent::PrimitiveType::Cube: {
                hit = RayIntersectsAABB(localOrigin, localDir, glm::vec3(-0.5f), glm::vec3(0.5f), t);
                break;
            }
            case scene::MeshRendererComponent::PrimitiveType::Plane: {
                // Thin AABB for plane
                hit = RayIntersectsAABB(localOrigin, localDir, 
                                        glm::vec3(-0.5f, -0.01f, -0.5f), 
                                        glm::vec3(0.5f, 0.01f, 0.5f), t);
                break;
            }
            case scene::MeshRendererComponent::PrimitiveType::Cylinder:
            case scene::MeshRendererComponent::PrimitiveType::Cone: {
                // Use AABB approximation for cylinder/cone
                hit = RayIntersectsAABB(localOrigin, localDir, 
                                        glm::vec3(-0.5f, -0.5f, -0.5f), 
                                        glm::vec3(0.5f, 0.5f, 0.5f), t);
                break;
            }
            default:
                break;
        }
        
        if (hit && t > 0.0f && t < closestT) {
            closestT = t;
            closestEntity = entity;
        }
    });
    
    return closestEntity;
}

bool EditorUI::RayIntersectsAABB(const glm::vec3& rayOrigin, const glm::vec3& rayDir,
                                  const glm::vec3& aabbMin, const glm::vec3& aabbMax, float& tOut) {
    float tmin = -FLT_MAX;
    float tmax = FLT_MAX;
    
    for (int i = 0; i < 3; ++i) {
        if (std::abs(rayDir[i]) < 1e-8f) {
            // Ray parallel to slab
            if (rayOrigin[i] < aabbMin[i] || rayOrigin[i] > aabbMax[i]) {
                return false;
            }
        } else {
            float ood = 1.0f / rayDir[i];
            float t1 = (aabbMin[i] - rayOrigin[i]) * ood;
            float t2 = (aabbMax[i] - rayOrigin[i]) * ood;
            
            if (t1 > t2) std::swap(t1, t2);
            
            tmin = std::max(tmin, t1);
            tmax = std::min(tmax, t2);
            
            if (tmin > tmax) return false;
        }
    }
    
    tOut = tmin > 0.0f ? tmin : tmax;
    return tmax >= 0.0f;
}

bool EditorUI::RayIntersectsSphere(const glm::vec3& rayOrigin, const glm::vec3& rayDir,
                                    const glm::vec3& center, float radius, float& tOut) {
    glm::vec3 oc = rayOrigin - center;
    float a = glm::dot(rayDir, rayDir);
    float b = 2.0f * glm::dot(oc, rayDir);
    float c = glm::dot(oc, oc) - radius * radius;
    float discriminant = b * b - 4.0f * a * c;
    
    if (discriminant < 0.0f) {
        return false;
    }
    
    float sqrtD = std::sqrt(discriminant);
    float t1 = (-b - sqrtD) / (2.0f * a);
    float t2 = (-b + sqrtD) / (2.0f * a);
    
    if (t1 > 0.0f) {
        tOut = t1;
        return true;
    }
    if (t2 > 0.0f) {
        tOut = t2;
        return true;
    }
    
    return false;
}

// ============================================================================
// Modals
// ============================================================================

void EditorUI::DrawModals() {
    // About modal
    if (m_ShowAboutModal) {
        ImGui::OpenPopup("About Lucent");
        m_ShowAboutModal = false;
    }
    
    if (ImGui::BeginPopupModal("About Lucent", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::TextColored(ImVec4(0.25f, 0.78f, 0.85f, 1.0f), "LUCENT");
        ImGui::Text("3D Editor with Vulkan Path Tracer");
        ImGui::Separator();
        ImGui::Text("Version: 0.1.0 (Development)");
        ImGui::Text("Build: Debug");
        ImGui::Spacing();
        
        // GPU info
        if (m_Context) {
            VkPhysicalDeviceProperties props;
            vkGetPhysicalDeviceProperties(m_Context->GetPhysicalDevice(), &props);
            ImGui::Text("GPU: %s", props.deviceName);
            ImGui::Text("Driver: %u.%u.%u", 
                VK_VERSION_MAJOR(props.driverVersion),
                VK_VERSION_MINOR(props.driverVersion),
                VK_VERSION_PATCH(props.driverVersion));
            ImGui::Text("Vulkan: %u.%u", 
                VK_VERSION_MAJOR(props.apiVersion),
                VK_VERSION_MINOR(props.apiVersion));
        }
        
        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Text("(c) 2024-2026");
        
        ImGui::Spacing();
        if (ImGui::Button("Close", ImVec2(120, 0))) {
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
    
    // Keyboard shortcuts modal
    if (m_ShowShortcutsModal) {
        ImGui::OpenPopup("Keyboard Shortcuts");
        m_ShowShortcutsModal = false;
    }
    
    if (ImGui::BeginPopupModal("Keyboard Shortcuts", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::Text("Navigation");
        ImGui::BulletText("Right-click + Drag: Rotate camera");
        ImGui::BulletText("Middle-click + Drag: Pan camera");
        ImGui::BulletText("Scroll: Zoom");
        ImGui::BulletText("F: Focus on selection");
        
        ImGui::Separator();
        ImGui::Text("Selection");
        ImGui::BulletText("Left-click: Select entity");
        ImGui::BulletText("Ctrl + Click: Toggle selection");
        ImGui::BulletText("Shift + Click: Add to selection");
        ImGui::BulletText("Ctrl+A: Select all");
        
        ImGui::Separator();
        ImGui::Text("Transform");
        ImGui::BulletText("W: Move tool");
        ImGui::BulletText("E: Rotate tool");
        ImGui::BulletText("R: Scale tool");
        
        ImGui::Separator();
        ImGui::Text("File");
        ImGui::BulletText("Ctrl+N: New scene");
        ImGui::BulletText("Ctrl+O: Open scene");
        ImGui::BulletText("Ctrl+S: Save scene");
        ImGui::BulletText("Ctrl+Shift+S: Save scene as");
        
        ImGui::Separator();
        ImGui::Text("Edit");
        ImGui::BulletText("Ctrl+D: Duplicate");
        ImGui::BulletText("Delete: Delete selection");
        
        ImGui::Spacing();
        if (ImGui::Button("Close", ImVec2(120, 0))) {
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
    
    // Preferences modal
    if (m_ShowPreferencesModal) {
        ImGui::OpenPopup("Preferences");
        m_ShowPreferencesModal = false;
    }
    
    if (ImGui::BeginPopupModal("Preferences", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::Text("Camera Settings");
        // These would be saved to a config file
        static float moveSpeed = 5.0f;
        static float rotateSpeed = 0.3f;
        ImGui::SliderFloat("Move Speed", &moveSpeed, 0.1f, 20.0f);
        ImGui::SliderFloat("Rotate Speed", &rotateSpeed, 0.01f, 1.0f);
        
        ImGui::Separator();
        ImGui::Text("UI Settings");
        static float uiScale = 1.0f;
        ImGui::SliderFloat("UI Scale", &uiScale, 0.5f, 2.0f);

        ImGui::Separator();
        ImGui::Text("Graphics");
        ImGui::TextDisabled("Changing GPU requires restart");

        // Enumerate Vulkan physical devices (by name)
        static std::vector<std::string> gpuNames;
        static int gpuIndex = 0;
        static bool gpuListInit = false;
        static bool gpuSaved = false;

        if (!gpuListInit && m_Context && m_Context->GetInstance() != VK_NULL_HANDLE) {
            gpuNames.clear();
            gpuNames.push_back("Auto (prefer discrete)");

            uint32_t count = 0;
            vkEnumeratePhysicalDevices(m_Context->GetInstance(), &count, nullptr);
            std::vector<VkPhysicalDevice> devs(count);
            if (count > 0) {
                vkEnumeratePhysicalDevices(m_Context->GetInstance(), &count, devs.data());
                for (auto d : devs) {
                    VkPhysicalDeviceProperties p{};
                    vkGetPhysicalDeviceProperties(d, &p);
                    gpuNames.emplace_back(p.deviceName);
                }
            }

            // Load current setting
            const EditorSettings s = EditorSettings::Load();
            gpuIndex = 0;
            if (!s.preferredGpuName.empty()) {
                for (int i = 1; i < (int)gpuNames.size(); ++i) {
                    if (gpuNames[i] == s.preferredGpuName) {
                        gpuIndex = i;
                        break;
                    }
                }
            }

            gpuListInit = true;
            gpuSaved = false;
        }

        if (!gpuNames.empty()) {
            std::vector<const char*> items;
            items.reserve(gpuNames.size());
            for (auto& s : gpuNames) items.push_back(s.c_str());

            ImGui::Combo("Preferred GPU", &gpuIndex, items.data(), (int)items.size());

            if (ImGui::Button("Save GPU Preference")) {
                EditorSettings s = EditorSettings::Load();
                if (gpuIndex <= 0) s.preferredGpuName.clear();
                else s.preferredGpuName = gpuNames[gpuIndex];
                s.Save();
                gpuSaved = true;
            }

            if (gpuSaved) {
                ImGui::SameLine();
                ImGui::TextColored(ImVec4(0.2f, 0.8f, 0.2f, 1.0f), "Saved. Restart to apply.");
            }
        } else {
            ImGui::TextDisabled("GPU list unavailable");
        }
        
        ImGui::Spacing();
        if (ImGui::Button("Close", ImVec2(120, 0))) {
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
}

void EditorUI::HandleGlobalShortcuts() {
    ImGuiIO& io = ImGui::GetIO();
    
    // Don't process shortcuts if typing in a text field
    if (io.WantTextInput) {
        return;
    }
    
    // Ctrl+Z - Undo
    if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_Z) && !io.KeyShift) {
        if (UndoStack::Get().CanUndo()) {
            UndoStack::Get().Undo();
        }
    }
    
    // Ctrl+Y or Ctrl+Shift+Z - Redo
    if ((io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_Y)) ||
        (io.KeyCtrl && io.KeyShift && ImGui::IsKeyPressed(ImGuiKey_Z))) {
        if (UndoStack::Get().CanRedo()) {
            UndoStack::Get().Redo();
        }
    }
    
    // Delete - Delete selected entities
    if (ImGui::IsKeyPressed(ImGuiKey_Delete) && !m_SelectedEntities.empty()) {
        // Create undo command for delete
        // TODO: Implement delete with undo
        for (auto id : m_SelectedEntities) {
            if (m_Scene) {
                m_Scene->DestroyEntity(m_Scene->GetEntity(id));
            }
        }
        ClearSelection();
        m_SceneDirty = true;
    }
}

} // namespace lucent
