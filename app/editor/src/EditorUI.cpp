#include "EditorUI.h"
#include "SceneIO.h"
#include "Win32FileDialogs.h"
#include "EditorSettings.h"
#include "UndoStack.h"
#include "EditorIcons.h"
#include "lucent/gfx/VulkanContext.h"
#include "lucent/gfx/Device.h"
#include "lucent/gfx/Renderer.h"
#include "lucent/gfx/EnvironmentMapLibrary.h"
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

static ImVec4 WithAlpha(ImVec4 c, float a) {
    c.w = a;
    return c;
}

static ImVec4 MulRGB(ImVec4 c, float m) {
    c.x *= m;
    c.y *= m;
    c.z *= m;
    return c;
}

static ImVec4 ThemeAccent() {
    // Single source of truth for "accent" usage across the editor.
    // `SetupStyle()` assigns this to `ImGuiCol_CheckMark`.
    return ImGui::GetStyle().Colors[ImGuiCol_CheckMark];
}

static ImVec4 ThemeSuccess() { return ImVec4(0.33f, 0.78f, 0.47f, 1.0f); }
static ImVec4 ThemeWarning() { return ImVec4(0.95f, 0.70f, 0.28f, 1.0f); }
static ImVec4 ThemeError() { return ImVec4(0.92f, 0.34f, 0.34f, 1.0f); }
static ImVec4 ThemeMutedText() { return ImGui::GetStyle().Colors[ImGuiCol_TextDisabled]; }

