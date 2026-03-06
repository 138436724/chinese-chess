#pragma once

#include "scene/scene_node.h"
#include "vulkan_core/vulkan_buffer.h"
#include "vulkan_core/vulkan_pipeline.h"

class scene_brdflut
{
public:
	scene_brdflut() = default;
	~scene_brdflut() = default;

	void create(const vulkan_application* _app);

	vulkan_image& get_image() noexcept;
	const vk::raii::Sampler& get_sampler() const noexcept;

private:
	vulkan_pipeline pipeline;

	vulkan_image brdflut_image;
	vk::raii::Sampler brdflut_sampler = nullptr;

	uint32_t width = 512;
	uint32_t height = 512;
	vk::Format color_format = vk::Format::eR16G16Sfloat;
};
