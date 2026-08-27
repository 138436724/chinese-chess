#pragma once

#include <expected>
#include <string>
#include <vulkan/vulkan_raii.hpp>

class vulkan_pipeline_cache
{
public:
    vulkan_pipeline_cache()                                            = default;
    ~vulkan_pipeline_cache()                                           = default;
    vulkan_pipeline_cache(const vulkan_pipeline_cache&)                = delete;
    vulkan_pipeline_cache& operator=(const vulkan_pipeline_cache&)     = delete;
    vulkan_pipeline_cache(vulkan_pipeline_cache&&) noexcept            = delete;
    vulkan_pipeline_cache& operator=(vulkan_pipeline_cache&&) noexcept = delete;

    void create(const vk::raii::PhysicalDevice& _physical_device, const vk::raii::Device& _device) noexcept;
    void save() const noexcept;

    [[nodiscard]] const vk::raii::PipelineCache& operator*() const noexcept;

private:
    vk::raii::PipelineCache pipeline_cache = nullptr;
};
