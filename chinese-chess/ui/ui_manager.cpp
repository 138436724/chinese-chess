#include "tools/font_loader.h"
#include "ui_manager.h"
#include "vulkan_core/vulkan_common.h"
#include <imgui_impl_glfw.h>
#include <imgui_impl_vulkan.h>

constexpr std::u8string_view SCENE_SETTING = u8"场景设置";
constexpr std::u8string_view SCENE_MANAGER = u8"场景管理";
constexpr std::u8string_view USE_RAY_TRACING = u8"使用光线追踪";

void ui_manager::create(GLFWwindow* _window, vulkan_application* _app, scene_manager* _manager, uint32_t _width, uint32_t _height)
{
	app = _app;
	recycle_bin = app->get_recycle_bin_ptr();

	manager = _manager;

	ui_managers.emplace_back(pro::make_proxy<ui_base, ui_camera>());
	ui_managers.emplace_back(pro::make_proxy<ui_base, ui_light>());
	ui_managers.emplace_back(pro::make_proxy<ui_base, ui_node>());

	auto r = std::make_unique<ui_record>();
	chess_manager = r.get();
	ui_managers.emplace_back(std::move(r));

	std::ranges::for_each(ui_managers, [this](auto& m) { m->create(manager); });

	IMGUI_CHECKVERSION();
	ImGui::CreateContext();
	ImGuiIO& io = ImGui::GetIO();
	(void)io;
	io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard; // Enable Keyboard Controls
	io.ConfigFlags |= ImGuiConfigFlags_NavEnableGamepad; // Enable Gamepad Controls
	io.ConfigFlags |= ImGuiConfigFlags_DockingEnable; // Enable Docking
	io.ConfigFlags |= ImGuiConfigFlags_ViewportsEnable; // Enable Multi-Viewport / Platform Windows

	ImGui::StyleColorsDark();
	ImGuiStyle& style = ImGui::GetStyle();
	if (io.ConfigFlags & ImGuiConfigFlags_ViewportsEnable)
	{
		style.WindowRounding = 0.0f;
		style.Colors[ImGuiCol_WindowBg].w = 1.0f;
	}

	io.Fonts->AddFontFromFileTTF(STRING_HELPER::convert_to<std::string, std::u8string>(std::u8string(FONTS_PATH) + u8"LXGWWenKaiGB-Medium.ttf").c_str(), 13.0f, nullptr, io.Fonts->GetGlyphRangesChineseFull());

	ImGui_ImplGlfw_InitForVulkan(_window, true);

	graphic_queue.create(*app->get_device(), app->get_physical_device().get_queue_index(vk::QueueFlagBits::eGraphics));

	std::array pool_size{ vk::DescriptorPoolSize(vk::DescriptorType::eSampledImage, 1), vk::DescriptorPoolSize(vk::DescriptorType::eSampler, 1) };
	vk::DescriptorPoolCreateInfo pool_info(vk::DescriptorPoolCreateFlagBits::eFreeDescriptorSet, 4, pool_size);
	descriptor_pool = vk::raii::DescriptorPool(*app->get_device(), pool_info);

	color_format = app->get_swapchain().get_format();
	ImGui_ImplVulkan_PipelineInfo create_info = {
		.MSAASamples = static_cast<VkSampleCountFlagBits>(vulkan_common::MSAA_SAMPLE_COUNT),
		.PipelineRenderingCreateInfo = vk::PipelineRenderingCreateInfo({}, color_format, vk::Format::eUndefined, vk::Format::eUndefined, nullptr),
	};

	ImGui_ImplVulkan_InitInfo init_info = {
		.ApiVersion = vk::ApiVersion14,
		.Instance = *(app->get_instance()),
		.PhysicalDevice = *(*app->get_physical_device()),
		.Device = *(*app->get_device()),
		.QueueFamily = graphic_queue.get_index(),
		.Queue = *(graphic_queue.get_queue()),
		.DescriptorPool = *descriptor_pool,
		.MinImageCount = vulkan_common::MAX_FRAMES_IN_FLIGHT,
		.ImageCount = vulkan_common::MAX_FRAMES_IN_FLIGHT,
		.PipelineInfoMain = create_info,
		.UseDynamicRendering = true,
	};
	ImGui_ImplVulkan_Init(&init_info);

	commandbuffers = vulkan_commandbuffer::create(*app->get_device(), vk::CommandBufferAllocateInfo(graphic_queue.get_command_pool(), vk::CommandBufferLevel::ePrimary, vulkan_common::MAX_FRAMES_IN_FLIGHT), &graphic_queue, app->get_semaphore_ptr());

	resize(_width, _height);
}

