#include "scene_light_manager.h"

std::shared_ptr<scene_light> scene_light_manager::create()
{
	auto light = std::make_shared<scene_light>();
	lights.push_back(light);
	return light;
}

void scene_light_manager::clear_unused() noexcept
{
	std::erase_if(lights, [](const auto& w) { return w.expired(); });

	active_lights.clear();
	for (const auto& w : lights)
	{
		if (auto sp = w.lock())
		{
			active_lights.push_back(sp);
		}
	}
}

void scene_light_manager::clear() noexcept
{
	lights.clear();
	active_lights.clear();
}

const std::vector<std::shared_ptr<scene_light>>& scene_light_manager::get_lights() const noexcept
{
	return active_lights;
}

size_t scene_light_manager::get_light_count() const noexcept
{
	return active_lights.size();
}