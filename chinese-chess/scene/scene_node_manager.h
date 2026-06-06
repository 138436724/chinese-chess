#pragma once

#include "scene_node.h"
#include "vulkan_core/vulkan_application.h"
#include "vulkan_core/vulkan_buffer.h"
#include <memory>
#include <unordered_map>
#include <vector>

class scene_node_manager
{
public:
	scene_node_manager(vulkan_application* _app);
	~scene_node_manager() = default;

	std::shared_ptr<scene_node> create_node(const std::u8string& _model_path); // return shared_ptr, no save in this class
	void clear_unused_nodes(bool _need_reload = true) noexcept;

	void reload_buffer(vulkan_commandbuffer& _commandbuffer) noexcept;

	const vulkan_buffer& get_vertices_buffer() const noexcept;
	const vulkan_buffer& get_indices_buffer() const noexcept;

private:
	vulkan_application* app = nullptr;

	std::vector<std::weak_ptr<scene_node>> nodes;
	std::vector<std::weak_ptr<model_infomation>> models; // submit to gpu in order
	std::unordered_map<std::u8string, std::tuple<std::weak_ptr<model_infomation>, std::weak_ptr<vulkan_acceleration_structure>>> models_cache; // no need order

	vulkan_buffer vertices_buffer;
	vulkan_buffer indices_buffer;
};