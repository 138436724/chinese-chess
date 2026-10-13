#include "vulkan_sampler.h"

#include <bit>
#include <concepts>
#include <format>
#include <utility>

vulkan_sampler::vulkan_sampler(vulkan_sampler&& _other) noexcept
    : sampler(std::exchange(_other.sampler, nullptr))
{
}

vulkan_sampler& vulkan_sampler::operator=(vulkan_sampler&& _other) noexcept
{
    if (this != &_other)
    {
        std::ranges::swap(sampler, _other.sampler);
    }
    return *this;
}

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

const vk::raii::Sampler& vulkan_sampler::operator*() const noexcept
{
    return sampler;
}
