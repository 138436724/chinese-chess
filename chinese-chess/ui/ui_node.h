#pragma once

#include "scene/scene_manager.h"
#include <imgui.h>

class ui_node
{
public:
	ui_node() = default;
	~ui_node() = default;

	void create(scene_manager* _manager);
	void resize(uint32_t _width, uint32_t _height) noexcept;
	void update() noexcept;

private:
	void update_material() noexcept;
	void update_model() noexcept;

	std::optional<int> get_material_index(const std::weak_ptr<scene_material>& _material) const noexcept;

	scene_manager* manager = nullptr;

	std::vector<std::shared_ptr<scene_material>> materials;
	std::vector<std::shared_ptr<scene_model>> models;
	std::unordered_map<std::shared_ptr<scene_image>, std::filesystem::path> textures;
};