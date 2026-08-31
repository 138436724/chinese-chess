#include "ui_manager.h"

#include "scene/scene_manager.h"
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
#include <string_view>
#include <utility>


namespace {
constexpr std::string_view SCENE_SETTING             = "场景设置";
constexpr std::string_view SCENE_MANAGER             = "场景管理";
constexpr std::string_view RENDER_MODE               = "渲染模式";
constexpr std::string_view RENDER_MODE_RASTERIZATION = "光栅化";
constexpr std::string_view RENDER_MODE_RAY_TRACING   = "光线追踪 (RT Pipeline)";
constexpr std::string_view RENDER_MODE_RAY_QUERY     = "光线追踪 (Ray Query)";
}  // namespace


ui_manager::~ui_manager()
{
    app.wait();

    ImGui_ImplVulkan_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    ImGui::DestroyContext();
}

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
    (void)io;
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

    constexpr std::array               pool_size{vk::DescriptorPoolSize(vk::DescriptorType::eSampledImage, 1),
                                                 vk::DescriptorPoolSize(vk::DescriptorType::eSampler, 1)};
    const vk::DescriptorPoolCreateInfo pool_info(vk::DescriptorPoolCreateFlagBits::eFreeDescriptorSet, 4, pool_size);
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
    };
    ImGui_ImplVulkan_Init(&init_info);

    commandbuffers = vulkan_commandbuffer::create(*app.get_device(),
                                                  vk::CommandBufferAllocateInfo(graphic_queue.get_command_pool(),
                                                                                vk::CommandBufferLevel::ePrimary,
                                                                                vulkan_common::MAX_FRAMES_IN_FLIGHT),
                                                  &graphic_queue, &semaphore);

    resize(_width, _height);
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
                         vma::MemoryUsage::eGpuOnly, vk::ClearColorValue(0.f, 0.f, 0.f, 0.f));

    // msaa color
    recycle_bin.retire(std::move(color_image), "ui old color image.");
    vk::ImageCreateInfo color_image_info({}, vk::ImageType::e2D, color_format, vk::Extent3D(_width, _height, 1), 1, 1,
                                         vulkan_common::MSAA_SAMPLE_COUNT, vk::ImageTiling::eOptimal,
                                         vk::ImageUsageFlagBits::eColorAttachment, vk::SharingMode::eExclusive, queue_array);
    vk::ImageViewCreateInfo color_view_info({}, {}, vk::ImageViewType::e2D, color_format, {},
                                            vk::ImageSubresourceRange(vk::ImageAspectFlagBits::eColor, {}, 1, 0, 1), nullptr);
    color_image.create(app.get_allocator(), *app.get_device(), color_image_info, color_view_info,
                       vma::MemoryUsage::eGpuOnly, vk::ClearColorValue(0.f, 0.f, 0.f, 0.f));


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
        vk::ImageMemoryBarrier2(color_image.get_stage(), color_image.get_access(),
                                vk::PipelineStageFlagBits2::eColorAttachmentOutput, vk::AccessFlagBits2::eColorAttachmentWrite,
                                color_image.get_layout(), vk::ImageLayout::eColorAttachmentOptimal,
                                vk::QueueFamilyIgnored, vk::QueueFamilyIgnored, color_image.get_image(),
                                vk::ImageSubresourceRange(vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1));
    color_image.set_info(color_image_barrier);

    const auto render_output_barrier =
        vk::ImageMemoryBarrier2(render_output.get_stage(), render_output.get_access(),
                                vk::PipelineStageFlagBits2::eColorAttachmentOutput, vk::AccessFlagBits2::eColorAttachmentWrite,
                                render_output.get_layout(), vk::ImageLayout::eColorAttachmentOptimal,
                                vk::QueueFamilyIgnored, vk::QueueFamilyIgnored, render_output.get_image(),
                                vk::ImageSubresourceRange(vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1));
    render_output.set_info(render_output_barrier);

    const std::array begin_barriers = {color_image_barrier, render_output_barrier};
    (*commandbuffer).pipelineBarrier2(vk::DependencyInfo({}, {}, {}, begin_barriers, nullptr));

    const vk::RenderingAttachmentInfo colorAttachmentInfo(color_image.get_imageview(), vk::ImageLayout::eColorAttachmentOptimal,
                                                          vk::ResolveModeFlagBits::eAverage, render_output.get_imageview(),
                                                          vk::ImageLayout::eColorAttachmentOptimal, vk::AttachmentLoadOp::eClear,
                                                          vk::AttachmentStoreOp::eStore, color_image.get_clear_value());

    const vk::RenderingInfo renderingInfo({}, vk::Rect2D({0, 0}, app.get_swapchain().get_extent()), 1, {},
                                          colorAttachmentInfo, nullptr, nullptr, nullptr);

    (*commandbuffer).beginRendering(renderingInfo);

    ImGui_ImplVulkan_RenderDrawData(draw_data, *(*(commandbuffer)));

    (*commandbuffer).endRendering();


    commandbuffer.end_record();
    commandbuffer.submit(false);

    if (ImGui::GetIO().ConfigFlags & ImGuiConfigFlags_ViewportsEnable)
    {
        ImGui::UpdatePlatformWindows();
        ImGui::RenderPlatformWindowsDefault();
    }

    current_frame = (current_frame + 1) % vulkan_common::MAX_FRAMES_IN_FLIGHT;

    return commandbuffer.get_submit_info();
}

void ui_manager::handle(int _glfw_key) noexcept
{
    std::ranges::for_each(ui_managers, [&](auto& m) { m->handle(_glfw_key); });
}

vulkan_image& ui_manager::get_render_image() noexcept
{
    return render_output;
}

void ui_manager::render_mode_ui() noexcept
{
    ImGui::SeparatorText(SCENE_MANAGER.data());

    constexpr std::array render_mode_names = {RENDER_MODE_RASTERIZATION.data(), RENDER_MODE_RAY_TRACING.data(),
                                              RENDER_MODE_RAY_QUERY.data()};

    if (ImGui::Combo(RENDER_MODE.data(), &current_mode, render_mode_names.data(), static_cast<int>(render_mode_names.size())))
    {
        manager.set_render_mode(static_cast<render_mode>(current_mode));
    }
}
