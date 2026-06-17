#pragma once

#include "scene_light.h"
#include "vulkan_core/vulkan_application.h"
#include <memory>
#include <vector>

class scene_light_manager
{
public:
	scene_light_manager(vulkan_application* _app);
	~scene_light_manager() = default;

	std::shared_ptr<scene_light> create(light_type _type) noexcept;
	void remove(const std::weak_ptr<scene_light>& _light) noexcept;
	void update(vulkan_commandbuffer& _commandbuffer) noexcept;
	void clear() noexcept;

	const vulkan_buffer& get_ssbo_buffer() const noexcept;

    const std::vector<std::shared_ptr<scene_light>>& get_lights() const noexcept;

private:
	void update_ssbo(vulkan_commandbuffer& _commandbuffer) noexcept;

	vulkan_application* app = nullptr;

	std::vector<std::shared_ptr<scene_light>> lights;

	vulkan_buffer ssbo;
};