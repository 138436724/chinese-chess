#pragma once

#include "scene/scene_manager.h"
#include <imgui.h>

class ui_node
{
public:
	ui_node() = default;
	~ui_node() = default;

	void create(scene_manager* _manager);
	void update() noexcept;

private:
	void update_material() noexcept;
	void update_model() noexcept;

	scene_manager* manager = nullptr;

	std::vector<std::shared_ptr<scene_material>> materials;
	std::vector<std::shared_ptr<scene_model>> models;
};