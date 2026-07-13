#include "vulkan_descriptor.h"

#include <algorithm>
#include <ranges>

vulkan_descriptor::vulkan_descriptor(vulkan_descriptor&& _other) noexcept
    : max_size(std::exchange(_other.max_size, {}))
    , pool_infos(std::exchange(_other.pool_infos, {}))
    , pool_size(std::exchange(_other.pool_size, {}))
    , descriptor_pool(std::exchange(_other.descriptor_pool, nullptr))
    , descriptor_sets(std::exchange(_other.descriptor_sets, {}))
{
}

vulkan_descriptor& vulkan_descriptor::operator=(vulkan_descriptor&& _other) noexcept
{
    if (this != &_other)
    {
        std::ranges::swap(max_size, _other.max_size);
        std::ranges::swap(pool_infos, _other.pool_infos);
        std::ranges::swap(pool_size, _other.pool_size);
        std::ranges::swap(descriptor_pool, _other.descriptor_pool);
        std::ranges::swap(descriptor_sets, _other.descriptor_sets);
    }
    return *this;
}

void vulkan_descriptor::add_descriptor_info(vk::DescriptorType _descriptor_type, std::vector<DescriptorBufferOrImageInfo>&& _pool_info)
{
    if (max_size == 0)
    {
        max_size = static_cast<uint32_t>(_pool_info.size());
    }
    else if (max_size != _pool_info.size())
    {
        throw std::runtime_error("All pool info size must be same.");
    }

    pool_size.emplace_back(vk::DescriptorPoolSize(_descriptor_type, static_cast<uint32_t>(_pool_info.size())));
    pool_infos.emplace_back(std::move(_pool_info));
}

void vulkan_descriptor::clear_descriptor_info() noexcept
{
    max_size = 0;
    pool_size.clear();
    pool_infos.clear();
    descriptor_sets.clear();
    descriptor_pool.clear();
}

void vulkan_descriptor::update_descriptor_sets(const vk::raii::Device& _device, const vk::raii::DescriptorSetLayout& _descriptor_set_layout)
{
    // descriptor pool
    const vk::DescriptorPoolCreateInfo pool_create_info(vk::DescriptorPoolCreateFlagBits::eFreeDescriptorSet, max_size, pool_size);
    descriptor_pool = vk::raii::DescriptorPool(_device, pool_create_info);


    // descriptor set
    const std::vector<vk::DescriptorSetLayout> layouts(max_size, *(_descriptor_set_layout));
    const vk::DescriptorSetAllocateInfo        alloc_info(descriptor_pool, layouts);

    descriptor_sets.clear();
    descriptor_sets = _device.allocateDescriptorSets(alloc_info);


    std::ranges::for_each(descriptor_sets | std::views::enumerate, [&](const auto& _descriptor_pair) {
        const auto descriptor_write =
            std::views::zip(pool_size, pool_infos) | std::views::enumerate | std::views::transform([&](const auto& _pair) {
                const auto& [i, _descriptor_set]     = _descriptor_pair;
                const auto& [j, pool_zip]            = _pair;
                const auto& [_pool_size, _pool_info] = pool_zip;
                return vk::WriteDescriptorSet(_descriptor_set, static_cast<uint32_t>(j), 0, 1, _pool_size.type,
                                              std::get_if<vk::DescriptorImageInfo>(&_pool_info.at(i)),
                                              std::get_if<vk::DescriptorBufferInfo>(&_pool_info.at(i)), nullptr);
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
