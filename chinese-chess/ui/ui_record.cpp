#include "tools/font_loader.h"
#include "tools/record_loader.h"
#include "tools/string_helper.h"
#include "ui_record.h"
#include "vulkan_core/vulkan_common.h"
#include <algorithm>
#include <imgui_impl_glfw.h>
#include <imgui_impl_vulkan.h>
#include <ranges>
#ifdef _WIN32
#include <Windows.h>
#endif // _WIN32

void ui_record::create(GLFWwindow* _window, vulkan_application* _app, uint32_t _width, uint32_t _height)
{
	app = _app;

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

	io.Fonts->AddFontFromFileTTF(STRING_HELPER::convert_to<std::string, std::u8string>(std::u8string(FONTS_PATH) + u8"LXGWWenKaiGB-Medium.ttf", "utf8").c_str(), 13.0f, nullptr, io.Fonts->GetGlyphRangesChineseFull());

	ImGui_ImplGlfw_InitForVulkan(_window, true);

	std::array pool_size{ vk::DescriptorPoolSize(vk::DescriptorType::eCombinedImageSampler, 1) };
	vk::DescriptorPoolCreateInfo pool_info(vk::DescriptorPoolCreateFlagBits::eFreeDescriptorSet, 1, pool_size);
	descriptor_pool = vk::raii::DescriptorPool(app->get_device(), pool_info);

	color_format = { app->get_swapchain().get_format() };
	create_info = vk::PipelineRenderingCreateInfo({}, color_format, vk::Format::eUndefined, vk::Format::eUndefined, nullptr);

	ImGui_ImplVulkan_InitInfo init_info = {
		.ApiVersion = vk::ApiVersion13,
		.Instance = *(app->get_instance()),
		.PhysicalDevice = *(app->get_physical_device()),
		.Device = *(app->get_device()),
		.QueueFamily = app->get_queue(vk::QueueFlagBits::eGraphics).get_index(),
		.Queue = *(app->get_queue(vk::QueueFlagBits::eGraphics).get_queue()),
		.DescriptorPool = *descriptor_pool,
		.MinImageCount = vulkan_common::MAX_FRAMES_IN_FLIGHT,
		.ImageCount = vulkan_common::MAX_FRAMES_IN_FLIGHT,
		.MSAASamples = static_cast<VkSampleCountFlagBits>(vulkan_common::MASS_SAMPLE_COUNT),
		.UseDynamicRendering = true,
		.PipelineRenderingCreateInfo = create_info
	};
	ImGui_ImplVulkan_Init(&init_info);
	ImGui_ImplVulkan_CreateFontsTexture();

	resize(_width, _height);
}

void ui_record::resize(uint32_t _width, uint32_t _height)
{
	//ImGui_ImplVulkanH_CreateOrResizeWindow(g_Instance, g_PhysicalDevice, g_Device, wd, g_QueueFamily, g_Allocator, fb_width, fb_height, g_MinImageCount, 0);

	// render_output
	vk::ImageCreateInfo render_image_info({}, vk::ImageType::e2D, color_format.front(), vk::Extent3D(_width, _height, 1), 1, 1, vk::SampleCountFlagBits::e1, vk::ImageTiling::eOptimal, vk::ImageUsageFlagBits::eColorAttachment | vk::ImageUsageFlagBits::eSampled, vk::SharingMode::eExclusive, 0);
	vk::ImageViewCreateInfo render_view_info({}, {}, vk::ImageViewType::e2D, color_format.front(), {}, vk::ImageSubresourceRange(vk::ImageAspectFlagBits::eColor, {}, 1, 0, 1), nullptr);
	render_output.create(app->get_physical_device(), app->get_device(), render_image_info, render_view_info, vk::MemoryPropertyFlagBits::eDeviceLocal, vk::ClearColorValue(0.f, 0.f, 0.f, 0.f));

	// msaa color
	vk::ImageCreateInfo color_image_info({}, vk::ImageType::e2D, color_format.front(), vk::Extent3D(_width, _height, 1), 1, 1, vulkan_common::MASS_SAMPLE_COUNT, vk::ImageTiling::eOptimal, vk::ImageUsageFlagBits::eColorAttachment, vk::SharingMode::eExclusive, 0);
	vk::ImageViewCreateInfo color_view_info({}, {}, vk::ImageViewType::e2D, color_format.front(), {}, vk::ImageSubresourceRange(vk::ImageAspectFlagBits::eColor, {}, 1, 0, 1), nullptr);
	color_image.create(app->get_physical_device(), app->get_device(), color_image_info, color_view_info, vk::MemoryPropertyFlagBits::eDeviceLocal, vk::ClearColorValue(0.f, 0.f, 0.f, 0.f));
}

