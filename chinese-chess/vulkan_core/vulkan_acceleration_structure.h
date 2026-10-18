/**
 * @file vulkan_acceleration_structure.h
 * @brief BLAS/TLAS 的 RAII 封装。负责加速结构和 backing buffer 的创建与构建命令的录制。
 */

#pragma once

#include "vulkan_buffer.h"

#include <cstdint>
#include <vulkan/vulkan_raii.hpp>

/**
 * @brief 底层/顶层加速结构（BLAS/TLAS）的构造与 refit，供光线追踪使用。
 */
class vulkan_acceleration_structure
{
public:
    vulkan_acceleration_structure()  = default;  ///< @brief 默认构造函数，实际数据需要使用 create_* 创建。
    ~vulkan_acceleration_structure() = default;  ///< @brief 默认析构函数，不等待GPU，立刻销毁数据。
    vulkan_acceleration_structure(const vulkan_acceleration_structure&)            = delete;    ///< @brief 禁止拷贝构造。
    vulkan_acceleration_structure& operator=(const vulkan_acceleration_structure&) = delete;    ///< @brief 禁止拷贝赋值。
    vulkan_acceleration_structure(vulkan_acceleration_structure&& _other) noexcept;             ///< @brief 移动构造函数。
    vulkan_acceleration_structure& operator=(vulkan_acceleration_structure&& _other) noexcept;  ///< @brief 移动赋值函数。

    /**
     * @brief 构建 BLAS。将 _vertex_data/_index_data 打包成 eTriangles 几何。
     * @param _physical_device 用于首次调用时查询加速结构的 scratch 偏移对齐。
     * @param _device 创建 AS、buffer 等数据。
     * @param _allocator VMA 分配器，分配 backing buffer 与 scratch buffer。
     * @param _commandbuffer 构建 BLAS 命令的录制目标，本函数仅录制不提交。
     * @param _vertex_count 顶点数量。
     * @param _vertex_data 顶点数据的地址。类型固定为 vulkan_model_vertex。
     * @param _index_count 索引数量。图元数量 = _index_count / 3。
     * @param _index_data 索引数据的地址，类型固定为 eUint32。
     * @param _queue backing buffer 与 scratch buffer 使用的 queue family。
     * @return 构建时用到的 scratch buffer。需持有到该命令缓冲提交完成，不能立即释放。
     * @throws vk::SystemError 由 vulkan 抛出。
     * @warning Vulkan 的 maxVertex 表示顶点数量减一，不能把当前直接透传的实现当成已完成该转换。
     * @note 几何标志固定为 eNoDuplicateAnyHitInvocation。
     */
    [[nodiscard("Staging buffer must be kept!")]] vulkan_buffer create_bottom_level_acceleration_structure(
        const vk::raii::PhysicalDevice& _physical_device,
        const vk::raii::Device&         _device,
        const vma::raii::Allocator&     _allocator,
        const vk::raii::CommandBuffer&  _commandbuffer,
        uint32_t                        _vertex_count,
        vk::DeviceOrHostAddressConstKHR _vertex_data,
        uint32_t                        _index_count,
        vk::DeviceOrHostAddressConstKHR _index_data,
        uint32_t                        _queue);

    /**
     * @brief 构建 TLAS。将实例数组地址打包成 eInstances 几何，默认携带 eAllowUpdate 供后续 refit。
     * @param _physical_device 用于首次调用时查询加速结构的 scratch 偏移对齐。
     * @param _device 创建 AS、buffer 等数据。
     * @param _allocator VMA 分配器，分配 backing buffer 与 scratch buffer。
     * @param _commandbuffer 构建 TLAS 命令的录制目标，本函数仅录制不提交。
     * @param _instances_size 实例数量，即 VkAccelerationStructureInstanceKHR 的个数，空场景传 0。
     * @param _instances_data 实例数据的地址。
     * @param _queue backing buffer 与 scratch buffer 使用的 queue family。
     * @return 构建时用到的 scratch buffer。需持有到该命令缓冲提交完成，不能立即释放。
     * @throws vk::SystemError 由 vulkan 抛出。
     */
    [[nodiscard("Staging buffer must be kept!")]] vulkan_buffer create_top_level_acceleration_structure(
        const vk::raii::PhysicalDevice& _physical_device,
        const vk::raii::Device&         _device,
        const vma::raii::Allocator&     _allocator,
        const vk::raii::CommandBuffer&  _commandbuffer,
        uint32_t                        _instances_size,
        vk::DeviceOrHostAddressConstKHR _instances_data,
        uint32_t                        _queue);

