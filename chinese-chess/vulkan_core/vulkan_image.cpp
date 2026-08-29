#include "vulkan_image.h"

#include <bit>
#include <format>
#include <stdexcept>
#include <utility>

vulkan_image::vulkan_image(vulkan_image&& _other) noexcept
    : format(std::exchange(_other.format, {}))
    , extent(std::exchange(_other.extent, {}))
    , clear_value(std::exchange(_other.clear_value, {}))
    , stage(std::exchange(_other.stage, {}))
    , access(std::exchange(_other.access, {}))
    , layout(std::exchange(_other.layout, {}))
    , queue(std::exchange(_other.queue, vk::QueueFamilyIgnored))
    , image(std::exchange(_other.image, nullptr))
    , imageview(std::exchange(_other.imageview, nullptr))
{
}

vulkan_image& vulkan_image::operator=(vulkan_image&& _other) noexcept
{
    if (this != &_other)
    {
        std::ranges::swap(format, _other.format);
        std::ranges::swap(extent, _other.extent);
        std::ranges::swap(clear_value, _other.clear_value);
        std::ranges::swap(stage, _other.stage);
        std::ranges::swap(access, _other.access);
        std::ranges::swap(layout, _other.layout);
        std::ranges::swap(queue, _other.queue);
        std::ranges::swap(image, _other.image);
        std::ranges::swap(imageview, _other.imageview);
    }
    return *this;
}

void vulkan_image::create(const vma::raii::Allocator& _allocator,
                          const vk::raii::Device&     _device,
                          const vk::ImageCreateInfo&  _image_info,
                          vk::ImageViewCreateInfo&    _imageview_info,
                          vma::MemoryUsage            _usage,
                          const vk::ClearValue&       _clear_value,
                          const std::string&          _name)
{
    if (_image_info.format != _imageview_info.format || _image_info.arrayLayers != _imageview_info.subresourceRange.layerCount)
    {
        throw std::runtime_error("Please check if image info and image view info match!");
    }
    if (_image_info.queueFamilyIndexCount != 1 || _image_info.sharingMode != vk::SharingMode::eExclusive)
    {
        throw std::runtime_error("Only support Exclusive mode!");
    }

    format      = _image_info.format;
    extent      = vk::Extent2D(_image_info.extent.width, _image_info.extent.height);
    clear_value = _clear_value;

    queue = *_image_info.pQueueFamilyIndices;

    vma::AllocationCreateInfo create_info{};
    create_info.setUsage(_usage);

    image = _allocator.createImage(_image_info, create_info);

    _imageview_info.image = image;
    imageview             = vk::raii::ImageView(_device, _imageview_info, _allocator.getAllocationCallbacks());

#ifndef NDEBUG
    _device.setDebugUtilsObjectNameEXT(vk::DebugUtilsObjectNameInfoEXT(
        image.objectType, std::bit_cast<uint64_t>(static_cast<VkImage>(*image)), std::format("Image{}", _name).c_str()));
    _device.setDebugUtilsObjectNameEXT(
        vk::DebugUtilsObjectNameInfoEXT(imageview.objectType, std::bit_cast<uint64_t>(static_cast<VkImageView>(*imageview)),
                                        std::format("ImageView{}", _name).c_str()));
#else
    (void)_name;
#endif  // !NDEBUG
}

void vulkan_image::set_info(const vk::ImageMemoryBarrier2& _barrier)
{
    if (_barrier.image != *image)
    {
        throw std::runtime_error("The barrier not used by this image!");
    }
    if ((_barrier.srcQueueFamilyIndex == vk::QueueFamilyIgnored) != (_barrier.dstQueueFamilyIndex == vk::QueueFamilyIgnored))
    {
        throw std::runtime_error("Queue must all ignore or all set new value!");
    }

    stage  = _barrier.dstStageMask;
    access = _barrier.dstAccessMask;
    layout = _barrier.newLayout;
    queue  = _barrier.dstQueueFamilyIndex == vk::QueueFamilyIgnored ? queue : _barrier.dstQueueFamilyIndex;
}

vk::Format vulkan_image::get_format() const noexcept
{
    return format;
}

vk::Extent2D vulkan_image::get_extent() const noexcept
{
    return extent;
}

vk::PipelineStageFlags2 vulkan_image::get_stage() const noexcept
{
    return stage;
}

vk::AccessFlags2 vulkan_image::get_access() const noexcept
{
    return access;
}

vk::ImageLayout vulkan_image::get_layout() const noexcept
{
    return layout;
}

uint32_t vulkan_image::get_queue() const noexcept
{
    return queue;
}

vk::ClearValue vulkan_image::get_clear_value() const noexcept
{
    return clear_value;
}

const vk::raii::Image& vulkan_image::get_image() const noexcept
{
    return image;
}

const vk::raii::ImageView& vulkan_image::get_imageview() const noexcept
{
    return imageview;
}

void vulkan_image::copy_image_to_buffer(const vk::raii::CommandBuffer& _commandbuffer,
                                        const vk::Image&               _image,
                                        const vk::Buffer&              _buffer,
                                        vk::ImageLayout                _layout,
                                        const vk::BufferImageCopy2&    _copy_info) noexcept
{
    _commandbuffer.copyImageToBuffer2(vk::CopyImageToBufferInfo2(_image, _layout, _buffer, _copy_info));
}

void vulkan_image::copy_image_to_image(const vk::raii::CommandBuffer& _commandbuffer,
                                       const vk::Image&               _image_src,
                                       const vk::Image&               _image_dst,
                                       vk::ImageLayout                _src_layout,
                                       vk::ImageLayout                _dst_layout,
                                       const vk::ImageCopy2&          _copy_info) noexcept
{
    _commandbuffer.copyImage2(vk::CopyImageInfo2(_image_src, _src_layout, _image_dst, _dst_layout, _copy_info));
}
