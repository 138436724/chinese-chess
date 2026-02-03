#pragma once

#include "scene_node.h"
#include "tools/model_loader.h"
#include "vulkan_core/vulkan_buffer.h"

class chess_board_line
{
public:
	chess_board_line() = default;
	~chess_board_line() = default;

	void create(const vulkan_application* _app, vk::SampleCountFlagBits _multisample_count, vk::Format _color_formats, vk::Format _depth_format);
	void resize(const vulkan_application* _app, uint32_t _width, uint32_t _height);
	void update(const scene_camera* _camera) noexcept;
	void render(const vk::raii::CommandBuffer& _commandbuffer) noexcept;
	void destroy() noexcept;

private:
	vk::raii::DescriptorPool descriptor_pool = nullptr;
	vulkan_pipeline pipeline;

	std::vector<model_vertex> vertices;
	vulkan_buffer vertices_buffer;

	std::vector<uint32_t> indices;
	vulkan_buffer indices_buffer;

	std::vector<vk::raii::DescriptorSet> descriptor_sets;

	uint32_t current_frame = 0;

	struct UBO
	{
		alignas(16) glm::mat4x4 model;
		alignas(16) glm::mat4x4 view;
		alignas(16) glm::mat4x4 proj;
		alignas(16) glm::vec3 camera_pos;
	};
	std::vector<vulkan_buffer> ubos;
};