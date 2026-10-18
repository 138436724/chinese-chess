/**
 * @file ui_manager.cpp
 * @brief ui_manager 的实现：ImGui 初始化、主 UI 逐帧录制与副窗口 WSI 编排。
 * @details 构造按 context → 字体 → GLFW backend → 描述符池 → Vulkan backend → 命令缓冲 →
 *          离屏图 → 副窗口渲染器的顺序初始化；update 在“场景设置”窗口里构建全部面板；
 *          render 用 MSAA 附件画 draw data、解析到 render_output 后提交，再驱动平台窗口路径。
 */
#include "ui_manager.h"

#include "scene/scene_manager.h"
#include "tools/font_loader.h"
#include "ui_camera.h"
#include "ui_environment.h"
#include "ui_light.h"
#include "ui_node.h"
#include "ui_record.h"
#include "ui_viewport_renderer.h"
#include "vulkan_core/vulkan_application.h"
#include "vulkan_core/vulkan_commandbuffer.h"
#include "vulkan_core/vulkan_common.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <imgui.h>
#include <imgui_impl_glfw.h>
#include <imgui_impl_vulkan.h>
#include <iostream>
#include <mutex>
#include <print>
#include <string>
#include <string_view>
#include <utility>


namespace {

constexpr std::string_view SCENE_SETTING             = "场景设置";
constexpr std::string_view SCENE_MANAGER             = "场景管理";
constexpr std::string_view RENDER_MODE               = "渲染模式";
constexpr std::string_view RENDER_MODE_RASTERIZATION = "光栅化";
constexpr std::string_view RENDER_MODE_RAY_TRACING   = "光线追踪 (RT Pipeline)";
constexpr std::string_view RENDER_MODE_RAY_QUERY     = "光线追踪 (Ray Query)";

#ifndef NDEBUG
/** @brief Debug 构建的 ImGui VkResult 回调：非 VK_SUCCESS 时把结果打到 stderr。 */
void imgui_callback(VkResult _result) noexcept
{
    if (_result != VK_SUCCESS) [[unlikely]]
    {
        std::println(std::cerr, "imgui error: {}", vk::to_string(static_cast<vk::Result>(_result)));
    }
}
#endif  // !NDEBUG

}  // namespace


/**
 * @brief 初始化 ImGui 双 backend、描述符池、命令缓冲、离屏图与副窗口渲染器。
 * @note 构造期间先按固定顺序注册五个面板，再建 context 与 backend；末尾 resize 会创建离屏图。
 */
ui_manager::ui_manager(GLFWwindow* _window, vulkan_application& _app, scene_manager& _manager, uint32_t _width, uint32_t _height)
    : app(_app)
    , graphic_queue(app.get_graphic_queue())
    , manager(_manager)
{
    recycle_bin.add_queue(graphic_queue);

    ui_managers.emplace_back(pro::make_proxy<ui_base, ui_camera>(manager));
    ui_managers.emplace_back(pro::make_proxy<ui_base, ui_light>(manager));
    ui_managers.emplace_back(pro::make_proxy<ui_base, ui_node>(manager));
    ui_managers.emplace_back(pro::make_proxy<ui_base, ui_record>(manager));
    ui_managers.emplace_back(pro::make_proxy<ui_base, ui_environment>(manager));


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
                                                  &graphic_queue);

    resize(_width, _height);
    viewport_renderer = std::make_unique<ui_viewport_renderer>(app);
}

/**
 * @brief 等 GPU 空闲后按平台窗口 → 副窗口渲染器 → 两个 backend → context 的顺序清理。
 * @note DestroyPlatformWindows 会回调副窗口的 destroy，先于 backend Shutdown 才能安全释放上游缓冲。
 */
ui_manager::~ui_manager()
{
    app.wait_idle_for_cleanup();

    ImGui::DestroyPlatformWindows();
    viewport_renderer.reset();
    ImGui_ImplVulkan_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    ImGui::DestroyContext();
}

/**
 * @brief 重建 render_output 与 MSAA color_image，并把新尺寸转发给所有面板。
 * @note 旧图先 retire_after_submitted 再创建新图，两张图都用图形队列的单一族做 exclusive sharing。
 */
