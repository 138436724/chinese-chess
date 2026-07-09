#include "vulkan_common.h"

#include "vulkan_commandbuffer.h"

vk::SemaphoreSubmitInfo vulkan_common::upload_buffer(const vma::raii::Allocator& _allocator,
                                                     const vk::raii::Device&     _device,
                                                     vulkan_recycle_bin&         _recycle_bin,
                                                     vulkan_semaphore*           _semaphore,
                                                     const vulkan_queue&         _graphic_queue,
                                                     const vulkan_queue&         _transfer_queue,
                                                     vulkan_buffer&              _buffer,
                                                     vk::BufferUsageFlags        _usage,
                                                     const std::span<uint8_t>    _data) noexcept
{
    const std::array queue_array = {_transfer_queue.get_index()};

    // create buffer
    _buffer.create(_allocator, _device,
                   vk::BufferCreateInfo({}, _data.size(), _usage | vk::BufferUsageFlagBits::eTransferDst,
                                        vk::SharingMode::eExclusive, queue_array),
                   vk::MemoryPropertyFlagBits::eDeviceLocal);

    // create staging buffer
    vulkan_buffer staging_buffer;
    staging_buffer.create(_allocator, _device,
                          vk::BufferCreateInfo({}, _data.size(), vk::BufferUsageFlagBits::eTransferSrc,
                                               vk::SharingMode::eExclusive, queue_array),
                          vk::MemoryPropertyFlagBits::eHostVisible | vk::MemoryPropertyFlagBits::eHostCoherent);


    // begin a transfer commandbuffer
    vulkan_commandbuffer transfer_commandbuffer =
        std::move(vulkan_commandbuffer::create(_device,
                                               vk::CommandBufferAllocateInfo(_transfer_queue.get_command_pool(),
                                                                             vk::CommandBufferLevel::ePrimary, 1),
                                               &_transfer_queue, _semaphore)
                      .front());
    transfer_commandbuffer.begin_record(vk::CommandBufferUsageFlagBits::eOneTimeSubmit);

    // create barrier to transfer write
    auto ssbo_begin_barrier =
        vk::BufferMemoryBarrier2(_buffer.get_stage(), _buffer.get_access(), vk::PipelineStageFlagBits2::eTransfer,
                                 vk::AccessFlagBits2::eTransferWrite, vk::QueueFamilyIgnored, vk::QueueFamilyIgnored,
                                 _buffer.get_buffer(), 0, vk::WholeSize);
    _buffer.set_info(ssbo_begin_barrier);
    const std::array begin_barrier = {ssbo_begin_barrier};
    (*transfer_commandbuffer).pipelineBarrier2(vk::DependencyInfo({}, {}, begin_barrier, {}));

    // copy data and to buffer
    memcpy(staging_buffer.get_buffer_address().hostAddress, _data.data(), _data.size());
    vulkan_buffer::copy_buffer_to_buffer(*transfer_commandbuffer, staging_buffer.get_buffer(), _buffer.get_buffer(),
                                         vk::BufferCopy2(0, 0, _data.size()));

    // create barrier to end transfer and ready to graphic
    auto ssbo_transfer_barrier =
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
                                               &_graphic_queue, _semaphore)
                      .front());
    graphic_commandbuffer.add_waited_info({transfer_commandbuffer.get_submit_info()});
    graphic_commandbuffer.begin_record(vk::CommandBufferUsageFlagBits::eOneTimeSubmit);

    // create barrier to graphic read
    auto ssbo_graphic_barrier =
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

    auto submit_info = graphic_commandbuffer.get_submit_info();

    _recycle_bin.retire(std::move(staging_buffer));
    _recycle_bin.retire(std::move(transfer_commandbuffer));
    _recycle_bin.retire(std::move(graphic_commandbuffer));

    return submit_info;
}

