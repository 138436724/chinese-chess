#include "scene_light_manager.h"

#include "scene_light.h"
#include "vulkan_core/vulkan_common.h"
#include "vulkan_core/vulkan_queue.h"
#include "vulkan_core/vulkan_recycle_bin.h"

#include <ranges>
#include <span>
#include <utility>

namespace {
struct light_data  // scalar layout
{
    glm::vec3 color            = glm::vec3(1.f);
    uint32_t  active_type      = std::to_underlying(light_type::directional);
    glm::vec3 direction        = glm::vec3(0.f, -1.f, 0.f);
    float     intensity        = 1.f;
    glm::vec3 position         = glm::vec3(0.f);
    float     range            = 10.f;
    float     inner_cone_angle = glm::radians(15.f);
    float     outer_cone_angle = glm::radians(30.f);
};
}  // namespace

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
                                     return light_data{.color            = sp->color,
                                                       .active_type      = std::to_underlying(sp->active_type),
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
            vk::PipelineStageFlagBits2::eRayTracingShaderKHR | vk::PipelineStageFlagBits2::eComputeShader, vk::AccessFlagBits2::eShaderRead,
            std::span(reinterpret_cast<const uint8_t*>(lights_ssbo.data()), sizeof(lights_ssbo.front()) * lights_ssbo.size()),
            "light_ssbo"));
    }
}
