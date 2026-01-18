module;

#ifdef _WIN32
#define NOMINMAX
#include <Windows.h>
#endif // _WIN32

#include <unicode/utypes.h>

#include <imgui_impl_glfw.h>
#include <imgui_impl_vulkan.h>

#ifdef __INTELLISENSE__
#include <chrono>
#include <fstream>
#include <ranges>
#include <algorithm>
#endif // __INTELLISENSE__

module ui;

import vulkan_hpp;
import vulkan_common;
import font_loader;
import record_loader;

void ui::create(GLFWwindow* _window, const vulkan_application* _app, vk::SampleCountFlagBits _multisample_count, vk::Format _color_formats, vk::Format _depth_format)
{
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

	io.Fonts->AddFontFromFileTTF((std::string(FONTS_PATH) + "LXGWWenKaiGB-Medium.ttf").c_str(), 13.0f, nullptr, io.Fonts->GetGlyphRangesChineseFull());

	ImGui_ImplGlfw_InitForVulkan(_window, true);

	std::array pool_size{ vk::DescriptorPoolSize(vk::DescriptorType::eCombinedImageSampler, 1) };
	vk::DescriptorPoolCreateInfo pool_info(vk::DescriptorPoolCreateFlagBits::eFreeDescriptorSet, 1, pool_size);
	descriptor_pool = vk::raii::DescriptorPool(_app->get_device(), pool_info);

	color_format = { _color_formats };
	create_info = vk::PipelineRenderingCreateInfo({}, color_format, _depth_format, vk::Format::eUndefined, nullptr); // color format same as scene manageer format

	ImGui_ImplVulkan_InitInfo init_info = {
		.ApiVersion = vk::ApiVersion13,
		.Instance = *(_app->get_instance()),
		.PhysicalDevice = *(_app->get_physical_device()),
		.Device = *(_app->get_device()),
		.QueueFamily = _app->get_queue_family(vk::QueueFlagBits::eGraphics),
		.Queue = *(_app->get_queue(vk::QueueFlagBits::eGraphics)),
		.DescriptorPool = *descriptor_pool,
		.MinImageCount = vulkan_common::MAX_FRAMES_IN_FLIGHT,
		.ImageCount = vulkan_common::MAX_FRAMES_IN_FLIGHT,
		.MSAASamples = static_cast<VkSampleCountFlagBits>(_multisample_count),
		.UseDynamicRendering = true,
		.PipelineRenderingCreateInfo = create_info
	};
	ImGui_ImplVulkan_Init(&init_info);
	ImGui_ImplVulkan_CreateFontsTexture();
}

void ui::resize(const vulkan_application* _app, uint32_t _width, uint32_t _height)
{
	//ImGui_ImplVulkanH_CreateOrResizeWindow(g_Instance, g_PhysicalDevice, g_Device, wd, g_QueueFamily, g_Allocator, fb_width, fb_height, g_MinImageCount, 0);
}

void ui::update(const scene_camera* _camera)
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
	ImGui::ShowDemoWindow(&show_demo_window);

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
			if (selected_index > 0)
			{
				selected_index--;
				manager->set_now_record_index(selected_index + 1);
			}
		}
		ImGui::SameLine();
		if (ImGui::Button(reinterpret_cast<const char*>(NEXT_STEP.data())))
		{
			if (selected_index < all_records.size() - 1)
			{
				selected_index++;
				manager->set_now_record_index(selected_index + 1);
			}
		}
	}

	ImGui::End();

	ImGui::Render();
	draw_data = ImGui::GetDrawData();
}

void ui::render(const vk::raii::CommandBuffer& _commandbuffer)
{
	ImGui_ImplVulkan_RenderDrawData(draw_data, *_commandbuffer);

	render_end();
}

void ui::destroy()
{
	ImGui_ImplVulkan_Shutdown();
	ImGui_ImplGlfw_Shutdown();
	ImGui::DestroyContext();
}

void ui::render_end()
{
	if (ImGui::GetIO().ConfigFlags & ImGuiConfigFlags_ViewportsEnable)
	{
		ImGui::UpdatePlatformWindows();
		ImGui::RenderPlatformWindowsDefault();
	}
}

void ui::set_chess_manager(chess_pieces* _manager)
{
	manager = _manager;
}

void ui::load_records(const std::filesystem::path& _record_path)
{
	all_records = std::move(record_loader::get_record_loader().load_records<std::u8string>(_record_path));

	all_records_c_str = all_records
		| std::views::transform([](const auto& _record) {return reinterpret_cast<const char*>(_record.data()); })
		| std::ranges::to<std::vector>();
}