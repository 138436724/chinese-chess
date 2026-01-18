module;

#define GLFW_INCLUDE_VULKAN
#include <GLFW/glfw3.h>

export module scene_node;

import <cstdint>;
import vulkan_hpp;
import vulkan_application;
import scene_camera;

export class scene_node
{
public:
	scene_node() = default;
	virtual ~scene_node() = default;

	virtual void create(GLFWwindow* _window, const vulkan_application* _app, vk::SampleCountFlagBits _multisample_count, vk::Format _color_formats, vk::Format _depth_format) = 0;
	virtual void resize(const vulkan_application* _app, uint32_t _width, uint32_t _height) = 0;
	virtual void update(const scene_camera* _camera) = 0;
	virtual void render(const vk::raii::CommandBuffer& _commandbuffer) = 0;
	virtual void destroy() = 0;
};