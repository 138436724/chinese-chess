#pragma once

#include "scene/scene_node.h"
#include "vulkan_core/vulkan_buffer.h"
#include "vulkan_core/vulkan_pipeline.h"
#include <filesystem>

class scene_cubemap
{
public:
	scene_cubemap() = default;
	~scene_cubemap() = default;

	void create(const vulkan_application* _app, const std::filesystem::path& _hdr_path);

	vulkan_image& get_image() noexcept;
	const vk::raii::Sampler& get_sampler() const noexcept;

private:
	vk::raii::DescriptorPool descriptor_pool = nullptr;
	vulkan_pipeline pipeline;

	vk::raii::DescriptorSet descriptor_set = nullptr;

	vulkan_image cubemap_image;
	vk::raii::Sampler cubemap_sampler = nullptr;

	uint32_t width = 0;
	uint32_t height = 0;
	vk::Format color_format = vk::Format::eR32G32B32A32Sfloat;
};