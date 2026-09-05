#include "vulkan_swapchain.h"

#include "vulkan_common.h"

#include <algorithm>
#include <limits>
#include <print>
#include <utility>
#include <vulkan/vulkan.hpp>

vulkan_swapchain::vulkan_swapchain(vulkan_swapchain&& _other) noexcept
    : surface_capabilities(std::exchange(_other.surface_capabilities, {}))
    , format(std::exchange(_other.format, {}))
    , extent(std::exchange(_other.extent, {}))
    , present_mode(std::exchange(_other.present_mode, {}))
    , surface(std::exchange(_other.surface, nullptr))
    , swapchain(std::exchange(_other.swapchain, nullptr))
    , images(std::exchange(_other.images, {}))
    , imageviews(std::exchange(_other.imageviews, {}))
    , current_index(std::exchange(_other.current_index, {}))
    , current_frame(std::exchange(_other.current_frame, {}))
    , present_queue(std::exchange(_other.present_queue, {}))
    , before_rendering(std::exchange(_other.before_rendering, {}))
    , after_rendering(std::exchange(_other.after_rendering, {}))
{
}

vulkan_swapchain& vulkan_swapchain::operator=(vulkan_swapchain&& _other) noexcept
{
    if (this != &_other)
    {
        std::ranges::swap(surface_capabilities, _other.surface_capabilities);
        std::ranges::swap(format, _other.format);
        std::ranges::swap(extent, _other.extent);
        std::ranges::swap(present_mode, _other.present_mode);
        std::ranges::swap(surface, _other.surface);
        std::ranges::swap(swapchain, _other.swapchain);
        std::ranges::swap(images, _other.images);
        std::ranges::swap(imageviews, _other.imageviews);
        std::ranges::swap(current_index, _other.current_index);
        std::ranges::swap(current_frame, _other.current_frame);
        std::ranges::swap(present_queue, _other.present_queue);
        std::ranges::swap(before_rendering, _other.before_rendering);
        std::ranges::swap(after_rendering, _other.after_rendering);
    }
    return *this;
}

void vulkan_swapchain::create(const vk::raii::Instance&       _instance,
                              const vk::raii::PhysicalDevice& _physical_device,
                              const vk::raii::Device&         _device,
                              vk::SurfaceKHR                  _surface,
                              uint32_t                        _present_index,
                              uint32_t                        _width,
                              uint32_t                        _height)
{
    surface = vk::raii::SurfaceKHR(_instance, _surface);

    const auto available_formats = _physical_device.getSurfaceFormatsKHR(surface);
    const auto format_iter       = std::ranges::find_if(available_formats, [](const auto& format) static {
        if constexpr (vulkan_common::USE_OCIO)
        {
            return format.format == vk::Format::eB8G8R8A8Unorm && format.colorSpace == vk::ColorSpaceKHR::eSrgbNonlinear;
        }
        else
        {
            return format.format == vk::Format::eB8G8R8A8Srgb && format.colorSpace == vk::ColorSpaceKHR::eSrgbNonlinear;
        }
    });
    format = format_iter != available_formats.end() ? format_iter->format : available_formats.front().format;

    const auto available_present_modes = _physical_device.getSurfacePresentModesKHR(surface);
    present_mode =
        std::ranges::any_of(available_present_modes,
                            [](const vk::PresentModeKHR value) static { return vk::PresentModeKHR::eMailbox == value; }) ?
            vk::PresentModeKHR::eMailbox :
            vk::PresentModeKHR::eFifo;

    present_queue.create(_device, _present_index);

    recreate(_physical_device, _device, _width, _height);
}

