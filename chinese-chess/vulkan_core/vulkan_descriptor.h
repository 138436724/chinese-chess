/**
 * @file vulkan_descriptor.h
 * @brief 累加式描述符声明容器：按类型收集池大小与每个 set 的写入信息，再由 update_descriptor_sets 一次建池、分配集、写入。
 * @details add_descriptor_info 的调用顺序就是 binding 号（从 0 起），一次调用里的 vector 下标就是 set 号，
 *          所以每次传入的 info 数量必须相同（= set 个数 = MAX_FRAMES_IN_FLIGHT）。
 *          池与集在 update_descriptor_sets 里整体重建，旧集先清空；调用方须保证旧集已不在飞帧使用。
 *          当前唯一使用点是 vulkan_application 的屏幕合成描述符（ImGui）；三个渲染器都自己建池与集，不走这里。
 */

#pragma once

#include <cstdint>
#include <variant>
#include <vector>
#include <vulkan/vulkan_raii.hpp>

/**
 * @brief 单个描述符的写入信息：缓冲用 vk::DescriptorBufferInfo，图像 / 采样器用 vk::DescriptorImageInfo。
 * @note 写入时用 std::get_if 二选一，因此每个元素必须持有与 DescriptorType 匹配的那个类型，另一个保持空。
 */
using DescriptorBufferOrImageInfo = std::variant<vk::DescriptorBufferInfo, vk::DescriptorImageInfo>;

/**
 * @brief 描述符池 + 描述符集 + 待写入信息的拥有者（声明的累加与提交分离）。
 * @details 先 add_descriptor_info 若干次（每次一个 binding），再 update_descriptor_sets 一次性建池并写入所有集；
 *          clear_descriptor_info 清空声明与句柄，供 resize 等场景重新声明。
 *          不是线程安全的，也不做 GPU 同步 —— 重建或销毁前必须保证旧集已不被在飞帧引用。
 */
class vulkan_descriptor
{
public:
    /** @brief 空对象：没有任何声明，也没有池与集。 */
    vulkan_descriptor()                                    = default;
    /** @brief 析构：由 vk::raii 释放池与集；须在设备空闲或帧已排空之后发生。 */
    ~vulkan_descriptor()                                   = default;
    /** @brief 禁用拷贝：池与集是独占资源。 */
    vulkan_descriptor(const vulkan_descriptor&)            = delete;
    /** @brief 禁用拷贝赋值：同上。 */
    vulkan_descriptor& operator=(const vulkan_descriptor&) = delete;
    /** @brief 移动构造：五个成员一律 std::exchange 搬走，源对象变成空描述符声明。 */
    vulkan_descriptor(vulkan_descriptor&& _other) noexcept;
    /** @brief 移动赋值：逐成员 std::ranges::swap，旧池与旧集被换到源对象、由它的析构释放。 */
    vulkan_descriptor& operator=(vulkan_descriptor&& _other) noexcept;

    /**
     * @brief 追加一个 binding 的声明：描述符类型 + 每个 set 一份的写入信息，并记下它的池大小。
     * @param _descriptor_type 该 binding 的描述符类型（采样图 / 采样器 / UBO 等）。
     * @param _pool_info 每个 descriptor set 一份的写入信息；size() 即 set 个数，首次调用时确定 max_size。
     * @throws std::runtime_error 本次 size() 与首次调用不一致时（所有 binding 的 set 数必须相同）。
     * @note 调用顺序即 binding 号；vector 下标即 set 号。池大小里的 count 是 set 个数 —— 每个 set 在该
     *       binding 上消费一个描述符。
     */
    void add_descriptor_info(vk::DescriptorType _descriptor_type, std::vector<DescriptorBufferOrImageInfo>&& _pool_info);
    /**
     * @brief 清空累积的声明，并释放描述符集与池（清空 RAII 句柄）。
     * @note 直接销毁池 ⇒ 调用前必须保证 GPU 与所有在飞帧都不再引用这些集；~vulkan_application() 先
     *       wait_idle_for_cleanup() 再调用它，bind_image() 在重建前也会先调用。
     */
    void clear_descriptor_info() noexcept;
    /**
     * @brief 重建描述符池、分配 max_size 个 set，并把所有 binding 逐个写入每个 set。
     * @param _device 建池、分配集与 updateDescriptorSets 用的逻辑设备。
     * @param _descriptor_set_layout 集布局：它的 binding 必须与 add_descriptor_info 的调用一一对应。
     * @throws vk::SystemError 池创建或集分配失败（布局与声明不匹配时通常在这里暴露）。
     * @throws std::out_of_range 写入时 pool_info.at(i) 越界，只在声明数量与 set 数不一致（不变量被破坏）时发生。
     * @note 旧集先 clear()（每个 set 用自己记住的池做 vkFreeDescriptorSets），旧池随后在新池赋值时销毁，
     *       所以释放顺序是安全的。每个 WriteDescriptorSet 的 descriptorCount 固定 1、dstArrayElement 固定 0
     *       ⇒ 假定一个 binding 在单个 set 内只有一个描述符，bindless 数组不走这条路径。
     */
    void update_descriptor_sets(const vk::raii::Device& _device, const vk::raii::DescriptorSetLayout& _descriptor_set_layout);

    /**
     * @brief 当前描述符池，供调试或自行分配集时使用。
     * @return 池句柄的引用；update_descriptor_sets 之前为空，下一次 update / clear 后失效。
     */
    [[nodiscard]] const vk::raii::DescriptorPool&             get_descriptor_pool() const noexcept;
    /**
     * @brief 当前描述符集数组，下标即 set 号（与帧 slot 对应）。
     * @return 集数组的引用；update_descriptor_sets 之前为空，下一次 update / clear 后失效。
     */
    [[nodiscard]] const std::vector<vk::raii::DescriptorSet>& get_descriptor_sets() const noexcept;

private:
    /** @brief set 个数 = 首次 add 时的 info 数量，同时作为池的 maxSets；也是每个 binding 的描述符个数。 */
    uint32_t                                              max_size = 0;
    /** @brief 按 binding 号排列的写入信息：外层下标是 binding，内层下标是 set。 */
    std::vector<std::vector<DescriptorBufferOrImageInfo>> pool_infos;
    /** @brief 按 binding 号排列的池大小（描述符类型 + 数量 = set 个数），直接交给池创建信息。 */
    std::vector<vk::DescriptorPoolSize>                   pool_size;
    /** @brief 最近一次 update 建的池；update 会整体替换，旧池随之销毁。 */
    vk::raii::DescriptorPool                              descriptor_pool = nullptr;
    /** @brief 最近一次 update 分配的 set 数组，供合成管线逐帧绑定。 */
    std::vector<vk::raii::DescriptorSet>                  descriptor_sets;
};
