#pragma once

#include <vulkan-memory-allocator-hpp/vk_mem_alloc_raii.hpp>

class vulkan_image
{
public:
    vulkan_image()                         = default;
    ~vulkan_image()                        = default;
    vulkan_image(vulkan_image&)            = delete;
    vulkan_image& operator=(vulkan_image&) = delete;
    vulkan_image(vulkan_image&& _other) noexcept;
    vulkan_image& operator=(vulkan_image&& _other) noexcept;

    void create(const vma::raii::Allocator& _allocator,
                const vk::raii::Device&     _device,
                const vk::ImageCreateInfo&  _image_info,
                vk::ImageViewCreateInfo&    _imageview_info,
                vk::MemoryPropertyFlags     _properties,
                const vk::ClearValue&       _clear_value);

    void set_info(const vk::ImageMemoryBarrier2& _barrier);

    vk::Format                 get_format() const noexcept;
    vk::Extent2D               get_extent() const noexcept;
    vk::PipelineStageFlags2    get_stage() const noexcept;
    vk::AccessFlags2           get_access() const noexcept;
    vk::ImageLayout            get_layout() const noexcept;
    uint32_t                   get_queue() const noexcept;
    vk::ClearValue             get_clear_value() const noexcept;
    const vk::raii::Image&     get_image() const noexcept;
    const vk::raii::ImageView& get_imageview() const noexcept;

    static void copy_image_to_buffer(const vk::raii::CommandBuffer& _commandbuffer,
                                     const vk::Image&               _image,
                                     const vk::Buffer&              _buffer,
                                     vk::ImageLayout                _layout,
                                     const vk::BufferImageCopy2&    _copy_info) noexcept;
    static void copy_image_to_image(const vk::raii::CommandBuffer& _commandbuffer,
                                    const vk::Image&               _image_src,
                                    const vk::Image&               _image_dst,
                                    vk::ImageLayout                _src_layout,
                                    vk::ImageLayout                _dst_layout,
                                    const vk::ImageCopy2&          _copy_info) noexcept;

private:
    vk::Format     format      = vk::Format::eUndefined;
    vk::Extent2D   extent      = vk::Extent2D{0, 0};
    vk::ClearValue clear_value = vk::ClearColorValue(0.f, 0.f, 0.f, 0.f);

    vk::PipelineStageFlags2 stage  = vk::PipelineStageFlagBits2::eNone;
    vk::AccessFlags2        access = vk::AccessFlagBits2::eNone;
    vk::ImageLayout         layout = vk::ImageLayout::eUndefined;
    uint32_t                queue  = vk::QueueFamilyIgnored;

    vma::raii::Image    image     = nullptr;
    vk::raii::ImageView imageview = nullptr;
};
