#pragma once

#include "scene/scene_manager.h"
#include <imgui.h>

class ui_light
{
public:
	ui_light() = default;
	~ui_light() = default;

	void create(scene_manager* _manager);
	void resize(uint32_t _width, uint32_t _height) noexcept;
	void update() noexcept;

private:
	scene_manager* manager = nullptr;

	int add_light_type = 0;
	std::vector<std::shared_ptr<scene_light>> lights;
};