/**
 * @file vulkan_common.cpp
 * @brief vulkan_common 传输辅助的实现：一次性命令缓冲、跨队列族交接与延迟销毁。
 * @details upload_buffer / upload_image / download_image 各自创建 staging 与命令缓冲，
 *          提交成功后才把中间资源交给 vulkan_recycle_bin；跨队列族时按 release/acquire 两段式交接。
 *          vulkan_submission_scope 在异常展开时先停提交再排空 token，避免半提交状态继续写资源。
 */
#include "vulkan_common.h"

#include "vulkan_buffer.h"
#include "vulkan_commandbuffer.h"
#include "vulkan_image.h"
#include "vulkan_queue.h"
#include "vulkan_recycle_bin.h"
#include "vulkan_submission_scope.h"

#include <array>
#include <cstdint>
#include <cstring>
#include <format>
#include <span>
#include <stdexcept>
#include <string>
#include <utility>
#include <vulkan/utility/vk_format_utils.h>

namespace {
constexpr vk::DeviceSize UPLOAD_BUFFER_INLINE_MAX = 64u * 1024u;

vulkan_commandbuffer make_commands(const vk::raii::Device& device, const vulkan_queue& queue)
{
    return std::move(vulkan_commandbuffer::create(
        device, vk::CommandBufferAllocateInfo(queue.get_command_pool(), vk::CommandBufferLevel::ePrimary, 1), &queue).front());
}

void buffer_barrier(const vulkan_commandbuffer& commands, const vk::BufferMemoryBarrier2& barrier)
{
    (*commands).pipelineBarrier2(vk::DependencyInfo({}, {}, barrier, {}));
}

void image_barrier(const vulkan_commandbuffer& commands, const vk::ImageMemoryBarrier2& barrier)
{
    (*commands).pipelineBarrier2(vk::DependencyInfo({}, {}, {}, barrier));
}

// A QFO acquire must repeat the release's family indices AND old/new layouts.
// Resource state tracking has already advanced to the release's target state.
vk::ImageMemoryBarrier2 acquire_image(vulkan_image& image, const vk::ImageMemoryBarrier2& release,
                                      vk::PipelineStageFlags2 stages, vk::AccessFlags2 access)
{
    auto acquire = image.transition_state(stages, access, release.newLayout);
    acquire.setOldLayout(release.oldLayout)
        .setSrcQueueFamilyIndex(release.srcQueueFamilyIndex)
        .setDstQueueFamilyIndex(release.dstQueueFamilyIndex);
    return acquire;
}
} // namespace

/**
 * @brief 实现上传：小数据走内联更新，大数据走 staging 加 transfer 队列复制，并完成所有权交接。
 * @note 所有局部资源都在 vulkan_submission_scope 之前构造，异常展开时先停提交再排空 token。
 * @note 旧缓冲、staging 与命令缓冲都按各自提交的 token 进入回收站，GPU 用完才真正销毁。
 */
