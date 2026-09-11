#pragma once

// need replace "vk_mem_alloc.h" by <vma/vk_mem_alloc.h> if use vcpkg install
#include <string>
#include <vulkan-memory-allocator-hpp/vk_mem_alloc_raii.hpp>

class vulkan_buffer
{
public:
    vulkan_buffer()                                = default;
    ~vulkan_buffer()                               = default;
    vulkan_buffer(const vulkan_buffer&)            = delete;
    vulkan_buffer& operator=(const vulkan_buffer&) = delete;
    vulkan_buffer(vulkan_buffer&& _other) noexcept;
    vulkan_buffer& operator=(vulkan_buffer&& _other) noexcept;

    void create(const vma::raii::Allocator& _allocator,
                const vk::raii::Device&     _device,
                const vk::BufferCreateInfo& _buffer_info,
                vma::MemoryUsage            _usage,
                vma::AllocationCreateFlags  _flags,
                const std::string&          _name = "");

    void flush() const;
    void invalidate() const;

    void set_info(const vk::BufferMemoryBarrier2& _barrier);

    [[nodiscard]] vk::PipelineStageFlags2    get_stage() const noexcept;
    [[nodiscard]] vk::AccessFlags2           get_access() const noexcept;
    [[nodiscard]] uint32_t                   get_queue() const noexcept;
    [[nodiscard]] const vk::raii::Buffer&    get_buffer() const noexcept;
    [[nodiscard]] vk::DeviceOrHostAddressKHR get_buffer_address() const noexcept;
    [[nodiscard]] vk::DeviceSize             get_size() const noexcept;

    static void copy_buffer_to_buffer(const vk::raii::CommandBuffer& _commandbuffer,
                                      const vk::Buffer&              _src_buffer,
                                      const vk::Buffer&              _dst_buffer,
                                      const vk::BufferCopy2&         _copy_info) noexcept;
    static void copy_buffer_to_image(const vk::raii::CommandBuffer& _commandbuffer,
                                     const vk::Buffer&              _buffer,
                                     const vk::Image&               _image,
                                     const vk::BufferImageCopy2&    _copy_info) noexcept;

private:
    vk::PipelineStageFlags2 stage  = vk::PipelineStageFlagBits2::eNone;
    vk::AccessFlags2        access = vk::AccessFlagBits2::eNone;
    uint32_t                queue  = vk::QueueFamilyIgnored;

    vma::raii::Buffer          buffer         = nullptr;
    vk::DeviceSize             buffer_size    = 0;
    vk::DeviceOrHostAddressKHR buffer_address = nullptr;
};
