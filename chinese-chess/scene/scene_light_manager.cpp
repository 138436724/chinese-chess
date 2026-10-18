#include "scene_light_manager.h"

#include "vulkan_core/vulkan_common.h"
#include "vulkan_core/vulkan_recycle_bin.h"

#include <cstddef>
#include <cstdint>
#include <glm/glm.hpp>
#include <ranges>
#include <span>
#include <utility>
#include <vector>

scene_light_manager::scene_light_manager(const vma::raii::Allocator& _allocator,
                                         const vk::raii::Device&     _device,
                                         vulkan_recycle_bin&         _recycle_bin,
                                         const vulkan_queue&         _graphic_queue,
                                         const vulkan_queue&         _transfer_queue) noexcept
    : allocator(_allocator)
    , device(_device)
    , recycle_bin(_recycle_bin)
    , graphic_queue(_graphic_queue)
    , transfer_queue(_transfer_queue)
{
}

light_handle scene_light_manager::create()
{
    is_dirty = true;
    return lights.insert(scene_light_data{});
}

scene_light_data* scene_light_manager::get(light_handle _handle) noexcept
{
    return lights.get(_handle);
}

bool scene_light_manager::destroy(light_handle _handle) noexcept
{
    if (!lights.erase(_handle))
    {
        return false;
    }

    is_dirty = true;
    return true;
}

bool scene_light_manager::update(std::vector<vulkan_submission_token>& _waited_infos)
{
    if (!is_dirty)
    {
        return false;
    }

    update_ssbo(_waited_infos);

    is_dirty = false;

    return true;
}

void scene_light_manager::need_update() noexcept
{
    is_dirty = true;
}

size_t scene_light_manager::get_lights_size() const noexcept
{
    return lights.live_size();
}

const vulkan_buffer& scene_light_manager::get_ssbo_buffer() const noexcept
{
    return ssbo;
}

void scene_light_manager::update_ssbo(std::vector<vulkan_submission_token>& _waited_infos)
{
    if (!lights.empty())
    {
        const auto lights_ssbo =
            lights.live() | std::views::transform([](const auto& _entry) {
                const scene_light_data& _light = _entry.value;
                return vulkan_light_data{.color       = _light.color,
                                         .active_type = std::to_underlying(_light.active_type),
                                         .direction = vulkan_common::safe_normalize(_light.direction, glm::vec3(0.f, -1.f, 0.f)),
                                         .intensity        = _light.intensity,
                                         .position         = _light.position,
                                         .range            = _light.range,
                                         .inner_cone_angle = _light.inner_cone_angle,
                                         .outer_cone_angle = _light.outer_cone_angle};
            })
            | std::ranges::to<std::vector>();

        _waited_infos.push_back(vulkan_common::upload_buffer(
            allocator, device, recycle_bin, graphic_queue, transfer_queue, ssbo,
            vk::BufferUsageFlagBits::eTransferDst | vk::BufferUsageFlagBits::eStorageBuffer | vk::BufferUsageFlagBits::eShaderDeviceAddress,
            vk::PipelineStageFlagBits2::eRayTracingShaderKHR | vk::PipelineStageFlagBits2::eComputeShader
                | vk::PipelineStageFlagBits2::eFragmentShader,
            vk::AccessFlagBits2::eShaderRead,
            std::span(reinterpret_cast<const uint8_t*>(lights_ssbo.data()),
                      sizeof(decltype(lights_ssbo)::value_type) * lights_ssbo.size()),
            "light_ssbo"));
    }
}
