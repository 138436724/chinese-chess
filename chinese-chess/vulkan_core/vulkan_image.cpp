/**
 * @file vulkan_image.cpp
 * @brief 图像/视图创建、布局转换与拷贝命令录制的实现。
 * @details create 校验格式与层数一致后分配 VMA 图像，再用同一个 allocator 的 callbacks 建
 *          imageview（按值传入的 _imageview_info 在函数内被补上 image 句柄）。transition_state
 *          用追踪的旧 layout/stage/access 作 src、入参作 dst，并在返回前更新这几个字段。
 *          Debug 下给 image 与 imageview 各设一个带后缀的对象名。
 */

#include "vulkan_image.h"

#include <bit>
#include <concepts>
#include <cstdint>
#include <format>
#include <stdexcept>
#include <string>
#include <utility>

/**
 * @brief 逐成员 std::exchange：句柄、格式、extent 与追踪状态一起移交，源对象留下空值。
 */
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

/**
 * @brief 自赋值提前返回，否则逐成员 std::ranges::swap，两边都可安全析构。
 */
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

/**
 * @brief 校验 image/imageview 的格式与层数，分配图像与视图并缓存格式、extent、clear value。
 * @note _imageview_info 按值传递，函数内把它的 image 成员指向新图像，调用方不必预先填句柄。
 * @note 只接受单 queue family 的 Exclusive 图像；_flags 原样交给 VMA，不映射也不补位。
 */
void vulkan_image::create(const vma::raii::Allocator&         _allocator,
                          const vk::raii::Device&             _device,
                          const vk::ImageCreateInfo&          _image_info,
                          vk::ImageViewCreateInfo             _imageview_info,
                          vma::MemoryUsage                    _usage,
                          vma::AllocationCreateFlags          _flags,
                          const vk::ClearValue&               _clear_value,
                          [[maybe_unused]] const std::string& _name)
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

    image = _allocator.createImage(_image_info, vma::AllocationCreateInfo(_flags, _usage));

    _imageview_info.image = image;
    imageview             = vk::raii::ImageView(_device, _imageview_info, _allocator.getAllocationCallbacks());

#ifndef NDEBUG
    _device.setDebugUtilsObjectNameEXT(vk::DebugUtilsObjectNameInfoEXT(
        image.objectType, std::bit_cast<uint64_t>(static_cast<VkImage>(*image)), std::format("Image{}", _name).c_str()));
    _device.setDebugUtilsObjectNameEXT(
        vk::DebugUtilsObjectNameInfoEXT(imageview.objectType, std::bit_cast<uint64_t>(static_cast<VkImageView>(*imageview)),
                                        std::format("ImageView{}", _name).c_str()));
#endif  // !NDEBUG
}

/**
 * @brief 生成 image barrier 并把追踪的 layout/stage/access/queue 更新为目标值。
 * @note 默认 _range 只覆盖 1 个 mip、1 个 array 层与 eColor 方面；深度图和多 mip 图必须显式传范围。
 * @note 返回的 barrier 必须被录制，否则追踪布局会领先于 GPU 实际布局，后续 oldLayout 就错了。
 */
vk::ImageMemoryBarrier2 vulkan_image::transition_state(vk::PipelineStageFlags2 _stage,
                                                       vk::AccessFlags2        _access,
                                                       vk::ImageLayout         _layout,
                                                       uint32_t                _queue /*= vk::QueueFamilyIgnored*/,
                                                       vk::ImageSubresourceRange _range /*= vk::ImageSubresourceRange(vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1)*/)
{
    auto barrier = vk::ImageMemoryBarrier2(stage, access, _stage, _access, layout, _layout, vk::QueueFamilyIgnored,
                                           vk::QueueFamilyIgnored, image, _range);

    if (_queue != vk::QueueFamilyIgnored && _queue != queue)
    {
        barrier.srcQueueFamilyIndex = queue;
        barrier.dstQueueFamilyIndex = _queue;
    }

    stage  = _stage;
    access = _access;
    layout = _layout;
    queue  = _queue == vk::QueueFamilyIgnored ? queue : _queue;

    return barrier;
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

/** @brief 返回追踪到的当前布局，下一条 barrier 的 oldLayout 就是它。 */
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

/**
 * @brief 录一条 vkCmdCopyImageToBuffer2，源布局由 _layout 显式给出（回读时通常是 eTransferSrcOptimal）。
 * @note 不插 barrier：调用前要保证图像已转好布局、目标缓冲已可写。
 */
void vulkan_image::copy_image_to_buffer(const vk::raii::CommandBuffer& _commandbuffer,
                                        const vk::Image&               _image,
                                        const vk::Buffer&              _buffer,
                                        vk::ImageLayout                _layout,
                                        const vk::BufferImageCopy2&    _copy_info) noexcept
{
    _commandbuffer.copyImageToBuffer2(vk::CopyImageToBufferInfo2(_image, _layout, _buffer, _copy_info));
}

/**
 * @brief 录一条 vkCmdCopyImage2，两端布局都由参数显式给出。
 * @note 不插 barrier；调用前两端都必须已转到入参给出的布局。
 */
void vulkan_image::copy_image_to_image(const vk::raii::CommandBuffer& _commandbuffer,
                                       const vk::Image&               _image_src,
                                       const vk::Image&               _image_dst,
                                       vk::ImageLayout                _src_layout,
                                       vk::ImageLayout                _dst_layout,
                                       const vk::ImageCopy2&          _copy_info) noexcept
{
    _commandbuffer.copyImage2(vk::CopyImageInfo2(_image_src, _src_layout, _image_dst, _dst_layout, _copy_info));
}
