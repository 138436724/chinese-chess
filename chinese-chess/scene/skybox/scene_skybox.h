#pragma once

#include "scene/scene_node.h"
#include "scene_cubemap.h"
#include "tools/model_loader.h"
#include "vulkan_core/vulkan_buffer.h"
#include "vulkan_core/vulkan_descriptor.h"
#include "vulkan_core/vulkan_pipeline.h"

class scene_skybox
{
public:
	scene_skybox() = default;
	~scene_skybox() = default;

	void create(const vulkan_application* _app, vk::SampleCountFlagBits _multisample_count, vk::Format _color_formats, vk::Format _depth_format);
	void resize(const vulkan_application* _app, uint32_t _width, uint32_t _height);
	void update(const scene_camera* _camera) noexcept;
	void render(const vk::raii::CommandBuffer& _commandbuffer) noexcept;
	void destroy() noexcept;

	void set_cubemap(scene_cubemap* _cubemap) noexcept;

private:
	vulkan_pipeline pipeline;

	std::vector<model_vertex> vertices;
	vulkan_buffer vertices_buffer;

	std::vector<uint32_t> indices;
	vulkan_buffer indices_buffer;

	vulkan_descriptor descriptor;

	uint32_t current_frame = 0;

	struct UBO
	{
		alignas(16) glm::mat4 projection;
		alignas(16) glm::mat4 model;
	};

	struct UBOParams
	{
		alignas(16) glm::vec4 lights[4];
		alignas(16) float exposure;
		float gamma;
	};

	std::vector<vulkan_buffer> ubos;
	std::vector<vulkan_buffer> ubo_params;

	scene_cubemap* cubemap = nullptr;
};