void ui_manager::resize(uint32_t _width, uint32_t _height)
{
	//ImGui_ImplVulkanH_CreateOrResizeWindow(g_Instance, g_PhysicalDevice, g_Device, wd, g_QueueFamily, g_Allocator, fb_width, fb_height, g_MinImageCount, 0);

	// render_output
	recycle_bin->retire(std::move(render_output));
	vk::ImageCreateInfo render_image_info({}, vk::ImageType::e2D, color_format, vk::Extent3D(_width, _height, 1), 1, 1, vk::SampleCountFlagBits::e1, vk::ImageTiling::eOptimal, vk::ImageUsageFlagBits::eColorAttachment | vk::ImageUsageFlagBits::eSampled, vk::SharingMode::eExclusive, 0);
	vk::ImageViewCreateInfo render_view_info({}, {}, vk::ImageViewType::e2D, color_format, {}, vk::ImageSubresourceRange(vk::ImageAspectFlagBits::eColor, {}, 1, 0, 1), nullptr);
	render_output.create(app->get_allocator(), *app->get_device(), render_image_info, render_view_info, vk::MemoryPropertyFlagBits::eDeviceLocal, vk::ClearColorValue(0.f, 0.f, 0.f, 0.f));

	// msaa color
	recycle_bin->retire(std::move(color_image));
	vk::ImageCreateInfo color_image_info({}, vk::ImageType::e2D, color_format, vk::Extent3D(_width, _height, 1), 1, 1, vulkan_common::MSAA_SAMPLE_COUNT, vk::ImageTiling::eOptimal, vk::ImageUsageFlagBits::eColorAttachment, vk::SharingMode::eExclusive, 0);
	vk::ImageViewCreateInfo color_view_info({}, {}, vk::ImageViewType::e2D, color_format, {}, vk::ImageSubresourceRange(vk::ImageAspectFlagBits::eColor, {}, 1, 0, 1), nullptr);
	color_image.create(app->get_allocator(), *app->get_device(), color_image_info, color_view_info, vk::MemoryPropertyFlagBits::eDeviceLocal, vk::ClearColorValue(0.f, 0.f, 0.f, 0.f));

	std::ranges::for_each(ui_managers, [&](auto& m) { m->resize(_width, _height); });
}

void ui_manager::update()
{
	ImGui_ImplVulkan_NewFrame();
	ImGui_ImplGlfw_NewFrame();
	ImGui::NewFrame();

	bool show_demo_window = true;

	//ImGui::ShowDemoWindow(&show_demo_window);

	ImGui::Begin(reinterpret_cast<const char*>(SCENE_SETTING.data()), &show_demo_window);

	ray_tracing_ui();
	std::ranges::for_each(ui_managers, [](auto& m) { m->update(); });

	ImGui::End();

	ImGui::Render();
	draw_data = ImGui::GetDrawData();
}

vk::SemaphoreSubmitInfo ui_manager::render()
{
	vulkan_commandbuffer& commandbuffer = commandbuffers.at(current_frame);
	commandbuffer.begin_record({});

	std::vector<vk::ImageMemoryBarrier2> begin_barrier;
	begin_barrier.emplace_back(color_image.set_layout(vk::ImageLayout::eColorAttachmentOptimal, vk::ImageSubresourceRange(vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1)));
	begin_barrier.emplace_back(render_output.set_layout(vk::ImageLayout::eColorAttachmentOptimal, vk::ImageSubresourceRange(vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1)));
	(*commandbuffer).pipelineBarrier2(vk::DependencyInfo({}, {}, {}, begin_barrier));

	vk::RenderingAttachmentInfo colorAttachmentInfo(color_image.get_imageview(), vk::ImageLayout::eColorAttachmentOptimal, vk::ResolveModeFlagBits::eAverage,
		render_output.get_imageview(), vk::ImageLayout::eColorAttachmentOptimal, vk::AttachmentLoadOp::eClear, vk::AttachmentStoreOp::eStore, color_image.get_clear_value());

	vk::RenderingInfo renderingInfo({}, vk::Rect2D({ 0, 0 }, app->get_swapchain().get_extent()), 1, {}, colorAttachmentInfo, nullptr, nullptr, nullptr);

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

void ui_manager::destroy()
{
	ImGui_ImplVulkan_Shutdown();
	ImGui_ImplGlfw_Shutdown();
	ImGui::DestroyContext();
}

void ui_manager::load_previous() noexcept
{
	chess_manager->load_previous();
}

void ui_manager::load_next() noexcept
{
	chess_manager->load_next();
}

vulkan_image& ui_manager::get_render_image() noexcept
{
	return render_output;
}

void ui_manager::ray_tracing_ui() noexcept
{
	ImGui::SeparatorText(reinterpret_cast<const char*>(SCENE_MANAGER.data()));

	if (ImGui::Checkbox(reinterpret_cast<const char*>(USE_RAY_TRACING.data()), &use_ray_tracing))
	{
		manager->set_use_ray_tracing(use_ray_tracing);
	}
}
