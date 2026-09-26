#include "ui_manager.h"

#include "scene/scene_manager.h"
#include "scene/scene_raytracing_render.h"
#include "tools/font_loader.h"
#include "ui_camera.h"
#include "ui_light.h"
#include "ui_node.h"
#include "ui_record.h"
#include "vulkan_core/vulkan_application.h"
#include "vulkan_core/vulkan_common.h"

#include <algorithm>
#include <array>
#include <imgui_impl_glfw.h>
#include <imgui_impl_vulkan.h>
#include <iostream>
#include <print>
#include <string_view>
#include <utility>


namespace {

constexpr std::string_view SCENE_SETTING             = "场景设置";
constexpr std::string_view SCENE_MANAGER             = "场景管理";
constexpr std::string_view RENDER_MODE               = "渲染模式";
constexpr std::string_view RENDER_MODE_RASTERIZATION = "光栅化";
constexpr std::string_view RENDER_MODE_RAY_TRACING   = "光线追踪 (RT Pipeline)";

// PBR 物理正确性调试面板
constexpr std::string_view PBR_DEBUG           = "PBR 物理调试";
constexpr std::string_view PBR_DISABLE_CLAMPS  = "关闭全部启发式钳制(能量实验必需)";
constexpr std::string_view PBR_FORCE_MATERIAL  = "强制材质参数(白炉测试)";
constexpr std::string_view PBR_FORCE_METALLIC  = "强制金属度";
constexpr std::string_view PBR_FORCE_ROUGHNESS = "强制粗糙度";
constexpr std::string_view PBR_FORCE_ALBEDO    = "强制反照率 = 白";
constexpr std::string_view PBR_DISABLE_AMBIENT = "关闭恒定环境光";
constexpr std::string_view PBR_VIZ             = "可视化通道";
constexpr std::string_view PBR_VIZ_NONE        = "关闭";
constexpr std::string_view PBR_VIZ_NORMAL      = "法线";
constexpr std::string_view PBR_VIZ_F0          = "F0";
constexpr std::string_view PBR_VIZ_ROUGHNESS   = "粗糙度";
constexpr std::string_view PBR_VIZ_METALLIC    = "金属度";
constexpr std::string_view PBR_VIZ_DIRECT      = "仅直接光";
constexpr std::string_view PBR_DIRECT_ONLY     = "只显示直接光(隔离 NEE,排除天空与弹射)";

#ifndef NDEBUG
void imgui_callback(VkResult _result) noexcept
{
    if (_result != VK_SUCCESS) [[unlikely]]
    {
        std::println(std::cerr, "imgui error: {}", vk::to_string(static_cast<vk::Result>(_result)));
    }
}
#endif  // !NDEBUG

}  // namespace


