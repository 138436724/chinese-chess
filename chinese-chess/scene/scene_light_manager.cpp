#include "scene_light_manager.h"

#include "vulkan_core/vulkan_common.h"

#include <ranges>

enum class light_type : uint32_t
{
    directional,
    point,
    spot
};

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

scene_light_manager::scene_light_manager(vulkan_application& _app,
                                         vulkan_recycle_bin& _recycle_bin,
                                         vulkan_queue&       _graphic_queue,
                                         vulkan_queue&       _transfer_queue)
    : app(_app)
    , recycle_bin(_recycle_bin)
    , graphic_queue(_graphic_queue)
    , transfer_queue(_transfer_queue)
{
}

void scene_light_manager::update(std::vector<vk::SemaphoreSubmitInfo>& _wait_info) noexcept
{
    std::erase_if(lights, [](const auto& p) { return p.expired(); });

    recycle_bin.retire(std::move(ssbo));

    update_ssbo(_wait_info);
}

void scene_light_manager::clear() noexcept
{
    lights.clear();
}

const vulkan_buffer& scene_light_manager::get_ssbo_buffer() const noexcept
{
    return ssbo;
}

const std::vector<std::weak_ptr<scene_light>>& scene_light_manager::get_lights() const noexcept
{
    return lights;
}

void scene_light_manager::update_ssbo(std::vector<vk::SemaphoreSubmitInfo>& _wait_info) noexcept
{
    if (!lights.empty())
    {
        auto lights_ssbo = lights | std::views::transform([this](const auto& p) {
                               auto sp = p.lock();
                               return std::visit(
                                   [](auto& light) {
                                       using T = std::decay_t<decltype(light)>;

                                       if constexpr (std::is_same_v<T, directional_light>)
                                       {
                                           return light_data{
                                               .color       = light.color,
                                               .active_type = static_cast<uint32_t>(light_type::directional),
                                               .direction   = glm::normalize(light.direction),
                                               .intensity   = light.intensity,
                                           };
                                       }
                                       else if constexpr (std::is_same_v<T, point_light>)
                                       {
                                           return light_data{
                                               .color       = light.color,
                                               .active_type = static_cast<uint32_t>(light_type::point),
                                               .intensity   = light.intensity,
                                               .position    = light.position,
                                               .range       = light.range,
                                           };
                                       }
                                       else if constexpr (std::is_same_v<T, spot_light>)
                                       {
                                           return light_data{
                                               .color            = light.color,
                                               .active_type      = static_cast<uint32_t>(light_type::spot),
                                               .direction        = glm::normalize(light.direction),
                                               .intensity        = light.intensity,
                                               .position         = light.position,
                                               .range            = light.range,
                                               .inner_cone_angle = light.inner_cone_angle,
                                               .outer_cone_angle = light.outer_cone_angle,
                                           };
                                       }
                                   },
                                   *sp);
                           })
                           | std::ranges::to<std::vector>();

        _wait_info.push_back(vulkan_common::upload_buffer(
            app.get_allocator(), *app.get_device(), recycle_bin, app.get_semaphore_ptr(), graphic_queue, transfer_queue, ssbo,
            vk::BufferUsageFlagBits::eTransferDst | vk::BufferUsageFlagBits::eStorageBuffer | vk::BufferUsageFlagBits::eShaderDeviceAddress,
            std::span(reinterpret_cast<uint8_t*>(lights_ssbo.data()), sizeof(lights_ssbo.front()) * lights_ssbo.size())));
    }
}
