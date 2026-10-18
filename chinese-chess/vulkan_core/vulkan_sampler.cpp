/**
 * @file vulkan_sampler.cpp
 * @brief 采样器创建与移动语义的实现：把 sampler_create_info 翻成 vk::SamplerCreateInfo。
 * @details 各向异性开启时取设备 limits.maxSamplerAnisotropy，关闭时写 1；borderColor 固定
 *          eFloatOpaqueBlack；compareEnable 固定 false（compareOp 为 eAlways），因此不启用比较采样。
 *          Debug 下按 _name 设置对象名；采样器没有参数去重，重复 create 会直接替换旧句柄。
 */

#include "vulkan_sampler.h"

#include <bit>
#include <concepts>
#include <format>
#include <utility>

/** @brief std::exchange 接管句柄，源对象回到 nullptr 状态。 */
vulkan_sampler::vulkan_sampler(vulkan_sampler&& _other) noexcept
    : sampler(std::exchange(_other.sampler, nullptr))
{
}

/** @brief 自赋值提前返回，否则 std::ranges::swap 两个句柄，两边都可安全析构。 */
vulkan_sampler& vulkan_sampler::operator=(vulkan_sampler&& _other) noexcept
{
    if (this != &_other)
    {
        std::ranges::swap(sampler, _other.sampler);
    }
    return *this;
}

/**
 * @brief 按 _info 组装 vk::SamplerCreateInfo 并创建采样器句柄。
 * @note 各向异性只在 enable_anisotropy 时生效，开启时用设备上限而不是固定倍数。
 * @note 边界色恒为不透明黑，只供 clamp-to-border 使用；clamp-to-edge 使用边缘 texel。
 */
void vulkan_sampler::create(const vk::raii::PhysicalDevice&     _physical_device,
                            const vk::raii::Device&             _device,
                            const sampler_create_info&          _info,
                            [[maybe_unused]] const std::string& _name)
{
    const vk::SamplerCreateInfo sampler_info(
        {}, _info.mag_filter, _info.min_filter, _info.mipmap_mode, _info.address_mode_u, _info.address_mode_v,
        _info.address_mode_w, 0.f, _info.enable_anisotropy,
        _info.enable_anisotropy ? _physical_device.getProperties2().properties.limits.maxSamplerAnisotropy : 1.f, vk::False,
        vk::CompareOp::eAlways, _info.min_lod, _info.max_lod, vk::BorderColor::eFloatOpaqueBlack, vk::False, nullptr);

    sampler = vk::raii::Sampler(_device, sampler_info);

#ifndef NDEBUG
    _device.setDebugUtilsObjectNameEXT(
        vk::DebugUtilsObjectNameInfoEXT(sampler.objectType, std::bit_cast<uint64_t>(static_cast<VkSampler>(*sampler)),
                                        std::format("Sampler{}", _name).c_str()));
#endif  // !NDEBUG
}

/** @brief 返回底层采样器句柄；未 create 时是 nullptr。 */
const vk::raii::Sampler& vulkan_sampler::operator*() const noexcept
{
    return sampler;
}
