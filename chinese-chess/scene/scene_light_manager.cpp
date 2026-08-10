#include "scene_light_manager.h"

#include "vulkan_core/vulkan_common.h"
#include "vulkan_core/vulkan_queue.h"
#include "vulkan_core/vulkan_recycle_bin.h"

#include <ranges>

struct light_data
{
    alignas(16) glm::vec3 color        = glm::vec3(1.f);
    uint32_t active_type               = static_cast<uint32_t>(light_type::directional);
    alignas(16) glm::vec3 direction    = glm::vec3(0.f, -1.f, 0.f);
    float intensity                    = 1.f;
    alignas(16) glm::vec3 position     = glm::vec3(0.f);
    float range                        = 10.f;
    alignas(16) float inner_cone_angle = glm::radians(15.f);
    float outer_cone_angle             = glm::radians(30.f);
};

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

std::shared_ptr<scene_light> scene_light_manager::create()
{
    auto light = std::make_shared<scene_light>();
    lights.push_back(light);
    return light;
}

void scene_light_manager::update(std::vector<vk::SemaphoreSubmitInfo>& _waited_infos)
{
    std::erase_if(lights, [](const auto& p) static { return p.expired(); });

    recycle_bin.retire(std::move(ssbo), "scene light manager old ssbo.");

    update_ssbo(_waited_infos);
}

void scene_light_manager::clear() noexcept
{
    lights.clear();
    recycle_bin.retire(std::move(ssbo), "scene light manager clear ssbo.");
}

const vulkan_buffer& scene_light_manager::get_ssbo_buffer() const noexcept
{
    return ssbo;
}

const std::vector<std::weak_ptr<scene_light>>& scene_light_manager::get_lights() const noexcept
{
    return lights;
}

void scene_light_manager::update_ssbo(std::vector<vk::SemaphoreSubmitInfo>& _waited_infos)
{
    if (!lights.empty())
    {
        const auto lights_ssbo = lights | std::views::transform([this](const auto& p) {
                                     const auto sp = p.lock();
                                     return light_data{.color            = sp->color,
                                                       .active_type      = static_cast<uint32_t>(sp->active_type),
                                                       .direction        = glm::normalize(sp->direction),
                                                       .intensity        = sp->intensity,
                                                       .position         = sp->position,
                                                       .range            = sp->range,
                                                       .inner_cone_angle = sp->inner_cone_angle,
                                                       .outer_cone_angle = sp->outer_cone_angle};
                                 })
                                 | std::ranges::to<std::vector>();

        _waited_infos.push_back(vulkan_common::upload_buffer(
            allocator, device, recycle_bin, semaphore, graphic_queue, transfer_queue, ssbo,
            vk::BufferUsageFlagBits::eTransferDst | vk::BufferUsageFlagBits::eStorageBuffer | vk::BufferUsageFlagBits::eShaderDeviceAddress,
            std::span(reinterpret_cast<const uint8_t*>(lights_ssbo.data()), sizeof(lights_ssbo.front()) * lights_ssbo.size())));
    }
}