ui_manager::ui_manager(GLFWwindow* _window, vulkan_application& _app, scene_manager& _manager, uint32_t _width, uint32_t _height)
    : app(_app)
    , manager(_manager)
{
    semaphore.create(*app.get_device());
    recycle_bin.create(&semaphore);

    ui_managers.emplace_back(pro::make_proxy<ui_base, ui_camera>(manager));
    ui_managers.emplace_back(pro::make_proxy<ui_base, ui_light>(manager));
    ui_managers.emplace_back(pro::make_proxy<ui_base, ui_node>(manager));
    ui_managers.emplace_back(pro::make_proxy<ui_base, ui_record>(manager));


    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();

    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;  // Enable Keyboard Controls
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableGamepad;   // Enable Gamepad Controls
    io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;      // Enable Docking
    io.ConfigFlags |= ImGuiConfigFlags_ViewportsEnable;    // Enable Multi-Viewport / Platform Windows

    ImGui::StyleColorsDark();
    ImGuiStyle& style = ImGui::GetStyle();
    if (io.ConfigFlags & ImGuiConfigFlags_ViewportsEnable)
    {
        style.WindowRounding              = 0.0f;
        style.Colors[ImGuiCol_WindowBg].w = 1.0f;
    }

    io.Fonts->AddFontFromFileTTF((std::string(FONTS_PATH) + "LXGWWenKaiGB-Medium.ttf").c_str(), 13.0f, nullptr,
                                 io.Fonts->GetGlyphRangesChineseFull());

    ImGui_ImplGlfw_InitForVulkan(_window, true);

    graphic_queue.create(*app.get_device(), app.get_physical_device().get_queue_index(vk::QueueFlagBits::eGraphics));

    constexpr std::array pool_size{
        vk::DescriptorPoolSize(vk::DescriptorType::eSampledImage, IMGUI_IMPL_VULKAN_MINIMUM_SAMPLED_IMAGE_POOL_SIZE),
        vk::DescriptorPoolSize(vk::DescriptorType::eSampler, IMGUI_IMPL_VULKAN_MINIMUM_SAMPLER_POOL_SIZE)};
    const vk::DescriptorPoolCreateInfo pool_info(vk::DescriptorPoolCreateFlagBits::eFreeDescriptorSet,
                                                 (IMGUI_IMPL_VULKAN_MINIMUM_SAMPLED_IMAGE_POOL_SIZE + IMGUI_IMPL_VULKAN_MINIMUM_SAMPLER_POOL_SIZE)
                                                     * vulkan_common::MAX_FRAMES_IN_FLIGHT,
                                                 pool_size);
    descriptor_pool = vk::raii::DescriptorPool(*app.get_device(), pool_info);

    color_format                                    = app.get_swapchain().get_format();
    const ImGui_ImplVulkan_PipelineInfo create_info = {
        .MSAASamples = static_cast<VkSampleCountFlagBits>(vulkan_common::MSAA_SAMPLE_COUNT),
        .PipelineRenderingCreateInfo =
            vk::PipelineRenderingCreateInfo({}, color_format, vk::Format::eUndefined, vk::Format::eUndefined, nullptr),
    };

    ImGui_ImplVulkan_InitInfo init_info = {
        .ApiVersion          = vk::ApiVersion14,
        .Instance            = *(app.get_instance()),
        .PhysicalDevice      = *(*app.get_physical_device()),
        .Device              = *(*app.get_device()),
        .QueueFamily         = graphic_queue.get_index(),
        .Queue               = *(graphic_queue.get_queue()),
        .DescriptorPool      = *descriptor_pool,
        .MinImageCount       = vulkan_common::MAX_FRAMES_IN_FLIGHT,
        .ImageCount          = vulkan_common::MAX_FRAMES_IN_FLIGHT,
        .PipelineInfoMain    = create_info,
        .UseDynamicRendering = true,
#ifndef NDEBUG
        .CheckVkResultFn = imgui_callback,
#endif  // !NDEBUG
    };
    ImGui_ImplVulkan_Init(&init_info);

    commandbuffers = vulkan_commandbuffer::create(*app.get_device(),
                                                  vk::CommandBufferAllocateInfo(graphic_queue.get_command_pool(),
                                                                                vk::CommandBufferLevel::ePrimary,
                                                                                vulkan_common::MAX_FRAMES_IN_FLIGHT),
                                                  &graphic_queue, &semaphore);

    resize(_width, _height);
}

ui_manager::~ui_manager()
{
    app.wait_idle();

    ImGui_ImplVulkan_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    ImGui::DestroyContext();
}

