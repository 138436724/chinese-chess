#pragma once
#define GLFW_INCLUDE_VULKAN

#include "scene/chess_manager.h"
#include "vulkan_core/vulkan_application.h"
#include <filesystem>
#include <GLFW/glfw3.h>
#include <imgui.h>

class ui_record
{
public:
	ui_record() = default;
	~ui_record() = default;

	void create(GLFWwindow* _window, vulkan_application* _app, uint32_t _width, uint32_t _height);
	void resize(uint32_t _width, uint32_t _height);
	void update();
	void render(const vk::raii::CommandBuffer& _commandbuffer);
	void destroy();

	void parse_back() noexcept;
	void parse_next() noexcept;

	void set_chess_manager(chess_manager* _manager) noexcept;

	vulkan_image& get_render_image() noexcept;

private:
	void load_records(const std::filesystem::path& _record_path);

	std::array<vk::Format, 1> color_format;

	vulkan_application* app = nullptr;
	vk::raii::DescriptorPool descriptor_pool = nullptr;
	vk::PipelineRenderingCreateInfo create_info;

	ImDrawData* draw_data = nullptr;

	vulkan_image color_image;
	vulkan_image render_output;

	chess_manager* manager = nullptr;

	int selected_index = 0;
	std::vector<std::u8string> all_records;
	std::vector<const char*> all_records_c_str;
};