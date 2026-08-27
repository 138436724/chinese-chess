#include "vulkan_acceleration_structure.h"

#include "tools/model_loader.h"
#include "vulkan_common.h"

vulkan_acceleration_structure::vulkan_acceleration_structure(vulkan_acceleration_structure&& _other) noexcept
    : scratch_alignment(std::exchange(_other.scratch_alignment, {}))
    , geometry(std::exchange(_other.geometry, {}))
    , range_info(std::exchange(_other.range_info, {}))
    , buffer(std::exchange(_other.buffer, {}))
    , acceleration_structure(std::exchange(_other.acceleration_structure, nullptr))
    , address(std::exchange(_other.address, {}))
{
}

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
        vk::AccelerationStructureGeometryTrianglesDataKHR(vk::Format::eR32G32B32Sfloat, _vertex_data, sizeof(model_vertex),
                                                          _vertex_count, vk::IndexType::eUint32, _index_data);

    geometry = vk::AccelerationStructureGeometryKHR(vk::GeometryTypeKHR::eTriangles, triangles_data,
                                                    vk::GeometryFlagBitsKHR::eNoDuplicateAnyHitInvocation);

    range_info = vk::AccelerationStructureBuildRangeInfoKHR(_index_count / 3u);

    return create_acceleration_structure(_physical_device, _device, _allocator, _commandbuffer,
                                         vk::AccelerationStructureTypeKHR::eBottomLevel,
                                         vk::BuildAccelerationStructureFlagBitsKHR::ePreferFastTrace, _queue);
}

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

const vk::raii::AccelerationStructureKHR& vulkan_acceleration_structure::get_acceleration_structure() const noexcept
{
    return acceleration_structure;
}

vk::DeviceAddress vulkan_acceleration_structure::get_address() const noexcept
{
    return address;
}

const vk::raii::Buffer& vulkan_acceleration_structure::get_buffer() const noexcept
{
    return buffer.get_buffer();
}

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
                  vma::MemoryUsage::eGpuOnly, _type == vk::AccelerationStructureTypeKHR::eBottomLevel ? "blas" : "tlas");

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
                          vma::MemoryUsage::eGpuOnly,
                          _type == vk::AccelerationStructureTypeKHR::eBottomLevel ? "blas_scratch" : "tlas_scratch");

    build_info.dstAccelerationStructure  = acceleration_structure;
    build_info.scratchData.deviceAddress = scratch_buffer.get_buffer_address().deviceAddress;

    _commandbuffer.buildAccelerationStructuresKHR(build_info, &range_info);

    return scratch_buffer;
}
