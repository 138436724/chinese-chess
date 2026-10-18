/**
 * @file vulkan_shader_binding_table.cpp
 * @brief vulkan_shader_binding_table 的实现：组句柄布局计算、staging 写入与设备拷贝录制。
 * @details 布局固定为 [raygen][miss_primary][miss_shadow][hit_primary][hit_shadow]，每个组起点按 base_alignment
 *          对齐，region 的 stride 取实际组间距；miss / hit region 的项数由 _group_count 决定（>= 5 时各 2 项）。
 *          写入路径：查属性 → 建 SBT 缓冲 → 建 staging → 取组句柄 memcpy → flush → 录 copy_buffer_to_buffer。
 *          函数返回时 copy 只是「录进命令缓冲」，所以 staging 的所有权移交给调用方（由它 retire 到回收站）。
 */

#include "vulkan_shader_binding_table.h"

#include "vulkan_common.h"

#include <array>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <ranges>
#include <utility>
#include <vector>

/** @brief 移动构造：三个对齐值、四个 region 与缓冲一律 std::exchange 搬走，源对象被置空。 */
vulkan_shader_binding_table::vulkan_shader_binding_table(vulkan_shader_binding_table&& _other) noexcept
    : handle_size(std::exchange(_other.handle_size, {}))
    , handle_alignment(std::exchange(_other.handle_alignment, {}))
    , base_alignment(std::exchange(_other.base_alignment, {}))
    , raygen_region(std::exchange(_other.raygen_region, {}))
    , miss_region(std::exchange(_other.miss_region, {}))
    , hit_region(std::exchange(_other.hit_region, {}))
    , callable_region(std::exchange(_other.callable_region, {}))
    , sbt_buffer(std::exchange(_other.sbt_buffer, {}))
{
}

/** @brief 移动赋值：逐成员 std::ranges::swap，旧缓冲被换给源对象并随其析构释放。 */
vulkan_shader_binding_table& vulkan_shader_binding_table::operator=(vulkan_shader_binding_table&& _other) noexcept
{
    if (this != &_other)
    {
        std::ranges::swap(handle_size, _other.handle_size);
        std::ranges::swap(handle_alignment, _other.handle_alignment);
        std::ranges::swap(base_alignment, _other.base_alignment);
        std::ranges::swap(raygen_region, _other.raygen_region);
        std::ranges::swap(miss_region, _other.miss_region);
        std::ranges::swap(hit_region, _other.hit_region);
        std::ranges::swap(callable_region, _other.callable_region);
        std::ranges::swap(sbt_buffer, _other.sbt_buffer);
    }
    return *this;
}

/**
 * @brief 按对齐规则把 _group_count 个组句柄排进 SBT 缓冲，并填好四个 region。
 * @note 只有第一次调用才查 PhysicalDeviceRayTracingPipelinePropertiesKHR，之后复用缓存的三个对齐值；
 *       offsets 数组固定 5 项，实际只拷贝 min(_group_count, 5) 项；少组时也不重排，不能据此当作通用 SBT 布局生成器。
 *       callable region 恒为 (0,0,0)；copy 录在调用方已 begin_record 的命令缓冲上，本函数不提交。
 */
