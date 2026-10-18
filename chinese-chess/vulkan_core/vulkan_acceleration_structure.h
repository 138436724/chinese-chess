/**
 * @file vulkan_acceleration_structure.h
 * @brief BLAS/TLAS 的 RAII 封装：负责加速结构、backing buffer 与构建命令的录制。
 * @details 类内持有加速结构句柄、它的 backing buffer、设备地址与最近一次的几何描述，构建用的
 *          scratch buffer 由 create_* 以返回值交给调用方，因此 [[nodiscard]] 的返回值必须接住
 *          并保持到该命令缓冲提交完成。BLAS 顶点格式固定为 vulkan_model_vertex（位置在前、
 *          步长 sizeof(vulkan_model_vertex)），TLAS 建时带 eAllowUpdate 以便后续 refit。
 *          所有构建命令都只录制、不提交，同步与资源回收由调用方负责。
 */

#pragma once

#include "vulkan_buffer.h"

#include <cstdint>
#include <vulkan/vulkan_raii.hpp>

/**
 * @brief 底层/顶层加速结构（BLAS/TLAS）的构造与 refit，供 RT 管线与 Ray Query 共用。
 * @details geometry 与 range_info 是「当前这次构建」的输入快照，build 与 refit 都会覆盖
 *          它们，所以它们不是可跨调用复用的几何缓存。类不等待 GPU、也不设对象名：
 *          scratch 以返回值交给调用方，由其按 submission_token 推迟销毁；backing buffer
 *          声明在加速结构句柄之前，析构时句柄先释放、buffer 随后才归还内存。build 尺寸
 *          在每次创建时重新查询，设备地址在创建后立即缓存，refit 不再查任何属性。
 */
class vulkan_acceleration_structure
{
public:
    /**
     * @brief 构造空对象：句柄为 nullptr、地址为 0，实际创建走两个 create_* 接口。
     */
    vulkan_acceleration_structure()                                                = default;
    /**
     * @brief 默认析构：不等待 GPU，销毁顺序由持有者的成员声明序保证。
     */
    ~vulkan_acceleration_structure()                                               = default;
    /**
     * @brief 禁止拷贝：加速结构句柄与 backing buffer 有唯一属主。
     */
    vulkan_acceleration_structure(const vulkan_acceleration_structure&)            = delete;
    /**
     * @brief 禁止拷贝赋值：所有权只能经移动转移。
     */
    vulkan_acceleration_structure& operator=(const vulkan_acceleration_structure&) = delete;
    /**
     * @brief 移动构造：逐成员 std::exchange 接管源对象，源对象留下 nullptr/0 的空状态。
     * @param _other 被移动的源对象，构造后其句柄与地址都为空。
     */
    vulkan_acceleration_structure(vulkan_acceleration_structure&& _other) noexcept;
    /**
     * @brief 移动赋值：自赋值直接返回，否则逐成员 std::ranges::swap，两边都能安全析构。
     * @param _other 被交换的源对象，交换后两边各自持有对方原来的资源。
     * @return *this。
     */
    vulkan_acceleration_structure& operator=(vulkan_acceleration_structure&& _other) noexcept;

    /**
     * @brief 建 BLAS：把 _vertex_data/_index_data 包成 eTriangles 几何后转交私有创建流程。
     * @param _physical_device 用于首次查询加速结构的 scratch 偏移对齐。
     * @param _device 创建设备侧的加速结构、缓冲并录制构建命令。
     * @param _allocator VMA 分配器，backing buffer 与 scratch 都由它分配。
     * @param _commandbuffer 构建命令的录制目标，本函数不提交它。
     * @param _vertex_count 顶点数，按 sizeof(vulkan_model_vertex) 定步长。
     * @param _vertex_data 顶点缓冲地址；位置必须在前 12 字节，格式固定 eR32G32B32Sfloat。
     * @param _index_count 索引总数，图元数量取其除以 3 的商。
     * @param _index_data 索引缓冲地址，索引类型固定 eUint32。
     * @param _queue backing buffer 与 scratch 使用的独占 queue family。
     * @return 构建用 scratch buffer；调用方负责保持到该命令缓冲提交完成，不能立即释放。
     * @throws std::runtime_error 内部 buffer.create 只接受 Exclusive 共享模式，其它情况抛出。
     * @throws vk::SystemError Vulkan 创建或 build sizes 查询失败时由 Vulkan-Hpp 抛出。
     * @note 只录制构建命令，不提交；几何标志为 eNoDuplicateAnyHitInvocation。
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
     * @brief 建 TLAS：把实例数组地址包成 eInstances 几何，并带 eAllowUpdate 供后续 refit。
     * @param _physical_device 用于首次查询 scratch 偏移对齐。
     * @param _device 创建加速结构、缓冲并录制构建命令。
     * @param _allocator VMA 分配器，负责 backing buffer 与 scratch。
     * @param _commandbuffer 构建命令的录制目标，本函数不提交它。
     * @param _instances_size 实例数量（VkAccelerationStructureInstanceKHR 的个数），空场景传 0。
     * @param _instances_data 实例缓冲的设备地址。
     * @param _queue backing buffer 与 scratch 使用的独占 queue family。
     * @return 构建用 scratch buffer；同一块在 refit 时仍可用（尺寸按 build/update 取 max）。
     * @throws std::runtime_error 内部 buffer.create 只接受 Exclusive 共享模式，其它情况抛出。
     * @throws vk::SystemError Vulkan 创建或 build sizes 查询失败时由 Vulkan-Hpp 抛出。
     * @note 本函数不保证构建已经执行，命令只是录进了 _commandbuffer。
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
     * @param _commandbuffer refit 命令的录制目标，本函数不提交它。
     * @param _instances_size 新的实例数量。
     * @param _instances_data 新的实例缓冲设备地址。
     * @param _scratch_address refit 用 scratch 的设备地址，必须来自创建时按 build/update 取 max 的那块。
     * @note 会覆盖成员 geometry 与 range_info，因此调用后不能再依赖上一次构建的描述。
     * @note 结构创建时必须带 eAllowUpdate，否则 update 模式在验证层会报错。
     */
    void update_top_level_acceleration_structure(const vk::raii::CommandBuffer&  _commandbuffer,
                                                 uint32_t                        _instances_size,
                                                 vk::DeviceOrHostAddressConstKHR _instances_data,
                                                 vk::DeviceAddress               _scratch_address);