    /**
     * @brief 以 eUpdate 模式 refit 现有 TLAS，src 与 dst 是同一个加速结构句柄。
     * @param _commandbuffer refit 命令的录制目标，本函数仅录制不提交。
     * @param _instances_size 实例数量，必须与构建时的 primitiveCount 保持一致。数量变化必须重建 TLAS。
     * @param _instances_data 实例数据的地址。
     * @param _scratch_address refit 用到 scratch buffer 的设备地址，须满足 updateScratchSize、usage 与地址对齐要求。不必使用原缓冲。
     * @note 会覆盖成员变量 geometry 与 range_info，因此调用后不能再依赖上一次构建的描述。
     * @note 结构创建时就会默认携带 eAllowUpdate，保证当前函数可用。
     */
    void update_top_level_acceleration_structure(const vk::raii::CommandBuffer&  _commandbuffer,
                                                 uint32_t                        _instances_size,
                                                 vk::DeviceOrHostAddressConstKHR _instances_data,
                                                 vk::DeviceAddress               _scratch_address);

    /**
     * @brief 获取加速结构句柄。
     * @return 内部 vk::raii::AccelerationStructureKHR 的常量引用。未创建时默认持有 nullptr。
     */
    [[nodiscard]] const vk::raii::AccelerationStructureKHR& get_acceleration_structure() const noexcept;

    /**
     * @brief 获取加速结构的设备地址。
     * @return 创建时由 vkGetAccelerationStructureDeviceAddressKHR 记录的地址，非内部 buffer 的地址，未创建时为 0。
     */
    [[nodiscard]] vk::DeviceAddress get_address() const noexcept;

    /**
     * @brief 获取加速结构的 backing buffer。
     * @return 内部 vulkan_buffer 持有 vk::raii::Buffer 的常量引用，未创建时默认持有 nullptr。
     * @note 返回的引用与内部 vulkan_buffer 生命周期一致。
     */
    [[nodiscard]] const vk::raii::Buffer& get_buffer() const noexcept;

private:
    /**
     * @brief BLAS 和 TLAS 的共用创建流程。
     * @param _physical_device 用于首次调用时查询加速结构的 scratch 偏移对齐。
     * @param _device 创建 AS、buffer 等数据。
     * @param _allocator VMA 分配器。分配 backing buffer 与 scratch buffer。
     * @param _commandbuffer 构建 BLAS/TLAS 命令的录制目标。本函数仅录制不提交。
     * @param _type 加速结构类型。决定 debug 中 buffer 的名称。
     * @param _flags 构建标志。含 eAllowUpdate 时 scratch 尺寸取 build 与 update 的较大者。
     * @param _queue backing buffer 与 scratch buffer 使用的 queue family。
     * @return 构建时用到的 scratch buffer，所有权移交外层函数。
     * @throws vk::SystemError 由 vulkan 抛出。
     * @note 调用前必须已填好 geometry 与 range_info，build sizes 查询依赖这两个成员。
     * @warning 当前只向上对齐 scratch buffer 的分配大小，直接使用缓冲基地址；并未显式对齐或校验 scratchData 的设备地址。
     */
    [[nodiscard("Staging buffer must be kept!")]] vulkan_buffer create_acceleration_structure(
        const vk::raii::PhysicalDevice&        _physical_device,
        const vk::raii::Device&                _device,
        const vma::raii::Allocator&            _allocator,
        const vk::raii::CommandBuffer&         _commandbuffer,
        vk::AccelerationStructureTypeKHR       _type,
        vk::BuildAccelerationStructureFlagsKHR _flags,
        uint32_t                               _queue);

private:
    uint32_t scratch_alignment = 0;  ///< @brief 从 PhysicalDeviceAccelerationStructurePropertiesKHR 取得的 scratch 偏移对齐，0 表示尚未查询。

    vk::AccelerationStructureGeometryKHR geometry;  ///<@brief 当前这次构建/refit 的几何描述（BLAS 是三角形、TLAS 是实例数组）
    vk::AccelerationStructureBuildRangeInfoKHR range_info;  ///<@brief 与 geometry 配套的图元数量（三角形数或实例数），build sizes 与录制都会用到。

    vulkan_buffer                      buffer;                            ///<@brief 加速结构自己的 backing buffer。
    vk::raii::AccelerationStructureKHR acceleration_structure = nullptr;  ///<@brief 加速结构句柄，未创建时为 nullptr。
    vk::DeviceAddress address = 0;  ///<@brief 加速结构的设备地址，供 shader 或 TLAS 实例引用，0 表示未创建。
};
