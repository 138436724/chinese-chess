#include "scene_light_manager.h"

#include "shared/cpu_shared_types.h"
#include "shared/vulkan_shared_utils.h"
#include "vulkan_core/vulkan_recycle_bin.h"

#include <cstddef>
#include <cstdint>
#include <glm/glm.hpp>
#include <memory>
#include <ranges>
#include <span>
#include <utility>
#include <vector>

scene_light_manager::scene_light_manager(const vma::raii::Allocator& _allocator,
                                         const vk::raii::Device&     _device,
                                         vulkan_recycle_bin&         _recycle_bin,
                                         vulkan_semaphore&           _semaphore,
                                         const vulkan_queue&         _graphic_queue,
                                         const vulkan_queue&         _transfer_queue) noexcept
    : allocator(_allocator)
    , device(_device)
    , recycle_bin(_recycle_bin)
    , semaphore(_semaphore)
    , graphic_queue(_graphic_queue)
    , transfer_queue(_transfer_queue)
{
}

std::shared_ptr<cpu_light_data> scene_light_manager::create()
{
    const auto light = std::make_shared<cpu_light_data>();
    lights.push_back(light);
    is_dirty = true;
    return light;
}

bool scene_light_manager::update(std::vector<vk::SemaphoreSubmitInfo>& _waited_infos)
{
    if (std::erase_if(lights, [](const auto& p) static { return p.expired(); }) != 0)
    {
        is_dirty = true;
    }

    if (!is_dirty)
    {
        return false;
    }

    update_ssbo(_waited_infos);

    is_dirty = false;

    return true;
}

void scene_light_manager::clear() noexcept
{
    lights.clear();
    recycle_bin.retire(std::move(ssbo), "scene light manager clear ssbo.");
    is_dirty = true;
}

void scene_light_manager::need_update() noexcept
{
    is_dirty = true;
}

size_t scene_light_manager::get_lights_size() const noexcept
{
    return lights.size();
}

const vulkan_buffer& scene_light_manager::get_ssbo_buffer() const noexcept
{
    return ssbo;
}

void scene_light_manager::update_ssbo(std::vector<vk::SemaphoreSubmitInfo>& _waited_infos)
{
    if (!lights.empty())
    {
        const auto lights_ssbo = lights | std::views::transform([](const auto& p) static {
                                     const auto sp = p.lock();
                                     return gpu_light_data{.color            = sp->color,
                                                           .active_type      = std::to_underlying(sp->active_type),
                                                           .direction        = glm::normalize(sp->direction),
                                                           .intensity        = sp->intensity,
                                                           .position         = sp->position,
                                                           .range            = sp->range,
                                                           .inner_cone_angle = sp->inner_cone_angle,
                                                           .outer_cone_angle = sp->outer_cone_angle};
                                 })
                                 | std::ranges::to<std::vector>();

        _waited_infos.push_back(vulkan_shared_utils::upload_buffer(
            allocator, device, recycle_bin, semaphore, graphic_queue, transfer_queue, ssbo,
            vk::BufferUsageFlagBits::eTransferDst | vk::BufferUsageFlagBits::eStorageBuffer | vk::BufferUsageFlagBits::eShaderDeviceAddress,
            vk::PipelineStageFlagBits2::eRayTracingShaderKHR | vk::PipelineStageFlagBits2::eComputeShader
                | vk::PipelineStageFlagBits2::eFragmentShader,
            vk::AccessFlagBits2::eShaderRead,
            std::span(reinterpret_cast<const uint8_t*>(lights_ssbo.data()),
                      sizeof(decltype(lights_ssbo)::value_type) * lights_ssbo.size()),
            "light_ssbo"));
    }
}