vk::SemaphoreSubmitInfo vulkan_common::upload_image(const vma::raii::Allocator&              _allocator,
                                                    const vk::raii::Device&                  _device,
                                                    vulkan_recycle_bin&                      _recycle_bin,
                                                    vulkan_semaphore*                        _semaphore,
                                                    const vulkan_queue&                      _graphic_queue,
                                                    const vulkan_queue&                      _transfer_queue,
                                                    vk::ImageType                            _image_type,
                                                    vk::ImageViewType                        _image_view_type,
                                                    vk::Format                               _image_format,
                                                    const vk::Extent3D&                      _image_extent,
                                                    vulkan_image&                            _image,
                                                    const std::span<uint8_t>                 _data,
                                                    const std::vector<vk::BufferImageCopy2>& _copy_info) noexcept
{
    const std::array queue_array = {_transfer_queue.get_index()};

    // create image
    vk::ImageCreateInfo image_info({}, _image_type, _image_format, _image_extent, 1, 1, vk::SampleCountFlagBits::e1,
                                   vk::ImageTiling::eOptimal, vk::ImageUsageFlagBits::eSampled | vk::ImageUsageFlagBits::eTransferDst,
                                   vk::SharingMode::eExclusive, queue_array);
    vk::ImageViewCreateInfo view_info({}, {}, _image_view_type, _image_format, {},
                                      vk::ImageSubresourceRange(vk::ImageAspectFlagBits::eColor, {}, 1, 0, 1), nullptr);
    _image.create(_allocator, _device, image_info, view_info, vk::MemoryPropertyFlagBits::eDeviceLocal,
                  vk::ClearColorValue(0.f, 0.f, 0.f, 1.f));

    // create staging buffer
    vulkan_buffer staging_buffer;
    staging_buffer.create(_allocator, _device,
                          vk::BufferCreateInfo({}, _data.size(), vk::BufferUsageFlagBits::eTransferSrc,
                                               vk::SharingMode::eExclusive, queue_array),
                          vk::MemoryPropertyFlagBits::eHostVisible | vk::MemoryPropertyFlagBits::eHostCoherent);
    memcpy(staging_buffer.get_buffer_address().hostAddress, _data.data(), _data.size());


    // begin a transfer commandbuffer
    vulkan_commandbuffer transfer_commandbuffer =
        std::move(vulkan_commandbuffer::create(_device,
                                               vk::CommandBufferAllocateInfo(_transfer_queue.get_command_pool(),
                                                                             vk::CommandBufferLevel::ePrimary, 1),
                                               &_transfer_queue, _semaphore)
                      .front());
    transfer_commandbuffer.begin_record(vk::CommandBufferUsageFlagBits::eOneTimeSubmit);

    // create barrier to transfer write
    auto image_begin_barrier =
        vk::ImageMemoryBarrier2(_image.get_stage(), _image.get_access(), vk::PipelineStageFlagBits2::eTransfer,
                                vk::AccessFlagBits2::eTransferWrite, _image.get_layout(),
                                vk::ImageLayout::eTransferDstOptimal, vk::QueueFamilyIgnored, vk::QueueFamilyIgnored,
                                _image.get_image(), vk::ImageSubresourceRange(vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1));
    _image.set_info(image_begin_barrier);
    const std::array begin_barrier = {image_begin_barrier};
    (*transfer_commandbuffer).pipelineBarrier2(vk::DependencyInfo({}, {}, {}, begin_barrier));


    // copy data to image
    if (_copy_info.empty())
    {
        vulkan_buffer::copy_buffer_to_image(
            *transfer_commandbuffer, staging_buffer.get_buffer(), _image.get_image(),
            vk::BufferImageCopy2(0, 0, 0, vk::ImageSubresourceLayers(vk::ImageAspectFlagBits::eColor, 0, 0, 1),
                                 vk::Offset3D(0, 0, 0), _image_extent));
    }
    else
    {
        std::ranges::for_each(_copy_info, [&](const auto& _info) {
            vulkan_buffer::copy_buffer_to_image(*transfer_commandbuffer, staging_buffer.get_buffer(), _image.get_image(), _info);
        });
    }

    // create barrier to end transfer and only transfer ownership to graphic
    auto image_end_barrier = vk::ImageMemoryBarrier2(_image.get_stage(), _image.get_access(), vk::PipelineStageFlagBits2::eNone,
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
    vulkan_commandbuffer graphic_commandbuffer =
        std::move(vulkan_commandbuffer::create(_device,
                                               vk::CommandBufferAllocateInfo(_graphic_queue.get_command_pool(),
                                                                             vk::CommandBufferLevel::ePrimary, 1),
                                               &_graphic_queue, _semaphore)
                      .front());
    graphic_commandbuffer.add_waited_info({transfer_commandbuffer.get_submit_info()});
    graphic_commandbuffer.begin_record(vk::CommandBufferUsageFlagBits::eOneTimeSubmit);

    // create barrier to get the ownership and transition layout for sampler
    auto image_graphic_barrier =
        vk::ImageMemoryBarrier2(_image.get_stage(), _image.get_access(),
                                vk::PipelineStageFlagBits2::eVertexShader | vk::PipelineStageFlagBits2::eFragmentShader
                                    | vk::PipelineStageFlagBits2::eRayTracingShaderKHR,
                                vk::AccessFlagBits2::eShaderRead, _image.get_layout(),
                                vk::ImageLayout::eShaderReadOnlyOptimal, _transfer_queue.get_index(), _image.get_queue(),
                                _image.get_image(), vk::ImageSubresourceRange(vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1));
    _image.set_info(image_graphic_barrier);
    const std::array graphic_barrier = {image_graphic_barrier};
    (*graphic_commandbuffer).pipelineBarrier2(vk::DependencyInfo({}, {}, {}, graphic_barrier));

    // end and submit the graphic commandbuffer
    graphic_commandbuffer.end_record();
    graphic_commandbuffer.submit(false);

    auto submit_info = graphic_commandbuffer.get_submit_info();

    _recycle_bin.retire(std::move(staging_buffer));
    _recycle_bin.retire(std::move(transfer_commandbuffer));
    _recycle_bin.retire(std::move(graphic_commandbuffer));

    return submit_info;
}