void ui_manager::resize(uint32_t _width, uint32_t _height)
{
    //ImGui_ImplVulkanH_CreateOrResizeWindow(g_Instance, g_PhysicalDevice, g_Device, wd, g_QueueFamily, g_Allocator, fb_width, fb_height, g_MinImageCount, 0);
    const std::array queue_array = {graphic_queue.get_index()};
    // render_output
    recycle_bin.retire(std::move(render_output), "ui old render output.");
    vk::ImageCreateInfo render_image_info({}, vk::ImageType::e2D, color_format, vk::Extent3D(_width, _height, 1), 1, 1,
                                          vk::SampleCountFlagBits::e1, vk::ImageTiling::eOptimal,
                                          vk::ImageUsageFlagBits::eColorAttachment | vk::ImageUsageFlagBits::eSampled,
                                          vk::SharingMode::eExclusive, queue_array);
    vk::ImageViewCreateInfo render_view_info({}, {}, vk::ImageViewType::e2D, color_format, {},
                                             vk::ImageSubresourceRange(vk::ImageAspectFlagBits::eColor, {}, 1, 0, 1), nullptr);
    render_output.create(app.get_allocator(), *app.get_device(), render_image_info, render_view_info,
                         vma::MemoryUsage::eAutoPreferDevice, {}, vk::ClearColorValue(0.f, 0.f, 0.f, 0.f));

    // msaa color
    recycle_bin.retire(std::move(color_image), "ui old color image.");
    vk::ImageCreateInfo color_image_info({}, vk::ImageType::e2D, color_format, vk::Extent3D(_width, _height, 1), 1, 1,
                                         vulkan_common::MSAA_SAMPLE_COUNT, vk::ImageTiling::eOptimal,
                                         vk::ImageUsageFlagBits::eColorAttachment, vk::SharingMode::eExclusive, queue_array);
    vk::ImageViewCreateInfo color_view_info({}, {}, vk::ImageViewType::e2D, color_format, {},
                                            vk::ImageSubresourceRange(vk::ImageAspectFlagBits::eColor, {}, 1, 0, 1), nullptr);
    color_image.create(app.get_allocator(), *app.get_device(), color_image_info, color_view_info,
                       vma::MemoryUsage::eAutoPreferDevice, {}, vk::ClearColorValue(0.f, 0.f, 0.f, 0.f));


    std::ranges::for_each(ui_managers, [&](auto& m) { m->resize(_width, _height); });
}

void ui_manager::update()
{
    ImGui_ImplVulkan_NewFrame();
    ImGui_ImplGlfw_NewFrame();
    ImGui::NewFrame();

    bool show_demo_window = true;

    //ImGui::ShowDemoWindow(&show_demo_window);

    ImGui::Begin(SCENE_SETTING.data(), &show_demo_window);

    render_mode_ui();
    std::ranges::for_each(ui_managers, [](auto& m) static { m->update(); });

    ImGui::End();

    ImGui::Render();
    draw_data = ImGui::GetDrawData();
}

vk::SemaphoreSubmitInfo ui_manager::render()
{
    recycle_bin.release();

    vulkan_commandbuffer& commandbuffer = commandbuffers.at(current_frame);
    commandbuffer.begin_record({});


    const auto color_image_barrier =
        color_image.transition_state(vk::PipelineStageFlagBits2::eColorAttachmentOutput, vk::AccessFlagBits2::eColorAttachmentWrite,
                                     vk::ImageLayout::eColorAttachmentOptimal, vk::QueueFamilyIgnored);

    const auto render_output_barrier =
        render_output.transition_state(vk::PipelineStageFlagBits2::eColorAttachmentOutput, vk::AccessFlagBits2::eColorAttachmentWrite,
                                       vk::ImageLayout::eColorAttachmentOptimal, vk::QueueFamilyIgnored);

    const std::array begin_barriers = {color_image_barrier, render_output_barrier};
    (*commandbuffer).pipelineBarrier2(vk::DependencyInfo({}, {}, {}, begin_barriers, nullptr));

    const vk::RenderingAttachmentInfo color_attachment_info(
        color_image.get_imageview(), vk::ImageLayout::eColorAttachmentOptimal, vk::ResolveModeFlagBits::eAverage,
        render_output.get_imageview(), vk::ImageLayout::eColorAttachmentOptimal, vk::AttachmentLoadOp::eClear,
        vk::AttachmentStoreOp::eStore, color_image.get_clear_value());

    const vk::RenderingInfo rendering_info({}, vk::Rect2D({0, 0}, app.get_swapchain().get_extent()), 1, {},
                                           color_attachment_info, nullptr, nullptr, nullptr);

    (*commandbuffer).beginRendering(rendering_info);

    ImGui_ImplVulkan_RenderDrawData(draw_data, *(*(commandbuffer)));

    (*commandbuffer).endRendering();


    commandbuffer.end_record();
    commandbuffer.submit();

    if (ImGui::GetIO().ConfigFlags & ImGuiConfigFlags_ViewportsEnable)
    {
        ImGui::UpdatePlatformWindows();
        ImGui::RenderPlatformWindowsDefault();
    }

    current_frame = (current_frame + 1) % vulkan_common::MAX_FRAMES_IN_FLIGHT;

    return commandbuffer.get_submit_info();
}

