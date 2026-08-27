#pragma once

#include <vulkan/vulkan_raii.hpp>

enum class sampler_type
{
    diffuse,
    color,
    normal,
    roughness,
    metallic,
    font,
    sky_box,
    screen
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

    void create(const vk::raii::PhysicalDevice& _physical_device, const vk::raii::Device& _device, sampler_type _type);

    [[nodiscard]] const vk::raii::Sampler& operator*() const noexcept;

private:
    vk::raii::Sampler sampler = nullptr;
};