void ui_record::update()
{
	constexpr std::u8string_view CHESS_RECORD = u8"象棋棋谱";
	constexpr std::u8string_view RECORDS_LIST = u8"棋谱列表";
	constexpr std::u8string_view OPEN_RECORDS = u8"加载棋谱";
	constexpr std::u8string_view LAST_STEP = u8"上一步";
	constexpr std::u8string_view NEXT_STEP = u8"下一步";

	ImGui_ImplVulkan_NewFrame();
	ImGui_ImplGlfw_NewFrame();
	ImGui::NewFrame();

	bool show_demo_window = true;

#ifndef NDEBUG
	ImGui::ShowDemoWindow(&show_demo_window);
#endif // !NDEBUG

	ImGui::Begin(reinterpret_cast<const char*>(CHESS_RECORD.data()), &show_demo_window);

	if (ImGui::Button(reinterpret_cast<const char*>(OPEN_RECORDS.data())))
	{
		std::filesystem::path file_path;

#ifdef _WIN32
		TCHAR szFile[MAX_PATH] = { 0 };

		OPENFILENAME ofn;
		ZeroMemory(&ofn, sizeof(ofn));
		ofn.lStructSize = sizeof(ofn);
		ofn.lpstrFile = szFile;
		ofn.nMaxFile = sizeof(szFile);
		ofn.lpstrFilter = L"Text\0*.txt\0All\0*.*\0";
		ofn.nFilterIndex = 1;
		ofn.Flags = OFN_PATHMUSTEXIST | OFN_FILEMUSTEXIST | OFN_NOCHANGEDIR;

		if (GetOpenFileName(&ofn))
		{
			file_path = szFile;
		}
#endif // _WIN32

		if (!file_path.empty())
		{
			load_records(file_path);
			manager->load_record(file_path);
			manager->set_now_record_index(1);
		}
	}

	if (!all_records.empty())
	{
		if (ImGui::ListBox(reinterpret_cast<const char*>(RECORDS_LIST.data()), &selected_index, all_records_c_str.data(), static_cast<int>(all_records_c_str.size())))
		{
			manager->set_now_record_index(selected_index + 1);
		}
		if (ImGui::Button(reinterpret_cast<const char*>(LAST_STEP.data())))
		{
			parse_back();
		}
		ImGui::SameLine();
		if (ImGui::Button(reinterpret_cast<const char*>(NEXT_STEP.data())))
		{
			parse_next();
		}
	}

	ImGui::End();

	ImGui::Render();
	draw_data = ImGui::GetDrawData();
}

void ui_record::render(const vk::raii::CommandBuffer& _commandbuffer)
{
	std::vector<vk::ImageMemoryBarrier2> begin_barrier;
	begin_barrier.emplace_back(color_image.set_layout(vk::ImageLayout::eColorAttachmentOptimal, vk::ImageSubresourceRange(vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1)));
	begin_barrier.emplace_back(render_output.set_layout(vk::ImageLayout::eColorAttachmentOptimal, vk::ImageSubresourceRange(vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1)));
	_commandbuffer.pipelineBarrier2(vk::DependencyInfo({}, {}, {}, begin_barrier));

	vk::RenderingAttachmentInfo colorAttachmentInfo(color_image.get_imageview(), vk::ImageLayout::eColorAttachmentOptimal, vk::ResolveModeFlagBits::eAverage,
		render_output.get_imageview(), vk::ImageLayout::eColorAttachmentOptimal, vk::AttachmentLoadOp::eClear, vk::AttachmentStoreOp::eStore, color_image.get_clear_value());

	vk::RenderingInfo renderingInfo({}, vk::Rect2D({ 0, 0 }, app->get_swapchain().get_extent()), 1, {}, colorAttachmentInfo, nullptr, nullptr, nullptr);

	_commandbuffer.beginRendering(renderingInfo);

	ImGui_ImplVulkan_RenderDrawData(draw_data, *_commandbuffer);

	_commandbuffer.endRendering();

	if (ImGui::GetIO().ConfigFlags & ImGuiConfigFlags_ViewportsEnable)
	{
		ImGui::UpdatePlatformWindows();
		ImGui::RenderPlatformWindowsDefault();
	}
}

void ui_record::destroy()
{
	ImGui_ImplVulkan_Shutdown();
	ImGui_ImplGlfw_Shutdown();
	ImGui::DestroyContext();
}

void ui_record::parse_back() noexcept
{
	if (selected_index > 0)
	{
		selected_index--;
		manager->set_now_record_index(selected_index + 1);
	}
}

void ui_record::parse_next() noexcept
{
	if (selected_index < all_records.size() - 1)
	{
		selected_index++;
		manager->set_now_record_index(selected_index + 1);
	}
}

void ui_record::set_chess_manager(chess_manager* _manager) noexcept
{
	manager = _manager;
}

vulkan_image& ui_record::get_render_image() noexcept
{
	return render_output;
}

void ui_record::load_records(const std::filesystem::path& _record_path)
{
	all_records = std::move(RECORD_LOADER.read_record<std::u8string>(_record_path));

	all_records_c_str = all_records
		| std::views::transform([](const auto& _record) {return reinterpret_cast<const char*>(_record.data()); })
		| std::ranges::to<std::vector>();
}