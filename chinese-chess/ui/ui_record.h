#pragma once
#define GLFW_INCLUDE_VULKAN

#include "scene/scene_manager.h"
#include "ui_chess_manager.h"
#include "vulkan_core/vulkan_application.h"
#include <filesystem>
#include <GLFW/glfw3.h>
#include <imgui.h>

class ui_record
{
public:
	ui_record() = default;
	~ui_record() = default;

	void create(GLFWwindow* _window, vulkan_application* _app, scene_manager* _manager, uint32_t _width, uint32_t _height);
	void resize(uint32_t _width, uint32_t _height);
	void update();
	const vulkan_commandbuffer& render();
	void destroy();

	void load_previous() noexcept;
	void load_next() noexcept;

	vulkan_image& get_render_image() noexcept;

public:
	bool use_ray_tracing = true;
	glm::vec3 light_direction = glm::vec3(1.f, 1.f, 1.f);
	glm::vec3 light_color = glm::vec3(1.0f, 0.95f, 0.85f);

private:
	void load_records(const std::filesystem::path& _record_path);

	vk::Format color_format = vk::Format::eUndefined;

	vulkan_application* app = nullptr;
	vk::raii::DescriptorPool descriptor_pool = nullptr;

	ImDrawData* draw_data = nullptr;

	vulkan_image color_image;
	vulkan_image render_output;

	scene_manager* manager = nullptr;
	std::unique_ptr<ui_chess_manager> chess_manager = nullptr;

	int selected_index = 0;
	std::vector<std::u8string> all_records;
	std::vector<const char*> all_records_c_str;

	std::vector<vulkan_commandbuffer> commandbuffers;
	uint32_t current_frame = 0;
};