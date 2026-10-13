#include "scene_material_manager.h"

#include "scene_texture_manager.h"
#include "vulkan_core/vulkan_common.h"
#include "vulkan_core/vulkan_data_type.h"
#include "vulkan_core/vulkan_recycle_bin.h"

#include <cstddef>
#include <cstdint>
#include <span>
#include <utility>
#include <vector>

scene_material_manager::scene_material_manager(const vma::raii::Allocator& _allocator,
                                               const vk::raii::Device&     _device,
                                               vulkan_recycle_bin&         _recycle_bin,
                                               vulkan_semaphore&           _semaphore,
                                               const vulkan_queue&         _graphic_queue,
                                               const vulkan_queue&         _transfer_queue,
                                               scene_texture_manager&      _texture_manager) noexcept
    : allocator(_allocator)
    , device(_device)
    , recycle_bin(_recycle_bin)
    , semaphore(_semaphore)
    , graphic_queue(_graphic_queue)
    , transfer_queue(_transfer_queue)
    , texture_manager(_texture_manager)
{
}

material_handle scene_material_manager::create()
{
    is_dirty = true;
    return materials.insert(material_entry{.data = scene_material_data{}});
}

scene_material_data* scene_material_manager::get(material_handle _handle) noexcept
{
    auto* entry = materials.get(_handle);
    return entry ? &entry->data : nullptr;
}

bool scene_material_manager::destroy(material_handle _handle) noexcept
{
    const auto* entry = materials.get(_handle);
    if (entry == nullptr)
    {
        return false;
    }

    for (const auto texture : entry->data.textures)
    {
        texture_manager.release(texture);
    }

    std::ignore = materials.erase(_handle);
    is_dirty    = true;
    return true;
}

bool scene_material_manager::update(std::vector<vk::SemaphoreSubmitInfo>& _waited_infos)
{
    if (!is_dirty)
    {
        return false;
    }

    update_ssbo(_waited_infos);

    is_dirty = false;
    return true;
}

void scene_material_manager::clear()
{
    for (auto&& entry : materials.live())
    {
        for (const auto texture : entry.value.data.textures)
        {
            texture_manager.release(texture);
        }
    }

    materials.clear();
    recycle_bin.retire(std::move(ssbo), "scene material manager clear ssbo.");
    is_dirty = true;
}

void scene_material_manager::need_update() noexcept
{
    is_dirty = true;
}

void scene_material_manager::set_texture(material_handle _handle, texture_channel _channel, texture_handle _texture) 
{
    auto* entry = materials.get(_handle);
    if (entry == nullptr)
    {
        return;
    }

    const auto index = std::to_underlying(_channel);
    if (index < MAX_CHANNEL_COUNT) [[likely]]
    {

        texture_handle& slot = entry->data.textures.at(index);
        texture_manager.release(slot);

        slot = _texture;
        texture_manager.retain(_texture);

        is_dirty = true;
    }
#ifndef NDEBUG
    else
    {
        throw std::runtime_error("Undefined channel!");
    }
#endif  // !NDEBUG
}

size_t scene_material_manager::get_materials_size() const noexcept
{
    return materials.live_size();
}

const vulkan_buffer& scene_material_manager::get_ssbo_buffer() const noexcept
{
    return ssbo;
}

uint32_t scene_material_manager::get_material_binding(material_handle _handle) const noexcept
{
    const auto* entry = materials.get(_handle);
    return entry ? entry->binding : scene_handle<material_tag>::npos;
}

void scene_material_manager::update_ssbo(std::vector<vk::SemaphoreSubmitInfo>& _waited_infos)
{
    if (materials.empty())
    {
        return;
    }

    const auto materials_ssbo =
        materials.live() | std::views::enumerate | std::views::transform([this](const auto& _pair) {
            const auto& [index, entry] = _pair;
            entry.value.binding        = static_cast<uint32_t>(index);
            const auto& material       = entry.value.data;
            return vulkan_material_data{.albedo        = material.albedo,
                                        .texture_index = texture_manager.get_texture_binding(
                                            material.textures[std::to_underlying(texture_channel::albedo)]),
                                        .roughness         = material.roughness,
                                        .emission          = material.emission,
                                        .emission_strength = material.emission_strength,
                                        .absorption        = material.absorption,
                                        .metallic          = material.metallic,
                                        .reflectance       = material.reflectance,
                                        .ior               = material.ior,
                                        .transmission      = material.transmission,
                                        .opacity           = material.opacity};
        })
        | std::ranges::to<std::vector>();

    _waited_infos.push_back(vulkan_common::upload_buffer(
        allocator, device, recycle_bin, semaphore, graphic_queue, transfer_queue, ssbo,
        vk::BufferUsageFlagBits::eTransferDst | vk::BufferUsageFlagBits::eStorageBuffer | vk::BufferUsageFlagBits::eShaderDeviceAddress,
        vk::PipelineStageFlagBits2::eFragmentShader | vk::PipelineStageFlagBits2::eRayTracingShaderKHR | vk::PipelineStageFlagBits2::eComputeShader,
        vk::AccessFlagBits2::eShaderRead,
        std::span(reinterpret_cast<const uint8_t*>(materials_ssbo.data()),
                  sizeof(decltype(materials_ssbo)::value_type) * materials_ssbo.size()),
        "material_ssbo"));
}
