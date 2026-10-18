/**
 * @file vulkan_descriptor.cpp
 * @brief vulkan_descriptor 的实现：移动语义、声明累积与一次性建池 / 分配 / 写入。
 * @details 移动构造用 std::exchange 搬空源对象（含 vector 与 RAII 池句柄），移动赋值用 std::ranges::swap。
 *          update_descriptor_sets 每次重建池并重新分配 set，再对每个 set 用 views::zip 组装 WriteDescriptorSet；
 *          图像与缓冲用 get_if 二选一，另一个传 nullptr，因此声明阶段的类型必须与写入时的成员对应。
 */

#include "vulkan_descriptor.h"

#include <algorithm>
#include <concepts>
#include <cstdint>
#include <ranges>
#include <stdexcept>
#include <utility>
#include <vector>

/** @brief 移动构造：五个成员一律 std::exchange 走，源对象退化成空描述符声明。 */
vulkan_descriptor::vulkan_descriptor(vulkan_descriptor&& _other) noexcept
    : max_size(std::exchange(_other.max_size, {}))
    , pool_infos(std::exchange(_other.pool_infos, {}))
    , pool_size(std::exchange(_other.pool_size, {}))
    , descriptor_pool(std::exchange(_other.descriptor_pool, nullptr))
    , descriptor_sets(std::exchange(_other.descriptor_sets, {}))
{
}

/** @brief 移动赋值：逐成员 std::ranges::swap，旧池与旧集被换给源对象并随其析构释放。 */
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

/**
 * @brief 记录一个 binding 的池大小与每 set 写入信息；首次调用确定 set 数，之后数量必须相等。
 * @note 顺序即 binding 号：pool_size 与 pool_infos 两个 vector 靠下标同步，写入时用 views::zip 配对。
 */
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

    pool_size.emplace_back(_descriptor_type, static_cast<uint32_t>(_pool_info.size()));
    pool_infos.emplace_back(std::move(_pool_info));
}

/**
 * @brief 清空声明并释放池 / 集；noexcept ⇒ 只做容器 clear 与 RAII 句柄的 clear()。
 * @note 句柄立刻销毁，所以调用点必须先确认 GPU 不再引用这些集（wait_idle 或帧已排空）。
 */
void vulkan_descriptor::clear_descriptor_info() noexcept
{
    max_size = 0;
    pool_size.clear();
    pool_infos.clear();
    descriptor_sets.clear();
    descriptor_pool.clear();
}

/**
 * @brief 重建池与 set，再逐 set、逐 binding 写描述符；池带 eFreeDescriptorSet，允许单独释放 set。
 * @note 旧集先 clear()（各自用记住的池做 vkFreeDescriptorSets），旧池在新池赋值时才销毁，释放顺序安全。
 *       每个 set 的写入用 views::zip(pool_size, pool_infos) 配对，binding 号取自 enumerate 的 j，
 *       写入信息取自 pool_info.at(i)（i 是 set 号）；两个数组等长由 add_descriptor_info 的不变量保证。
 */
void vulkan_descriptor::update_descriptor_sets(const vk::raii::Device& _device, const vk::raii::DescriptorSetLayout& _descriptor_set_layout)
{
    descriptor_sets.clear();

    // descriptor pool
    const vk::DescriptorPoolCreateInfo pool_create_info(vk::DescriptorPoolCreateFlagBits::eFreeDescriptorSet, max_size, pool_size);
    descriptor_pool = vk::raii::DescriptorPool(_device, pool_create_info);


    // descriptor set
    const std::vector<vk::DescriptorSetLayout> layouts(max_size, *(_descriptor_set_layout));
    const vk::DescriptorSetAllocateInfo        alloc_info(descriptor_pool, layouts);

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

/** @brief 返回当前池句柄；update_descriptor_sets 之前为空。 */
const vk::raii::DescriptorPool& vulkan_descriptor::get_descriptor_pool() const noexcept
{
    return descriptor_pool;
}

/** @brief 返回当前 set 数组成员的引用；下标是 set 号，update / clear 会使旧元素引用失效。 */
const std::vector<vk::raii::DescriptorSet>& vulkan_descriptor::get_descriptor_sets() const noexcept
{
    return descriptor_sets;
}
