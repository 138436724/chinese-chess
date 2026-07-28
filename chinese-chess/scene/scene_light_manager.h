#pragma once

#include "scene_light.h"
#include "vulkan_core/vulkan_application.h"

#include <memory>
#include <vector>

class scene_light_manager
{
public:
    scene_light_manager(const vma::raii::Allocator& _allocator,
                        const vk::raii::Device&     _device,
                        vulkan_recycle_bin&         _recycle_bin,
                        vulkan_semaphore&           _semaphore,
                        const vulkan_queue&         _graphic_queue,
                        const vulkan_queue&         _transfer_queue) noexcept;
    ~scene_light_manager() = default;

    template <typename T>
        requires(std::same_as<T, directional_light> || std::same_as<T, point_light> || std::same_as<T, spot_light>)
    [[nodiscard]] std::shared_ptr<scene_light> create();

    void update(std::vector<vk::SemaphoreSubmitInfo>& _waited_infos);
    void clear() noexcept;

    [[nodiscard]] const vulkan_buffer& get_ssbo_buffer() const noexcept;

    [[nodiscard]] const std::vector<std::weak_ptr<scene_light>>& get_lights() const noexcept;

private:
    void update_ssbo(std::vector<vk::SemaphoreSubmitInfo>& _waited_infos);

    const vma::raii::Allocator& allocator;
    const vk::raii::Device&     device;
    vulkan_recycle_bin&         recycle_bin;
    vulkan_semaphore&           semaphore;
    const vulkan_queue&         graphic_queue;
    const vulkan_queue&         transfer_queue;

    std::vector<std::weak_ptr<scene_light>> lights;

    vulkan_buffer ssbo;
};

template <typename T>
    requires(std::same_as<T, directional_light> || std::same_as<T, point_light> || std::same_as<T, spot_light>)
inline std::shared_ptr<scene_light> scene_light_manager::create()
{
    auto light = std::make_shared<scene_light>();
    *light     = T();
    lights.push_back(light);
    return light;
}
