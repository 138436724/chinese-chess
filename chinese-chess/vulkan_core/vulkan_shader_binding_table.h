/**
 * @file vulkan_shader_binding_table.h
 * @brief 光追管线 SBT 的构建与持有：一张 eShaderBindingTableKHR 设备缓冲 + 四个 strided region。
 * @details create 从管线取 shader group handle，按 shaderGroupHandleSize / HandleAlignment / BaseAlignment
 *          把 5 个组（raygen、两个 miss、两个 hit）排进同一张缓冲，并填好 raygen / miss / hit / callable region。
 *          句柄先写 host 可见 staging 再拷到设备缓冲，所以 staging 必须在拷贝提交完成前保持存活。
 *          本类不提交、不同步：录制与提交由调用方完成（见 scene_raytracing_render::create_pipeline_and_sbt）。
 */

#pragma once

#include "vulkan_buffer.h"

#include <cstdint>
#include <vulkan/vulkan_raii.hpp>

/**
 * @brief SBT 缓冲与四个 vk::StridedDeviceAddressRegionKHR 的拥有者。
 * @details 只支持本项目固定的 5 组布局（raygen / miss_primary / miss_shadow / hit_primary / hit_shadow）；
 *          region 里存的是设备地址，因此缓冲被释放或重建后必须重新 create。
 *          移动后源对象被置空（句柄与 region 全部换走），旧 region 里的地址随之失效。
 */
class vulkan_shader_binding_table
{
public:
    /** @brief 空对象：对齐值为 0、四个 region 全 0、SBT 缓冲为空。 */
    vulkan_shader_binding_table()                                              = default;
    /** @brief 析构：由 vk::raii 释放 SBT 缓冲；须在 GPU 与所有在飞帧都不再 traceRays 之后发生。 */
    ~vulkan_shader_binding_table()                                             = default;
    /** @brief 禁用拷贝：缓冲与设备地址都是独占资源。 */
    vulkan_shader_binding_table(const vulkan_shader_binding_table&)            = delete;
    /** @brief 禁用拷贝赋值：同上。 */
    vulkan_shader_binding_table& operator=(const vulkan_shader_binding_table&) = delete;
    /** @brief 移动构造：对齐值、四个 region 与缓冲一律 std::exchange 搬走，源对象被置空。 */
    vulkan_shader_binding_table(vulkan_shader_binding_table&& _other) noexcept;
    /** @brief 移动赋值：逐成员 std::ranges::swap，旧缓冲被换给源对象并随其析构释放。 */
    vulkan_shader_binding_table& operator=(vulkan_shader_binding_table&& _other) noexcept;

    /**
     * @brief 为给定光追管线构建 SBT：查对齐、算布局、写 staging、录拷贝、填四个 region。
     * @param _physical_device 首次调用时用于查询 shaderGroupHandleSize / HandleAlignment / BaseAlignment。
     * @param _device 创建缓冲、映射 staging 与取组句柄的逻辑设备。
     * @param _allocator VMA 分配器。
     * @param _commandbuffer 已 begin_record 的命令缓冲：本函数只录一条 copy，不 end 也不 submit。
     * @param _pipeline 刚建好的光追管线，组句柄从它取。
     * @param _group_count 着色器组个数；>= 5 时 miss / hit region 各 2 项，否则各 1 项。
     * @param _queue 缓冲的 queue family index，必须与录制 / 提交该命令缓冲的队列一致。
     * @return staging buffer（transfer src + host 可见）；调用方必须保留到拷贝提交完成，否则设备会读已释放内存。
     * @throws vk::SystemError 缓冲创建、staging 映射或组句柄查询失败时由 vk::raii 抛出。
     * @note 三个对齐值在首次成功调用后缓存，后续 create 不再查设备属性（同一对象的第二次 create 会复用旧对齐）。
     *       region 的 stride 用 base_alignment 对齐后的组间距而不是 handle_alignment，两者不可混用。
     */
    [[nodiscard("Staging buffer must be kept!")]]
    vulkan_buffer create(const vk::raii::PhysicalDevice& _physical_device,
                         const vk::raii::Device&         _device,
                         const vma::raii::Allocator&     _allocator,
                         const vk::raii::CommandBuffer&  _commandbuffer,
                         const vk::raii::Pipeline&       _pipeline,
                         uint32_t                        _group_count,
                         uint32_t                        _queue);

    /**
     * @brief raygen region：起点 0，1 项。
     * @return region 的引用（设备地址 + stride + size）；create 之前全 0。
     */
    [[nodiscard]] const vk::StridedDeviceAddressRegionKHR& get_raygen_region() const noexcept;
    /**
     * @brief miss region：从 miss_primary 起，2 项（主 miss + 阴影 miss）或 1 项。
     * @return region 的引用；stride 是两组之间的实际间距。
     */
    [[nodiscard]] const vk::StridedDeviceAddressRegionKHR& get_miss_region() const noexcept;
    /**
     * @brief hit region：从 hit_primary 起，2 项（closest hit + shadow any-hit）或 1 项。
     * @return region 的引用；stride 是两组之间的实际间距。
     */
    [[nodiscard]] const vk::StridedDeviceAddressRegionKHR& get_hit_region() const noexcept;
    /**
     * @brief callable region：本项目不注册 callable 着色器，所以 create 后仍是全 0。
     * @return 全 0 的 region 引用，可原样传给 traceRays。
     */
    [[nodiscard]] const vk::StridedDeviceAddressRegionKHR& get_callable_region() const noexcept;
    /**
     * @brief SBT 缓冲本身，供调试或自行补 barrier / 记录地址时使用。
     * @return 缓冲引用；未 create 时是空缓冲，create 之后是设备本地缓冲。
     */
    [[nodiscard]] const vulkan_buffer&                     get_shader_binding_table_buffer() const noexcept;

private:
    /** @brief 一个 shader group handle 的字节数（shaderGroupHandleSize）。 */
    uint32_t handle_size      = 0;
    /** @brief handle 在 SBT 内的最小对齐（shaderGroupHandleAlignment），用于算单个 entry 的跨度。 */
    uint32_t handle_alignment = 0;
    /** @brief 每个组的起点对齐（shaderGroupBaseAlignment），所有组偏移按它向上取整。 */
    uint32_t base_alignment   = 0;

    /** @brief raygen region：1 项，stride = 该 entry 对齐后的大小。 */
    vk::StridedDeviceAddressRegionKHR raygen_region;
    /** @brief miss region：从主 miss 起，stride = base_alignment 对齐后的组间距。 */
    vk::StridedDeviceAddressRegionKHR miss_region;
    /** @brief hit region：从主 hit 起，stride = base_alignment 对齐后的组间距。 */
    vk::StridedDeviceAddressRegionKHR hit_region;
    /** @brief callable region：保持全 0（无 callable 着色器）。 */
    vk::StridedDeviceAddressRegionKHR callable_region;

    /** @brief SBT 设备缓冲：eShaderBindingTableKHR + transfer dst + device address，句柄由 staging 拷入。 */
    vulkan_buffer sbt_buffer;
};
