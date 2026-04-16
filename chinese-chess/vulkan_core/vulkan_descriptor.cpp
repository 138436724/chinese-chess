#include "vulkan_descriptor.h"
#include <algorithm>
#include <ranges>

vulkan_descriptor::vulkan_descriptor(vulkan_descriptor&& _other) noexcept
	:pool_info(std::move(_other.pool_info)),
	pool_size(std::move(_other.pool_size)),
	descriptor_pool(std::move(_other.descriptor_pool)),
	descriptor_sets(std::move(_other.descriptor_sets))
{
}

vulkan_descriptor& vulkan_descriptor::operator=(vulkan_descriptor&& _other) noexcept
{
	if (this != &_other)
	{
		std::swap(pool_info, _other.pool_info);
		std::swap(pool_size, _other.pool_size);
		std::swap(descriptor_pool, _other.descriptor_pool);
		std::swap(descriptor_sets, _other.descriptor_sets);
	}
	return *this;
}

void vulkan_descriptor::add_descriptor_info(vk::DescriptorType _descriptor_type, const std::vector<DescriptorBufferOrImageInfo>& _pool_info) noexcept
{
	pool_size.emplace_back(vk::DescriptorPoolSize(_descriptor_type, static_cast<uint32_t>(_pool_info.size())));
	pool_info.push_back(_pool_info);
}

void vulkan_descriptor::clear_descriptor_info() noexcept
{
	pool_size.clear();
	pool_info.clear();
	descriptor_sets.clear();
	descriptor_pool.clear();
}

void vulkan_descriptor::update_descriptor_sets(const vk::raii::Device& _device, uint32_t _max_size_count, const vk::raii::DescriptorSetLayout& _descriptor_set_layout) noexcept
{
	// descriptor pool
	vk::DescriptorPoolCreateInfo pool_create_info(vk::DescriptorPoolCreateFlagBits::eFreeDescriptorSet, _max_size_count, pool_size);
	descriptor_pool = vk::raii::DescriptorPool(_device, pool_create_info);


	// descriptor set
	std::vector<vk::DescriptorSetLayout> layouts(_max_size_count, *(_descriptor_set_layout));
	vk::DescriptorSetAllocateInfo alloc_info(descriptor_pool, layouts);

	descriptor_sets.clear();
	descriptor_sets = _device.allocateDescriptorSets(alloc_info);


	std::ranges::for_each(descriptor_sets | std::views::enumerate, [&](const auto& _descriptor_pair) {
		auto descriptor_write = std::views::zip(pool_size, pool_info)
			| std::views::enumerate
			| std::views::transform([&](const auto& _pair)
				{
					const auto& [i, _descriptor_set] = _descriptor_pair;
					const auto& [j, pool_zip] = _pair;
					const auto& [_pool_size, _pool_info] = pool_zip;
					return vk::WriteDescriptorSet(_descriptor_set, static_cast<uint32_t>(j), 0, 1, _pool_size.type, std::get_if<vk::DescriptorImageInfo>(&_pool_info.at(i)), std::get_if<vk::DescriptorBufferInfo>(&_pool_info.at(i)), nullptr);
				})
			| std::ranges::to<std::vector>();

		_device.updateDescriptorSets(descriptor_write, {});
		});
}

const vk::raii::DescriptorPool& vulkan_descriptor::get_descriptor_pool() const noexcept
{
	return descriptor_pool;
}

const std::vector<vk::raii::DescriptorSet>& vulkan_descriptor::get_descriptor_sets() const noexcept
{
	return descriptor_sets;
}
