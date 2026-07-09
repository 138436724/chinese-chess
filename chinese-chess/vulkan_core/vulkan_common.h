#pragma once

#include "vulkan_buffer.h"
#include "vulkan_image.h"
#include "vulkan_queue.h"
#include "vulkan_recycle_bin.h"
#include "vulkan_semaphore.h"

#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/type_ptr.hpp>
#include <ranges>
#include <span>
#include <vulkan-memory-allocator-hpp/vk_mem_alloc_raii.hpp>

class vulkan_common
{
public:
    inline static vk::SampleCountFlagBits MSAA_SAMPLE_COUNT    = vk::SampleCountFlagBits::e1;
    inline static vk::Format              DEPTH_FORMAT         = vk::Format::eUndefined;
    inline static constexpr uint32_t      MAX_FRAMES_IN_FLIGHT = 3;
    inline static constexpr bool          USE_OCIO             = true;

    inline static std::optional<vk::Format> find_supported_format(const vk::raii::PhysicalDevice& _physical_device,
                                                                  const std::vector<vk::Format>&  _candidates,
                                                                  vk::ImageTiling                 _tiling,
                                                                  vk::FormatFeatureFlags          _features) noexcept
    {
        auto format_iter = std::ranges::find_if(_candidates, [&](const auto& format) {
            vk::FormatProperties props = _physical_device.getFormatProperties(format);
            return (_tiling == vk::ImageTiling::eLinear && (props.linearTilingFeatures & _features) == _features)
                   || (_tiling == vk::ImageTiling::eOptimal && (props.optimalTilingFeatures & _features) == _features);
        });

        return format_iter == _candidates.end() ? std::nullopt : std::optional<vk::Format>(*format_iter);
    }

    inline static auto align_up(auto value, size_t alignment) noexcept
    {
        return ((value + alignment - 1) & ~(alignment - 1));
    };

    // VkTransformMatrixKHR is row-major 3x4, glm::mat4 is column-major; transpose before memcpy.
    inline static auto glm_matrix_to_vulkan(const glm::mat4& m) noexcept
    {
        vk::TransformMatrixKHR t;
        memcpy(&t, glm::value_ptr(glm::transpose(m)), sizeof(t));
        return t;
    };

    static vk::SemaphoreSubmitInfo upload_buffer(const vma::raii::Allocator& _allocator,
                                                 const vk::raii::Device&     _device,
                                                 vulkan_recycle_bin&         _recycle_bin,
                                                 vulkan_semaphore*           _semaphore,
                                                 const vulkan_queue&         _graphic_queue,
                                                 const vulkan_queue&         _transfer_queue,
                                                 vulkan_buffer&              _buffer,
                                                 vk::BufferUsageFlags        _usage,
                                                 const std::span<uint8_t>    _data) noexcept;

    static vk::SemaphoreSubmitInfo upload_image(const vma::raii::Allocator&              _allocator,
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
                                                const std::vector<vk::BufferImageCopy2>& _copy_info) noexcept;
};
