#pragma once

#include "scene/scene_node.h"
#include "scene_cubemap.h"

class scene_skybox : public scene_node
{
public:
	scene_skybox() = default;
	~scene_skybox() override = default;

	void create(const vulkan_application* _app, vk::SampleCountFlagBits _multisample_count, vk::Format _color_formats, vk::Format _depth_format) override;
	void resize(const vulkan_application* _app, uint32_t _width, uint32_t _height) override;
	void update(const scene_camera* _camera) noexcept override;
	void render(const vk::raii::CommandBuffer& _commandbuffer) noexcept override;
	void destroy() noexcept override;

	void set_cubemap(scene_cubemap* _cubemap) noexcept;

private:
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

	std::vector<vulkan_buffer> ubo_params;

	scene_cubemap* cubemap = nullptr;
};