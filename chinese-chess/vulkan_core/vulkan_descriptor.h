#pragma once

#include <variant>
#include <vector>
#include <vulkan/vulkan_raii.hpp>

using DescriptorBufferOrImageInfo = std::variant<vk::DescriptorBufferInfo, vk::DescriptorImageInfo>;

class vulkan_descriptor
{
public:
    vulkan_descriptor()                                    = default;
    ~vulkan_descriptor()                                   = default;
    vulkan_descriptor(const vulkan_descriptor&)            = delete;
    vulkan_descriptor& operator=(const vulkan_descriptor&) = delete;
    vulkan_descriptor(vulkan_descriptor&& _other) noexcept;
    vulkan_descriptor& operator=(vulkan_descriptor&& _other) noexcept;

    void add_descriptor_info(vk::DescriptorType _descriptor_type, std::vector<DescriptorBufferOrImageInfo>&& _pool_info);
    void clear_descriptor_info() noexcept;
    void update_descriptor_sets(const vk::raii::Device& _device, const vk::raii::DescriptorSetLayout& _descriptor_set_layout);

    [[nodiscard]] const vk::raii::DescriptorPool&             get_descriptor_pool() const noexcept;
    [[nodiscard]] const std::vector<vk::raii::DescriptorSet>& get_descriptor_sets() const noexcept;

private:
    uint32_t                                              max_size = 0;
    std::vector<std::vector<DescriptorBufferOrImageInfo>> pool_infos;
    std::vector<vk::DescriptorPoolSize>                   pool_size;
    vk::raii::DescriptorPool                              descriptor_pool = nullptr;
    std::vector<vk::raii::DescriptorSet>                  descriptor_sets;
};
