/**
 * @file vulkan_acceleration_structure.cpp
 * @brief 加速结构创建、TLAS refit 与移动语义的实现。
 * @details 两个 create_* 只负责把几何描述填进成员，分配与命令录制都在私有
 *          create_acceleration_structure 里；scratch 偏移对齐在第一次构建时从
 *          PhysicalDeviceAccelerationStructurePropertiesKHR 懒查询并缓存，带 eAllowUpdate
 *          的结构把 scratch 尺寸取 build 与 update 的较大者。设备地址在创建后立即查询，
 *          refit 只重录构建命令、不再查任何属性。
 */

#include "vulkan_acceleration_structure.h"

#include "vulkan_common.h"
#include "vulkan_data_type.h"

#include <algorithm>
#include <concepts>
#include <cstdint>
#include <utility>

/**
 * @brief 逐成员 std::exchange：句柄与地址移到新对象，源对象留下 nullptr/0 的空状态。
 */
vulkan_acceleration_structure::vulkan_acceleration_structure(vulkan_acceleration_structure&& _other) noexcept
    : scratch_alignment(std::exchange(_other.scratch_alignment, {}))
    , geometry(std::exchange(_other.geometry, {}))
    , range_info(std::exchange(_other.range_info, {}))
    , buffer(std::exchange(_other.buffer, {}))
    , acceleration_structure(std::exchange(_other.acceleration_structure, nullptr))
    , address(std::exchange(_other.address, {}))
{
}

/**
 * @brief 自赋值提前返回，否则逐成员 std::ranges::swap，两边都可安全析构。
 */
vulkan_acceleration_structure& vulkan_acceleration_structure::operator=(vulkan_acceleration_structure&& _other) noexcept
{
    if (this != &_other)
    {
        std::ranges::swap(scratch_alignment, _other.scratch_alignment);
        std::ranges::swap(geometry, _other.geometry);
        std::ranges::swap(range_info, _other.range_info);
        std::ranges::swap(buffer, _other.buffer);
        std::ranges::swap(acceleration_structure, _other.acceleration_structure);
        std::ranges::swap(address, _other.address);
    }
    return *this;
}

/**
 * @brief 填三角形几何（顶点步长 sizeof(vulkan_model_vertex)、索引 eUint32）后转交私有创建流程。
 * @note primitiveCount 取 _index_count / 3u 的整商，索引数不是 3 的倍数时余数被静默丢弃。
 * @note 顶点位置必须在结构体偏移 0 处，因为这里只声明了 eR32G32B32Sfloat 一种格式。
 */
vulkan_buffer vulkan_acceleration_structure::create_bottom_level_acceleration_structure(const vk::raii::PhysicalDevice& _physical_device,
                                                                                        const vk::raii::Device& _device,
                                                                                        const vma::raii::Allocator& _allocator,
                                                                                        const vk::raii::CommandBuffer& _commandbuffer,
                                                                                        uint32_t _vertex_count,
                                                                                        vk::DeviceOrHostAddressConstKHR _vertex_data,
                                                                                        uint32_t _index_count,
                                                                                        vk::DeviceOrHostAddressConstKHR _index_data,
                                                                                        uint32_t _queue)
{
    const vk::AccelerationStructureGeometryTrianglesDataKHR triangles_data =
        vk::AccelerationStructureGeometryTrianglesDataKHR(vk::Format::eR32G32B32Sfloat, _vertex_data, sizeof(vulkan_model_vertex),
                                                          _vertex_count, vk::IndexType::eUint32, _index_data);

    geometry = vk::AccelerationStructureGeometryKHR(vk::GeometryTypeKHR::eTriangles, triangles_data,
                                                    vk::GeometryFlagBitsKHR::eNoDuplicateAnyHitInvocation);

    range_info = vk::AccelerationStructureBuildRangeInfoKHR(_index_count / 3u);

    return create_acceleration_structure(_physical_device, _device, _allocator, _commandbuffer,
                                         vk::AccelerationStructureTypeKHR::eBottomLevel,
                                         vk::BuildAccelerationStructureFlagBitsKHR::ePreferFastTrace, _queue);
}

