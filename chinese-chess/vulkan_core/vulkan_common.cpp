#include "vulkan_common.h"

#include "vulkan_buffer.h"
#include "vulkan_commandbuffer.h"
#include "vulkan_image.h"
#include "vulkan_queue.h"
#include "vulkan_recycle_bin.h"

#include <array>
#include <vulkan/utility/vk_format_utils.h>

// 转换布局需要转移所有权之后，在新队列转换布局，不能交出所有权的时候转换布局
// 交出的时候需要pipeline stage和access flag需要为eNone
vk::SemaphoreSubmitInfo vulkan_common::upload_buffer(const vma::raii::Allocator&    _allocator,
                                                     const vk::raii::Device&        _device,
                                                     vulkan_recycle_bin&            _recycle_bin,
                                                     vulkan_semaphore&              _semaphore,
                                                     const vulkan_queue&            _graphic_queue,
                                                     const vulkan_queue&            _transfer_queue,
                                                     vulkan_buffer&                 _buffer,
                                                     vk::BufferUsageFlags           _usage,
                                                     const std::span<const uint8_t> _data,
                                                     const std::string&             _buffer_name)
{
    const std::array queue_array = {_transfer_queue.get_index()};

    // create buffer
    _buffer.create(_allocator, _device,
                   vk::BufferCreateInfo({}, _data.size(), _usage | vk::BufferUsageFlagBits::eTransferDst,
                                        vk::SharingMode::eExclusive, queue_array),
                   vma::MemoryUsage::eGpuOnly, _buffer_name);

    // create staging buffer
    vulkan_buffer staging_buffer;
    staging_buffer.create(_allocator, _device,
                          vk::BufferCreateInfo({}, _data.size(), vk::BufferUsageFlagBits::eTransferSrc,
                                               vk::SharingMode::eExclusive, queue_array),
                          vma::MemoryUsage::eCpuToGpu, std::format("Staging{}", _buffer_name));


    // begin a transfer commandbuffer
    vulkan_commandbuffer transfer_commandbuffer =
        std::move(vulkan_commandbuffer::create(_device,
                                               vk::CommandBufferAllocateInfo(_transfer_queue.get_command_pool(),
                                                                             vk::CommandBufferLevel::ePrimary, 1),
                                               &_transfer_queue, &_semaphore)
                      .front());
    transfer_commandbuffer.begin_record(vk::CommandBufferUsageFlagBits::eOneTimeSubmit);

    // create barrier to transfer write
    const auto ssbo_begin_barrier =
        vk::BufferMemoryBarrier2(_buffer.get_stage(), _buffer.get_access(), vk::PipelineStageFlagBits2::eTransfer,
                                 vk::AccessFlagBits2::eTransferWrite, vk::QueueFamilyIgnored, vk::QueueFamilyIgnored,
                                 _buffer.get_buffer(), 0, vk::WholeSize);
    _buffer.set_info(ssbo_begin_barrier);
    const std::array begin_barrier = {ssbo_begin_barrier};
    (*transfer_commandbuffer).pipelineBarrier2(vk::DependencyInfo({}, {}, begin_barrier, {}));

    // copy data to buffer
    std::memcpy(staging_buffer.get_buffer_address().hostAddress, _data.data(), _data.size());
    staging_buffer.flush();

    vulkan_buffer::copy_buffer_to_buffer(*transfer_commandbuffer, staging_buffer.get_buffer(), _buffer.get_buffer(),
                                         vk::BufferCopy2(0, 0, _data.size()));

    // create barrier to end transfer and ready to graphic
    const auto ssbo_transfer_barrier =
        vk::BufferMemoryBarrier2(_buffer.get_stage(), _buffer.get_access(), vk::PipelineStageFlagBits2::eNone,
                                 vk::AccessFlagBits2::eNone, _buffer.get_queue(), _graphic_queue.get_index(),
                                 _buffer.get_buffer(), 0, vk::WholeSize);
    _buffer.set_info(ssbo_transfer_barrier);
    const std::array end_barrier = {ssbo_transfer_barrier};
    (*transfer_commandbuffer).pipelineBarrier2(vk::DependencyInfo({}, {}, end_barrier, {}));

    // end and submit the transfer commandbuffer
    transfer_commandbuffer.end_record();
    transfer_commandbuffer.submit(false);


    // begin a graphic commandbuffer and add wait for transfer commandbuffer
    vulkan_commandbuffer graphic_commandbuffer =
        std::move(vulkan_commandbuffer::create(_device,
                                               vk::CommandBufferAllocateInfo(_graphic_queue.get_command_pool(),
                                                                             vk::CommandBufferLevel::ePrimary, 1),
                                               &_graphic_queue, &_semaphore)
                      .front());
    graphic_commandbuffer.begin_record(vk::CommandBufferUsageFlagBits::eOneTimeSubmit);
    graphic_commandbuffer.add_waited_info(transfer_commandbuffer.get_submit_info());

    // create barrier to graphic read
    const auto ssbo_graphic_barrier =
        vk::BufferMemoryBarrier2(_buffer.get_stage(), _buffer.get_access(),
                                 vk::PipelineStageFlagBits2::eVertexShader | vk::PipelineStageFlagBits2::eFragmentShader
                                     | vk::PipelineStageFlagBits2::eRayTracingShaderKHR,
                                 vk::AccessFlagBits2::eShaderRead, _transfer_queue.get_index(), _buffer.get_queue(),
                                 _buffer.get_buffer(), 0, vk::WholeSize);
    _buffer.set_info(ssbo_graphic_barrier);
    const std::array graphic_barrier = {ssbo_graphic_barrier};
    (*graphic_commandbuffer).pipelineBarrier2(vk::DependencyInfo({}, {}, graphic_barrier, {}));

    // end and submit the graphic commandbuffer
    graphic_commandbuffer.end_record();
    graphic_commandbuffer.submit(false);

    const auto submit_info = graphic_commandbuffer.get_submit_info();

    _recycle_bin.retire(std::move(staging_buffer), "upload buffer staging buffer.");
    _recycle_bin.retire(std::move(transfer_commandbuffer), "upload buffer transfer commandbuffer.");
    _recycle_bin.retire(std::move(graphic_commandbuffer), "upload buffer graphic commandbuffer.");

    return submit_info;
}