void ui_manager::resize(uint32_t _width, uint32_t _height)
{
    //ImGui_ImplVulkanH_CreateOrResizeWindow(g_Instance, g_PhysicalDevice, g_Device, wd, g_QueueFamily, g_Allocator, fb_width, fb_height, g_MinImageCount, 0);
    const std::array queue_array = {graphic_queue.get_index()};
    // render_output
    recycle_bin.retire_after_submitted(std::move(render_output), "ui old render output.");
    const vk::ImageCreateInfo render_image_info({}, vk::ImageType::e2D, color_format, vk::Extent3D(_width, _height, 1),
                                                1, 1, vk::SampleCountFlagBits::e1, vk::ImageTiling::eOptimal,
                                                vk::ImageUsageFlagBits::eColorAttachment | vk::ImageUsageFlagBits::eSampled,
                                                vk::SharingMode::eExclusive, queue_array);
    const vk::ImageViewCreateInfo render_view_info({}, {}, vk::ImageViewType::e2D, color_format, {},
                                                   vk::ImageSubresourceRange(vk::ImageAspectFlagBits::eColor, {}, 1, 0, 1), nullptr);
    render_output.create(app.get_allocator(), *app.get_device(), render_image_info, render_view_info,
                         vma::MemoryUsage::eAutoPreferDevice, {}, vk::ClearColorValue(0.f, 0.f, 0.f, 0.f));

    recycle_bin.retire_after_submitted(std::move(color_image), "ui old color image.");
    const vk::ImageCreateInfo color_image_info({}, vk::ImageType::e2D, color_format, vk::Extent3D(_width, _height, 1),
        1, 1, vulkan_common::MSAA_SAMPLE_COUNT, vk::ImageTiling::eOptimal,
        vk::ImageUsageFlagBits::eColorAttachment, vk::SharingMode::eExclusive, queue_array);
    const vk::ImageViewCreateInfo color_view_info({}, {}, vk::ImageViewType::e2D, color_format, {},
        vk::ImageSubresourceRange(vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1));
    color_image.create(app.get_allocator(), *app.get_device(), color_image_info, color_view_info,
        vma::MemoryUsage::eAutoPreferDevice, {}, vk::ClearColorValue(0.f, 0.f, 0.f, 0.f));

    std::ranges::for_each(ui_managers, [&](auto& m) { m->resize(_width, _height); });
}

/**
 * @brief 起一帧 ImGui、构建“场景设置”窗口与全部面板，最后缓存 draw data。
 * @note 面板顺序固定为摄像机、灯光、物体、棋局、环境；draw_data 在 render 前一直有效。
 */
void ui_manager::update()
{
    ImGui_ImplVulkan_NewFrame();
    ImGui_ImplGlfw_NewFrame();
    ImGui::NewFrame();

    ImGui::Begin(SCENE_SETTING.data());

    render_mode_ui();
    std::ranges::for_each(ui_managers, [](auto& m) static { m->update(); });

    ImGui::End();

    ImGui::Render();
    draw_data = ImGui::GetDrawData();
}

/**
 * @brief 录制主 UI 命令：两条 layout barrier → MSAA 渲染 → 解析到 render_output → 提交。
 * @param frame_slot 命令缓冲下标，必须与 begin_frame 返回的帧槽一致。
 * @return 图形队列最新提交 token，application 合成时以它作为 UI 依赖。
 * @note ImGui_ImplVulkan_RenderDrawData 在提交锁内录制，避免与上游同队列的纹理上传交错。
 */
vulkan_submission_token ui_manager::render(uint32_t frame_slot)
{
    recycle_bin.release();

    vulkan_commandbuffer& commandbuffer = commandbuffers.at(frame_slot);
    commandbuffer.begin_record({});

    const auto color_image_barrier = color_image.transition_state(vk::PipelineStageFlagBits2::eColorAttachmentOutput,
        vk::AccessFlagBits2::eColorAttachmentWrite, vk::ImageLayout::eColorAttachmentOptimal, vk::QueueFamilyIgnored);

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

    {
        // The upstream font/texture uploader can submit and wait on this queue.
        const std::lock_guard lock(graphic_queue.get_submission_state()->mutex);
        ImGui_ImplVulkan_RenderDrawData(draw_data, *(*(commandbuffer)));
    }

    (*commandbuffer).endRendering();


    commandbuffer.end_record();
    (void)commandbuffer.submit();

    if (ImGui::GetIO().ConfigFlags & ImGuiConfigFlags_ViewportsEnable)
    {
        ImGui::UpdatePlatformWindows();
        viewport_renderer->check_errors();
        ImGui::RenderPlatformWindowsDefault();
        viewport_renderer->check_errors();
    }

    return graphic_queue.get_latest_token();
}

void ui_manager::handle(int _key, int _scancode, int _action, int _mods) noexcept
{
    std::ranges::for_each(ui_managers, [&](auto& m) { m->handle(_key, _scancode, _action, _mods); });
}

bool ui_manager::wants_keyboard() const noexcept
{
    return ImGui::GetIO().WantCaptureKeyboard;
}

vulkan_image& ui_manager::get_render_image() noexcept
{
    return render_output;
}

/** @brief 构建渲染模式下拉；选择变化时直接切换场景管理器当前使用的渲染器。 */
void ui_manager::render_mode_ui()
{
    ImGui::SeparatorText(SCENE_MANAGER.data());

    constexpr std::array render_mode_names = {RENDER_MODE_RASTERIZATION.data(), RENDER_MODE_RAY_TRACING.data(),
                                              RENDER_MODE_RAY_QUERY.data()};

    if (ImGui::Combo(RENDER_MODE.data(), &current_mode, render_mode_names.data(), static_cast<int>(render_mode_names.size())))
    {
        manager.set_render_mode(static_cast<render_mode>(current_mode));
    }
}