    /**
     * @brief 取加速结构句柄。
     * @return vk::raii::AccelerationStructureKHR 的常量引用；未创建时内部持有 nullptr。
     */
    [[nodiscard]] const vk::raii::AccelerationStructureKHR& get_acceleration_structure() const noexcept;
    /**
     * @brief 取加速结构的设备地址。
     * @return 创建时由 vkGetAccelerationStructureDeviceAddressKHR 缓存的地址，未创建时为 0。
     */
    [[nodiscard]] vk::DeviceAddress                         get_address() const noexcept;
    /**
     * @brief 取加速结构的 backing buffer。
     * @return 其内部 vulkan_buffer 持有的 vk::raii::Buffer 常量引用，未创建时为空句柄。
     * @note 返回的引用与内部 vulkan_buffer 同寿命，不要把它存到缓冲重建之后。
     */
    [[nodiscard]] const vk::raii::Buffer&                   get_buffer() const noexcept;

private:
    /**
     * @brief 完整创建流程：懒查 scratch 对齐 → 查 build 尺寸 → 建 backing buffer 与结构 → 取地址 → 建 scratch → 录 build。
     * @param _physical_device 首次调用时用来缓存 scratch 偏移对齐。
     * @param _device 创建加速结构与缓冲并录制构建命令。
     * @param _allocator VMA 分配器，backing buffer 与 scratch 都由它分配。
     * @param _commandbuffer 构建命令的录制目标，本函数不提交它。
     * @param _type 加速结构类型，同时决定 buffer 的 debug 名字。
     * @param _flags 构建标志；含 eAllowUpdate 时 scratch 尺寸取 build 与 update 的较大者。
     * @param _queue backing buffer 与 scratch 的独占 queue family。
     * @return scratch buffer，所有权移交给调用方。
     * @throws std::runtime_error buffer.create 拒绝非 Exclusive 共享模式。
     * @throws vk::SystemError Vulkan 调用失败时由 Vulkan-Hpp 抛出。
     * @note 调用前必须已填好 geometry 与 range_info，build sizes 查询依赖这两个成员。
     */
    [[nodiscard("Staging buffer must be kept!")]] vulkan_buffer create_acceleration_structure(
        const vk::raii::PhysicalDevice&        _physical_device,
        const vk::raii::Device&                _device,
        const vma::raii::Allocator&            _allocator,
        const vk::raii::CommandBuffer&         _commandbuffer,
        vk::AccelerationStructureTypeKHR       _type,
        vk::BuildAccelerationStructureFlagsKHR _flags,
        uint32_t                               _queue);

    /** @brief 从 PhysicalDeviceAccelerationStructurePropertiesKHR 懒取得的 scratch 偏移对齐；0 表示尚未查询。 */
    uint32_t scratch_alignment = 0;

    /** @brief 当前这次构建/refit 的几何描述（BLAS 是三角形、TLAS 是实例数组）。 */
    vk::AccelerationStructureGeometryKHR       geometry;
    /** @brief 与 geometry 配套的图元数量（三角形数或实例数），build sizes 与录制都用它。 */
    vk::AccelerationStructureBuildRangeInfoKHR range_info;

    /** @brief 加速结构自己的 backing buffer，声明在 acceleration_structure 之前，因此比句柄活得久。 */
    vulkan_buffer                      buffer;
    /** @brief 加速结构句柄本身，未创建时为 nullptr。 */
    vk::raii::AccelerationStructureKHR acceleration_structure = nullptr;
    /** @brief 加速结构的设备地址，供 shader 或 TLAS 实例引用；0 表示未创建。 */
    vk::DeviceAddress                  address                = 0;
};
