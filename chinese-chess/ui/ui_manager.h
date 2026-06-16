#pragma once
#define GLFW_INCLUDE_VULKAN

#include "scene/scene_manager.h"
#include "ui_record.h"
#include "vulkan_core/vulkan_application.h"
#include <filesystem>
#include <GLFW/glfw3.h>
#include <imgui.h>

class ui_manager
{
public:
	ui_manager() = default;
	~ui_manager() = default;

	void create(GLFWwindow* _window, vulkan_application* _app, scene_manager* _manager, uint32_t _width, uint32_t _height);
	void resize(uint32_t _width, uint32_t _height);
	void update();
	const vulkan_commandbuffer& render();
	void destroy();

	void load_previous() noexcept;
	void load_next() noexcept;

	vulkan_image& get_render_image() noexcept;

private:
	void ray_tracing_ui() noexcept;
	void camera_ui() noexcept;

	bool use_ray_tracing = true;
	glm::vec3 light_direction = glm::vec3(1.f, 1.f, 1.f);
	glm::vec3 light_color = glm::vec3(1.0f, 0.95f, 0.85f);
	bool camera_type = static_cast<bool>(projection_type::orthographic);
	glm::vec3 camera_position = glm::vec3(0.f, 0.f, 0.f);

private:
	vk::Format color_format = vk::Format::eUndefined;

	vulkan_application* app = nullptr;
	vk::raii::DescriptorPool descriptor_pool = nullptr;

	ImDrawData* draw_data = nullptr;

	vulkan_image color_image;
	vulkan_image render_output;

	scene_manager* manager = nullptr;
	std::unique_ptr<ui_record> chess_manager = nullptr;

	std::vector<vulkan_commandbuffer> commandbuffers;
	uint32_t current_frame = 0;
};