static std::filesystem::path GetExecutableDir() {
#if defined(_WIN32)
    wchar_t buf[MAX_PATH]{};
    DWORD len = GetModuleFileNameW(nullptr, buf, MAX_PATH);
    if (len == 0 || len == MAX_PATH) {
        return std::filesystem::current_path();
    }
    std::filesystem::path p(buf);
    return p.parent_path();
#else
    return std::filesystem::current_path();
#endif
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
    
    SetupFonts();
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

void EditorUI::SetupFonts() {
    ImGuiIO& io = ImGui::GetIO();
    m_IconFontLoaded = false;
    
    // Look for fonts in both:
    // - alongside the executable (packaged builds)
    // - the current working directory (VS debugging uses project root)
    const std::filesystem::path exeFontsDir = GetExecutableDir() / "Assets" / "Fonts";
    const std::filesystem::path cwdFontsDir = std::filesystem::current_path() / "Assets" / "Fonts";
    
    const std::filesystem::path uiFontPathA = exeFontsDir / "Roboto.ttf";
    const std::filesystem::path uiFontPathB = cwdFontsDir / "Roboto.ttf";
    const std::filesystem::path iconFontPathA = exeFontsDir / "fa-solid-900.ttf";
    const std::filesystem::path iconFontPathB = cwdFontsDir / "fa-solid-900.ttf";
    
    const std::filesystem::path uiFontPath = std::filesystem::exists(uiFontPathA) ? uiFontPathA : uiFontPathB;
    const std::filesystem::path iconFontPath = std::filesystem::exists(iconFontPathA) ? iconFontPathA : iconFontPathB;
    
    // Base UI font
    ImFont* baseFont = nullptr;
    if (!uiFontPath.empty() && std::filesystem::exists(uiFontPath)) {
        baseFont = io.Fonts->AddFontFromFileTTF(uiFontPath.string().c_str(), 16.0f);
    }
    if (!baseFont) {
        baseFont = io.Fonts->AddFontDefault();
    }
    io.FontDefault = baseFont;
    
    // Optional icon pack: merge into the base font so icons can be used inline with text.
    // Font Awesome solid sits mostly in U+F000..U+F8FF.
    if (!iconFontPath.empty() && std::filesystem::exists(iconFontPath)) {
        static const ImWchar iconRanges[] = { 0xF000, 0xF8FF, 0 };
        
        ImFontConfig iconConfig{};
        iconConfig.MergeMode = true;
        iconConfig.PixelSnapH = true;
        iconConfig.GlyphMinAdvanceX = 13.0f; // helps align icon glyph width
        
        ImFont* icons = io.Fonts->AddFontFromFileTTF(iconFontPath.string().c_str(), 16.0f, &iconConfig, iconRanges);
        m_IconFontLoaded = (icons != nullptr);
    }
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
    // Modern Flat Dark Theme - Neutral surfaces + subtle blue accent
    // ========================================================================
    
    // Base colors
    const ImVec4 bg_dark      = ImVec4(0.060f, 0.062f, 0.070f, 1.0f);  // ~#0F1012
    const ImVec4 bg_main      = ImVec4(0.086f, 0.090f, 0.102f, 1.0f);  // ~#16171A
    const ImVec4 bg_light     = ImVec4(0.110f, 0.114f, 0.132f, 1.0f);  // ~#1C1D22
    const ImVec4 bg_lighter   = ImVec4(0.142f, 0.146f, 0.168f, 1.0f);  // ~#24262B
    const ImVec4 border       = ImVec4(0.220f, 0.225f, 0.252f, 1.0f);  // ~#383941
    
    // Text colors
    const ImVec4 text_bright  = ImVec4(0.95f, 0.95f, 0.95f, 1.0f);
    const ImVec4 text_normal  = ImVec4(0.78f, 0.78f, 0.80f, 1.0f);
    const ImVec4 text_dim     = ImVec4(0.50f, 0.50f, 0.55f, 1.0f);
    
    // Accent color - Subtle blue (used across selection, highlights, and UI affordances)
    const ImVec4 accent       = ImVec4(0.31f, 0.64f, 0.98f, 1.0f);     // ~#4FA3FA
    const ImVec4 accent_hover = ImVec4(0.39f, 0.71f, 1.00f, 1.0f);
    const ImVec4 accent_dim   = ImVec4(0.22f, 0.46f, 0.74f, 1.0f);
    
    // Secondary accent - Warm amber for warnings/attention
    const ImVec4 highlight    = ThemeWarning();     // ~#F2B247
    
    // Backgrounds
    colors[ImGuiCol_WindowBg]             = bg_main;
    colors[ImGuiCol_ChildBg]              = WithAlpha(bg_dark, 0.55f);
    colors[ImGuiCol_PopupBg]              = WithAlpha(bg_light, 0.98f);
    colors[ImGuiCol_Border]               = WithAlpha(border, 0.75f);
    colors[ImGuiCol_BorderShadow]         = ImVec4(0.0f, 0.0f, 0.0f, 0.0f);
    
    // Text
    colors[ImGuiCol_Text]                 = text_normal;
    colors[ImGuiCol_TextDisabled]         = text_dim;
    
    // Headers (collapsing headers, tree nodes)
    colors[ImGuiCol_Header]               = WithAlpha(accent, 0.12f);
    colors[ImGuiCol_HeaderHovered]        = WithAlpha(accent, 0.20f);
    colors[ImGuiCol_HeaderActive]         = WithAlpha(accent, 0.26f);
    
    // Buttons
    colors[ImGuiCol_Button]               = bg_lighter;
    colors[ImGuiCol_ButtonHovered]        = WithAlpha(accent, 0.22f);
    colors[ImGuiCol_ButtonActive]         = WithAlpha(accent, 0.32f);
    
    // Frame backgrounds (input fields, checkboxes)
    colors[ImGuiCol_FrameBg]              = bg_dark;
    colors[ImGuiCol_FrameBgHovered]       = bg_light;
    colors[ImGuiCol_FrameBgActive]        = WithAlpha(accent, 0.14f);
    
    // Tabs
    colors[ImGuiCol_Tab]                  = bg_light;
    colors[ImGuiCol_TabHovered]           = WithAlpha(accent, 0.28f);
    colors[ImGuiCol_TabActive]            = bg_lighter;
    colors[ImGuiCol_TabUnfocused]         = bg_dark;
    colors[ImGuiCol_TabUnfocusedActive]   = bg_light;
    
    // Title bars
    colors[ImGuiCol_TitleBg]              = bg_dark;
    colors[ImGuiCol_TitleBgActive]        = bg_light;
    colors[ImGuiCol_TitleBgCollapsed]     = bg_dark;
    
    // Scrollbar
    colors[ImGuiCol_ScrollbarBg]          = bg_dark;
    colors[ImGuiCol_ScrollbarGrab]        = bg_lighter;
    colors[ImGuiCol_ScrollbarGrabHovered] = WithAlpha(accent, 0.25f);
    colors[ImGuiCol_ScrollbarGrabActive]  = WithAlpha(accent, 0.35f);
    
    // Slider
    colors[ImGuiCol_SliderGrab]           = accent_dim;
    colors[ImGuiCol_SliderGrabActive]     = accent;
    
    // Checkmarks and selection
    colors[ImGuiCol_CheckMark]            = accent;
    
    // Separators
    colors[ImGuiCol_Separator]            = WithAlpha(border, 0.55f);
    colors[ImGuiCol_SeparatorHovered]     = WithAlpha(accent, 0.45f);
    colors[ImGuiCol_SeparatorActive]      = WithAlpha(accent_hover, 0.55f);
    
    // Resize grips
    colors[ImGuiCol_ResizeGrip]           = WithAlpha(accent, 0.00f);
    colors[ImGuiCol_ResizeGripHovered]    = WithAlpha(accent, 0.18f);
    colors[ImGuiCol_ResizeGripActive]     = WithAlpha(accent, 0.28f);
    
    // Docking
    colors[ImGuiCol_DockingPreview]       = WithAlpha(accent, 0.45f);
    colors[ImGuiCol_DockingEmptyBg]       = bg_main;
    
    // Menu bar
    colors[ImGuiCol_MenuBarBg]            = bg_main;
    
    // Tables
    colors[ImGuiCol_TableHeaderBg]        = bg_lighter;
    colors[ImGuiCol_TableBorderStrong]    = border;
    colors[ImGuiCol_TableBorderLight]     = WithAlpha(border, 0.5f);
    colors[ImGuiCol_TableRowBg]           = ImVec4(0.0f, 0.0f, 0.0f, 0.0f);
    colors[ImGuiCol_TableRowBgAlt]        = ImVec4(1.0f, 1.0f, 1.0f, 0.02f);
    
    // Text selection
    colors[ImGuiCol_TextSelectedBg]       = WithAlpha(accent, 0.28f);
    
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
    // Style Settings - Flat, clean, consistent spacing
    // ========================================================================
    
    // Rounding - subtle (modern flat, not boxy)
    style.WindowRounding    = 4.0f;
    style.ChildRounding     = 4.0f;
    style.FrameRounding     = 3.0f;
    style.PopupRounding     = 4.0f;
    style.ScrollbarRounding = 6.0f;
    style.GrabRounding      = 3.0f;
    style.TabRounding       = 3.0f;
    
    // Padding and spacing - slightly tighter (editor-friendly)
    style.WindowPadding     = ImVec2(10, 10);
    style.FramePadding      = ImVec2(8, 5);
    style.CellPadding       = ImVec2(8, 4);
    style.ItemSpacing       = ImVec2(8, 6);
    style.ItemInnerSpacing  = ImVec2(6, 4);
    style.TouchExtraPadding = ImVec2(0, 0);
    style.IndentSpacing     = 20.0f;
    style.ScrollbarSize     = 12.0f;
    style.GrabMinSize       = 12.0f;
    
    // Borders
    style.WindowBorderSize  = 0.0f;
    style.ChildBorderSize   = 0.0f;
    style.PopupBorderSize   = 0.0f;
    style.FrameBorderSize   = 1.0f;
    style.TabBorderSize     = 0.0f;
    
    style.DisabledAlpha     = 0.55f;
    
    // Alignment
    style.WindowTitleAlign  = ImVec2(0.0f, 0.5f); // Left-align titles (more standard for editors)
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
    
    // Draw render preview window (if requested)
    DrawRenderPreviewWindow(&m_ShowRenderPreview);
    
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

void EditorUI::SetRenderPreviewTexture(VkImageView view, VkSampler sampler) {
    // Remove old descriptor if exists
    if (m_RenderPreviewDescriptor != VK_NULL_HANDLE) {
        // ImGui will handle cleanup
    }
    
    // Create new descriptor for the render preview texture
    m_RenderPreviewDescriptor = ImGui_ImplVulkan_AddTexture(sampler, view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
}

void EditorUI::DrawRenderPreviewWindow(bool* pOpen) {
    if (!pOpen || !*pOpen) return;
    
    if (!m_Renderer) return;
    
    gfx::FinalRender* finalRender = m_Renderer->GetFinalRender();
    if (!finalRender) {
        ImGui::Begin("Render Preview", pOpen);
        ImGui::TextDisabled("Final render is not available in this build.");
        ImGui::End();
        return;
    }
    
    if (m_RenderPreviewJustOpened) {
        ImGuiViewport* vp = ImGui::GetMainViewport();
        ImGui::SetNextWindowPos(ImVec2(vp->WorkPos.x + vp->WorkSize.x * 0.5f, vp->WorkPos.y + vp->WorkSize.y * 0.5f),
                                ImGuiCond_Always, ImVec2(0.5f, 0.5f));
        ImGui::SetNextWindowFocus();
        m_RenderPreviewJustOpened = false;
    }
    ImGui::SetNextWindowSize(ImVec2(900, 650), ImGuiCond_FirstUseEver);
    if (ImGui::Begin("Render Preview", pOpen)) {
        gfx::FinalRenderStatus status = finalRender->GetStatus();
        
        // Status header
        ImGui::Text("Status: ");
        ImGui::SameLine();
        switch (status) {
            case gfx::FinalRenderStatus::Rendering:
                ImGui::TextColored(ThemeAccent(), "Rendering...");
                break;
            case gfx::FinalRenderStatus::Completed:
                ImGui::TextColored(ThemeSuccess(), "Completed");
                break;
            case gfx::FinalRenderStatus::Failed:
                ImGui::TextColored(ThemeError(), "Failed");
                break;
            case gfx::FinalRenderStatus::Cancelled:
                ImGui::TextColored(ThemeWarning(), "Cancelled");
                break;
            default:
                ImGui::TextDisabled("Idle");
                break;
        }
        
        // Progress bar
        if (status == gfx::FinalRenderStatus::Rendering) {
            ImGui::ProgressBar(finalRender->GetProgress(), ImVec2(-1, 0));
            ImGui::Text("Samples: %u / %u", finalRender->GetCurrentSample(), finalRender->GetTotalSamples());
            ImGui::Text("Time: %.2f seconds", finalRender->GetElapsedTime());
            
            if (ImGui::Button("Cancel Render")) {
                finalRender->Cancel();
            }
        }
        
        ImGui::Separator();
        
        // Display render image
        gfx::Image* renderImage = finalRender->GetRenderImage();
        if (renderImage && renderImage->GetView() != VK_NULL_HANDLE && m_RenderPreviewDescriptor != VK_NULL_HANDLE) {
            ImVec2 availSize = ImGui::GetContentRegionAvail();
            
            // Maintain aspect ratio
            uint32_t imgWidth = renderImage->GetWidth();
            uint32_t imgHeight = renderImage->GetHeight();
            if (imgWidth > 0 && imgHeight > 0) {
                float aspect = (float)imgWidth / (float)imgHeight;
                ImVec2 displaySize = availSize;
                if (availSize.x / availSize.y > aspect) {
                    displaySize.x = availSize.y * aspect;
                } else {
                    displaySize.y = availSize.x / aspect;
                }
                
                ImGui::Image((ImTextureID)m_RenderPreviewDescriptor, displaySize);
            }
        } else {
            ImGui::TextDisabled("No render image available.");
        }
        
        // Export button
        if (status == gfx::FinalRenderStatus::Completed) {
            ImGui::Separator();
            static char outputPath[256] = "render.png";
            ImGui::InputText("Output Path", outputPath, sizeof(outputPath));
            if (ImGui::Button("Save Render")) {
                finalRender->ExportImage(outputPath);
            }
        }
    }
    ImGui::End();
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
        ImGui::PushStyleColor(ImGuiCol_Text, ThemeAccent());
        ImGui::Text("LUCENT");
        ImGui::PopStyleColor();
        ImGui::Separator();
        
        if (ImGui::BeginMenu("File")) {
            if (ImGui::MenuItem(m_IconFontLoaded ? (LUCENT_ICON_FILE " New Scene") : "New Scene", "Ctrl+N")) {
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
                    m_Scene->SetEnvironmentMapPath("");
                    ApplySceneEnvironment();
                    
                    ClearSelection();
                    m_CurrentScenePath.clear();
                    m_SceneDirty = false;
                }
            }
            if (ImGui::MenuItem(m_IconFontLoaded ? (LUCENT_ICON_OPEN " Open Scene...") : "Open Scene...", "Ctrl+O")) {
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
                            ApplySceneEnvironment();
                        } else {
                            Win32FileDialogs::ShowError(L"Error", L"Failed to load scene file.");
                        }
                    }
                }
            }
            ImGui::Separator();
            if (ImGui::MenuItem(m_IconFontLoaded ? (LUCENT_ICON_SAVE " Save Scene") : "Save Scene", "Ctrl+S")) {
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
            if (ImGui::MenuItem(m_IconFontLoaded ? (LUCENT_ICON_SAVE " Save Scene As...") : "Save Scene As...", "Ctrl+Shift+S")) {
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
            if (ImGui::MenuItem(m_IconFontLoaded ? (LUCENT_ICON_IMPORT " Import...") : "Import...")) {
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
            if (ImGui::MenuItem(m_IconFontLoaded ? (LUCENT_ICON_IMPORT " Import glTF...") : "Import glTF...")) {
                if (!m_Scene || !m_Device) {
                    Win32FileDialogs::ShowError(L"Import glTF", L"Scene or device not initialized.");
                } else {
                    std::string path = Win32FileDialogs::OpenFile(
                        L"Import glTF/GLB",
                        {{L"glTF", L"*.gltf;*.glb"}, {L"All Files", L"*.*"}},
                        L"gltf"
                    );
                    if (!path.empty()) {
                        int added = SceneIO::ImportGLTF(m_Scene, m_Device, path);
                        if (added < 0) {
                            LUCENT_CORE_ERROR("Import glTF failed: {}", SceneIO::GetLastError());
                            Win32FileDialogs::ShowError(L"Import glTF", L"Import failed. See console for details.");
                        } else {
                            m_SceneDirty = true;
                            if (m_Renderer) m_Renderer->GetSettings().MarkDirty();
                            LUCENT_CORE_INFO("Imported {} entities from glTF: {}", added, path);
                        }
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
            if (ImGui::MenuItem(m_IconFontLoaded ? (LUCENT_ICON_TRASH " Exit") : "Exit", "Alt+F4")) {
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
            if (m_IconFontLoaded) {
                undoLabel = std::string(LUCENT_ICON_UNDO " ") + undoLabel;
                redoLabel = std::string(LUCENT_ICON_REDO " ") + redoLabel;
            }
            
            if (ImGui::MenuItem(undoLabel.c_str(), "Ctrl+Z", false, undoStack.CanUndo())) {
                undoStack.Undo();
            }
            if (ImGui::MenuItem(redoLabel.c_str(), "Ctrl+Y", false, undoStack.CanRedo())) {
                undoStack.Redo();
            }
            ImGui::Separator();
            if (ImGui::MenuItem(m_IconFontLoaded ? (LUCENT_ICON_CUT " Cut") : "Cut", "Ctrl+X", false, !m_SelectedEntities.empty())) {
                // Copy to clipboard then delete
                m_Clipboard.clear();
                for (auto id : m_SelectedEntities) {
                    if (m_Scene) {
                        scene::Entity src = m_Scene->GetEntity(id);
                        if (!src.IsValid()) continue;
                        
                        ClipboardEntity clip;
                        auto* tag = src.GetComponent<scene::TagComponent>();
                        clip.name = tag ? tag->name : "Entity";
                        
                        if (auto* t = src.GetComponent<scene::TransformComponent>()) {
                            clip.transform = *t;
                        }
                        if (auto* c = src.GetComponent<scene::CameraComponent>()) {
                            clip.camera = *c;
                        }
                        if (auto* l = src.GetComponent<scene::LightComponent>()) {
                            clip.light = *l;
                        }
                        if (auto* m = src.GetComponent<scene::MeshRendererComponent>()) {
                            clip.meshRenderer = *m;
                        }
                        m_Clipboard.push_back(clip);
                        m_Scene->DestroyEntity(src);
                    }
                }
                ClearSelection();
                m_SceneDirty = true;
            }
            if (ImGui::MenuItem(m_IconFontLoaded ? (LUCENT_ICON_COPY " Copy") : "Copy", "Ctrl+C", false, !m_SelectedEntities.empty())) {
                m_Clipboard.clear();
                for (auto id : m_SelectedEntities) {
                    if (m_Scene) {
                        scene::Entity src = m_Scene->GetEntity(id);
                        if (!src.IsValid()) continue;
                        
                        ClipboardEntity clip;
                        auto* tag = src.GetComponent<scene::TagComponent>();
                        clip.name = tag ? tag->name : "Entity";
                        
                        if (auto* t = src.GetComponent<scene::TransformComponent>()) {
                            clip.transform = *t;
                        }
                        if (auto* c = src.GetComponent<scene::CameraComponent>()) {
                            clip.camera = *c;
                        }
                        if (auto* l = src.GetComponent<scene::LightComponent>()) {
                            clip.light = *l;
                        }
                        if (auto* m = src.GetComponent<scene::MeshRendererComponent>()) {
                            clip.meshRenderer = *m;
                        }
                        m_Clipboard.push_back(clip);
                    }
                }
            }
            if (ImGui::MenuItem(m_IconFontLoaded ? (LUCENT_ICON_PASTE " Paste") : "Paste", "Ctrl+V", false, !m_Clipboard.empty())) {
                if (m_Scene) {
                    std::vector<scene::Entity> newEntities;
                    for (const auto& clip : m_Clipboard) {
                        scene::Entity ent = m_Scene->CreateEntity(clip.name + " (Pasted)");
                        
                        // Apply transform with offset
                        if (auto* t = ent.GetComponent<scene::TransformComponent>()) {
                            *t = clip.transform;
                            t->position += glm::vec3(1.0f, 0.0f, 0.0f); // Offset
                        }
                        
                        if (clip.camera) {
                            ent.AddComponent<scene::CameraComponent>() = *clip.camera;
                        }
                        if (clip.light) {
                            ent.AddComponent<scene::LightComponent>() = *clip.light;
                        }
                        if (clip.meshRenderer) {
                            ent.AddComponent<scene::MeshRendererComponent>() = *clip.meshRenderer;
                        }
                        newEntities.push_back(ent);
                    }
                    ClearSelection();
                    for (auto& e : newEntities) {
                        AddToSelection(e);
                    }
                    m_SceneDirty = true;
                }
            }
            if (ImGui::MenuItem(m_IconFontLoaded ? (LUCENT_ICON_DUPLICATE " Duplicate") : "Duplicate", "Ctrl+D", false, !m_SelectedEntities.empty())) {
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
            if (ImGui::MenuItem(m_IconFontLoaded ? (LUCENT_ICON_TRASH " Delete") : "Delete", "Del", false, !m_SelectedEntities.empty())) {
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
            if (ImGui::MenuItem(m_IconFontLoaded ? (LUCENT_ICON_SETTINGS " Preferences...") : "Preferences...")) {
                m_ShowPreferencesModal = true;
            }
            ImGui::EndMenu();
        }
        
        if (ImGui::BeginMenu("Create")) {
            ImGui::TextDisabled("Primitives");
            if (ImGui::MenuItem(m_IconFontLoaded ? (LUCENT_ICON_CUBE " Cube") : "Cube")) {
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
            if (ImGui::MenuItem(m_IconFontLoaded ? (LUCENT_ICON_LIGHT " Point Light") : "Point Light")) {
                auto e = m_Scene->CreateEntity("Point Light");
                auto& l = e.AddComponent<scene::LightComponent>();
                l.type = scene::LightType::Point;
            }
            if (ImGui::MenuItem(m_IconFontLoaded ? (LUCENT_ICON_LIGHT " Directional Light") : "Directional Light")) {
                auto e = m_Scene->CreateEntity("Directional Light");
                auto& l = e.AddComponent<scene::LightComponent>();
                l.type = scene::LightType::Directional;
            }
            if (ImGui::MenuItem(m_IconFontLoaded ? (LUCENT_ICON_LIGHT " Spot Light") : "Spot Light")) {
                auto e = m_Scene->CreateEntity("Spot Light");
                auto& l = e.AddComponent<scene::LightComponent>();
                l.type = scene::LightType::Spot;
            }
            ImGui::Separator();
            if (ImGui::MenuItem(m_IconFontLoaded ? (LUCENT_ICON_CAMERA " Camera") : "Camera")) {
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
            if (ImGui::MenuItem(m_IconFontLoaded ? (LUCENT_ICON_HELP " Documentation") : "Documentation")) {
                // Open docs folder in explorer
                ShellExecuteW(NULL, L"explore", L"docs", NULL, NULL, SW_SHOWNORMAL);
            }
            if (ImGui::MenuItem(m_IconFontLoaded ? (LUCENT_ICON_INFO " Keyboard Shortcuts") : "Keyboard Shortcuts")) {
                m_ShowShortcutsModal = true;
            }
            ImGui::Separator();
            if (ImGui::MenuItem(m_IconFontLoaded ? (LUCENT_ICON_INFO " About Lucent") : "About Lucent")) {
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
        
        // Handle drag-drop onto viewport
        if (ImGui::BeginDragDropTarget()) {
            if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("MATERIAL_PATH")) {
                std::string materialPath(static_cast<const char*>(payload->Data));
                HandleMaterialDrop(materialPath);
            }
            if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("MODEL_PATH")) {
                if (m_Scene && m_Device) {
                    std::string modelPath(static_cast<const char*>(payload->Data));
                    int added = SceneIO::ImportModel(m_Scene, m_Device, modelPath);
                    if (added < 0) {
                        LUCENT_CORE_ERROR("Import model failed: {}", SceneIO::GetLastError());
                        Win32FileDialogs::ShowError(L"Import Model", L"Import failed. See console for details.");
                    } else {
                        m_SceneDirty = true;
                        if (m_Renderer) m_Renderer->GetSettings().MarkDirty();
                        LUCENT_CORE_INFO("Imported {} entities from model: {}", added, modelPath);
                    }
                }
            }
            ImGui::EndDragDropTarget();
        }
    } else {
        ImGui::Text("Viewport not available");
    }
    
    // Draw gizmo if entity selected (only in Object Mode)
    if (m_EditorMode == EditorMode::Object) {
        DrawGizmo();
    }
    
    // Handle viewport click for selection (after gizmo so gizmo takes priority)
    if (m_EditorMode == EditorMode::Object) {
        HandleViewportClick();
    } else {
        HandleEditModeClick();
    }
    
    // Draw Edit Mode overlay (vertices, edges, faces)
    DrawEditModeOverlay();

    // Draw scene indicators (lights/cameras) as 2D overlay projected from world space
    DrawEntityIndicators();
    
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
    
    // Editor Mode indicator (second row)
    ImGui::SetCursorPos(ImVec2(10, 60));
    
    if (m_EditorMode == EditorMode::Object) {
        ImGui::TextColored(ImVec4(0.7f, 0.8f, 1.0f, 1.0f), "Object Mode");
        ImGui::SameLine();
        ImGui::TextDisabled("(Tab to Edit)");
    } else {
        ImGui::TextColored(ImVec4(1.0f, 0.7f, 0.3f, 1.0f), "Edit Mode");
        ImGui::SameLine();
        
        // Selection mode buttons
        const char* modes[] = { "Vertex", "Edge", "Face" };
        const char* keys[] = { "1", "2", "3" };
        for (int i = 0; i < 3; i++) {
            ImGui::SameLine();
            bool selected = (static_cast<int>(m_MeshSelectMode) == i);
            if (selected) ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.6f, 0.4f, 0.2f, 1.0f));
            char label[32];
            snprintf(label, sizeof(label), "[%s] %s", keys[i], modes[i]);
            if (ImGui::SmallButton(label)) {
                m_MeshSelectMode = static_cast<MeshSelectMode>(i);
            }
            if (selected) ImGui::PopStyleColor();
        }
        
        ImGui::SameLine();
        ImGui::TextDisabled("(Tab to exit)");
    }
    
    // Draw interactive transform HUD
    DrawInteractiveTransformHUD();
    
    // Update interactive transform if active
    if (IsInInteractiveTransform()) {
        UpdateInteractiveTransform();
    }
    
    ImGui::PopStyleVar(2);
    
    ImGui::End();
    ImGui::PopStyleVar();
}

// ============================================================================
// Scene Indicators (lights/cameras)
// ============================================================================

namespace {

inline bool IsOnScreen01(float z) {
    return z >= 0.0f && z <= 1.0f;
}

inline ImU32 MulAlpha(ImU32 c, float a01) {
    const ImU32 a = (c >> IM_COL32_A_SHIFT) & 0xFF;
    const ImU32 na = (ImU32)glm::clamp((float)a * a01, 0.0f, 255.0f);
    return (c & ~(0xFFu << IM_COL32_A_SHIFT)) | (na << IM_COL32_A_SHIFT);
}

} // namespace

void EditorUI::DrawEntityIndicators() {
    if (!m_ShowIndicators) return;
    if (!m_Scene || !m_EditorCamera) return;
    if (m_ViewportSize.x <= 1.0f || m_ViewportSize.y <= 1.0f) return;

    // Draw in the viewport window, not the global foreground layer (prevents drawing over UI).
    ImDrawList* drawList = ImGui::GetWindowDrawList();
    const ImVec2 clipMin(m_ViewportPosition.x, m_ViewportPosition.y);
    const ImVec2 clipMax(m_ViewportPosition.x + m_ViewportSize.x, m_ViewportPosition.y + m_ViewportSize.y);
    drawList->PushClipRect(clipMin, clipMax, true);

    const float aspect = (m_ViewportSize.y > 0.0f) ? (m_ViewportSize.x / m_ViewportSize.y) : 1.0f;

    auto drawLine3D = [&](const glm::vec3& a, const glm::vec3& b, ImU32 color, float thickness) {
        glm::vec3 sa = WorldToScreen(a);
        glm::vec3 sb = WorldToScreen(b);
        // Keep overlays stable: only discard if behind camera. Allow partially clipped segments.
        if (sa.z < 0.0f || sb.z < 0.0f) return;
        drawList->AddLine(ImVec2(sa.x, sa.y), ImVec2(sb.x, sb.y), color, thickness);
    };

    auto drawCircle3D = [&](const glm::vec3& center, const glm::vec3& axisX, const glm::vec3& axisY,
                            float radius, ImU32 color, int segments, float thickness) {
        if (segments < 8) segments = 8;
        glm::vec3 ax = glm::normalize(axisX);
        glm::vec3 ay = glm::normalize(axisY);
        glm::vec3 prev = center + radius * ax;
        for (int i = 1; i <= segments; ++i) {
            float t = (float)i / (float)segments;
            float ang = t * glm::two_pi<float>();
            glm::vec3 p = center + radius * (std::cos(ang) * ax + std::sin(ang) * ay);
            drawLine3D(prev, p, color, thickness);
            prev = p;
        }
    };

    auto drawWireSphere = [&](const glm::vec3& center, float radius, ImU32 color, float thickness) {
        const int seg = 48;
        // 3 great circles in world axes
        drawCircle3D(center, glm::vec3(1, 0, 0), glm::vec3(0, 1, 0), radius, MulAlpha(color, 0.85f), seg, thickness);
        drawCircle3D(center, glm::vec3(1, 0, 0), glm::vec3(0, 0, 1), radius, MulAlpha(color, 0.70f), seg, thickness);
        drawCircle3D(center, glm::vec3(0, 1, 0), glm::vec3(0, 0, 1), radius, MulAlpha(color, 0.70f), seg, thickness);
    };

    auto drawArrow = [&](const glm::vec3& origin, const glm::vec3& dir, float length, ImU32 color) {
        glm::vec3 d = glm::normalize(dir);
        glm::vec3 tip = origin + d * length;
        drawLine3D(origin, tip, color, 2.0f);

        // Arrow head (simple V)
        glm::vec3 up = glm::abs(glm::dot(d, glm::vec3(0, 1, 0))) > 0.95f ? glm::vec3(1, 0, 0) : glm::vec3(0, 1, 0);
        glm::vec3 right = glm::normalize(glm::cross(d, up));
        glm::vec3 headUp = glm::normalize(glm::cross(right, d));
        float headLen = length * 0.12f;
        float headWid = length * 0.06f;
        drawLine3D(tip, tip - d * headLen + right * headWid, color, 2.0f);
        drawLine3D(tip, tip - d * headLen - right * headWid, color, 2.0f);
        drawLine3D(tip, tip - d * headLen + headUp * headWid, color, 2.0f);
        drawLine3D(tip, tip - d * headLen - headUp * headWid, color, 2.0f);
    };

    auto drawWireCone = [&](const glm::vec3& apex, const glm::vec3& forward, const glm::vec3& right, const glm::vec3& up,
                            float length, float outerAngleDeg, ImU32 color) {
        if (length <= 0.001f) return;
        float ang = glm::radians(glm::clamp(outerAngleDeg, 0.1f, 89.0f));
        float radius = std::tan(ang) * length;
        glm::vec3 f = glm::normalize(forward);
        glm::vec3 r = glm::normalize(right);
        glm::vec3 u = glm::normalize(up);
        glm::vec3 baseCenter = apex + f * length;

        const int seg = 40;
        // Base circle
        drawCircle3D(baseCenter, r, u, radius, MulAlpha(color, 0.85f), seg, 1.6f);

        // Side lines (apex -> base)
        for (int i = 0; i < 8; ++i) {
            float t = (float)i / 8.0f;
            float a = t * glm::two_pi<float>();
            glm::vec3 rim = baseCenter + radius * (std::cos(a) * r + std::sin(a) * u);
            drawLine3D(apex, rim, MulAlpha(color, 0.80f), 1.4f);
        }

        // Direction line
        drawLine3D(apex, baseCenter, MulAlpha(color, 0.90f), 1.8f);
    };

    auto drawCameraFrustum = [&](const glm::vec3& pos, const glm::vec3& fwd, const glm::vec3& right, const glm::vec3& up,
                                 const scene::CameraComponent& cam, ImU32 color) {
        glm::vec3 f = glm::normalize(fwd);
        glm::vec3 r = glm::normalize(right);
        glm::vec3 u = glm::normalize(up);

        const float n = std::max(0.001f, cam.nearClip);
        const float fdist = std::max(n + 0.001f, cam.farClip);

        glm::vec3 nc = pos + f * n;
        glm::vec3 fc = pos + f * fdist;

        glm::vec3 ntr, ntl, nbr, nbl;
        glm::vec3 ftr, ftl, fbr, fbl;

        if (cam.projectionType == scene::CameraComponent::ProjectionType::Perspective) {
            float vFov = glm::radians(glm::clamp(cam.fov, 1.0f, 179.0f));
            float nh = std::tan(vFov * 0.5f) * n;
            float nw = nh * aspect;
            float fh = std::tan(vFov * 0.5f) * fdist;
            float fw = fh * aspect;

            ntl = nc + u * nh - r * nw;
            ntr = nc + u * nh + r * nw;
            nbl = nc - u * nh - r * nw;
            nbr = nc - u * nh + r * nw;

            ftl = fc + u * fh - r * fw;
            ftr = fc + u * fh + r * fw;
            fbl = fc - u * fh - r * fw;
            fbr = fc - u * fh + r * fw;
        } else {
            float oh = std::max(0.001f, cam.orthoSize);
            float ow = oh * aspect;

            ntl = nc + u * oh - r * ow;
            ntr = nc + u * oh + r * ow;
            nbl = nc - u * oh - r * ow;
            nbr = nc - u * oh + r * ow;

            ftl = fc + u * oh - r * ow;
            ftr = fc + u * oh + r * ow;
            fbl = fc - u * oh - r * ow;
            fbr = fc - u * oh + r * ow;
        }

        const ImU32 c = MulAlpha(color, 0.90f);
        const float t = 1.6f;

        // Near plane
        drawLine3D(ntl, ntr, c, t); drawLine3D(ntr, nbr, c, t);
        drawLine3D(nbr, nbl, c, t); drawLine3D(nbl, ntl, c, t);
        // Far plane
        drawLine3D(ftl, ftr, c, t); drawLine3D(ftr, fbr, c, t);
        drawLine3D(fbr, fbl, c, t); drawLine3D(fbl, ftl, c, t);
        // Connectors
        drawLine3D(ntl, ftl, c, t); drawLine3D(ntr, ftr, c, t);
        drawLine3D(nbl, fbl, c, t); drawLine3D(nbr, fbr, c, t);

        // Forward axis (small)
        drawArrow(pos, f, std::min(1.5f, std::max(0.3f, n * 6.0f)), MulAlpha(color, 1.0f));
    };

    // Color palette
    const ImU32 pointColor = IM_COL32(255, 220, 80, 255);
    const ImU32 spotColor  = IM_COL32(255, 170, 80, 255);
    const ImU32 dirColor   = IM_COL32(120, 200, 255, 255);
    const ImU32 camColor   = IM_COL32(160, 255, 170, 255);

    if (m_ShowLightIndicators) {
        auto viewLights = m_Scene->GetView<scene::LightComponent, scene::TransformComponent>();
        viewLights.Each([&](scene::Entity e, scene::LightComponent& light, scene::TransformComponent& tr) {
            if (m_IndicatorsSelectedOnly && !IsSelected(e)) return;

            const glm::vec3 pos = tr.position;
            const glm::vec3 fwd = tr.GetForward();
            const glm::vec3 right = tr.GetRight();
            const glm::vec3 up = tr.GetUp();

            switch (light.type) {
                case scene::LightType::Point: {
                    drawWireSphere(pos, std::max(0.0f, light.range), pointColor, 1.4f);
                    drawList->AddCircleFilled(ImVec2(WorldToScreen(pos).x, WorldToScreen(pos).y), 3.0f, pointColor);
                    break;
                }
                case scene::LightType::Spot: {
                    drawWireCone(pos, fwd, right, up, std::max(0.0f, light.range), light.outerAngle, spotColor);
                    drawList->AddCircleFilled(ImVec2(WorldToScreen(pos).x, WorldToScreen(pos).y), 3.0f, spotColor);
                    break;
                }
                case scene::LightType::Directional: {
                    // Draw an arrow showing direction (longer when selected)
                    float len = IsSelected(e) ? 4.0f : 2.5f;
                    // Match engine convention: directional lights are uploaded as -transform.forward
                    // (see Application.cpp -> gpuLight.direction = -forward).
                    drawArrow(pos, -fwd, len, dirColor);
                    break;
                }
                default:
                    break;
            }
        });
    }

    if (m_ShowCameraIndicators) {
        auto viewCams = m_Scene->GetView<scene::CameraComponent, scene::TransformComponent>();
        viewCams.Each([&](scene::Entity e, scene::CameraComponent& cam, scene::TransformComponent& tr) {
            if (m_CameraIndicatorsSelectedOnly && !IsSelected(e)) return;
            drawCameraFrustum(tr.position, tr.GetForward(), tr.GetRight(), tr.GetUp(), cam, camColor);
        });
    }

    drawList->PopClipRect();
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
        ImGui::TextColored(WithAlpha(ThemeAccent(), 0.9f), "Scene: %s", m_Scene->GetName().c_str());
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
        ImVec4 accent = ThemeAccent();
        ImGui::PushStyleColor(ImGuiCol_Button, WithAlpha(accent, 0.18f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, WithAlpha(accent, 0.26f));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, WithAlpha(accent, 0.34f));
        const char* addEntityLabel = m_IconFontLoaded ? (LUCENT_ICON_PLUS " Add Entity") : "+ Add Entity";
        if (ImGui::Button(addEntityLabel, ImVec2(buttonWidth, 28))) {
            ImGui::OpenPopup("AddEntityPopup");
        }
        ImGui::PopStyleColor(3);
        
        // Add entity popup
        if (ImGui::BeginPopup("AddEntityPopup")) {
            ImGui::TextDisabled("Create New Entity");
            ImGui::Separator();
            
            if (ImGui::MenuItem(m_IconFontLoaded ? (LUCENT_ICON_FILE " Empty") : "Empty")) {
                m_Scene->CreateEntity("New Entity");
            }
            
            ImGui::Separator();
            ImGui::TextDisabled("Primitives");
            
            if (ImGui::MenuItem(m_IconFontLoaded ? (LUCENT_ICON_CUBE " Cube") : "Cube")) {
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
            
            if (ImGui::MenuItem(m_IconFontLoaded ? (LUCENT_ICON_LIGHT " Point Light") : "Point Light")) {
                auto light = m_Scene->CreateEntity("Point Light");
                auto& l = light.AddComponent<scene::LightComponent>();
                l.type = scene::LightType::Point;
            }
            if (ImGui::MenuItem(m_IconFontLoaded ? (LUCENT_ICON_LIGHT " Directional Light") : "Directional Light")) {
                auto light = m_Scene->CreateEntity("Directional Light");
                auto& l = light.AddComponent<scene::LightComponent>();
                l.type = scene::LightType::Directional;
            }
            if (ImGui::MenuItem(m_IconFontLoaded ? (LUCENT_ICON_CAMERA " Camera") : "Camera")) {
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
    
    if (m_IconFontLoaded) {
        if (entity.HasComponent<scene::CameraComponent>()) {
            icon = LUCENT_ICON_CAMERA;
        } else if (entity.HasComponent<scene::LightComponent>()) {
            icon = LUCENT_ICON_LIGHT;
        } else if (entity.HasComponent<scene::MeshRendererComponent>()) {
            icon = LUCENT_ICON_CUBE;
        }
    } else {
        // Fallback: ASCII tags for when icon font isn't present
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
    }
    
    // Push colors for selection
    if (isSelected) {
        ImVec4 accent = ThemeAccent();
        ImGui::PushStyleColor(ImGuiCol_Header, WithAlpha(accent, 0.22f));
        ImGui::PushStyleColor(ImGuiCol_HeaderHovered, WithAlpha(accent, 0.28f));
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
                const char* shapeNames[] = { "Disk", "Rect" };
                int shapeIdx = static_cast<int>(light->areaShape);
                if (ImGui::Combo("Shape", &shapeIdx, shapeNames, 2)) {
                    light->areaShape = static_cast<scene::AreaShape>(shapeIdx);
                }
                
                if (light->areaShape == scene::AreaShape::Disk) {
                    ImGui::DragFloat("Radius", &light->areaWidth, 0.1f, 0.01f, 100.0f);
                } else {
                    ImGui::DragFloat("Width", &light->areaWidth, 0.1f, 0.01f, 100.0f);
                    ImGui::DragFloat("Height", &light->areaHeight, 0.1f, 0.01f, 100.0f);
                }
            }
            
            // Soft shadow controls for non-area lights
            if (light->type != scene::LightType::Area) {
                ImGui::DragFloat("Shadow Softness", &light->shadowSoftness, 0.01f, 0.0f, 1.0f, "%.3f");
                ImGui::SameLine();
                ImGui::TextDisabled("(?)");
                if (ImGui::IsItemHovered()) {
                    ImGui::SetTooltip("Controls soft shadow radius.\nDirectional: angular radius in radians\nPoint/Spot: physical radius in world units");
                }
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
            const char* editGraphLabel = m_IconFontLoaded ? (LUCENT_ICON_EDIT " Edit Graph") : "Edit Graph";
            if (ImGui::Button(editGraphLabel, ImVec2(115.0f, 0.0f))) {
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
                        ImGui::TextColored(ThemeSuccess(), "[OK]");
                    } else {
                        ImGui::SameLine();
                        ImGui::TextColored(ThemeError(), "[ERROR]");
                    }
                }
            }
        }
    }
    
    ImGui::Separator();
    
    // Add component button
    if (ImGui::Button(m_IconFontLoaded ? (LUCENT_ICON_PLUS " Add Component") : "Add Component")) {
        ImGui::OpenPopup("AddComponentPopup");
    }
    
    if (ImGui::BeginPopup("AddComponentPopup")) {
        if (!entity.HasComponent<scene::CameraComponent>() && ImGui::MenuItem(m_IconFontLoaded ? (LUCENT_ICON_CAMERA " Camera") : "Camera")) {
            entity.AddComponent<scene::CameraComponent>();
        }
        if (!entity.HasComponent<scene::LightComponent>() && ImGui::MenuItem(m_IconFontLoaded ? (LUCENT_ICON_LIGHT " Light") : "Light")) {
            entity.AddComponent<scene::LightComponent>();
        }
        if (!entity.HasComponent<scene::MeshRendererComponent>() && ImGui::MenuItem(m_IconFontLoaded ? (LUCENT_ICON_CUBE " Mesh Renderer") : "Mesh Renderer")) {
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
    
    ImVec4 accent = ThemeAccent();
    ImGui::PushStyleColor(ImGuiCol_Button, WithAlpha(accent, 0.18f));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, WithAlpha(accent, 0.26f));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, WithAlpha(accent, 0.34f));
    const char* importLabel = m_IconFontLoaded ? (LUCENT_ICON_IMPORT " Import") : "Import";
    if (ImGui::Button(importLabel)) {
        std::string path = Win32FileDialogs::OpenFile(L"Import Asset", 
            {{L"All Supported", L"*.png;*.jpg;*.hdr;*.obj;*.fbx;*.gltf;*.glb;*.lucent"}, 
             {L"Images", L"*.png;*.jpg;*.hdr"},
             {L"Models", L"*.obj;*.fbx;*.gltf;*.glb"},
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
    ImGui::PopStyleColor(3);
    
    ImGui::SameLine();
    const char* newFolderLabel = m_IconFontLoaded ? (LUCENT_ICON_FOLDER " New Folder") : "New Folder";
    if (ImGui::Button(newFolderLabel)) {
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
                color = ThemeWarning();
                icon = "[DIR]";
            } else if (ext == ".png" || ext == ".jpg" || ext == ".jpeg" || ext == ".hdr") {
                color = ThemeAccent();
                icon = "[TEX]";
            } else if (ext == ".obj" || ext == ".fbx" || ext == ".gltf" || ext == ".glb") {
                color = ThemeSuccess();
                icon = "[OBJ]";
            } else if (ext == ".lucent") {
                color = ThemeAccent();
                icon = "[SCN]";
            } else if (ext == ".mat") {
                color = ImVec4(0.72f, 0.52f, 0.95f, 1.0f);
                icon = "[MAT]";
            } else {
                color = WithAlpha(ThemeMutedText(), 1.0f);
                icon = "[???]";
            }
            
            ImGui::BeginGroup();
            
            // Thumbnail button
            ImGui::PushStyleColor(ImGuiCol_Button, WithAlpha(MulRGB(color, 0.18f), 0.55f));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, WithAlpha(MulRGB(color, 0.24f), 0.70f));
            ImGui::PushStyleColor(ImGuiCol_ButtonActive, WithAlpha(MulRGB(color, 0.30f), 0.85f));
            
            if (ImGui::Button("##thumb", ImVec2(thumbnailSize, thumbnailSize))) {
                if (isDirectory) {
                    m_ContentBrowserPath = entry.path();
                }
            }
            
            // Drag source for compatible files (textures, materials, models)
            if (!isDirectory && ImGui::BeginDragDropSource(ImGuiDragDropFlags_SourceAllowNullID)) {
                std::string pathStr = entry.path().string();
                
                // Determine payload type based on extension
                const char* payloadType = "ASSET_PATH";
                if (ext == ".png" || ext == ".jpg" || ext == ".jpeg" || ext == ".hdr") {
                    payloadType = "TEXTURE_PATH";
                } else if (ext == ".lmat") {
                    payloadType = "MATERIAL_PATH";
                } else if (ext == ".obj" || ext == ".fbx" || ext == ".gltf" || ext == ".glb") {
                    payloadType = "MODEL_PATH";
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
    ImGui::PushStyleColor(ImGuiCol_Button, WithAlpha(ImGui::GetStyle().Colors[ImGuiCol_Button], 0.85f));
    if (ImGui::Button(m_IconFontLoaded ? (LUCENT_ICON_TRASH " Clear") : "Clear")) {
        // Clear console (would clear log buffer)
    }
    ImGui::SameLine();
    if (ImGui::Button(m_IconFontLoaded ? (LUCENT_ICON_COPY " Copy") : "Copy")) {
        // Copy to clipboard
    }
    ImGui::PopStyleColor();
    
    ImGui::SameLine();
    ImGui::Spacing();
    ImGui::SameLine();
    
    // Filter buttons
    static bool showInfo = true, showWarn = true, showError = true;
    
    ImVec4 off = WithAlpha(ImGui::GetStyle().Colors[ImGuiCol_Button], 0.60f);
    ImVec4 info = ThemeAccent();
    ImVec4 warn = ThemeWarning();
    ImVec4 err = ThemeError();
    
    ImGui::PushStyleColor(ImGuiCol_Button, showInfo ? WithAlpha(info, 0.18f) : off);
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, WithAlpha(info, 0.26f));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, WithAlpha(info, 0.34f));
    if (ImGui::Button(m_IconFontLoaded ? (LUCENT_ICON_INFO " Info") : "Info")) showInfo = !showInfo;
    ImGui::PopStyleColor(3);
    
    ImGui::SameLine();
    ImGui::PushStyleColor(ImGuiCol_Button, showWarn ? WithAlpha(warn, 0.18f) : off);
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, WithAlpha(warn, 0.26f));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, WithAlpha(warn, 0.34f));
    if (ImGui::Button(m_IconFontLoaded ? (LUCENT_ICON_WARN " Warn") : "Warn")) showWarn = !showWarn;
    ImGui::PopStyleColor(3);
    
    ImGui::SameLine();
    ImGui::PushStyleColor(ImGuiCol_Button, showError ? WithAlpha(err, 0.18f) : off);
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, WithAlpha(err, 0.26f));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, WithAlpha(err, 0.34f));
    if (ImGui::Button(m_IconFontLoaded ? (LUCENT_ICON_ERROR " Error") : "Error")) showError = !showError;
    ImGui::PopStyleColor(3);
    
    ImGui::SameLine();
    static bool autoScroll = true;
    ImGui::Checkbox("Auto-scroll", &autoScroll);
    
    ImGui::Separator();
    
    // Log output area with colored background
    ImVec4 consoleBg = ImGui::GetStyle().Colors[ImGuiCol_FrameBg];
    consoleBg.w = 1.0f;
    ImGui::PushStyleColor(ImGuiCol_ChildBg, consoleBg);
    ImGui::BeginChild("ScrollingRegion", ImVec2(0, 0), true, ImGuiWindowFlags_HorizontalScrollbar);
    
    // Demo log messages with timestamps
    ImGui::PushStyleColor(ImGuiCol_Text, ThemeMutedText());
    ImGui::TextUnformatted("[11:00:00]");
    ImGui::PopStyleColor();
    ImGui::SameLine();
    ImGui::PushStyleColor(ImGuiCol_Text, WithAlpha(ThemeAccent(), 0.95f));
    ImGui::TextUnformatted("[INFO]");
    ImGui::PopStyleColor();
    ImGui::SameLine();
    ImGui::TextUnformatted("Lucent Engine initialized");
    
    ImGui::PushStyleColor(ImGuiCol_Text, ThemeMutedText());
    ImGui::TextUnformatted("[11:00:00]");
    ImGui::PopStyleColor();
    ImGui::SameLine();
    ImGui::PushStyleColor(ImGuiCol_Text, WithAlpha(ThemeAccent(), 0.95f));
    ImGui::TextUnformatted("[INFO]");
    ImGui::PopStyleColor();
    ImGui::SameLine();
    ImGui::TextUnformatted("Vulkan context initialized successfully");
    
    ImGui::PushStyleColor(ImGuiCol_Text, ThemeMutedText());
    ImGui::TextUnformatted("[11:00:00]");
    ImGui::PopStyleColor();
    ImGui::SameLine();
    ImGui::PushStyleColor(ImGuiCol_Text, WithAlpha(ThemeAccent(), 0.95f));
    ImGui::TextUnformatted("[INFO]");
    ImGui::PopStyleColor();
    ImGui::SameLine();
    ImGui::TextUnformatted("Renderer initialized");
    
    ImGui::PushStyleColor(ImGuiCol_Text, ThemeMutedText());
    ImGui::TextUnformatted("[11:00:01]");
    ImGui::PopStyleColor();
    ImGui::SameLine();
    ImGui::PushStyleColor(ImGuiCol_Text, WithAlpha(ThemeAccent(), 0.95f));
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
                ImGui::PushStyleColor(ImGuiCol_Text, ThemeMutedText());
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
            ImGui::TextColored(ThemeSuccess(), "(Converged)");
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

    // === Output ===
    if (ImGui::CollapsingHeader("Output", ImGuiTreeNodeFlags_DefaultOpen)) {
        int renderSize[2] = { static_cast<int>(settings.renderWidth), static_cast<int>(settings.renderHeight) };
        if (ImGui::InputInt2("Render Resolution", renderSize)) {
            settings.renderWidth = static_cast<uint32_t>(std::max(16, renderSize[0]));
            settings.renderHeight = static_cast<uint32_t>(std::max(16, renderSize[1]));
        }
        if (ImGui::Checkbox("Transparent Background", &settings.transparentBackground)) {
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
    
    // === Environment (HDRI) ===
    if (currentMode != gfx::RenderMode::Simple) {
        if (ImGui::CollapsingHeader("Environment")) {
            if (ImGui::Checkbox("Use Environment Map", &settings.useEnvMap)) {
                settings.MarkDirty();  // Reset accumulation when env changes
            }
            if (ImGui::DragFloat("Env Intensity", &settings.envIntensity, 0.01f, 0.0f, 10.0f, "%.2f")) {
                settings.MarkDirty();
            }
            float rotationDeg = glm::degrees(settings.envRotation);
            if (ImGui::DragFloat("Env Rotation", &rotationDeg, 1.0f, -180.0f, 180.0f, "%.1f deg")) {
                settings.envRotation = glm::radians(rotationDeg);
                settings.MarkDirty();
            }
            ImGui::Text("HDRI");
            ImGui::SameLine();
            const char* browseLabel = m_IconFontLoaded ? (LUCENT_ICON_FOLDER " Browse") : "Browse";
            if (ImGui::Button(browseLabel)) {
                std::string path = Win32FileDialogs::OpenFile(L"Open HDRI",
                    {{L"HDR Images", L"*.hdr;*.exr"}, {L"All Files", L"*.*"}});
                if (!path.empty()) {
                    uint32_t handle = gfx::EnvironmentMapLibrary::Get().LoadFromFile(path);
                    if (handle != gfx::EnvironmentMapLibrary::InvalidHandle) {
                        settings.envMapPath = path;
                        settings.envMapHandle = handle;
                        settings.MarkDirty();
                        if (m_Scene) {
                            m_Scene->SetEnvironmentMapPath(path);
                        }
                    } else {
                        Win32FileDialogs::ShowError(L"Open HDRI", L"Failed to load the HDR environment map.");
                    }
                }
            }
            ImGui::SameLine();
            if (ImGui::Button("Use Default")) {
                uint32_t handle = gfx::EnvironmentMapLibrary::Get().GetDefaultHandle();
                if (handle != gfx::EnvironmentMapLibrary::InvalidHandle) {
                    settings.envMapPath.clear();
                    settings.envMapHandle = handle;
                    settings.MarkDirty();
                    if (m_Scene) {
                        m_Scene->SetEnvironmentMapPath("");
                    }
                }
            }
            if (settings.envMapPath.empty()) {
                ImGui::TextDisabled("Using default sky environment.");
            } else {
                ImGui::TextWrapped("%s", settings.envMapPath.c_str());
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
    
    // === Rasterization ===
    if (ImGui::CollapsingHeader("Rasterization", ImGuiTreeNodeFlags_DefaultOpen)) {
        if (ImGui::Checkbox("Backface Culling", &settings.enableBackfaceCulling)) {
            // No accumulation reset needed in Simple mode, but keep behavior consistent
            settingsChanged = true;
        }
        ImGui::TextDisabled("Tip: disable this for debugging normals / editing open meshes.");
    }

    // === Editor Overlays ===
    if (ImGui::CollapsingHeader("Editor Overlays", ImGuiTreeNodeFlags_DefaultOpen)) {
        ImGui::Checkbox("Show Indicators", &m_ShowIndicators);
        ImGui::SameLine();
        ImGui::Checkbox("Light Selected Only", &m_IndicatorsSelectedOnly);

        ImGui::Indent();
        ImGui::Checkbox("Lights", &m_ShowLightIndicators);
        ImGui::SameLine();
        ImGui::Checkbox("Cameras", &m_ShowCameraIndicators);
        if (m_ShowCameraIndicators) {
            ImGui::Checkbox("Camera Selected Only", &m_CameraIndicatorsSelectedOnly);
        }
        ImGui::Unindent();

        ImGui::TextDisabled("Indicators are editor-only overlays (sphere/cone/frustum).");
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
                        ImGui::PushStyleColor(ImGuiCol_Text, ThemeMutedText());
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

    // === Final Render ===
    if (ImGui::CollapsingHeader("Final Render", ImGuiTreeNodeFlags_DefaultOpen)) {
        gfx::FinalRender* finalRender = m_Renderer->GetFinalRender();
        if (!finalRender) {
            ImGui::TextDisabled("Final render is not available in this build.");
        } else {
            gfx::FinalRenderStatus status = finalRender->GetStatus();
            switch (status) {
                case gfx::FinalRenderStatus::Rendering:
                    ImGui::TextColored(ThemeAccent(), "Rendering...");
                    ImGui::ProgressBar(finalRender->GetProgress(), ImVec2(0.0f, 0.0f));
                    ImGui::Text("Samples: %u / %u", finalRender->GetCurrentSample(), finalRender->GetTotalSamples());
                    if (ImGui::Button("Cancel Render")) {
                        finalRender->Cancel();
                    }
                    break;
                case gfx::FinalRenderStatus::Completed:
                    ImGui::TextColored(ThemeSuccess(), "Completed");
                    break;
                case gfx::FinalRenderStatus::Failed:
                    ImGui::TextColored(ThemeError(), "Failed");
                    break;
                case gfx::FinalRenderStatus::Cancelled:
                    ImGui::TextColored(ThemeWarning(), "Cancelled");
                    break;
                default:
                    ImGui::TextDisabled("Idle");
                    break;
            }

            static char outputPath[256] = "render.png";
            ImGui::InputText("Output Path", outputPath, sizeof(outputPath));
            if (status == gfx::FinalRenderStatus::Completed) {
                if (ImGui::Button("Save Render")) {
                    finalRender->ExportImage(outputPath);
                }
            }

            ImGui::TextDisabled("Press F12 to open render preview window.");
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

void EditorUI::ApplySceneEnvironment() {
    if (!m_Renderer || !m_Scene) {
        return;
    }

    gfx::RenderSettings& settings = m_Renderer->GetSettings();
    const std::string& path = m_Scene->GetEnvironmentMapPath();
    if (path.empty()) {
        uint32_t handle = gfx::EnvironmentMapLibrary::Get().GetDefaultHandle();
        if (handle != gfx::EnvironmentMapLibrary::InvalidHandle) {
            settings.envMapPath.clear();
            settings.envMapHandle = handle;
            settings.MarkDirty();
        }
        return;
    }

    uint32_t handle = gfx::EnvironmentMapLibrary::Get().LoadFromFile(path);
    if (handle == gfx::EnvironmentMapLibrary::InvalidHandle) {
        LUCENT_CORE_WARN("Failed to load HDRI from scene: {}", path);
        return;
    }

    settings.envMapPath = path;
    settings.envMapHandle = handle;
    settings.MarkDirty();
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
        ImGui::TextColored(ThemeAccent(), "LUCENT");
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

            if (ImGui::Button(m_IconFontLoaded ? (LUCENT_ICON_SAVE " Save GPU Preference") : "Save GPU Preference")) {
                EditorSettings s = EditorSettings::Load();
                if (gpuIndex <= 0) s.preferredGpuName.clear();
                else s.preferredGpuName = gpuNames[gpuIndex];
                s.Save();
                gpuSaved = true;
            }

            if (gpuSaved) {
                ImGui::SameLine();
                ImGui::TextColored(ThemeSuccess(), "Saved. Restart to apply.");
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
    
    // Handle interactive transform mode
    if (IsInInteractiveTransform()) {
        // Numeric input (simple): 0-9, '.', '-', backspace
        auto appendChar = [&](char c) {
            // Only allow one '-' at the start and one '.'
            if (c == '-') {
                if (!m_TransformNumeric.empty()) return;
            }
            if (c == '.') {
                if (m_TransformNumeric.find('.') != std::string::npos) return;
                if (m_TransformNumeric.empty()) m_TransformNumeric = "0";
            }
            m_TransformNumeric.push_back(c);
        };
        
        if (ImGui::IsKeyPressed(ImGuiKey_Backspace) && !m_TransformNumeric.empty()) {
            m_TransformNumeric.pop_back();
        }
        if (ImGui::IsKeyPressed(ImGuiKey_Minus)) appendChar('-');
        if (ImGui::IsKeyPressed(ImGuiKey_Period) || ImGui::IsKeyPressed(ImGuiKey_KeypadDecimal)) appendChar('.');
        if (ImGui::IsKeyPressed(ImGuiKey_0) || ImGui::IsKeyPressed(ImGuiKey_Keypad0)) appendChar('0');
        if (ImGui::IsKeyPressed(ImGuiKey_1) || ImGui::IsKeyPressed(ImGuiKey_Keypad1)) appendChar('1');
        if (ImGui::IsKeyPressed(ImGuiKey_2) || ImGui::IsKeyPressed(ImGuiKey_Keypad2)) appendChar('2');
        if (ImGui::IsKeyPressed(ImGuiKey_3) || ImGui::IsKeyPressed(ImGuiKey_Keypad3)) appendChar('3');
        if (ImGui::IsKeyPressed(ImGuiKey_4) || ImGui::IsKeyPressed(ImGuiKey_Keypad4)) appendChar('4');
        if (ImGui::IsKeyPressed(ImGuiKey_5) || ImGui::IsKeyPressed(ImGuiKey_Keypad5)) appendChar('5');
        if (ImGui::IsKeyPressed(ImGuiKey_6) || ImGui::IsKeyPressed(ImGuiKey_Keypad6)) appendChar('6');
        if (ImGui::IsKeyPressed(ImGuiKey_7) || ImGui::IsKeyPressed(ImGuiKey_Keypad7)) appendChar('7');
        if (ImGui::IsKeyPressed(ImGuiKey_8) || ImGui::IsKeyPressed(ImGuiKey_Keypad8)) appendChar('8');
        if (ImGui::IsKeyPressed(ImGuiKey_9) || ImGui::IsKeyPressed(ImGuiKey_Keypad9)) appendChar('9');
        
        // X/Y/Z - Set axis constraint
        if (ImGui::IsKeyPressed(ImGuiKey_X)) {
            m_AxisConstraint = (m_AxisConstraint == AxisConstraint::X) ? AxisConstraint::None : AxisConstraint::X;
        }
        if (ImGui::IsKeyPressed(ImGuiKey_Y)) {
            m_AxisConstraint = (m_AxisConstraint == AxisConstraint::Y) ? AxisConstraint::None : AxisConstraint::Y;
        }
        if (ImGui::IsKeyPressed(ImGuiKey_Z)) {
            m_AxisConstraint = (m_AxisConstraint == AxisConstraint::Z) ? AxisConstraint::None : AxisConstraint::Z;
        }
        
        // Enter - Confirm (Blender-style)
        if (ImGui::IsKeyPressed(ImGuiKey_Enter) || ImGui::IsKeyPressed(ImGuiKey_KeypadEnter)) {
            ConfirmInteractiveTransform();
            return;
        }
        
        // Left mouse button - Confirm
        if (ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
            ConfirmInteractiveTransform();
            return;
        }
        
        // Escape or Right mouse button - Cancel
        if (ImGui::IsKeyPressed(ImGuiKey_Escape) || ImGui::IsMouseClicked(ImGuiMouseButton_Right)) {
            CancelInteractiveTransform();
            return;
        }
        
        // Don't process other shortcuts while in interactive transform
        return;
    }
    
    // G - Start Grab in Object mode (only when viewport is hovered)
    if (ImGui::IsKeyPressed(ImGuiKey_G) && !io.KeyCtrl && m_ViewportHovered) {
        if (m_EditorMode == EditorMode::Object && !m_SelectedEntities.empty()) {
            StartInteractiveTransform(InteractiveTransformType::Grab);
            return;
        }
    }
    
    // R - Start Rotate in Object mode (only when viewport is hovered)
    if (ImGui::IsKeyPressed(ImGuiKey_R) && !io.KeyCtrl && m_ViewportHovered) {
        if (m_EditorMode == EditorMode::Object && !m_SelectedEntities.empty()) {
            StartInteractiveTransform(InteractiveTransformType::Rotate);
            return;
        }
    }
    
    // S - Start Scale in Object mode (only when viewport is hovered)
    if (ImGui::IsKeyPressed(ImGuiKey_S) && !io.KeyCtrl && m_ViewportHovered) {
        if (m_EditorMode == EditorMode::Object && !m_SelectedEntities.empty()) {
            StartInteractiveTransform(InteractiveTransformType::Scale);
            return;
        }
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
        for (auto id : m_SelectedEntities) {
            if (m_Scene) {
                m_Scene->DestroyEntity(m_Scene->GetEntity(id));
            }
        }
        ClearSelection();
        m_SceneDirty = true;
    }
    
    // Ctrl+C - Copy
    if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_C) && !m_SelectedEntities.empty()) {
        m_Clipboard.clear();
        for (auto id : m_SelectedEntities) {
            if (m_Scene) {
                scene::Entity src = m_Scene->GetEntity(id);
                if (!src.IsValid()) continue;
                
                ClipboardEntity clip;
                auto* tag = src.GetComponent<scene::TagComponent>();
                clip.name = tag ? tag->name : "Entity";
                
                if (auto* t = src.GetComponent<scene::TransformComponent>()) clip.transform = *t;
                if (auto* c = src.GetComponent<scene::CameraComponent>()) clip.camera = *c;
                if (auto* l = src.GetComponent<scene::LightComponent>()) clip.light = *l;
                if (auto* m = src.GetComponent<scene::MeshRendererComponent>()) clip.meshRenderer = *m;
                m_Clipboard.push_back(clip);
            }
        }
    }
    
    // Ctrl+X - Cut
    if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_X) && !m_SelectedEntities.empty()) {
        m_Clipboard.clear();
        for (auto id : m_SelectedEntities) {
            if (m_Scene) {
                scene::Entity src = m_Scene->GetEntity(id);
                if (!src.IsValid()) continue;
                
                ClipboardEntity clip;
                auto* tag = src.GetComponent<scene::TagComponent>();
                clip.name = tag ? tag->name : "Entity";
                
                if (auto* t = src.GetComponent<scene::TransformComponent>()) clip.transform = *t;
                if (auto* c = src.GetComponent<scene::CameraComponent>()) clip.camera = *c;
                if (auto* l = src.GetComponent<scene::LightComponent>()) clip.light = *l;
                if (auto* m = src.GetComponent<scene::MeshRendererComponent>()) clip.meshRenderer = *m;
                m_Clipboard.push_back(clip);
                m_Scene->DestroyEntity(src);
            }
        }
        ClearSelection();
        m_SceneDirty = true;
    }
    
    // Ctrl+V - Paste
    if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_V) && !m_Clipboard.empty() && m_Scene) {
        std::vector<scene::Entity> newEntities;
        for (const auto& clip : m_Clipboard) {
            scene::Entity ent = m_Scene->CreateEntity(clip.name + " (Pasted)");
            
            if (auto* t = ent.GetComponent<scene::TransformComponent>()) {
                *t = clip.transform;
                t->position += glm::vec3(1.0f, 0.0f, 0.0f);
            }
            if (clip.camera) ent.AddComponent<scene::CameraComponent>() = *clip.camera;
            if (clip.light) ent.AddComponent<scene::LightComponent>() = *clip.light;
            if (clip.meshRenderer) ent.AddComponent<scene::MeshRendererComponent>() = *clip.meshRenderer;
            newEntities.push_back(ent);
        }
        ClearSelection();
        for (auto& e : newEntities) AddToSelection(e);
        m_SceneDirty = true;
    }
    
    // Ctrl+D - Duplicate
    if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_D) && !m_SelectedEntities.empty() && m_Scene) {
        std::vector<scene::Entity> newEntities;
        for (auto id : m_SelectedEntities) {
            scene::Entity src = m_Scene->GetEntity(id);
            if (!src.IsValid()) continue;
            
            auto* tag = src.GetComponent<scene::TagComponent>();
            scene::Entity dup = m_Scene->CreateEntity(tag ? tag->name + " Copy" : "Entity Copy");
            
            if (auto* t = src.GetComponent<scene::TransformComponent>()) {
                auto* dt = dup.GetComponent<scene::TransformComponent>();
                if (dt) {
                    *dt = *t;
                    dt->position += glm::vec3(1.0f, 0.0f, 0.0f);
                }
            }
            if (auto* c = src.GetComponent<scene::CameraComponent>()) dup.AddComponent<scene::CameraComponent>() = *c;
            if (auto* l = src.GetComponent<scene::LightComponent>()) dup.AddComponent<scene::LightComponent>() = *l;
            if (auto* m = src.GetComponent<scene::MeshRendererComponent>()) dup.AddComponent<scene::MeshRendererComponent>() = *m;
            newEntities.push_back(dup);
        }
        ClearSelection();
        for (auto& e : newEntities) AddToSelection(e);
        m_SceneDirty = true;
    }
    
    // Tab - Toggle Object/Edit mode
    if (ImGui::IsKeyPressed(ImGuiKey_Tab) && !io.KeyCtrl && !io.KeyAlt) {
        ToggleEditorMode();
    }
    
    // In Edit Mode: 1/2/3 for selection mode
    if (m_EditorMode == EditorMode::Edit) {
        if (ImGui::IsKeyPressed(ImGuiKey_1)) {
            SetMeshSelectMode(MeshSelectMode::Vertex);
        }
        if (ImGui::IsKeyPressed(ImGuiKey_2)) {
            SetMeshSelectMode(MeshSelectMode::Edge);
        }
        if (ImGui::IsKeyPressed(ImGuiKey_3)) {
            SetMeshSelectMode(MeshSelectMode::Face);
        }
        
        // Get editable mesh for operations
        scene::Entity entity = GetEditedEntity();
        scene::EditableMeshComponent* editMesh = nullptr;
        if (entity.IsValid()) {
            editMesh = entity.GetComponent<scene::EditableMeshComponent>();
        }
        
        if (editMesh && editMesh->HasMesh()) {
            auto* meshPtr = editMesh->mesh.get();
            uint32_t entityId = entity.GetID();
            
            // Helper lambda to push undo command
            auto pushMeshUndo = [this, editMesh, entityId](const std::string& opName, 
                                                            const MeshEditCommand::MeshSnapshot& before) {
                auto after = MeshEditCommand::CaptureSnapshot(editMesh);
                auto cmd = std::make_unique<MeshEditCommand>(
                    m_Scene, entityId, opName, before, std::move(after)
                );
                UndoStack::Get().Push(std::move(cmd));
            };
            
            // E - Extrude
            if (ImGui::IsKeyPressed(ImGuiKey_E) && !io.KeyCtrl) {
                if (!meshPtr->GetSelection().faces.empty()) {
                    auto before = MeshEditCommand::CaptureSnapshot(editMesh);
                    mesh::MeshOps::ExtrudeFaces(*meshPtr, 0.5f);
                    editMesh->MarkDirty();
                    m_SceneDirty = true;
                    pushMeshUndo("Extrude", before);
                    LUCENT_CORE_INFO("Extruded {} faces", meshPtr->GetSelection().faces.size());
                }
            }
            
            // I - Inset
            if (ImGui::IsKeyPressed(ImGuiKey_I) && !io.KeyCtrl) {
                if (!meshPtr->GetSelection().faces.empty()) {
                    auto before = MeshEditCommand::CaptureSnapshot(editMesh);
                    mesh::MeshOps::InsetFaces(*meshPtr, 0.2f);
                    editMesh->MarkDirty();
                    m_SceneDirty = true;
                    pushMeshUndo("Inset", before);
                    LUCENT_CORE_INFO("Inset {} faces", meshPtr->GetSelection().faces.size());
                }
            }
            
            // X - Delete
            if (ImGui::IsKeyPressed(ImGuiKey_X) && !io.KeyCtrl) {
                auto before = MeshEditCommand::CaptureSnapshot(editMesh);
                bool didDelete = false;
                
                switch (m_MeshSelectMode) {
                    case MeshSelectMode::Vertex:
                        if (!meshPtr->GetSelection().vertices.empty()) {
                            mesh::MeshOps::DeleteVertices(*meshPtr);
                            didDelete = true;
                        }
                        break;
                    case MeshSelectMode::Edge:
                        if (!meshPtr->GetSelection().edges.empty()) {
                            mesh::MeshOps::DeleteEdges(*meshPtr);
                            didDelete = true;
                        }
                        break;
                    case MeshSelectMode::Face:
                        if (!meshPtr->GetSelection().faces.empty()) {
                            mesh::MeshOps::DeleteFaces(*meshPtr);
                            didDelete = true;
                        }
                        break;
                }
                
                if (didDelete) {
                    editMesh->MarkDirty();
                    m_SceneDirty = true;
                    pushMeshUndo("Delete", before);
                }
            }
            
            // M - Merge vertices
            if (ImGui::IsKeyPressed(ImGuiKey_M)) {
                if (!meshPtr->GetSelection().vertices.empty()) {
                    auto before = MeshEditCommand::CaptureSnapshot(editMesh);
                    mesh::MeshOps::MergeVerticesAtCenter(*meshPtr);
                    editMesh->MarkDirty();
                    m_SceneDirty = true;
                    pushMeshUndo("Merge", before);
                    LUCENT_CORE_INFO("Merged vertices at center");
                }
            }
            
            // A - Select All / Deselect All
            if (ImGui::IsKeyPressed(ImGuiKey_A) && !io.KeyCtrl) {
                if (meshPtr->GetSelection().Empty()) {
                    meshPtr->SelectAll();
                } else {
                    meshPtr->DeselectAll();
                }
            }
            
            // G - Start interactive Grab
            if (ImGui::IsKeyPressed(ImGuiKey_G) && !io.KeyCtrl && m_ViewportHovered) {
                if (!meshPtr->GetSelection().Empty()) {
                    StartInteractiveTransform(InteractiveTransformType::Grab);
                }
            }
            
            // R - Rotate (still uses gizmo for now)
            if (ImGui::IsKeyPressed(ImGuiKey_R) && !io.KeyCtrl && !io.KeyShift) {
                m_GizmoOperation = GizmoOperation::Rotate;
            }
            
            // S - Scale (still uses gizmo for now)
            if (ImGui::IsKeyPressed(ImGuiKey_S) && !io.KeyCtrl) {
                m_GizmoOperation = GizmoOperation::Scale;
            }
        }
    }
}

// ============================================================================
// Edit Mode
// ============================================================================

void EditorUI::SetEditorMode(EditorMode mode) {
    if (m_EditorMode == mode) return;
    
    if (mode == EditorMode::Edit) {
        // Entering Edit Mode - ensure we have a selected entity with a mesh
        if (m_SelectedEntities.size() != 1) {
            LUCENT_CORE_WARN("Edit Mode requires exactly one selected entity");
            return;
        }
        
        scene::Entity entity = m_Scene->GetEntity(m_SelectedEntities[0]);
        if (!entity.IsValid()) {
            LUCENT_CORE_WARN("Selected entity is invalid");
            return;
        }
        
        auto* meshRenderer = entity.GetComponent<scene::MeshRendererComponent>();
        if (!meshRenderer) {
            LUCENT_CORE_WARN("Selected entity has no mesh renderer");
            return;
        }
        
        // Create EditableMeshComponent if it doesn't exist
        if (!entity.HasComponent<scene::EditableMeshComponent>()) {
            auto& editMesh = entity.AddComponent<scene::EditableMeshComponent>();
            
            // Initialize from primitive type if applicable
            if (meshRenderer->primitiveType != scene::MeshRendererComponent::PrimitiveType::None) {
                editMesh.InitFromPrimitive(meshRenderer->primitiveType);
            } else {
                LUCENT_CORE_WARN("Cannot enter Edit Mode: mesh is not a primitive (import support TODO)");
                return;
            }
        }
        
        m_EditedEntityID = m_SelectedEntities[0];
        m_EditorMode = EditorMode::Edit;
        m_MeshSelectMode = MeshSelectMode::Vertex;
        
        LUCENT_CORE_INFO("Entered Edit Mode for entity: {}", entity.GetComponent<scene::TagComponent>()->name);
    } else {
        // Exiting Edit Mode
        m_EditorMode = EditorMode::Object;
        m_EditedEntityID = UINT32_MAX;
        
        LUCENT_CORE_INFO("Exited Edit Mode");
    }
}

void EditorUI::ToggleEditorMode() {
    if (m_EditorMode == EditorMode::Object) {
        SetEditorMode(EditorMode::Edit);
    } else {
        SetEditorMode(EditorMode::Object);
    }
}

void EditorUI::SetMeshSelectMode(MeshSelectMode mode) {
    if (m_EditorMode != EditorMode::Edit) return;
    
    m_MeshSelectMode = mode;
    
    const char* modeName = "Vertex";
    switch (mode) {
        case MeshSelectMode::Vertex: modeName = "Vertex"; break;
        case MeshSelectMode::Edge: modeName = "Edge"; break;
        case MeshSelectMode::Face: modeName = "Face"; break;
    }
    LUCENT_CORE_DEBUG("Mesh selection mode: {}", modeName);
}

scene::Entity EditorUI::GetEditedEntity() const {
    if (m_EditorMode != EditorMode::Edit || m_EditedEntityID == UINT32_MAX || !m_Scene) {
        return scene::Entity();
    }
    return m_Scene->GetEntity(m_EditedEntityID);
}

// ============================================================================
// Edit Mode Picking and Overlay
// ============================================================================

glm::vec3 EditorUI::WorldToScreen(const glm::vec3& worldPos) {
    if (!m_EditorCamera) return glm::vec3(0);
    
    glm::mat4 view = m_EditorCamera->GetViewMatrix();

    // IMPORTANT: Use viewport aspect for overlays/picking. The editor camera's stored aspect
    // can lag behind docking/resizing and causes overlays (e.g. camera frustums) to drift/flicker.
    const float aspectRatio = (m_ViewportSize.y > 0.0f) ? (m_ViewportSize.x / m_ViewportSize.y) : 1.0f;
    glm::mat4 proj = glm::perspective(
        glm::radians(m_EditorCamera->GetFOV()),
        aspectRatio,
        m_EditorCamera->GetNearClip(),
        m_EditorCamera->GetFarClip()
    );
    
    glm::vec4 clipPos = proj * view * glm::vec4(worldPos, 1.0f);
    if (clipPos.w <= 0.0f) return glm::vec3(-1000, -1000, -1); // Behind camera
    
    glm::vec3 ndcPos = glm::vec3(clipPos) / clipPos.w;
    
    // Convert to screen coordinates
    // Note: Vulkan uses Y-down in framebuffer, and we're not flipping in the projection,
    // so don't flip Y here either - just map NDC directly to screen space
    float screenX = m_ViewportPosition.x + (ndcPos.x * 0.5f + 0.5f) * m_ViewportSize.x;
    float screenY = m_ViewportPosition.y + (ndcPos.y * 0.5f + 0.5f) * m_ViewportSize.y;
    
    return glm::vec3(screenX, screenY, ndcPos.z);
}

void EditorUI::HandleEditModeClick() {
    if (m_EditorMode != EditorMode::Edit) return;
    if (!m_ViewportHovered || m_UsingGizmo) return;
    
    ImGuiIO& io = ImGui::GetIO();
    if (!ImGui::IsMouseClicked(ImGuiMouseButton_Left)) return;
    
    glm::vec2 mousePos(io.MousePos.x, io.MousePos.y);
    
    scene::Entity entity = GetEditedEntity();
    if (!entity.IsValid()) return;
    
    auto* editMesh = entity.GetComponent<scene::EditableMeshComponent>();
    if (!editMesh || !editMesh->HasMesh()) return;
    
    auto* mesh = editMesh->mesh.get();
    bool shiftHeld = io.KeyShift;
    bool ctrlHeld = io.KeyCtrl;
    
    switch (m_MeshSelectMode) {
        case MeshSelectMode::Vertex: {
            mesh::VertexID vid = PickVertex(mousePos);
            if (vid != mesh::INVALID_ID) {
                if (ctrlHeld) {
                    // Toggle selection
                    if (mesh->GetSelection().vertices.count(vid)) {
                        mesh->GetSelection().vertices.erase(vid);
                        auto* v = mesh->GetVertex(vid);
                        if (v) v->selected = false;
                    } else {
                        mesh->SelectVertex(vid, true);
                    }
                } else {
                    mesh->SelectVertex(vid, shiftHeld);
                }
            } else if (!shiftHeld && !ctrlHeld) {
                mesh->DeselectAll();
            }
            break;
        }
        case MeshSelectMode::Edge: {
            mesh::EdgeID eid = PickEdge(mousePos);
            if (eid != mesh::INVALID_ID) {
                if (ctrlHeld) {
                    if (mesh->GetSelection().edges.count(eid)) {
                        mesh->GetSelection().edges.erase(eid);
                        auto* e = mesh->GetEdge(eid);
                        if (e) e->selected = false;
                    } else {
                        mesh->SelectEdge(eid, true);
                    }
                } else {
                    mesh->SelectEdge(eid, shiftHeld);
                }
            } else if (!shiftHeld && !ctrlHeld) {
                mesh->DeselectAll();
            }
            break;
        }
        case MeshSelectMode::Face: {
            mesh::FaceID fid = PickFace(mousePos);
            if (fid != mesh::INVALID_ID) {
                if (ctrlHeld) {
                    if (mesh->GetSelection().faces.count(fid)) {
                        mesh->GetSelection().faces.erase(fid);
                        auto* f = mesh->GetFace(fid);
                        if (f) f->selected = false;
                    } else {
                        mesh->SelectFace(fid, true);
                    }
                } else {
                    mesh->SelectFace(fid, shiftHeld);
                }
            } else if (!shiftHeld && !ctrlHeld) {
                mesh->DeselectAll();
            }
            break;
        }
    }
}

mesh::VertexID EditorUI::PickVertex(const glm::vec2& mousePos, float radius) {
    scene::Entity entity = GetEditedEntity();
    if (!entity.IsValid()) return mesh::INVALID_ID;
    
    auto* editMesh = entity.GetComponent<scene::EditableMeshComponent>();
    if (!editMesh || !editMesh->HasMesh()) return mesh::INVALID_ID;
    
    auto* transform = entity.GetComponent<scene::TransformComponent>();
    glm::mat4 modelMatrix = transform ? transform->GetLocalMatrix() : glm::mat4(1.0f);
    
    auto* mesh = editMesh->mesh.get();
    mesh::VertexID closestVid = mesh::INVALID_ID;
    float closestDist = radius * radius;
    
    for (const auto& v : mesh->GetVertices()) {
        if (v.id == mesh::INVALID_ID) continue;
        
        glm::vec3 worldPos = glm::vec3(modelMatrix * glm::vec4(v.position, 1.0f));
        glm::vec3 screenPos = WorldToScreen(worldPos);
        
        if (screenPos.z < 0 || screenPos.z > 1) continue; // Behind camera or too far
        
        float dx = screenPos.x - mousePos.x;
        float dy = screenPos.y - mousePos.y;
        float distSq = dx * dx + dy * dy;
        
        if (distSq < closestDist) {
            closestDist = distSq;
            closestVid = v.id;
        }
    }
    
    return closestVid;
}

mesh::EdgeID EditorUI::PickEdge(const glm::vec2& mousePos, float radius) {
    scene::Entity entity = GetEditedEntity();
    if (!entity.IsValid()) return mesh::INVALID_ID;
    
    auto* editMesh = entity.GetComponent<scene::EditableMeshComponent>();
    if (!editMesh || !editMesh->HasMesh()) return mesh::INVALID_ID;
    
    auto* transform = entity.GetComponent<scene::TransformComponent>();
    glm::mat4 modelMatrix = transform ? transform->GetLocalMatrix() : glm::mat4(1.0f);
    
    auto* mesh = editMesh->mesh.get();
    mesh::EdgeID closestEid = mesh::INVALID_ID;
    float closestDist = radius;
    
    for (const auto& e : mesh->GetEdges()) {
        if (e.id == mesh::INVALID_ID) continue;
        
        const mesh::EMVertex* v0 = mesh->GetVertex(e.v0);
        const mesh::EMVertex* v1 = mesh->GetVertex(e.v1);
        if (!v0 || !v1) continue;
        
        glm::vec3 worldP0 = glm::vec3(modelMatrix * glm::vec4(v0->position, 1.0f));
        glm::vec3 worldP1 = glm::vec3(modelMatrix * glm::vec4(v1->position, 1.0f));
        
        glm::vec3 screenP0 = WorldToScreen(worldP0);
        glm::vec3 screenP1 = WorldToScreen(worldP1);
        
        // Skip if edge is behind camera
        if (screenP0.z < 0 || screenP1.z < 0 || screenP0.z > 1 || screenP1.z > 1) continue;
        
        // Calculate distance from point to line segment
        glm::vec2 p0(screenP0.x, screenP0.y);
        glm::vec2 p1(screenP1.x, screenP1.y);
        glm::vec2 p(mousePos.x, mousePos.y);
        
        glm::vec2 lineDir = p1 - p0;
        float lineLenSq = glm::dot(lineDir, lineDir);
        if (lineLenSq < 0.0001f) continue;
        
        float t = glm::clamp(glm::dot(p - p0, lineDir) / lineLenSq, 0.0f, 1.0f);
        glm::vec2 closest = p0 + t * lineDir;
        float dist = glm::length(p - closest);
        
        if (dist < closestDist) {
            closestDist = dist;
            closestEid = e.id;
        }
    }
    
    return closestEid;
}

mesh::FaceID EditorUI::PickFace(const glm::vec2& mousePos) {
    scene::Entity entity = GetEditedEntity();
    if (!entity.IsValid()) return mesh::INVALID_ID;
    
    auto* editMesh = entity.GetComponent<scene::EditableMeshComponent>();
    if (!editMesh || !editMesh->HasMesh()) return mesh::INVALID_ID;
    
    auto* transform = entity.GetComponent<scene::TransformComponent>();
    glm::mat4 modelMatrix = transform ? transform->GetLocalMatrix() : glm::mat4(1.0f);
    
    auto* meshData = editMesh->mesh.get();
    
    // Cast a ray and check against triangulated faces
    if (!m_EditorCamera) return mesh::INVALID_ID;
    
    // Convert mouse to normalized device coordinates
    float ndcX = ((mousePos.x - m_ViewportPosition.x) / m_ViewportSize.x) * 2.0f - 1.0f;
    float ndcY = 1.0f - ((mousePos.y - m_ViewportPosition.y) / m_ViewportSize.y) * 2.0f;
    
    glm::mat4 view = m_EditorCamera->GetViewMatrix();
    glm::mat4 proj = m_EditorCamera->GetProjectionMatrix();
    glm::mat4 invViewProj = glm::inverse(proj * view);
    
    glm::vec4 rayClipNear(ndcX, ndcY, 0.0f, 1.0f);
    glm::vec4 rayClipFar(ndcX, ndcY, 1.0f, 1.0f);
    
    glm::vec4 rayWorldNear = invViewProj * rayClipNear;
    glm::vec4 rayWorldFar = invViewProj * rayClipFar;
    
    rayWorldNear /= rayWorldNear.w;
    rayWorldFar /= rayWorldFar.w;
    
    glm::vec3 rayOrigin = glm::vec3(rayWorldNear);
    glm::vec3 rayDir = glm::normalize(glm::vec3(rayWorldFar - rayWorldNear));
    
    // Transform ray to model space
    glm::mat4 invModel = glm::inverse(modelMatrix);
    glm::vec3 localRayOrigin = glm::vec3(invModel * glm::vec4(rayOrigin, 1.0f));
    glm::vec3 localRayDir = glm::normalize(glm::vec3(invModel * glm::vec4(rayDir, 0.0f)));
    
    mesh::FaceID closestFace = mesh::INVALID_ID;
    float closestT = std::numeric_limits<float>::max();
    
    // Check each face
    for (const auto& face : meshData->GetFaces()) {
        if (face.id == mesh::INVALID_ID) continue;
        
        // Collect face vertices
        std::vector<glm::vec3> faceVerts;
        meshData->ForEachFaceVertex(face.id, [&](const mesh::EMVertex& v) {
            faceVerts.push_back(v.position);
        });
        
        if (faceVerts.size() < 3) continue;
        
        // Triangulate and test each triangle
        for (size_t i = 1; i + 1 < faceVerts.size(); ++i) {
            glm::vec3 v0 = faceVerts[0];
            glm::vec3 v1 = faceVerts[i];
            glm::vec3 v2 = faceVerts[i + 1];
            
            // Ray-triangle intersection (Moller-Trumbore)
            glm::vec3 edge1 = v1 - v0;
            glm::vec3 edge2 = v2 - v0;
            glm::vec3 h = glm::cross(localRayDir, edge2);
            float a = glm::dot(edge1, h);
            
            if (std::abs(a) < 0.00001f) continue;
            
            float f = 1.0f / a;
            glm::vec3 s = localRayOrigin - v0;
            float u = f * glm::dot(s, h);
            
            if (u < 0.0f || u > 1.0f) continue;
            
            glm::vec3 q = glm::cross(s, edge1);
            float v = f * glm::dot(localRayDir, q);
            
            if (v < 0.0f || u + v > 1.0f) continue;
            
            float t = f * glm::dot(edge2, q);
            
            if (t > 0.001f && t < closestT) {
                closestT = t;
                closestFace = face.id;
            }
        }
    }
    
    return closestFace;
}

void EditorUI::DrawEditModeOverlay() {
    if (m_EditorMode != EditorMode::Edit) return;
    
    scene::Entity entity = GetEditedEntity();
    if (!entity.IsValid()) return;
    
    auto* editMesh = entity.GetComponent<scene::EditableMeshComponent>();
    if (!editMesh || !editMesh->HasMesh()) return;
    
    auto* transform = entity.GetComponent<scene::TransformComponent>();
    glm::mat4 modelMatrix = transform ? transform->GetLocalMatrix() : glm::mat4(1.0f);
    
    auto* mesh = editMesh->mesh.get();
    ImDrawList* drawList = ImGui::GetForegroundDrawList();
    
    // Colors
    const ImU32 vertexColor = IM_COL32(200, 200, 255, 200);
    const ImU32 vertexSelectedColor = IM_COL32(255, 150, 50, 255);
    const ImU32 edgeColor = IM_COL32(150, 150, 200, 100);
    const ImU32 edgeSelectedColor = IM_COL32(255, 150, 50, 255);
    const ImU32 faceColor = IM_COL32(100, 100, 150, 40);           // Subtle face overlay
    const ImU32 faceSelectedColor = IM_COL32(255, 150, 50, 120);   // Much more visible
    const ImU32 faceOutlineColor = IM_COL32(255, 180, 100, 255);   // Bright outline
    
    // Draw faces (in face mode, show all faces with subtle overlay)
    if (m_MeshSelectMode == MeshSelectMode::Face) {
        for (const auto& face : mesh->GetFaces()) {
            if (face.id == mesh::INVALID_ID) continue;
            
            std::vector<ImVec2> screenVerts;
            bool allVisible = true;
            
            mesh->ForEachFaceVertex(face.id, [&](const mesh::EMVertex& v) {
                glm::vec3 worldPos = glm::vec3(modelMatrix * glm::vec4(v.position, 1.0f));
                glm::vec3 screenPos = WorldToScreen(worldPos);
                if (screenPos.z < 0 || screenPos.z > 1) allVisible = false;
                screenVerts.push_back(ImVec2(screenPos.x, screenPos.y));
            });
            
            if (allVisible && screenVerts.size() >= 3) {
                ImU32 fillColor = face.selected ? faceSelectedColor : faceColor;
                drawList->AddConvexPolyFilled(screenVerts.data(), static_cast<int>(screenVerts.size()), fillColor);
                
                // Draw outline for selected faces
                if (face.selected) {
                    for (size_t i = 0; i < screenVerts.size(); ++i) {
                        size_t next = (i + 1) % screenVerts.size();
                        drawList->AddLine(screenVerts[i], screenVerts[next], faceOutlineColor, 2.0f);
                    }
                }
            }
        }
    } else {
        // In other modes, still show selected faces
        for (const auto& face : mesh->GetFaces()) {
            if (face.id == mesh::INVALID_ID) continue;
            if (!face.selected) continue;
            
            std::vector<ImVec2> screenVerts;
            bool allVisible = true;
            
            mesh->ForEachFaceVertex(face.id, [&](const mesh::EMVertex& v) {
                glm::vec3 worldPos = glm::vec3(modelMatrix * glm::vec4(v.position, 1.0f));
                glm::vec3 screenPos = WorldToScreen(worldPos);
                if (screenPos.z < 0 || screenPos.z > 1) allVisible = false;
                screenVerts.push_back(ImVec2(screenPos.x, screenPos.y));
            });
            
            if (allVisible && screenVerts.size() >= 3) {
                drawList->AddConvexPolyFilled(screenVerts.data(), static_cast<int>(screenVerts.size()), faceSelectedColor);
            }
        }
    }
    
    // Draw edges (in all modes for better visibility)
    if (m_MeshSelectMode == MeshSelectMode::Edge || m_MeshSelectMode == MeshSelectMode::Vertex || m_MeshSelectMode == MeshSelectMode::Face) {
        for (const auto& e : mesh->GetEdges()) {
            if (e.id == mesh::INVALID_ID) continue;
            
            const mesh::EMVertex* v0 = mesh->GetVertex(e.v0);
            const mesh::EMVertex* v1 = mesh->GetVertex(e.v1);
            if (!v0 || !v1) continue;
            
            glm::vec3 worldP0 = glm::vec3(modelMatrix * glm::vec4(v0->position, 1.0f));
            glm::vec3 worldP1 = glm::vec3(modelMatrix * glm::vec4(v1->position, 1.0f));
            
            glm::vec3 screenP0 = WorldToScreen(worldP0);
            glm::vec3 screenP1 = WorldToScreen(worldP1);
            
            if (screenP0.z < 0 || screenP1.z < 0 || screenP0.z > 1 || screenP1.z > 1) continue;
            
            ImU32 color = e.selected ? edgeSelectedColor : edgeColor;
            float thickness = e.selected ? 2.0f : 1.0f;
            
            drawList->AddLine(
                ImVec2(screenP0.x, screenP0.y),
                ImVec2(screenP1.x, screenP1.y),
                color, thickness
            );
        }
    }
    
    // Draw vertices
    if (m_MeshSelectMode == MeshSelectMode::Vertex) {
        for (const auto& v : mesh->GetVertices()) {
            if (v.id == mesh::INVALID_ID) continue;
            
            glm::vec3 worldPos = glm::vec3(modelMatrix * glm::vec4(v.position, 1.0f));
            glm::vec3 screenPos = WorldToScreen(worldPos);
            
            if (screenPos.z < 0 || screenPos.z > 1) continue;
            
            ImU32 color = v.selected ? vertexSelectedColor : vertexColor;
            float radius = v.selected ? 5.0f : 3.0f;
            
            drawList->AddCircleFilled(ImVec2(screenPos.x, screenPos.y), radius, color);
        }
    }
}

// ============================================================================
// Interactive Transform (Blender-style G/R/S)
// ============================================================================

void EditorUI::StartInteractiveTransform(InteractiveTransformType type) {
    if (type == InteractiveTransformType::None) return;
    
    // Store starting mouse position
    ImVec2 mousePos = ImGui::GetMousePos();
    m_TransformStartMousePos = glm::vec2(mousePos.x, mousePos.y);
    m_AxisConstraint = AxisConstraint::None;
    m_InteractiveTransform = type;
    m_TransformNumeric.clear();
    
    if (m_EditorMode == EditorMode::Object) {
        // Object mode - store starting position of selected entity
        scene::Entity selected = GetSelectedEntity();
        if (!selected.IsValid()) {
            m_InteractiveTransform = InteractiveTransformType::None;
            return;
        }
        
        auto* transform = selected.GetComponent<scene::TransformComponent>();
        if (!transform) {
            m_InteractiveTransform = InteractiveTransformType::None;
            return;
        }
        
        m_TransformStartValue = transform->position;
        m_TransformStartRotation = transform->rotation;
        m_TransformStartScale = transform->scale;
        m_TransformPivotLocal = glm::vec3(0.0f); // unused for object mode
        switch (type) {
            case InteractiveTransformType::Grab:   LUCENT_CORE_INFO("Started interactive Grab (Object Mode)"); break;
            case InteractiveTransformType::Rotate: LUCENT_CORE_INFO("Started interactive Rotate (Object Mode)"); break;
            case InteractiveTransformType::Scale:  LUCENT_CORE_INFO("Started interactive Scale (Object Mode)"); break;
            default: break;
        }
    } else {
        // Edit mode - store starting positions of all selected vertices
        scene::Entity entity = GetEditedEntity();
        if (!entity.IsValid()) {
            m_InteractiveTransform = InteractiveTransformType::None;
            return;
        }
        
        auto* editMesh = entity.GetComponent<scene::EditableMeshComponent>();
        if (!editMesh || !editMesh->HasMesh()) {
            m_InteractiveTransform = InteractiveTransformType::None;
            return;
        }
        
        auto* meshPtr = editMesh->mesh.get();
        
        // Convert edge/face selection to vertex selection for grabbing
        // Store IDs of vertices to move and their starting positions
        m_TransformStartPositions.clear();
        m_TransformVertexIDs.clear();
        
        std::unordered_set<mesh::VertexID> vertexSet;
        
        // Collect vertices from current selection based on mode
        switch (m_MeshSelectMode) {
            case MeshSelectMode::Vertex:
                for (const auto& v : meshPtr->GetVertices()) {
                    if (v.selected && v.id != mesh::INVALID_ID) {
                        vertexSet.insert(v.id);
                    }
                }
                break;
            case MeshSelectMode::Edge:
                for (const auto& e : meshPtr->GetEdges()) {
                    if (e.selected && e.id != mesh::INVALID_ID) {
                        if (e.v0 != mesh::INVALID_ID) vertexSet.insert(e.v0);
                        if (e.v1 != mesh::INVALID_ID) vertexSet.insert(e.v1);
                    }
                }
                break;
            case MeshSelectMode::Face:
                for (const auto& f : meshPtr->GetFaces()) {
                    if (f.selected && f.id != mesh::INVALID_ID) {
                        meshPtr->ForEachFaceVertex(f.id, [&](const mesh::EMVertex& v) {
                            if (v.id != mesh::INVALID_ID) vertexSet.insert(v.id);
                        });
                    }
                }
                break;
        }
        
        // Store starting positions for all affected vertices
        for (mesh::VertexID vid : vertexSet) {
            const auto* v = meshPtr->GetVertex(vid);
            if (v) {
                m_TransformVertexIDs.push_back(vid);
                m_TransformStartPositions.push_back(v->position);
            }
        }
        
        if (m_TransformStartPositions.empty()) {
            m_InteractiveTransform = InteractiveTransformType::None;
            return;
        }
        
        // Compute pivot (selection center) in local space
        glm::vec3 center(0.0f);
        for (const auto& p : m_TransformStartPositions) center += p;
        center /= static_cast<float>(m_TransformStartPositions.size());
        m_TransformPivotLocal = center;
        
        switch (type) {
            case InteractiveTransformType::Grab:
                LUCENT_CORE_INFO("Started interactive Grab (Edit Mode) - {} vertices", m_TransformStartPositions.size());
                break;
            case InteractiveTransformType::Rotate:
                LUCENT_CORE_INFO("Started interactive Rotate (Edit Mode) - {} vertices", m_TransformStartPositions.size());
                break;
            case InteractiveTransformType::Scale:
                LUCENT_CORE_INFO("Started interactive Scale (Edit Mode) - {} vertices", m_TransformStartPositions.size());
                break;
            default: break;
        }
    }
}

void EditorUI::UpdateInteractiveTransform() {
    if (m_InteractiveTransform == InteractiveTransformType::None) return;
    
    auto parseNumeric = [&]() -> std::optional<float> {
        if (m_TransformNumeric.empty()) return std::nullopt;
        try {
            size_t idx = 0;
            float v = std::stof(m_TransformNumeric, &idx);
            if (idx != m_TransformNumeric.size()) return std::nullopt;
            return v;
        } catch (...) {
            return std::nullopt;
        }
    };
    
    // Calculate mouse delta
    ImVec2 mousePos = ImGui::GetMousePos();
    glm::vec2 mouseDelta = glm::vec2(mousePos.x, mousePos.y) - m_TransformStartMousePos;
    
    // Get camera for screen-to-world conversion
    if (!m_EditorCamera) return;
    
    // Get camera basis vectors from the view matrix
    // The view matrix rows contain the camera's basis vectors in world space
    glm::mat4 view = m_EditorCamera->GetViewMatrix();
    glm::vec3 camRight = glm::normalize(glm::vec3(view[0][0], view[1][0], view[2][0]));
    glm::vec3 camUp = glm::normalize(glm::vec3(view[0][1], view[1][1], view[2][1]));
    
    float sensitivity = m_TransformSensitivity;
    std::optional<float> numeric = parseNumeric();
    
    // =========================================================================
    // Grab (Translate)
    // =========================================================================
    if (m_InteractiveTransform == InteractiveTransformType::Grab) {
        glm::vec3 worldDelta(0.0f);
        
        switch (m_AxisConstraint) {
            case AxisConstraint::X: {
                float magnitude = numeric ? *numeric : (mouseDelta.x * sensitivity);
                worldDelta = glm::vec3(magnitude, 0, 0);
                break;
            }
            case AxisConstraint::Y: {
                float magnitude = numeric ? *numeric : (-mouseDelta.y * sensitivity);
                worldDelta = glm::vec3(0, magnitude, 0);
                break;
            }
            case AxisConstraint::Z: {
                float magnitude = numeric ? *numeric : (mouseDelta.x * sensitivity);
                worldDelta = glm::vec3(0, 0, magnitude);
                break;
            }
            case AxisConstraint::None:
            default: {
                worldDelta = (camRight * mouseDelta.x - camUp * mouseDelta.y) * sensitivity;
                if (numeric) {
                    float len = glm::length(worldDelta);
                    if (len > 1e-6f) worldDelta = (worldDelta / len) * (*numeric);
                }
                break;
            }
        }
        
        if (m_SnapEnabled) {
            worldDelta.x = std::round(worldDelta.x / m_TranslateSnap) * m_TranslateSnap;
            worldDelta.y = std::round(worldDelta.y / m_TranslateSnap) * m_TranslateSnap;
            worldDelta.z = std::round(worldDelta.z / m_TranslateSnap) * m_TranslateSnap;
        }
        
        if (m_EditorMode == EditorMode::Object) {
            scene::Entity selected = GetSelectedEntity();
            if (!selected.IsValid()) return;
            auto* transform = selected.GetComponent<scene::TransformComponent>();
            if (!transform) return;
            transform->position = m_TransformStartValue + worldDelta;
            m_SceneDirty = true;
        } else {
            scene::Entity entity = GetEditedEntity();
            if (!entity.IsValid()) return;
            auto* editMesh = entity.GetComponent<scene::EditableMeshComponent>();
            if (!editMesh || !editMesh->HasMesh()) return;
            
            auto* transform = entity.GetComponent<scene::TransformComponent>();
            glm::mat4 modelMatrix = transform ? transform->GetLocalMatrix() : glm::mat4(1.0f);
            glm::mat4 invModelMatrix = glm::inverse(modelMatrix);
            glm::vec3 localDelta = glm::vec3(invModelMatrix * glm::vec4(worldDelta, 0.0f));
            
            for (size_t idx = 0; idx < m_TransformVertexIDs.size() && idx < m_TransformStartPositions.size(); ++idx) {
                auto* vert = editMesh->mesh->GetVertex(m_TransformVertexIDs[idx]);
                if (vert) vert->position = m_TransformStartPositions[idx] + localDelta;
            }
            
            editMesh->mesh->RecalculateNormals();
            editMesh->MarkDirty();
            m_SceneDirty = true;
        }
        
        return;
    }
    
    // =========================================================================
    // Rotate
    // =========================================================================
    if (m_InteractiveTransform == InteractiveTransformType::Rotate) {
        float degrees = numeric ? *numeric : (mouseDelta.x * sensitivity * 50.0f);
        if (m_SnapEnabled) degrees = std::round(degrees / m_RotateSnap) * m_RotateSnap;
        
        glm::vec3 axis(0.0f, 1.0f, 0.0f);
        switch (m_AxisConstraint) {
            case AxisConstraint::X: axis = glm::vec3(1, 0, 0); break;
            case AxisConstraint::Y: axis = glm::vec3(0, 1, 0); break;
            case AxisConstraint::Z: axis = glm::vec3(0, 0, 1); break;
            case AxisConstraint::None: default: break;
        }
        
        if (m_EditorMode == EditorMode::Object) {
            scene::Entity selected = GetSelectedEntity();
            if (!selected.IsValid()) return;
            auto* transform = selected.GetComponent<scene::TransformComponent>();
            if (!transform) return;
            
            if (m_AxisConstraint == AxisConstraint::None) {
                transform->rotation = m_TransformStartRotation + glm::vec3(mouseDelta.y, -mouseDelta.x, 0.0f) * (sensitivity * 50.0f);
            } else {
                glm::vec3 delta(0.0f);
                if (axis.x != 0) delta.x = degrees;
                if (axis.y != 0) delta.y = degrees;
                if (axis.z != 0) delta.z = degrees;
                transform->rotation = m_TransformStartRotation + delta;
            }
            m_SceneDirty = true;
        } else {
            scene::Entity entity = GetEditedEntity();
            if (!entity.IsValid()) return;
            auto* editMesh = entity.GetComponent<scene::EditableMeshComponent>();
            if (!editMesh || !editMesh->HasMesh()) return;
            
            glm::mat4 rot(1.0f);
            if (m_AxisConstraint == AxisConstraint::None) {
                float yaw = (-mouseDelta.x) * (sensitivity * 50.0f);
                float pitch = (mouseDelta.y) * (sensitivity * 50.0f);
                rot = glm::rotate(glm::mat4(1.0f), glm::radians(yaw), glm::vec3(0, 1, 0));
                rot = glm::rotate(rot, glm::radians(pitch), glm::vec3(1, 0, 0));
            } else {
                rot = glm::rotate(glm::mat4(1.0f), glm::radians(degrees), axis);
            }
            
            for (size_t idx = 0; idx < m_TransformVertexIDs.size() && idx < m_TransformStartPositions.size(); ++idx) {
                auto* vert = editMesh->mesh->GetVertex(m_TransformVertexIDs[idx]);
                if (!vert) continue;
                glm::vec3 p = m_TransformStartPositions[idx] - m_TransformPivotLocal;
                glm::vec3 pr = glm::vec3(rot * glm::vec4(p, 0.0f));
                vert->position = m_TransformPivotLocal + pr;
            }
            
            editMesh->mesh->RecalculateNormals();
            editMesh->MarkDirty();
            m_SceneDirty = true;
        }
        
        return;
    }
    
    // =========================================================================
    // Scale
    // =========================================================================
    if (m_InteractiveTransform == InteractiveTransformType::Scale) {
        float factor = numeric ? *numeric : (1.0f + (mouseDelta.x * sensitivity));
        factor = std::max(factor, 0.001f);
        
        if (m_SnapEnabled) {
            float step = m_ScaleSnap;
            float delta = factor - 1.0f;
            delta = std::round(delta / step) * step;
            factor = 1.0f + delta;
        }
        
        if (m_EditorMode == EditorMode::Object) {
            scene::Entity selected = GetSelectedEntity();
            if (!selected.IsValid()) return;
            auto* transform = selected.GetComponent<scene::TransformComponent>();
            if (!transform) return;
            
            glm::vec3 newScale = m_TransformStartScale;
            switch (m_AxisConstraint) {
                case AxisConstraint::X: newScale.x = m_TransformStartScale.x * factor; break;
                case AxisConstraint::Y: newScale.y = m_TransformStartScale.y * factor; break;
                case AxisConstraint::Z: newScale.z = m_TransformStartScale.z * factor; break;
                case AxisConstraint::None: default: newScale = m_TransformStartScale * factor; break;
            }
            transform->scale = newScale;
            m_SceneDirty = true;
        } else {
            scene::Entity entity = GetEditedEntity();
            if (!entity.IsValid()) return;
            auto* editMesh = entity.GetComponent<scene::EditableMeshComponent>();
            if (!editMesh || !editMesh->HasMesh()) return;
            
            glm::vec3 scaleVec(1.0f);
            switch (m_AxisConstraint) {
                case AxisConstraint::X: scaleVec = glm::vec3(factor, 1, 1); break;
                case AxisConstraint::Y: scaleVec = glm::vec3(1, factor, 1); break;
                case AxisConstraint::Z: scaleVec = glm::vec3(1, 1, factor); break;
                case AxisConstraint::None: default: scaleVec = glm::vec3(factor); break;
            }
            
            for (size_t idx = 0; idx < m_TransformVertexIDs.size() && idx < m_TransformStartPositions.size(); ++idx) {
                auto* vert = editMesh->mesh->GetVertex(m_TransformVertexIDs[idx]);
                if (!vert) continue;
                glm::vec3 p = m_TransformStartPositions[idx] - m_TransformPivotLocal;
                vert->position = m_TransformPivotLocal + (p * scaleVec);
            }
            
            editMesh->mesh->RecalculateNormals();
            editMesh->MarkDirty();
            m_SceneDirty = true;
        }
        
        return;
    }
}

void EditorUI::ConfirmInteractiveTransform() {
    if (m_InteractiveTransform == InteractiveTransformType::None) return;
    
    if (m_EditorMode == EditorMode::Object) {
        // Push undo command for object transform
        scene::Entity selected = GetSelectedEntity();
        if (selected.IsValid()) {
            auto* transform = selected.GetComponent<scene::TransformComponent>();
            if (transform) {
                TransformCommand::TransformState before{
                    m_TransformStartValue,
                    m_TransformStartRotation,
                    m_TransformStartScale
                };
                TransformCommand::TransformState after{
                    transform->position,
                    transform->rotation,
                    transform->scale
                };
                auto cmd = std::make_unique<TransformCommand>(
                    m_Scene, 
                    selected.GetID(),
                    before,
                    after
                );
                UndoStack::Get().Push(std::move(cmd));
            }
        }
        switch (m_InteractiveTransform) {
            case InteractiveTransformType::Grab:   LUCENT_CORE_INFO("Confirmed interactive Grab (Object Mode)"); break;
            case InteractiveTransformType::Rotate: LUCENT_CORE_INFO("Confirmed interactive Rotate (Object Mode)"); break;
            case InteractiveTransformType::Scale:  LUCENT_CORE_INFO("Confirmed interactive Scale (Object Mode)"); break;
            default: break;
        }
    } else {
        // In Edit mode, push a mesh edit command
        scene::Entity entity = GetEditedEntity();
        if (entity.IsValid()) {
            auto* editMesh = entity.GetComponent<scene::EditableMeshComponent>();
            if (editMesh && editMesh->HasMesh()) {
                // We need to capture before/after snapshots - for now just log
                switch (m_InteractiveTransform) {
                    case InteractiveTransformType::Grab:   LUCENT_CORE_INFO("Confirmed interactive Grab (Edit Mode)"); break;
                    case InteractiveTransformType::Rotate: LUCENT_CORE_INFO("Confirmed interactive Rotate (Edit Mode)"); break;
                    case InteractiveTransformType::Scale:  LUCENT_CORE_INFO("Confirmed interactive Scale (Edit Mode)"); break;
                    default: break;
                }
                // TODO: Add proper undo for edit mode vertex movement
            }
        }
    }
    
    m_InteractiveTransform = InteractiveTransformType::None;
    m_AxisConstraint = AxisConstraint::None;
    m_TransformStartPositions.clear();
    m_TransformVertexIDs.clear();
    m_TransformNumeric.clear();
}

void EditorUI::CancelInteractiveTransform() {
    if (m_InteractiveTransform == InteractiveTransformType::None) return;
    
    if (m_EditorMode == EditorMode::Object) {
        // Restore original transform
        scene::Entity selected = GetSelectedEntity();
        if (selected.IsValid()) {
            auto* transform = selected.GetComponent<scene::TransformComponent>();
            if (transform) {
                transform->position = m_TransformStartValue;
                transform->rotation = m_TransformStartRotation;
                transform->scale = m_TransformStartScale;
            }
        }
        switch (m_InteractiveTransform) {
            case InteractiveTransformType::Grab:   LUCENT_CORE_INFO("Cancelled interactive Grab (Object Mode)"); break;
            case InteractiveTransformType::Rotate: LUCENT_CORE_INFO("Cancelled interactive Rotate (Object Mode)"); break;
            case InteractiveTransformType::Scale:  LUCENT_CORE_INFO("Cancelled interactive Scale (Object Mode)"); break;
            default: break;
        }
    } else {
        // Restore original vertex positions using stored IDs
        scene::Entity entity = GetEditedEntity();
        if (entity.IsValid()) {
            auto* editMesh = entity.GetComponent<scene::EditableMeshComponent>();
            if (editMesh && editMesh->HasMesh()) {
                for (size_t idx = 0; idx < m_TransformVertexIDs.size() && idx < m_TransformStartPositions.size(); ++idx) {
                    auto* vert = editMesh->mesh->GetVertex(m_TransformVertexIDs[idx]);
                    if (vert) {
                        vert->position = m_TransformStartPositions[idx];
                    }
                }
                editMesh->mesh->RecalculateNormals();
                editMesh->MarkDirty();
            }
        }
        switch (m_InteractiveTransform) {
            case InteractiveTransformType::Grab:   LUCENT_CORE_INFO("Cancelled interactive Grab (Edit Mode)"); break;
            case InteractiveTransformType::Rotate: LUCENT_CORE_INFO("Cancelled interactive Rotate (Edit Mode)"); break;
            case InteractiveTransformType::Scale:  LUCENT_CORE_INFO("Cancelled interactive Scale (Edit Mode)"); break;
            default: break;
        }
    }
    
    m_InteractiveTransform = InteractiveTransformType::None;
    m_AxisConstraint = AxisConstraint::None;
    m_TransformStartPositions.clear();
    m_TransformVertexIDs.clear();
    m_TransformNumeric.clear();
    m_SceneDirty = true;
}

void EditorUI::DrawInteractiveTransformHUD() {
    if (m_InteractiveTransform == InteractiveTransformType::None) return;
    
    // Draw HUD at bottom of viewport
    ImDrawList* drawList = ImGui::GetWindowDrawList();
    
    float hudY = m_ViewportPosition.y + m_ViewportSize.y - 40;
    float hudX = m_ViewportPosition.x + 10;
    
    // Background
    ImVec2 bgMin(hudX - 5, hudY - 5);
    ImVec2 bgMax(hudX + 350, hudY + 30);
    drawList->AddRectFilled(bgMin, bgMax, IM_COL32(0, 0, 0, 180), 4.0f);
    
    // Build status string
    std::string typeStr;
    switch (m_InteractiveTransform) {
        case InteractiveTransformType::Grab: typeStr = "GRAB (G)"; break;
        case InteractiveTransformType::Rotate: typeStr = "ROTATE (R)"; break;
        case InteractiveTransformType::Scale: typeStr = "SCALE (S)"; break;
        default: break;
    }
    
    std::string axisStr;
    ImU32 axisColor = IM_COL32(255, 255, 255, 255);
    switch (m_AxisConstraint) {
        case AxisConstraint::X: 
            axisStr = " [X AXIS]"; 
            axisColor = IM_COL32(255, 80, 80, 255);
            break;
        case AxisConstraint::Y: 
            axisStr = " [Y AXIS]"; 
            axisColor = IM_COL32(80, 255, 80, 255);
            break;
        case AxisConstraint::Z: 
            axisStr = " [Z AXIS]"; 
            axisColor = IM_COL32(80, 80, 255, 255);
            break;
        default:
            axisStr = " [FREE]";
            break;
    }
    
    // Draw type text
    drawList->AddText(ImVec2(hudX, hudY), IM_COL32(255, 200, 100, 255), typeStr.c_str());
    
    // Draw axis constraint
    drawList->AddText(ImVec2(hudX + 100, hudY), axisColor, axisStr.c_str());
    
    // Draw help text
    std::string help = "X/Y/Z: Lock axis | Enter/LMB: Confirm | ESC/RMB: Cancel";
    if (!m_TransformNumeric.empty()) {
        help += " | Value: " + m_TransformNumeric;
    }
    drawList->AddText(ImVec2(hudX, hudY + 15), IM_COL32(180, 180, 180, 255), help.c_str());
}

} // namespace lucent
