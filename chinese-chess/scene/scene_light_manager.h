#pragma once

#include "scene_data_type.h"
#include "scene_handle.h"
#include "scene_handle_map.h"
#include "vulkan_core/vulkan_buffer.h"

#include <cstddef>
#include <vector>

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

    [[nodiscard]] light_handle      create();
    [[nodiscard]] scene_light_data* get(light_handle _handle) noexcept;
    [[nodiscard]] bool              destroy(light_handle _handle) noexcept;

    [[nodiscard]] bool update(std::vector<vk::SemaphoreSubmitInfo>& _waited_infos);
    void               clear() noexcept;

    void need_update() noexcept;

    [[nodiscard]] size_t               get_lights_size() const noexcept;
    [[nodiscard]] const vulkan_buffer& get_ssbo_buffer() const noexcept;


private:
    void update_ssbo(std::vector<vk::SemaphoreSubmitInfo>& _waited_infos);

    const vma::raii::Allocator& allocator;
    const vk::raii::Device&     device;
    vulkan_recycle_bin&         recycle_bin;
    vulkan_semaphore&           semaphore;
    const vulkan_queue&         graphic_queue;
    const vulkan_queue&         transfer_queue;

    bool is_dirty = true;

    scene_handle_map<scene_light_data, light_tag> lights;

    vulkan_buffer ssbo;
};