vk::SemaphoreSubmitInfo vulkan_common::upload_image(const vma::raii::Allocator&    _allocator,
                                                    const vk::raii::Device&        _device,
                                                    vulkan_recycle_bin&            _recycle_bin,
                                                    vulkan_semaphore&              _semaphore,
                                                    const vulkan_queue&            _graphic_queue,
                                                    const vulkan_queue&            _transfer_queue,
                                                    vk::ImageType                  _image_type,
                                                    vk::ImageViewType              _image_view_type,
                                                    vk::Format                     _image_format,
                                                    const vk::Extent3D&            _image_extent,
                                                    vulkan_image&                  _image,
                                                    const std::span<const uint8_t> _data,
                                                    const std::string&             _image_name)
{

    // create image
    const std::array          image_queue_array = {_graphic_queue.get_index()};
    const vk::ImageCreateInfo image_info({}, _image_type, _image_format, _image_extent, 1, 1,
                                         vk::SampleCountFlagBits::e1, vk::ImageTiling::eOptimal,
                                         vk::ImageUsageFlagBits::eSampled | vk::ImageUsageFlagBits::eTransferDst,
                                         vk::SharingMode::eExclusive, image_queue_array);
    vk::ImageViewCreateInfo view_info({}, {}, _image_view_type, _image_format, {},
                                      vk::ImageSubresourceRange(vk::ImageAspectFlagBits::eColor, {}, 1, 0, 1), nullptr);
    _image.create(_allocator, _device, image_info, view_info, vma::MemoryUsage::eGpuOnly,
                  vk::ClearColorValue(0.f, 0.f, 0.f, 1.f), _image_name);

    // create staging buffer
    const std::array buffer_queue_array = {_transfer_queue.get_index()};
    vulkan_buffer    staging_buffer;
    staging_buffer.create(_allocator, _device,
                          vk::BufferCreateInfo({}, _data.size(), vk::BufferUsageFlagBits::eTransferSrc,
                                               vk::SharingMode::eExclusive, buffer_queue_array),
                          vma::MemoryUsage::eCpuToGpu, std::format("Staging{}", _image_name));
    std::memcpy(staging_buffer.get_buffer_address().hostAddress, _data.data(), _data.size());
    staging_buffer.flush();


    // begin a graphic commandbuffer
    vulkan_commandbuffer graphic_commandbuffer =
        std::move(vulkan_commandbuffer::create(_device,
                                               vk::CommandBufferAllocateInfo(_graphic_queue.get_command_pool(),
                                                                             vk::CommandBufferLevel::ePrimary, 1),
                                               &_graphic_queue, &_semaphore)
                      .front());
    graphic_commandbuffer.begin_record(vk::CommandBufferUsageFlagBits::eOneTimeSubmit);

    // create barrier to transfer layout for clear
    const auto image_graphic_begin_barrier =
        vk::ImageMemoryBarrier2(_image.get_stage(), _image.get_access(), vk::PipelineStageFlagBits2::eTransfer,
                                vk::AccessFlagBits2::eTransferWrite, _image.get_layout(),
                                vk::ImageLayout::eTransferDstOptimal, _image.get_queue(), _image.get_queue(),
                                _image.get_image(), vk::ImageSubresourceRange(vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1));
    _image.set_info(image_graphic_begin_barrier);
    const std::array graphic_begin_barrier = {image_graphic_begin_barrier};
    (*graphic_commandbuffer).pipelineBarrier2(vk::DependencyInfo({}, {}, {}, graphic_begin_barrier));

    if (!vkuFormatIsCompressed(static_cast<VkFormat>(_image.get_format())))
    {
        (*graphic_commandbuffer)
            .clearColorImage(_image.get_image(), _image.get_layout(), _image.get_clear_value().color,
                             vk::ImageSubresourceRange(vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1));
    }

    // create barrier to give the ownership
    const auto image_graphic_end_barrier =
        vk::ImageMemoryBarrier2(_image.get_stage(), _image.get_access(), vk::PipelineStageFlagBits2::eNone,
                                vk::AccessFlagBits2::eNone, _image.get_layout(), _image.get_layout(),
                                _image.get_queue(), _transfer_queue.get_index(), _image.get_image(),
                                vk::ImageSubresourceRange(vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1));
    _image.set_info(image_graphic_end_barrier);
    const std::array graphic_end_barrier = {image_graphic_end_barrier};
    (*graphic_commandbuffer).pipelineBarrier2(vk::DependencyInfo({}, {}, {}, graphic_end_barrier));

    // end and submit the graphic commandbuffer
    graphic_commandbuffer.end_record();
    graphic_commandbuffer.submit(false);


    // begin a transfer commandbuffer
    vulkan_commandbuffer transfer_commandbuffer =
        std::move(vulkan_commandbuffer::create(_device,
                                               vk::CommandBufferAllocateInfo(_transfer_queue.get_command_pool(),
                                                                             vk::CommandBufferLevel::ePrimary, 1),
                                               &_transfer_queue, &_semaphore)
                      .front());
    transfer_commandbuffer.begin_record(vk::CommandBufferUsageFlagBits::eOneTimeSubmit);
    transfer_commandbuffer.add_waited_info(graphic_commandbuffer.get_submit_info());

    // create barrier to transfer write
    const auto image_begin_barrier =
        vk::ImageMemoryBarrier2(_image.get_stage(), _image.get_access(), vk::PipelineStageFlagBits2::eTransfer,
                                vk::AccessFlagBits2::eTransferWrite, _image.get_layout(),
                                vk::ImageLayout::eTransferDstOptimal, _graphic_queue.get_index(), _image.get_queue(),
                                _image.get_image(), vk::ImageSubresourceRange(vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1));
    _image.set_info(image_begin_barrier);
    const std::array begin_barrier = {image_begin_barrier};
    (*transfer_commandbuffer).pipelineBarrier2(vk::DependencyInfo({}, {}, {}, begin_barrier));

    // copy data to image
    vulkan_buffer::copy_buffer_to_image(
        *transfer_commandbuffer, staging_buffer.get_buffer(), _image.get_image(),
        vk::BufferImageCopy2(0, 0, 0, vk::ImageSubresourceLayers(vk::ImageAspectFlagBits::eColor, 0, 0, 1),
                             vk::Offset3D(0, 0, 0), _image_extent));

    // create barrier to end transfer and only transfer ownership to graphic
    const auto image_end_barrier =
        vk::ImageMemoryBarrier2(_image.get_stage(), _image.get_access(), vk::PipelineStageFlagBits2::eNone,
                                vk::AccessFlagBits2::eNone, _image.get_layout(), _image.get_layout(),
                                _image.get_queue(), _graphic_queue.get_index(), _image.get_image(),
                                vk::ImageSubresourceRange(vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1));
    _image.set_info(image_end_barrier);
    const std::array end_barrier = {image_end_barrier};
    (*transfer_commandbuffer).pipelineBarrier2(vk::DependencyInfo({}, {}, {}, end_barrier));

    // end and submit the transfer commandbuffer
    transfer_commandbuffer.end_record();
    transfer_commandbuffer.submit(false);


    // begin a graphic commandbuffer and add wait for transfer commandbuffer
    vulkan_commandbuffer graphic_commandbuffer2 =
        std::move(vulkan_commandbuffer::create(_device,
                                               vk::CommandBufferAllocateInfo(_graphic_queue.get_command_pool(),
                                                                             vk::CommandBufferLevel::ePrimary, 1),
                                               &_graphic_queue, &_semaphore)
                      .front());
    graphic_commandbuffer2.begin_record(vk::CommandBufferUsageFlagBits::eOneTimeSubmit);
    graphic_commandbuffer2.add_waited_info(transfer_commandbuffer.get_submit_info());

    // create barrier to get the ownership and transition layout for sampler
    const auto image_graphic_barrier2 =
        vk::ImageMemoryBarrier2(_image.get_stage(), _image.get_access(),
                                vk::PipelineStageFlagBits2::eVertexShader | vk::PipelineStageFlagBits2::eFragmentShader
                                    | vk::PipelineStageFlagBits2::eRayTracingShaderKHR,
                                vk::AccessFlagBits2::eShaderRead, _image.get_layout(),
                                vk::ImageLayout::eShaderReadOnlyOptimal, _transfer_queue.get_index(), _image.get_queue(),
                                _image.get_image(), vk::ImageSubresourceRange(vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1));
    _image.set_info(image_graphic_barrier2);
    const std::array graphic_barrier2 = {image_graphic_barrier2};
    (*graphic_commandbuffer2).pipelineBarrier2(vk::DependencyInfo({}, {}, {}, graphic_barrier2));

    // end and submit the graphic commandbuffer
    graphic_commandbuffer2.end_record();
    graphic_commandbuffer2.submit(false);

    const auto submit_info = graphic_commandbuffer2.get_submit_info();

    _recycle_bin.retire(std::move(staging_buffer), "upload image staging buffer.");
    _recycle_bin.retire(std::move(graphic_commandbuffer), "upload image graphic commandbuffer.");
    _recycle_bin.retire(std::move(transfer_commandbuffer), "upload image transfer commandbuffer.");
    _recycle_bin.retire(std::move(graphic_commandbuffer2), "upload image graphic commandbuffer.");

    return submit_info;
}

