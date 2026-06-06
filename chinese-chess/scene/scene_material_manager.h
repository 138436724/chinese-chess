#pragma once

#include "scene_material.h"
#include "vulkan_core/vulkan_application.h"
#include <map>
#include <memory>
#include <vector>

class scene_material_manager
{
public:
	scene_material_manager(vulkan_application* _app);
	~scene_material_manager() = default;

	std::shared_ptr<scene_material> create_material(const std::u8string& _font_path, uint32_t _font_size, const std::wstring& _characters); // like scene_node_manager
	void clear_unused_materials() noexcept;

	std::optional<uint32_t> get_texture_index(const std::weak_ptr<vulkan_image>& _texture) const noexcept;
	std::vector<vk::DescriptorImageInfo> get_descriptor_info(const std::span<vk::Sampler> _samplers) const;

private:
	vulkan_application* app = nullptr;

	std::vector<std::weak_ptr<scene_material>> materials;
	std::vector<std::weak_ptr<vulkan_image>> images; // need order
	std::map<std::tuple<std::wstring, uint32_t>, std::weak_ptr<vulkan_image>> images_cache;
};