/**
 * @brief 把实例数组地址包成 eInstances 几何，建 TLAS 并允许后续 refit。
 * @note 实例数传 0 是合法的空 TLAS，此时实例地址不会被读取。
 */
vulkan_buffer vulkan_acceleration_structure::create_top_level_acceleration_structure(const vk::raii::PhysicalDevice& _physical_device,
                                                                                     const vk::raii::Device& _device,
                                                                                     const vma::raii::Allocator& _allocator,
                                                                                     const vk::raii::CommandBuffer& _commandbuffer,
                                                                                     uint32_t _instances_size,
                                                                                     vk::DeviceOrHostAddressConstKHR _instances_data,
                                                                                     uint32_t _queue)
{
    const vk::AccelerationStructureGeometryInstancesDataKHR geometry_instances({}, _instances_data);

    geometry = vk::AccelerationStructureGeometryKHR(vk::GeometryTypeKHR::eInstances, geometry_instances, {});

    range_info = vk::AccelerationStructureBuildRangeInfoKHR(_instances_size);

    return create_acceleration_structure(_physical_device, _device, _allocator, _commandbuffer,
                                         vk::AccelerationStructureTypeKHR::eTopLevel,
                                         vk::BuildAccelerationStructureFlagBitsKHR::ePreferFastTrace
                                             | vk::BuildAccelerationStructureFlagBitsKHR::eAllowUpdate,
                                         _queue);
}

/**
 * @brief 以 eUpdate 模式重录 TLAS 的构建命令，src 与 dst 用同一个加速结构句柄。
 * @note 会覆盖 geometry 与 range_info 两个成员，因此不能与另一次构建共用本对象。
 * @note scratch 由调用方给：必须与创建时按 max(build, update) 分配的那块缓冲一致或同样大。
 */
void vulkan_acceleration_structure::update_top_level_acceleration_structure(const vk::raii::CommandBuffer& _commandbuffer,
                                                                            uint32_t _instances_size,
                                                                            vk::DeviceOrHostAddressConstKHR _instances_data,
                                                                            vk::DeviceAddress _scratch_address)
{
    const vk::AccelerationStructureGeometryInstancesDataKHR geometry_instances({}, _instances_data);

    geometry = vk::AccelerationStructureGeometryKHR(vk::GeometryTypeKHR::eInstances, geometry_instances, {});

    range_info = vk::AccelerationStructureBuildRangeInfoKHR(_instances_size);

    vk::AccelerationStructureBuildGeometryInfoKHR build_info(
        vk::AccelerationStructureTypeKHR::eTopLevel,
        vk::BuildAccelerationStructureFlagBitsKHR::ePreferFastTrace | vk::BuildAccelerationStructureFlagBitsKHR::eAllowUpdate,
        vk::BuildAccelerationStructureModeKHR::eUpdate, acceleration_structure, acceleration_structure, geometry);
    build_info.scratchData.deviceAddress = _scratch_address;

    _commandbuffer.buildAccelerationStructuresKHR(build_info, &range_info);
}

/**
 * @brief 直接返回成员句柄，不做空值检查。
 */
const vk::raii::AccelerationStructureKHR& vulkan_acceleration_structure::get_acceleration_structure() const noexcept
{
    return acceleration_structure;
}

/**
 * @brief 返回创建时缓存的设备地址，refit 不会改变它。
 */
vk::DeviceAddress vulkan_acceleration_structure::get_address() const noexcept
{
    return address;
}

/**
 * @brief 返回 backing buffer 内部持有的句柄；未创建时是空句柄，可直接用它判空。
 */
const vk::raii::Buffer& vulkan_acceleration_structure::get_buffer() const noexcept
{
    return buffer.get_buffer();
}

/**
 * @brief 完整创建流程：懒查 scratch 对齐 → 查 build 尺寸 → 建 backing buffer 与结构 → 取地址 → 建 scratch → 录 build。
 * @note backing buffer 用 eAccelerationStructureStorageKHR | eShaderDeviceAddress 建，scratch 另需 eStorageBuffer。
 * @note scratch 以返回值移交、backing buffer 留在成员里，两者都不由本函数回收。
 */