void ui_manager::handle(int _key, int _scancode, int _action, int _mods) noexcept
{
    std::ranges::for_each(ui_managers, [&](auto& m) { m->handle(_key, _scancode, _action, _mods); });
}

vulkan_image& ui_manager::get_render_image() noexcept
{
    return render_output;
}

void ui_manager::render_mode_ui() noexcept
{
    ImGui::SeparatorText(SCENE_MANAGER.data());

    constexpr std::array render_mode_names = {RENDER_MODE_RASTERIZATION.data(), RENDER_MODE_RAY_TRACING.data()};

    if (ImGui::Combo(RENDER_MODE.data(), &current_mode, render_mode_names.data(), static_cast<int>(render_mode_names.size())))
    {
        manager.set_render_mode(static_cast<render_mode>(current_mode));
    }

    pbr_debug_ui();
}

void ui_manager::pbr_debug_ui() noexcept
{
    ImGui::SeparatorText(PBR_DEBUG.data());

    // 光栅化模式不消费这些开关(渲染器实现为空操作),但仍允许读写以保留状态
    if (current_mode != static_cast<int>(render_mode::ray_tracing))
    {
        ImGui::TextUnformatted("仅光线追踪模式生效");
    }

    uint32_t flags                    = manager.get_debug_flags();
    bool     disable_clamps  = (flags & static_cast<uint32_t>(pbr_debug_flags::disable_firefly_clamp)) != 0u;
    bool     force_material  = (flags & static_cast<uint32_t>(pbr_debug_flags::force_metallic)) != 0u;
    bool     force_albedo    = (flags & static_cast<uint32_t>(pbr_debug_flags::force_albedo)) != 0u;
    bool     disable_ambient = (flags & static_cast<uint32_t>(pbr_debug_flags::disable_ambient)) != 0u;

    // ---- 一键白炉:关闭全部钳制 + 只把反照率强制为白 ----
    // 注意**不**强制 metallic/roughness —— 那样会把球体阵抹成同一材质,
    // 失去"逐格核查"的意义。要单独隔离某个参数时再用下面的强制开关。
    ImGui::SeparatorText("白炉测试(能量守恒)");
    if (ImGui::Button("启动白炉预设(保留每格材质)"))
    {
        manager.set_debug_flags(furnace_preset());
    }
    ImGui::SameLine();
    if (ImGui::Button("白炉 + 关环境光"))
    {
        manager.set_debug_flags(furnace_preset_direct_only());
    }
    ImGui::TextUnformatted("配合「棋局管理 → 显示球体阵」逐格核查方向反射率");

    // ---- 一键白炉:关闭全部钳制 + 强制白反照率 ----
    constexpr uint32_t clamp_bits = static_cast<uint32_t>(pbr_debug_flags::disable_firefly_clamp)
                                  | static_cast<uint32_t>(pbr_debug_flags::disable_direct_clamp)
                                  | static_cast<uint32_t>(pbr_debug_flags::disable_throughput_clamp);
    if (ImGui::Checkbox(PBR_DISABLE_CLAMPS.data(), &disable_clamps))
    {
        flags = disable_clamps ? (flags | clamp_bits) : (flags & ~clamp_bits);
        manager.set_debug_flags(flags);
    }

    if (ImGui::Checkbox(PBR_FORCE_MATERIAL.data(), &force_material))
    {
        constexpr uint32_t force_bits = static_cast<uint32_t>(pbr_debug_flags::force_metallic)
                                      | static_cast<uint32_t>(pbr_debug_flags::force_roughness);
        flags = force_material ? (flags | force_bits) : (flags & ~force_bits);
        manager.set_debug_flags(flags);
    }

    float force_metallic  = manager.get_debug_force_metallic();
    float force_roughness = manager.get_debug_force_roughness();
    // 两个 Slider 都要各自求值(不能 || 短路,否则先拖动的那个会吞掉另一个)
    const bool metallic_changed  = ImGui::SliderFloat(PBR_FORCE_METALLIC.data(), &force_metallic, 0.0f, 1.0f);
    const bool roughness_changed = ImGui::SliderFloat(PBR_FORCE_ROUGHNESS.data(), &force_roughness, 0.02f, 1.0f);
    if (metallic_changed || roughness_changed)
    {
        manager.set_debug_mat_override(force_metallic, force_roughness);
    }

    if (ImGui::Checkbox(PBR_FORCE_ALBEDO.data(), &force_albedo))
    {
        flags = force_albedo ? (flags | static_cast<uint32_t>(pbr_debug_flags::force_albedo))
                             : (flags & ~static_cast<uint32_t>(pbr_debug_flags::force_albedo));
        manager.set_debug_flags(flags);
    }

    if (ImGui::Checkbox(PBR_DISABLE_AMBIENT.data(), &disable_ambient))
    {
        flags = disable_ambient ? (flags | static_cast<uint32_t>(pbr_debug_flags::disable_ambient))
                                : (flags & ~static_cast<uint32_t>(pbr_debug_flags::disable_ambient));
        manager.set_debug_flags(flags);
    }

    // ---- 可视化通道(互斥) ----
    constexpr std::array viz_names = {PBR_VIZ_NONE.data(), PBR_VIZ_NORMAL.data(), PBR_VIZ_F0.data(),
                                      PBR_VIZ_ROUGHNESS.data(), PBR_VIZ_METALLIC.data()};
    constexpr uint32_t viz_mask = static_cast<uint32_t>(pbr_debug_flags::viz_normal)
                                | static_cast<uint32_t>(pbr_debug_flags::viz_f0)
                                | static_cast<uint32_t>(pbr_debug_flags::viz_roughness)
                                | static_cast<uint32_t>(pbr_debug_flags::viz_metallic);

    int viz_current = 0;
    if ((flags & static_cast<uint32_t>(pbr_debug_flags::viz_normal)) != 0u)
        viz_current = 1;
    else if ((flags & static_cast<uint32_t>(pbr_debug_flags::viz_f0)) != 0u)
        viz_current = 2;
    else if ((flags & static_cast<uint32_t>(pbr_debug_flags::viz_roughness)) != 0u)
        viz_current = 3;
    else if ((flags & static_cast<uint32_t>(pbr_debug_flags::viz_metallic)) != 0u)
        viz_current = 4;

    if (ImGui::Combo(PBR_VIZ.data(), &viz_current, viz_names.data(), static_cast<int>(viz_names.size())))
    {
        flags &= ~viz_mask;
        switch (viz_current)
        {
            case 1:
                flags |= static_cast<uint32_t>(pbr_debug_flags::viz_normal);
                break;
            case 2:
                flags |= static_cast<uint32_t>(pbr_debug_flags::viz_f0);
                break;
            case 3:
                flags |= static_cast<uint32_t>(pbr_debug_flags::viz_roughness);
                break;
            case 4:
                flags |= static_cast<uint32_t>(pbr_debug_flags::viz_metallic);
                break;
            default:
                break;
        }
        manager.set_debug_flags(flags);
    }

    bool direct_only = (flags & static_cast<uint32_t>(pbr_debug_flags::viz_direct_only)) != 0u;
    if (ImGui::Checkbox(PBR_DIRECT_ONLY.data(), &direct_only))
    {
        flags = direct_only ? (flags | static_cast<uint32_t>(pbr_debug_flags::viz_direct_only))
                            : (flags & ~static_cast<uint32_t>(pbr_debug_flags::viz_direct_only));
        manager.set_debug_flags(flags);
    }
}