vulkan_submission_token vulkan_common::upload_buffer(const vma::raii::Allocator& _allocator,
                                               const vk::raii::Device& _device,
                                               vulkan_recycle_bin& _recycle_bin,
                                               const vulkan_queue& _owner_queue,
                                               const vulkan_queue& _transfer_queue,
                                               vulkan_buffer& _buffer,
                                               vk::BufferUsageFlags _usage,
                                               vk::PipelineStageFlags2 _consumer_stages,
                                               vk::AccessFlags2 _consumer_access,
                                               const std::span<const uint8_t> _data,
                                               const std::string& _buffer_name)
{
    if (_data.empty()) return {};

    const vk::DeviceSize data_size = _data.size();
    const bool inline_upload = data_size <= UPLOAD_BUFFER_INLINE_MAX && data_size % 4 == 0;
    const bool new_buffer = _buffer.get_size() < data_size;
    const bool distinct_families = _owner_queue.get_index() != _transfer_queue.get_index();

    // All owning locals precede the unwind guard.
    vulkan_buffer staging;
    vulkan_commandbuffer owner_release;
    auto transfer = make_commands(_device, inline_upload ? _owner_queue : _transfer_queue);
    vulkan_commandbuffer owner_acquire;
    const vulkan_submission_scope failure_scope(_owner_queue, _transfer_queue);

    if (!inline_upload && distinct_families)
    {
        if (!new_buffer) owner_release = make_commands(_device, _owner_queue);
        owner_acquire = make_commands(_device, _owner_queue);
    }

    if (new_buffer)
    {
        _recycle_bin.retire_after_submitted(std::move(_buffer), std::format("{} old buffer.", _buffer_name));
        const std::array families{inline_upload ? _owner_queue.get_index() : _transfer_queue.get_index()};
        _buffer.create(_allocator, _device,
                       vk::BufferCreateInfo({}, data_size, _usage | vk::BufferUsageFlagBits::eTransferDst,
                                            vk::SharingMode::eExclusive, families),
                       vma::MemoryUsage::eAutoPreferDevice, {}, _buffer_name);
    }

    transfer.begin_record(vk::CommandBufferUsageFlagBits::eOneTimeSubmit);
    if (!new_buffer && !inline_upload && distinct_families)
    {
        if (_buffer.get_queue() != _owner_queue.get_index())
            throw std::runtime_error("Upload requires the previous consumer to own the buffer.");

        owner_release.begin_record(vk::CommandBufferUsageFlagBits::eOneTimeSubmit);
        const auto release = _buffer.transition_state(vk::PipelineStageFlagBits2::eNone, {},
                                                       _transfer_queue.get_index());
        buffer_barrier(owner_release, release);
        owner_release.end_record();
        const auto released = owner_release.submit();
        transfer.add_dependency(released);

        auto acquire = _buffer.transition_state(vk::PipelineStageFlagBits2::eTransfer, vk::AccessFlagBits2::eTransferWrite);
        acquire.setSrcQueueFamilyIndex(release.srcQueueFamilyIndex).setDstQueueFamilyIndex(release.dstQueueFamilyIndex);
        buffer_barrier(transfer, acquire);
        _recycle_bin.retire(std::move(owner_release), released, "buffer owner release");
    }
    else
    {
        // Also covers queue aliases; their timeline and submission lock are shared.
        if (!new_buffer) transfer.add_dependency(_owner_queue.get_latest_token());
        buffer_barrier(transfer, _buffer.transition_state(vk::PipelineStageFlagBits2::eTransfer,
                                                          vk::AccessFlagBits2::eTransferWrite));
    }

    if (inline_upload)
    {
        (*transfer).updateBuffer<uint8_t>(*_buffer.get_buffer(), 0, _data);
    }
    else
    {
        const std::array families{_transfer_queue.get_index()};
        staging.create(_allocator, _device,
                       vk::BufferCreateInfo({}, data_size, vk::BufferUsageFlagBits::eTransferSrc,
                                            vk::SharingMode::eExclusive, families),
                       vma::MemoryUsage::eAuto, vma::AllocationCreateFlagBits::eHostAccessSequentialWrite,
                       std::format("staging {}", _buffer_name));
        std::memcpy(staging.get_buffer_address().hostAddress, _data.data(), _data.size());
        staging.flush();
        vulkan_buffer::copy_buffer_to_buffer(*transfer, staging.get_buffer(), _buffer.get_buffer(),
                                             vk::BufferCopy2(0, 0, data_size));
    }

    vk::BufferMemoryBarrier2 release;
    if (!inline_upload && distinct_families)
    {
        release = _buffer.transition_state(vk::PipelineStageFlagBits2::eNone, {}, _owner_queue.get_index());
        buffer_barrier(transfer, release);
    }
    else
    {
        buffer_barrier(transfer, _buffer.transition_state(_consumer_stages, _consumer_access));
    }
    transfer.end_record();
    const auto transferred = transfer.submit();
    if (!inline_upload) _recycle_bin.retire(std::move(staging), transferred, "buffer staging");
    _recycle_bin.retire(std::move(transfer), transferred, "buffer transfer commands");

    if (inline_upload || !distinct_families) return transferred;

    owner_acquire.begin_record(vk::CommandBufferUsageFlagBits::eOneTimeSubmit);
    owner_acquire.add_dependency(transferred);
    auto acquire = _buffer.transition_state(_consumer_stages, _consumer_access);
    acquire.setSrcQueueFamilyIndex(release.srcQueueFamilyIndex).setDstQueueFamilyIndex(release.dstQueueFamilyIndex);
    buffer_barrier(owner_acquire, acquire);
    owner_acquire.end_record();
    const auto completed = owner_acquire.submit();
    _recycle_bin.retire(std::move(owner_acquire), completed, "buffer owner acquire");
    return completed;
}

/**
 * @brief 实现图像上传：建图像与 staging，复制后转入 eShaderReadOnlyOptimal 并替换旧图像。
 * @note transfer 侧先做布局转换与 release，再按队列族是否相同决定 owner 侧是否需要 acquire。
 * @note 空数据在入口直接抛 std::invalid_argument，避免创建出尺寸为 0 的图像。
 */