vulkan_buffer vulkan_shader_binding_table::create(const vk::raii::PhysicalDevice& _physical_device,
                                                  const vk::raii::Device&         _device,
                                                  const vma::raii::Allocator&     _allocator,
                                                  const vk::raii::CommandBuffer&  _commandbuffer,
                                                  const vk::raii::Pipeline&       _pipeline,
                                                  uint32_t                        _group_count,
                                                  uint32_t                        _queue)
{
    if (handle_size == 0 && handle_alignment == 0 && base_alignment == 0)
    {
        const auto props = _physical_device.getProperties2<vk::PhysicalDeviceProperties2, vk::PhysicalDeviceRayTracingPipelinePropertiesKHR,
                                                           vk::PhysicalDeviceAccelerationStructurePropertiesKHR>();
        const auto& properties = props.get<vk::PhysicalDeviceRayTracingPipelinePropertiesKHR>();

        handle_size      = properties.shaderGroupHandleSize;
        handle_alignment = properties.shaderGroupHandleAlignment;
        base_alignment   = properties.shaderGroupBaseAlignment;
    }

    // SBT layout for 5 groups:
    // [Group 0: raygen] [Group 1: miss_primary] [Group 2: miss_shadow] [Group 3: hit_primary] [Group 4: hit_shadow]
    //
    // Vulkan SBT regions:
    // - raygen_region: points to Group 0 (1 entry, stride = raygen_entry_size)
    // - miss_region:   points to Group 1 (2 entries, stride = miss_stride)
    // - hit_region:    points to Group 3 (2 entries, stride = hit_stride)

    const uint32_t raygen_entry_size = static_cast<uint32_t>(vulkan_common::align_up(handle_size, handle_alignment));
    const uint32_t miss_entry_count = _group_count >= 5 ? 2 : 1;  // 2 miss shaders (primary + shadow) if 5 groups present
    const uint32_t hit_entry_count = _group_count >= 5 ? 2 : 1;  // 2 hit shaders (primary + shadow) if 5 groups present

    // Calculate offsets for each group in the SBT buffer
    // Group 0: raygen
    const uint32_t raygen_offset = 0;
    // Group 1: miss_primary
    const uint32_t miss_primary_offset =
        static_cast<uint32_t>(vulkan_common::align_up(raygen_offset + raygen_entry_size, base_alignment));
    // Group 2: miss_shadow
    const uint32_t miss_shadow_offset =
        static_cast<uint32_t>(vulkan_common::align_up(miss_primary_offset + raygen_entry_size, base_alignment));
    // Group 3: hit_primary
    const uint32_t hit_primary_offset =
        static_cast<uint32_t>(vulkan_common::align_up(miss_shadow_offset + raygen_entry_size, base_alignment));
    // Group 4: hit_shadow
    const uint32_t hit_shadow_offset =
        static_cast<uint32_t>(vulkan_common::align_up(hit_primary_offset + raygen_entry_size, base_alignment));
    // Total size
    const vk::DeviceSize buffer_size =
        static_cast<uint64_t>(vulkan_common::align_up(hit_shadow_offset + raygen_entry_size, base_alignment));

    // Stride must match actual spacing between groups (base_alignment-aligned), not handle_alignment-aligned
    const uint32_t miss_stride = miss_shadow_offset - miss_primary_offset;
    const uint32_t hit_stride  = hit_shadow_offset - hit_primary_offset;

    sbt_buffer.create(_allocator, _device,
                      vk::BufferCreateInfo({}, buffer_size,
                                           vk::BufferUsageFlagBits::eShaderBindingTableKHR | vk::BufferUsageFlagBits::eTransferDst
                                               | vk::BufferUsageFlagBits::eShaderDeviceAddress,
                                           vk::SharingMode::eExclusive, 1, &_queue),
                      vma::MemoryUsage::eAutoPreferDevice, {}, "sbt");

    vulkan_buffer staging_buffer;
    staging_buffer.create(_allocator, _device,
                          vk::BufferCreateInfo({}, buffer_size, vk::BufferUsageFlagBits::eTransferSrc,
                                               vk::SharingMode::eExclusive, 1, &_queue),
                          vma::MemoryUsage::eAuto, vma::AllocationCreateFlagBits::eHostAccessSequentialWrite, "sbt_staging");

    const std::vector<uint8_t> shader_handles =
        _pipeline.getRayTracingShaderGroupHandlesKHR<uint8_t>(0, _group_count, static_cast<size_t>(handle_size) * _group_count);

    uint8_t* buffer_address = static_cast<uint8_t*>(staging_buffer.get_buffer_address().hostAddress);

    // Copy handles for all groups
    const std::array offsets = {raygen_offset, miss_primary_offset, miss_shadow_offset, hit_primary_offset, hit_shadow_offset};
    for (auto [group_index, offset] : offsets | std::views::enumerate | std::views::take(_group_count))
    {
        std::memcpy(buffer_address + offset, shader_handles.data() + group_index * handle_size, handle_size);
    }

    staging_buffer.flush();

    // Create StridedDeviceAddressRegion for each Vulkan SBT region
    // raygen: single group at raygen_offset, stride = raygen_entry_size (size of 1 entry)
    raygen_region = vk::StridedDeviceAddressRegionKHR(sbt_buffer.get_buffer_address().deviceAddress + raygen_offset, raygen_entry_size,
                                                      raygen_entry_size * 1  // total size = 1 entry
    );

    // miss: starts at miss_primary_offset, stride = actual interval between miss groups, 2 entries (primary + shadow)
    miss_region = vk::StridedDeviceAddressRegionKHR(sbt_buffer.get_buffer_address().deviceAddress + miss_primary_offset, miss_stride,
                                                    miss_stride * miss_entry_count  // total size = 2 entries
    );

    // hit: starts at hit_primary_offset, stride = actual interval between hit groups, 2 entries (primary + shadow)
    hit_region = vk::StridedDeviceAddressRegionKHR(sbt_buffer.get_buffer_address().deviceAddress + hit_primary_offset, hit_stride,
                                                   hit_stride * hit_entry_count  // total size = 2 entries
    );

    callable_region = vk::StridedDeviceAddressRegionKHR(0, 0, 0);

    vulkan_buffer::copy_buffer_to_buffer(_commandbuffer, staging_buffer.get_buffer(), sbt_buffer.get_buffer(),
                                         vk::BufferCopy2(0, 0, buffer_size));

    return staging_buffer;
}

/** @brief 返回 raygen region（设备地址 + stride + size）；create 之前全 0。 */
const vk::StridedDeviceAddressRegionKHR& vulkan_shader_binding_table::get_raygen_region() const noexcept
{
    return raygen_region;
}

/** @brief 返回 miss region：起始为主 miss，stride 是 base_alignment 对齐后的组间距。 */
const vk::StridedDeviceAddressRegionKHR& vulkan_shader_binding_table::get_miss_region() const noexcept
{
    return miss_region;
}

/** @brief 返回 hit region：起始为主 hit，stride 是 base_alignment 对齐后的组间距。 */
const vk::StridedDeviceAddressRegionKHR& vulkan_shader_binding_table::get_hit_region() const noexcept
{
    return hit_region;
}

/** @brief 返回 callable region；本项目不注册 callable 着色器，所以恒为 (0,0,0)。 */
const vk::StridedDeviceAddressRegionKHR& vulkan_shader_binding_table::get_callable_region() const noexcept
{
    return callable_region;
}

/** @brief 返回 SBT 缓冲；未 create 时为空缓冲，create 之后是设备本地的 eShaderBindingTableKHR 缓冲。 */
const vulkan_buffer& vulkan_shader_binding_table::get_shader_binding_table_buffer() const noexcept
{
    return sbt_buffer;
}
