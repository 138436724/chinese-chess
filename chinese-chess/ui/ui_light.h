#pragma once

#include "scene/scene_manager.h"
#include <imgui.h>

class ui_light
{
public:
	ui_light() = default;
	~ui_light() = default;

	void create(scene_manager* _manager);
	void update() noexcept;

private:
	scene_manager* manager = nullptr;

	int add_light_type = 0;
	int selected_light = -1;
	std::vector<std::shared_ptr<scene_light>> lights;
};