vulkan_submission_token vulkan_common::upload_image(const vma::raii::Allocator& _allocator,
                                              const vk::raii::Device& _device,
                                              vulkan_recycle_bin& _recycle_bin,
                                              const vulkan_queue& _owner_queue,
                                              const vulkan_queue& _transfer_queue,
                                              vk::ImageType _image_type,
                                              vk::ImageViewType _image_view_type,
                                              vk::Format _image_format,
                                              const vk::Extent3D& _image_extent,
                                              vulkan_image& _image,
                                              vk::PipelineStageFlags2 _consumer_stages,
                                              vk::AccessFlags2 _consumer_access,
                                              const std::span<const uint8_t> _data,
                                              const std::string& _image_name)
{
    if (_data.empty()) throw std::invalid_argument("Cannot upload an empty image.");
    const bool distinct_families = _owner_queue.get_index() != _transfer_queue.get_index();
    vulkan_image image;
    vulkan_buffer staging;
    auto transfer = make_commands(_device, _transfer_queue);
    vulkan_commandbuffer owner;
    const vulkan_submission_scope failure_scope(_owner_queue, _transfer_queue);
    if (distinct_families) owner = make_commands(_device, _owner_queue);

    const std::array families{_transfer_queue.get_index()};
    const vk::ImageCreateInfo image_info({}, _image_type, _image_format, _image_extent, 1, 1,
                                         vk::SampleCountFlagBits::e1, vk::ImageTiling::eOptimal,
                                         vk::ImageUsageFlagBits::eSampled | vk::ImageUsageFlagBits::eTransferDst
                                             | vk::ImageUsageFlagBits::eTransferSrc,
                                         vk::SharingMode::eExclusive, families);
    const vk::ImageViewCreateInfo view_info({}, {}, _image_view_type, _image_format, {},
                                            vk::ImageSubresourceRange(vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1));
    image.create(_allocator, _device, image_info, view_info, vma::MemoryUsage::eAutoPreferDevice, {},
                 vk::ClearColorValue(0.f, 0.f, 0.f, 1.f), _image_name);
    staging.create(_allocator, _device,
                   vk::BufferCreateInfo({}, _data.size(), vk::BufferUsageFlagBits::eTransferSrc,
                                        vk::SharingMode::eExclusive, families),
                   vma::MemoryUsage::eAuto, vma::AllocationCreateFlagBits::eHostAccessSequentialWrite,
                   std::format("staging {}", _image_name));
    std::memcpy(staging.get_buffer_address().hostAddress, _data.data(), _data.size());
    staging.flush();

    transfer.begin_record(vk::CommandBufferUsageFlagBits::eOneTimeSubmit);
    image_barrier(transfer, image.transition_state(vk::PipelineStageFlagBits2::eTransfer,
                                                   vk::AccessFlagBits2::eTransferWrite, vk::ImageLayout::eTransferDstOptimal));
    vulkan_buffer::copy_buffer_to_image(
        *transfer, staging.get_buffer(), image.get_image(),
        vk::BufferImageCopy2(0, 0, 0, vk::ImageSubresourceLayers(vk::ImageAspectFlagBits::eColor, 0, 0, 1),
                            vk::Offset3D(0, 0, 0), _image_extent));

    const auto release = image.transition_state(
        distinct_families ? vk::PipelineStageFlags2{} : _consumer_stages,
        distinct_families ? vk::AccessFlags2{} : _consumer_access,
        vk::ImageLayout::eShaderReadOnlyOptimal, _owner_queue.get_index());
    image_barrier(transfer, release);
    transfer.end_record();
    const auto transferred = transfer.submit();
    _recycle_bin.retire(std::move(staging), transferred, "image staging");
    _recycle_bin.retire(std::move(transfer), transferred, "image transfer commands");

    auto completed = transferred;
    if (distinct_families)
    {
        owner.begin_record(vk::CommandBufferUsageFlagBits::eOneTimeSubmit);
        owner.add_dependency(transferred);
        image_barrier(owner, acquire_image(image, release, _consumer_stages, _consumer_access));
        owner.end_record();
        completed = owner.submit();
        _recycle_bin.retire(std::move(owner), completed, "image owner acquire");
    }

    _recycle_bin.retire_after_submitted(std::move(_image), "replaced uploaded image");
    _image = std::move(image);
    return completed;
}

