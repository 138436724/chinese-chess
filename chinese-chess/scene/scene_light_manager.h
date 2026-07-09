#pragma once

#include "scene_light.h"
#include "vulkan_core/vulkan_application.h"

#include <memory>
#include <vector>

class scene_light_manager
{
public:
    scene_light_manager(vulkan_application& _app, vulkan_recycle_bin& _recycle_bin, vulkan_queue& _graphic_queue, vulkan_queue& _transfer_queue);
    ~scene_light_manager() = default;

    template <typename T>
        requires(std::same_as<T, directional_light> || std::same_as<T, point_light> || std::same_as<T, spot_light>)
    std::shared_ptr<scene_light> create() noexcept;

    void update(std::vector<vk::SemaphoreSubmitInfo>& _wait_info) noexcept;
    void clear() noexcept;

    const vulkan_buffer& get_ssbo_buffer() const noexcept;

    const std::vector<std::weak_ptr<scene_light>>& get_lights() const noexcept;

private:
    void update_ssbo(std::vector<vk::SemaphoreSubmitInfo>& _wait_info) noexcept;

    vulkan_application& app;
    vulkan_recycle_bin& recycle_bin;

    vulkan_queue& graphic_queue;
    vulkan_queue& transfer_queue;

    std::vector<std::weak_ptr<scene_light>> lights;

    vulkan_buffer ssbo;
};

template <typename T>
    requires(std::same_as<T, directional_light> || std::same_as<T, point_light> || std::same_as<T, spot_light>)
inline std::shared_ptr<scene_light> scene_light_manager::create() noexcept
{
    auto light = std::make_shared<scene_light>();
    *light     = T();
    lights.push_back(light);
    return light;
}