vk::SemaphoreSubmitInfo vulkan_common::download_image(const vma::raii::Allocator& _allocator,
                                                      const vk::raii::Device&     _device,
                                                      vulkan_recycle_bin&         _recycle_bin,
                                                      vulkan_semaphore&           _semaphore,
                                                      const vulkan_queue&         _graphic_queue,
                                                      const vulkan_queue&         _transfer_queue,
                                                      vulkan_image&               _image,
                                                      vulkan_buffer&              _buffer,
                                                      const std::string&          _buffer_name)
{
    const auto [image_width, image_height]     = _image.get_extent();
    const VkFormat                image_format = static_cast<VkFormat>(_image.get_format());
    const uint32_t                block_size   = vkuFormatTexelBlockSize(image_format);
    const vk::PipelineStageFlags2 old_stage    = _image.get_stage();
    const vk::AccessFlags2        old_access   = _image.get_access();
    const vk::ImageLayout         old_layout   = _image.get_layout();

    // create staging buffer
    const std::array queue_array = {_transfer_queue.get_index()};
    _buffer.create(_allocator, _device,
                   vk::BufferCreateInfo({}, static_cast<size_t>(image_width) * image_height * block_size,
                                        vk::BufferUsageFlagBits::eTransferDst, vk::SharingMode::eExclusive, queue_array),
                   vma::MemoryUsage::eGpuToCpu, _buffer_name);

    // begin a graphic commandbuffer
    vulkan_commandbuffer graphic_commandbuffer =
        std::move(vulkan_commandbuffer::create(_device,
                                               vk::CommandBufferAllocateInfo(_graphic_queue.get_command_pool(),
                                                                             vk::CommandBufferLevel::ePrimary, 1),
                                               &_graphic_queue, &_semaphore)
                      .front());
    graphic_commandbuffer.begin_record(vk::CommandBufferUsageFlagBits::eOneTimeSubmit);

    // create barrier to get the ownership and transition layout for sampler
    const auto image_graphic_barrier =
        vk::ImageMemoryBarrier2(_image.get_stage(), _image.get_access(), vk::PipelineStageFlagBits2::eNone,
                                vk::AccessFlagBits2::eNone, _image.get_layout(), _image.get_layout(),
                                _image.get_queue(), _transfer_queue.get_index(), _image.get_image(),
                                vk::ImageSubresourceRange(vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1));
    _image.set_info(image_graphic_barrier);
    const std::array graphic_barrier = {image_graphic_barrier};
    (*graphic_commandbuffer).pipelineBarrier2(vk::DependencyInfo({}, {}, {}, graphic_barrier));

    // end and submit the graphic commandbuffer
    graphic_commandbuffer.end_record();
    graphic_commandbuffer.submit(false);

    // begin a transfer commandbuffer
    vulkan_commandbuffer transfer_commandbuffer =
        std::move(vulkan_commandbuffer::create(_device,
                                               vk::CommandBufferAllocateInfo(_transfer_queue.get_command_pool(),
                                                                             vk::CommandBufferLevel::ePrimary, 1),
                                               &_transfer_queue, &_semaphore)
                      .front());
    transfer_commandbuffer.begin_record(vk::CommandBufferUsageFlagBits::eOneTimeSubmit);
    transfer_commandbuffer.add_waited_info(graphic_commandbuffer.get_submit_info());

    // create barrier to transfer write and get the ownership
    const auto image_begin_barrier =
        vk::ImageMemoryBarrier2(_image.get_stage(), _image.get_access(), vk::PipelineStageFlagBits2::eTransfer,
                                vk::AccessFlagBits2::eTransferRead, _image.get_layout(),
                                vk::ImageLayout::eTransferSrcOptimal, _graphic_queue.get_index(), _image.get_queue(),
                                _image.get_image(), vk::ImageSubresourceRange(vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1));
    _image.set_info(image_begin_barrier);
    const std::array begin_barrier = {image_begin_barrier};
    (*transfer_commandbuffer).pipelineBarrier2(vk::DependencyInfo({}, {}, {}, begin_barrier));

    // copy image data to buffer
    vulkan_image::copy_image_to_buffer(
        *transfer_commandbuffer, _image.get_image(), _buffer.get_buffer(), vk::ImageLayout::eTransferSrcOptimal,
        vk::BufferImageCopy2(0, 0, 0, vk::ImageSubresourceLayers(vk::ImageAspectFlagBits::eColor, 0, 0, 1),
                             vk::Offset3D(0, 0, 0), vk::Extent3D(image_width, image_height, 1)));

    // create barrier to end transfer and transfer ownership to graphic
    const auto image_end_barrier =
        vk::ImageMemoryBarrier2(_image.get_stage(), _image.get_access(), vk::PipelineStageFlagBits2::eNone,
                                vk::AccessFlagBits2::eNone, _image.get_layout(), _image.get_layout(),
                                _image.get_queue(), _graphic_queue.get_index(), _image.get_image(),
                                vk::ImageSubresourceRange(vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1));
    _image.set_info(image_end_barrier);
    const std::array end_barrier = {image_end_barrier};
    (*transfer_commandbuffer).pipelineBarrier2(vk::DependencyInfo({}, {}, {}, end_barrier));

    // end and submit the transfer commandbuffer
    transfer_commandbuffer.end_record();
    transfer_commandbuffer.submit(false);


    // begin a graphic commandbuffer and add wait for transfer commandbuffer
    vulkan_commandbuffer graphic_commandbuffer2 =
        std::move(vulkan_commandbuffer::create(_device,
                                               vk::CommandBufferAllocateInfo(_graphic_queue.get_command_pool(),
                                                                             vk::CommandBufferLevel::ePrimary, 1),
                                               &_graphic_queue, &_semaphore)
                      .front());
    graphic_commandbuffer2.begin_record(vk::CommandBufferUsageFlagBits::eOneTimeSubmit);
    graphic_commandbuffer2.add_waited_info(transfer_commandbuffer.get_submit_info());

    // create barrier to get the ownership and transition layout for sampler
    const auto image_graphic_barrier2 =
        vk::ImageMemoryBarrier2(_image.get_stage(), _image.get_access(), old_stage, old_access, _image.get_layout(),
                                old_layout, _transfer_queue.get_index(), _image.get_queue(), _image.get_image(),
                                vk::ImageSubresourceRange(vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1));
    _image.set_info(image_graphic_barrier2);
    const std::array graphic_barrier2 = {image_graphic_barrier2};
    (*graphic_commandbuffer2).pipelineBarrier2(vk::DependencyInfo({}, {}, {}, graphic_barrier2));

    // end and submit the graphic commandbuffer
    graphic_commandbuffer2.end_record();
    graphic_commandbuffer2.submit(false);

    const auto submit_info = graphic_commandbuffer2.get_submit_info();

    _recycle_bin.retire(std::move(graphic_commandbuffer), "download image graphic commandbuffer1.");
    _recycle_bin.retire(std::move(transfer_commandbuffer), "download image transfer commandbuffer.");
    _recycle_bin.retire(std::move(graphic_commandbuffer2), "download image graphic commandbuffer2.");

    return submit_info;
}