/**
 * @brief 实现回读：owner release 后由 transfer 复制到 host 可见缓冲，再把图像恢复原状态。
 * @note 第一段收 _waited_info 作为依赖，保证复制读到的是生产者提交之后的内容。
 * @note 恢复用的 old_stage/old_access/old_layout 在入口快照，跨族与同族两条路径共用。
 */
vulkan_submission_token vulkan_common::download_image(const vma::raii::Allocator& _allocator,
                                                const vk::raii::Device& _device,
                                                vulkan_recycle_bin& _recycle_bin,
                                                const vulkan_queue& _owner_queue,
                                                const vulkan_queue& _transfer_queue,
                                                vulkan_submission_token _waited_info,
                                                vulkan_image& _image,
                                                vulkan_buffer& _buffer,
                                                const std::string& _buffer_name)
{
    const auto [width, height] = _image.get_extent();
    const auto old_stage = _image.get_stage();
    const auto old_access = _image.get_access();
    const auto old_layout = _image.get_layout();
    const bool distinct_families = _owner_queue.get_index() != _transfer_queue.get_index();
    const auto bytes = static_cast<size_t>(width) * height * vkuFormatTexelBlockSize(static_cast<VkFormat>(_image.get_format()));

    vulkan_buffer readback;
    auto owner_release = make_commands(_device, _owner_queue);
    auto transfer = make_commands(_device, _transfer_queue);
    auto owner_acquire = make_commands(_device, _owner_queue);
    const vulkan_submission_scope failure_scope(_owner_queue, _transfer_queue);

    const std::array families{_transfer_queue.get_index()};
    readback.create(_allocator, _device,
                    vk::BufferCreateInfo({}, bytes, vk::BufferUsageFlagBits::eTransferDst,
                                         vk::SharingMode::eExclusive, families),
                    vma::MemoryUsage::eAutoPreferHost, vma::AllocationCreateFlagBits::eHostAccessRandom, _buffer_name);

    owner_release.begin_record(vk::CommandBufferUsageFlagBits::eOneTimeSubmit);
    owner_release.add_dependency(_waited_info);
    const auto release = _image.transition_state({}, {}, vk::ImageLayout::eTransferSrcOptimal, _transfer_queue.get_index());
    image_barrier(owner_release, release);
    owner_release.end_record();
    const auto released = owner_release.submit();
    _recycle_bin.retire(std::move(owner_release), released, "readback owner release");

    transfer.begin_record(vk::CommandBufferUsageFlagBits::eOneTimeSubmit);
    transfer.add_dependency(released);
    const auto acquire = distinct_families
        ? acquire_image(_image, release, vk::PipelineStageFlagBits2::eTransfer, vk::AccessFlagBits2::eTransferRead)
        : _image.transition_state(vk::PipelineStageFlagBits2::eTransfer, vk::AccessFlagBits2::eTransferRead,
                                   vk::ImageLayout::eTransferSrcOptimal);
    image_barrier(transfer, acquire);
    buffer_barrier(transfer, readback.transition_state(vk::PipelineStageFlagBits2::eTransfer, vk::AccessFlagBits2::eTransferWrite));
    vulkan_image::copy_image_to_buffer(
        *transfer, _image.get_image(), readback.get_buffer(), vk::ImageLayout::eTransferSrcOptimal,
        vk::BufferImageCopy2(0, 0, 0, vk::ImageSubresourceLayers(vk::ImageAspectFlagBits::eColor, 0, 0, 1),
                            vk::Offset3D(0, 0, 0), vk::Extent3D(width, height, 1)));
    buffer_barrier(transfer, readback.transition_state(vk::PipelineStageFlagBits2::eHost, vk::AccessFlagBits2::eHostRead));
    const auto return_release = _image.transition_state({}, {}, old_layout, _owner_queue.get_index());
    image_barrier(transfer, return_release);
    transfer.end_record();
    const auto transferred = transfer.submit();
    _recycle_bin.retire(std::move(transfer), transferred, "readback transfer commands");

    owner_acquire.begin_record(vk::CommandBufferUsageFlagBits::eOneTimeSubmit);
    owner_acquire.add_dependency(transferred);
    const auto return_acquire = distinct_families
        ? acquire_image(_image, return_release, old_stage, old_access)
        : _image.transition_state(old_stage, old_access, old_layout);
    image_barrier(owner_acquire, return_acquire);
    owner_acquire.end_record();
    const auto completed = owner_acquire.submit();
    _recycle_bin.retire(std::move(owner_acquire), completed, "readback owner acquire");
    _recycle_bin.retire_after_submitted(std::move(_buffer), "replaced readback buffer");
    _buffer = std::move(readback);
    return completed;
}
