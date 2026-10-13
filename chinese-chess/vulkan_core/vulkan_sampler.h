#pragma once

#include <compare>
#include <cstdint>
#include <string>
#include <utility>
#include <vulkan/vulkan_raii.hpp>

struct sampler_create_info
{
    vk::Filter            mag_filter  = vk::Filter::eLinear;
    vk::Filter            min_filter  = vk::Filter::eLinear;
    vk::SamplerMipmapMode mipmap_mode = vk::SamplerMipmapMode::eLinear;

    vk::SamplerAddressMode address_mode_u = vk::SamplerAddressMode::eRepeat;
    vk::SamplerAddressMode address_mode_v = vk::SamplerAddressMode::eRepeat;
    vk::SamplerAddressMode address_mode_w = vk::SamplerAddressMode::eRepeat;

    bool  enable_anisotropy = false;
    float min_lod           = 0.f;
    float max_lod           = vk::LodClampNone;

    bool operator==(const sampler_create_info&) const  = default;
    auto operator<=>(const sampler_create_info&) const = default;
};

class vulkan_sampler
{
public:
    vulkan_sampler()                                 = default;
    ~vulkan_sampler()                                = default;
    vulkan_sampler(const vulkan_sampler&)            = delete;
    vulkan_sampler& operator=(const vulkan_sampler&) = delete;
    vulkan_sampler(vulkan_sampler&& _other) noexcept;
    vulkan_sampler& operator=(vulkan_sampler&& _other) noexcept;

    void create(const vk::raii::PhysicalDevice& _physical_device,
                const vk::raii::Device&         _device,
                const sampler_create_info&      _info,
                const std::string&              _name);

    [[nodiscard]] const vk::raii::Sampler& operator*() const noexcept;

private:
    vk::raii::Sampler sampler = nullptr;
};