vulkan_buffer vulkan_acceleration_structure::create_acceleration_structure(const vk::raii::PhysicalDevice& _physical_device,
                                                                           const vk::raii::Device&     _device,
                                                                           const vma::raii::Allocator& _allocator,
                                                                           const vk::raii::CommandBuffer& _commandbuffer,
                                                                           vk::AccelerationStructureTypeKHR _type,
                                                                           vk::BuildAccelerationStructureFlagsKHR _flags,
                                                                           uint32_t _queue)
{
    if (scratch_alignment == 0)
    {
        const auto props = _physical_device.getProperties2<vk::PhysicalDeviceProperties2, vk::PhysicalDeviceRayTracingPipelinePropertiesKHR,
                                                           vk::PhysicalDeviceAccelerationStructurePropertiesKHR>();
        scratch_alignment = props.get<vk::PhysicalDeviceAccelerationStructurePropertiesKHR>().minAccelerationStructureScratchOffsetAlignment;
    }

    vk::AccelerationStructureBuildGeometryInfoKHR build_info(_type, _flags, vk::BuildAccelerationStructureModeKHR::eBuild,
                                                             {}, {}, geometry);

    const vk::AccelerationStructureBuildSizesInfoKHR build_size =
        _device.getAccelerationStructureBuildSizesKHR(vk::AccelerationStructureBuildTypeKHR::eDevice, build_info,
                                                      range_info.primitiveCount);

    buffer.create(_allocator, _device,
                  vk::BufferCreateInfo({}, build_size.accelerationStructureSize,
                                       vk::BufferUsageFlagBits::eAccelerationStructureStorageKHR | vk::BufferUsageFlagBits::eShaderDeviceAddress,
                                       vk::SharingMode::eExclusive, 1, &_queue),
                  vma::MemoryUsage::eAutoPreferDevice, {},
                  _type == vk::AccelerationStructureTypeKHR::eBottomLevel ? "blas" : "tlas");

    vk::AccelerationStructureCreateInfoKHR create_info({}, buffer.get_buffer(), {},
                                                       build_size.accelerationStructureSize, _type, {});

    acceleration_structure = _device.createAccelerationStructureKHR(create_info);


    const vk::AccelerationStructureDeviceAddressInfoKHR address_info(acceleration_structure);

    address = _device.getAccelerationStructureAddressKHR(address_info);

    VkDeviceSize scratch_size = vulkan_common::align_up(build_size.buildScratchSize, scratch_alignment);
    if (_flags & vk::BuildAccelerationStructureFlagBitsKHR::eAllowUpdate)
    {
        const vk::AccelerationStructureBuildGeometryInfoKHR update_build_info(
            _type, _flags, vk::BuildAccelerationStructureModeKHR::eUpdate, {}, acceleration_structure, geometry);
        const auto update_size = _device.getAccelerationStructureBuildSizesKHR(vk::AccelerationStructureBuildTypeKHR::eDevice,
                                                                               update_build_info, range_info.primitiveCount);
        scratch_size = std::max(scratch_size, vulkan_common::align_up(update_size.updateScratchSize, scratch_alignment));
    }

    vulkan_buffer scratch_buffer;
    scratch_buffer.create(_allocator, _device,
                          vk::BufferCreateInfo({}, scratch_size,
                                               vk::BufferUsageFlagBits::eStorageBuffer | vk::BufferUsageFlagBits::eShaderDeviceAddress
                                                   | vk::BufferUsageFlagBits::eAccelerationStructureStorageKHR,
                                               vk::SharingMode::eExclusive, 1, &_queue),
                          vma::MemoryUsage::eAutoPreferDevice, {},
                          _type == vk::AccelerationStructureTypeKHR::eBottomLevel ? "blas_scratch" : "tlas_scratch");

    build_info.dstAccelerationStructure  = acceleration_structure;
    build_info.scratchData.deviceAddress = scratch_buffer.get_buffer_address().deviceAddress;

    _commandbuffer.buildAccelerationStructuresKHR(build_info, &range_info);

    return scratch_buffer;
}
