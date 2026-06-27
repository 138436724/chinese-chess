#pragma once
#define GLFW_INCLUDE_VULKAN

#include "ui_base.h"
#include "ui_camera.h"
#include "ui_light.h"
#include "ui_node.h"
#include "ui_record.h"
#include "vulkan_core/vulkan_application.h"
#include <GLFW/glfw3.h>
#include <imgui.h>

class ui_manager
{
public:
	ui_manager() = default;
	~ui_manager() = default;

	void create(GLFWwindow* _window, const vulkan_application* _app, scene_manager* _manager, uint32_t _width, uint32_t _height);
	void resize(uint32_t _width, uint32_t _height);
	void update();
	const vulkan_commandbuffer& render();
	void destroy();

	void load_previous() noexcept;
	void load_next() noexcept;

	vulkan_image& get_render_image() noexcept;

private:
	void ray_tracing_ui() noexcept;
	bool use_ray_tracing = true;

private:
	vk::Format color_format = vk::Format::eUndefined;

	const vulkan_application* app = nullptr;
	vulkan_queue graphic_queue;
	vk::raii::DescriptorPool descriptor_pool = nullptr;

	ImDrawData* draw_data = nullptr;
	
	vulkan_image color_image;
	vulkan_image render_output;

	scene_manager* manager = nullptr;
	ui_record* chess_manager = nullptr;
	std::vector<pro::proxy<ui_base>> ui_managers;

	std::vector<vulkan_commandbuffer> commandbuffers;
	uint32_t current_frame = 0;
};