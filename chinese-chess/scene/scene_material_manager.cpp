/**
 * @file scene_material_manager.cpp
 * @brief 材质槽表的增删、贴图引用维护与 SSBO 打包上传的实现。
 * @details create 只插入槽，destroy 先对所有通道 release 再擦除；set_texture 先 release 旧句柄、
 *          再 retain 新句柄并置脏。update() 走 is_dirty 门控，update_ssbo() 用 enumerate 重排 binding，
 *          并把 PBR 参数与 albedo 通道绑定写成 vulkan_material_data 数组交给 upload_buffer。
 * @note 空材质表不重建 SSBO；其余四个贴图通道目前只留在 CPU 侧，不进入 GPU ABI。
 */
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

/**
 * @brief 保存六份引用，SSBO 留到第一次 update 再创建。
 * @note 构造不触碰 Vulkan，因此 noexcept。
 */
scene_material_manager::scene_material_manager(const vma::raii::Allocator& _allocator,
                                               const vk::raii::Device&     _device,
                                               vulkan_recycle_bin&         _recycle_bin,
                                               const vulkan_queue&         _graphic_queue,
                                               const vulkan_queue&         _transfer_queue,
                                               scene_texture_manager&      _texture_manager) noexcept
    : allocator(_allocator)
    , device(_device)
    , recycle_bin(_recycle_bin)
    , graphic_queue(_graphic_queue)
    , transfer_queue(_transfer_queue)
    , texture_manager(_texture_manager)
{
}

/**
 * @brief 插入默认材质并置脏，binding 暂为 npos。
 * @note 只有槽表操作，不立即产生 GPU 工作。
 */
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

/**
 * @brief 释放材质全部通道的贴图引用，再擦除槽并置脏。
 * @note 标 noexcept；release 对无效句柄是 no-op，因此未设贴图的通道不影响流程。
 */
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

/**
 * @brief 脏时才重排 binding 并重传材质 SSBO，成功后清标志。
 * @note 上传失败会从 update_ssbo 抛出，is_dirty 保持 true，下一次调用重试。
 */
bool scene_material_manager::update(std::vector<vulkan_submission_token>& _waited_infos)
{
    if (!is_dirty)
    {
        return false;
    }

    update_ssbo(_waited_infos);

    is_dirty = false;
    return true;
}

void scene_material_manager::need_update() noexcept
{
    is_dirty = true;
}

/**
 * @brief 替换某通道的贴图句柄，并对新旧句柄分别 release / retain。
 * @note 材质句柄无效时直接返回；Debug 下通道越界会抛 runtime_error，Release 下静默忽略。
 * @note 只有实际写入槽位才置 is_dirty，越过校验的调用不会触发重传。
 */
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

/**
 * @brief 取上次 SSBO 打包分配的紧凑 binding，句柄无效或已死时返回 npos。
 * @note binding 只在 update_ssbo 中刷新，因此新建材质在下一次 update 之前查不到下标。
 */
uint32_t scene_material_manager::get_material_binding(material_handle _handle) const noexcept
{
    const auto* entry = materials.get(_handle);
    return entry ? entry->binding : scene_handle<material_tag>::npos;
}

/**
 * @brief 按 live() 顺序重新编号 binding，并上传材质参数与 albedo 贴图绑定。
 * @note 空表直接返回，旧 SSBO 内容与地址保持不变；只有 albedo 通道进入 GPU ABI。
 * @note binding 的写入发生在上传之前，即使上传抛异常，下标也已反映本次槽序。
 */
void scene_material_manager::update_ssbo(std::vector<vulkan_submission_token>& _waited_infos)
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
        allocator, device, recycle_bin, graphic_queue, transfer_queue, ssbo,
        vk::BufferUsageFlagBits::eTransferDst | vk::BufferUsageFlagBits::eStorageBuffer | vk::BufferUsageFlagBits::eShaderDeviceAddress,
        vk::PipelineStageFlagBits2::eFragmentShader | vk::PipelineStageFlagBits2::eRayTracingShaderKHR | vk::PipelineStageFlagBits2::eComputeShader,
        vk::AccessFlagBits2::eShaderRead,
        std::span(reinterpret_cast<const uint8_t*>(materials_ssbo.data()),
                  sizeof(decltype(materials_ssbo)::value_type) * materials_ssbo.size()),
        "material_ssbo"));
}
