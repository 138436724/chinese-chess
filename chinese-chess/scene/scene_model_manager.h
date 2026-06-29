#pragma once

#include "scene_material_manager.h"
#include "scene_model.h"
#include "vulkan_core/vulkan_application.h"
#include "vulkan_core/vulkan_buffer.h"
#include "vulkan_core/vulkan_recycle_bin.h"
#include <filesystem>
#include <memory>
#include <unordered_map>
#include <vector>

class scene_model_manager
{
public:
	scene_model_manager(vulkan_application* _app, vulkan_recycle_bin* _recycle_bin, vulkan_queue* _transfer_queue, scene_material_manager* _manager);
	~scene_model_manager() = default;

	std::shared_ptr<scene_model> create(const std::filesystem::path& _model_path);
	void update(vulkan_commandbuffer& _commandbuffer) noexcept;
	void clear() noexcept;

	const std::vector<std::weak_ptr<scene_model>>& get_models() const noexcept;
	const vulkan_buffer& get_vertices_buffer() const noexcept;
	const vulkan_buffer& get_indices_buffer() const noexcept;
	const vulkan_buffer& get_ssbo_buffer() const noexcept;

private:
	void update_meshs(vulkan_commandbuffer& _commandbuffer) noexcept;
	void update_ssbo(vulkan_commandbuffer& _commandbuffer) noexcept;

	vulkan_application* app = nullptr;
	vulkan_recycle_bin* recycle_bin = nullptr;

	vulkan_queue* transfer_queue = nullptr;
	scene_material_manager* manager = nullptr;

	std::vector<std::weak_ptr<scene_model>> models;
	std::vector<std::weak_ptr<model_infomation>> meshs; // submit to gpu in order
	std::unordered_map<std::filesystem::path, std::weak_ptr<model_infomation>> models_cache; // no need order

	vulkan_buffer vertices_buffer;
	vulkan_buffer indices_buffer;

	vulkan_buffer ssbo;
};