void vulkan_swapchain::recreate(const vk::raii::PhysicalDevice& _physical_device, const vk::raii::Device& _device, uint32_t _width, uint32_t _height)
{
    surface_capabilities = _physical_device.getSurfaceCapabilitiesKHR(surface);

    extent = vk::Extent2D(
        std::clamp<uint32_t>(_width, surface_capabilities.minImageExtent.width, surface_capabilities.maxImageExtent.width),
        std::clamp<uint32_t>(_height, surface_capabilities.minImageExtent.height, surface_capabilities.maxImageExtent.height));

    const auto requested_count = std::max(vulkan_common::MAX_FRAMES_IN_FLIGHT, surface_capabilities.minImageCount);
    const auto image_count     = surface_capabilities.maxImageCount == 0 ?
                                     requested_count :
                                     std::min(requested_count, surface_capabilities.maxImageCount);
    const vk::SwapchainCreateInfoKHR swapchain_create_info(
        {}, surface, image_count, format, vk::ColorSpaceKHR::eSrgbNonlinear, extent, 1,
        vk::ImageUsageFlagBits::eColorAttachment, vk::SharingMode::eExclusive, 0, nullptr, surface_capabilities.currentTransform,
        vk::CompositeAlphaFlagBitsKHR::eOpaque, present_mode, vk::True, swapchain, nullptr);

    swapchain = vk::raii::SwapchainKHR(_device, swapchain_create_info);
    images    = swapchain.getImages();

    imageviews.clear();
    before_rendering.clear();
    after_rendering.clear();

    for (const auto& image : images)
    {
        const vk::ImageViewCreateInfo viewInfo({}, image, vk::ImageViewType::e2D, format, {},
                                               vk::ImageSubresourceRange(vk::ImageAspectFlagBits::eColor, {}, 1, 0, 1), nullptr);

        imageviews.emplace_back(vk::raii::ImageView(_device, viewInfo));

        before_rendering.emplace_back(vk::raii::Semaphore(_device, vk::SemaphoreCreateInfo()));

        after_rendering.emplace_back(vk::raii::Semaphore(_device, vk::SemaphoreCreateInfo()));
    }
}

bool vulkan_swapchain::acquire_image()
{
    try
    {
        current_frame = (current_frame + 1) % static_cast<uint32_t>(images.size());
        const auto [result, index] =
            swapchain.acquireNextImage(std::numeric_limits<uint64_t>::max(), before_rendering.at(current_frame), nullptr);
        current_index = index;
        (void)result;  // vulkan will check result and throw exception
        return true;
    }
    catch (vk::OutOfDateKHRError e)
    {
#ifndef NDEBUG
        std::println("acquire image: {}", e.what());
#endif  // !NDEBUG
        return false;
    }
    catch (std::system_error)
    {
        throw std::runtime_error("failed to acquire swap chain image!");
    }
}

void vulkan_swapchain::present_image()
{
    try
    {
        const vk::Result result = present_queue.get_queue().presentKHR(
            vk::PresentInfoKHR(*(after_rendering.at(current_frame)), (*swapchain), current_index, {}));
        (void)result;  // vulkan will check result and throw exception
        return;
    }
    catch (vk::OutOfDateKHRError e)
    {
#ifndef NDEBUG
        std::println("present image: {}", e.what());
#endif  // !NDEBUG
        return;
    }
    catch (std::system_error)
    {
        throw std::runtime_error("failed to present swap chain image!");
    }
}

vk::Format vulkan_swapchain::get_format() const noexcept
{
    return format;
}

vk::Extent2D vulkan_swapchain::get_extent() const noexcept
{
    return extent;
}

const vk::raii::SwapchainKHR& vulkan_swapchain::get_swapchain() const noexcept
{
    return swapchain;
}

vk::Image vulkan_swapchain::get_current_image() const noexcept
{
    return images.at(current_index);
}

const vk::raii::ImageView& vulkan_swapchain::get_current_imageview() const noexcept
{
    return imageviews.at(current_index);
}

const vulkan_queue& vulkan_swapchain::get_present_queue() const noexcept
{
    return present_queue;
}

vk::SemaphoreSubmitInfo vulkan_swapchain::get_waited_info() const noexcept
{
    return vk::SemaphoreSubmitInfo(*(before_rendering.at(current_frame)), {}, vk::PipelineStageFlagBits2::eColorAttachmentOutput);
}

vk::SemaphoreSubmitInfo vulkan_swapchain::get_signal_info() const noexcept
{
    return vk::SemaphoreSubmitInfo(*(after_rendering.at(current_frame)), {}, vk::PipelineStageFlagBits2::eColorAttachmentOutput);
}
