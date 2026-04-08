#pragma once
#define VK_USE_PLATFORM_WIN32_KHR

#include <variant>
#include <vulkan/vulkan_raii.hpp>

using DescriptorBufferOrImageInfo = std::variant<vk::DescriptorBufferInfo, vk::DescriptorImageInfo>;

class vulkan_descriptor
{
public:
	vulkan_descriptor() = default;
	~vulkan_descriptor() = default;
	vulkan_descriptor(vulkan_descriptor&) = delete;
	vulkan_descriptor(vulkan_descriptor&& _other) noexcept;
	vulkan_descriptor& operator=(vulkan_descriptor&) = delete;
	vulkan_descriptor& operator=(vulkan_descriptor&& _other) noexcept;

	void add_descriptor_info(vk::DescriptorType _descriptor_type, const std::vector<DescriptorBufferOrImageInfo>& _pool_info) noexcept;
	void clear_descriptor_info() noexcept;
	void update_descriptor_sets(const vk::raii::Device& _device, uint32_t _max_size_count, const vk::raii::DescriptorSetLayout& _descriptor_set_layout) noexcept;

	const vk::raii::DescriptorPool& get_descriptor_pool() const noexcept;
	const std::vector<vk::raii::DescriptorSet>& get_descriptor_sets() const noexcept;

private:
	std::vector<std::vector<DescriptorBufferOrImageInfo>> pool_info;
	std::vector<vk::DescriptorPoolSize> pool_size;
	vk::raii::DescriptorPool descriptor_pool = nullptr;
	std::vector<vk::raii::DescriptorSet> descriptor_sets;
};