#include "vulkan_sampler.h"

#include <utility>

namespace {
struct sampler_settings
{
    vk::Filter             mag_filter;
    vk::Filter             min_filter;
    vk::SamplerMipmapMode  mipmap_mode;
    vk::SamplerAddressMode address_mode;
    bool                   enable_anisotropy;
    float                  min_lod;
    float                  max_lod;
};

constexpr sampler_settings get_sampler_settings(sampler_type _type) noexcept
{
    switch (_type)
    {
        case sampler_type::diffuse:
        case sampler_type::color:
        case sampler_type::normal:
        case sampler_type::roughness:
        case sampler_type::metallic:
            return {vk::Filter::eLinear,
                    vk::Filter::eLinear,
                    vk::SamplerMipmapMode::eLinear,
                    vk::SamplerAddressMode::eRepeat,
                    true,
                    0.f,
                    vk::LodClampNone};
        case sampler_type::font:
            return {vk::Filter::eLinear, vk::Filter::eLinear, vk::SamplerMipmapMode::eNearest, vk::SamplerAddressMode::eClampToBorder, false, 0.f, 0.f};
        case sampler_type::sky_box:
            return {vk::Filter::eLinear,
                    vk::Filter::eLinear,
                    vk::SamplerMipmapMode::eLinear,
                    vk::SamplerAddressMode::eClampToEdge,
                    true,
                    0.f,
                    vk::LodClampNone};
        case sampler_type::screen:
            return {vk::Filter::eLinear, vk::Filter::eLinear, vk::SamplerMipmapMode::eLinear, vk::SamplerAddressMode::eClampToEdge, false, 0.f, 0.f};
        default:
            std::unreachable();
    }
}
}  // namespace

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

void vulkan_sampler::create(const vk::raii::PhysicalDevice& _physical_device, const vk::raii::Device& _device, sampler_type _type)
{
    const vk::PhysicalDeviceProperties properties = _physical_device.getProperties();
    const sampler_settings             settings   = get_sampler_settings(_type);

    const vk::SamplerCreateInfo sampler_info({}, settings.mag_filter, settings.min_filter, settings.mipmap_mode,
                                             settings.address_mode, settings.address_mode, settings.address_mode, 0.f,
                                             settings.enable_anisotropy,
                                             (settings.enable_anisotropy ? properties.limits.maxSamplerAnisotropy : 1.f),
                                             vk::False, vk::CompareOp::eAlways, settings.min_lod, settings.max_lod,
                                             vk::BorderColor::eFloatOpaqueBlack, vk::False, nullptr);

    sampler = vk::raii::Sampler(_device, sampler_info);
}

const vk::raii::Sampler& vulkan_sampler::operator*() const noexcept
{
    return sampler;
}
