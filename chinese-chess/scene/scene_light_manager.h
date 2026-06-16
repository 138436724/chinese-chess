#pragma once

#include "scene_light.h"
#include <memory>
#include <vector>

class scene_light_manager
{
public:
	scene_light_manager() = default;
	~scene_light_manager() = default;

	std::shared_ptr<scene_light> create();
	void clear_unused() noexcept;
	void clear() noexcept;

	const std::vector<std::shared_ptr<scene_light>>& get_lights() const noexcept;
	size_t get_light_count() const noexcept;

private:
	std::vector<std::weak_ptr<scene_light>> lights; // weak tracking for lifecycle
	std::vector<std::shared_ptr<scene_light>> active_lights; // ordered, exposed to consumers
};