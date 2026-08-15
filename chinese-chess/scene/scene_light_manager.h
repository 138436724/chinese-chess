#pragma once

#include "vulkan_core/vulkan_buffer.h"

#include <memory>
#include <vector>

struct scene_light;
class vulkan_recycle_bin;
class vulkan_semaphore;
class vulkan_queue;

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

    [[nodiscard]] std::shared_ptr<scene_light> create();

    [[nodiscard]] bool update(std::vector<vk::SemaphoreSubmitInfo>& _waited_infos);
    void               clear() noexcept;

    void need_update() noexcept;

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

    bool is_dirty = true;

    std::vector<std::weak_ptr<scene_light>> lights;

    vulkan_buffer ssbo;
};
