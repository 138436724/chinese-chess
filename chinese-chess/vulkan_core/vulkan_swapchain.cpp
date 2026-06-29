#include "vulkan_common.h"
#include "vulkan_swapchain.h"
#include <print>
#include <vulkan/vulkan.hpp>

vulkan_swapchain::vulkan_swapchain(vulkan_swapchain&& _other) noexcept
	:present_queue(std::exchange(_other.present_queue, {})),
	surface_capabilities(std::exchange(_other.surface_capabilities, {})),
	surface(std::exchange(_other.surface, nullptr)),
	swapchain(std::exchange(_other.swapchain, nullptr)),
	format(std::exchange(_other.format, {})),
	extent(std::exchange(_other.extent, {})),
	present_mode(std::exchange(_other.present_mode, {})),
	images(std::exchange(_other.images, {})),
	imageviews(std::exchange(_other.imageviews, {})),
	current_index(std::exchange(_other.current_index, {})),
	max_index(std::exchange(_other.max_index, {})),
	present_used(std::exchange(_other.present_used, {})),
	present_waited(std::exchange(_other.present_waited, {}))
{
}

vulkan_swapchain& vulkan_swapchain::operator=(vulkan_swapchain&& _other) noexcept
{
	if (this != &_other)
	{
		std::ranges::swap(present_queue, _other.present_queue);
		std::ranges::swap(surface_capabilities, _other.surface_capabilities);
		std::ranges::swap(surface, _other.surface);
		std::ranges::swap(swapchain, _other.swapchain);
		std::ranges::swap(format, _other.format);
		std::ranges::swap(extent, _other.extent);
		std::ranges::swap(present_mode, _other.present_mode);
		std::ranges::swap(images, _other.images);
		std::ranges::swap(imageviews, _other.imageviews);
		std::ranges::swap(current_index, _other.current_index);
		std::ranges::swap(max_index, _other.max_index);
		std::ranges::swap(present_used, _other.present_used);
		std::ranges::swap(present_waited, _other.present_waited);
	}
	return *this;
}

void vulkan_swapchain::create(const vk::raii::Instance& _instance, const vk::raii::PhysicalDevice& _physical_device, const vk::raii::Device& _device, vk::SurfaceKHR _surface, uint32_t _present_index, uint32_t _width, uint32_t _height)
{
	surface = vk::raii::SurfaceKHR(_instance, _surface);

	auto available_formats = _physical_device.getSurfaceFormatsKHR(surface);
	const auto format_iter = std::ranges::find_if(available_formats,
		[](const auto& format)
		{
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

	auto available_present_modes = _physical_device.getSurfacePresentModesKHR(surface);
	present_mode = std::ranges::any_of(available_present_modes, [](const vk::PresentModeKHR value) { return vk::PresentModeKHR::eMailbox == value; })
		? vk::PresentModeKHR::eMailbox : vk::PresentModeKHR::eFifo;

	present_queue.create(_device, _present_index);

	recreate(_physical_device, _device, _width, _height);
}

void vulkan_swapchain::recreate(const vk::raii::PhysicalDevice& _physical_device, const vk::raii::Device& _device, uint32_t _width, uint32_t _height)
{
	surface_capabilities = _physical_device.getSurfaceCapabilitiesKHR(surface);

	extent = vk::Extent2D(std::clamp<uint32_t>(_width, surface_capabilities.minImageExtent.width, surface_capabilities.maxImageExtent.width),
		std::clamp<uint32_t>(_height, surface_capabilities.minImageExtent.height, surface_capabilities.maxImageExtent.height));

	max_index = std::max(3u, surface_capabilities.minImageCount);
	vk::SwapchainCreateInfoKHR swapchain_create_info({}, surface, max_index, format,
		vk::ColorSpaceKHR::eSrgbNonlinear, extent, 1, vk::ImageUsageFlagBits::eColorAttachment, vk::SharingMode::eExclusive,
		0, nullptr, surface_capabilities.currentTransform, vk::CompositeAlphaFlagBitsKHR::eOpaque, present_mode,
		vk::True, swapchain, nullptr);

	current_index = max_index - 1;

	swapchain = vk::raii::SwapchainKHR(_device, swapchain_create_info);
	images = swapchain.getImages();

	imageviews.clear();
	present_used.clear();
	present_waited.clear();

	for (const auto& image : images)
	{
		vk::ImageViewCreateInfo viewInfo({}, image, vk::ImageViewType::e2D, format, {}, vk::ImageSubresourceRange(vk::ImageAspectFlagBits::eColor, {}, 1, 0, 1), nullptr);
		imageviews.emplace_back(std::move(vk::raii::ImageView(_device, viewInfo)));

		present_used.emplace_back(std::move(vk::raii::Semaphore(_device, vk::SemaphoreCreateInfo())));

		present_waited.emplace_back(std::move(vk::raii::Semaphore(_device, vk::SemaphoreCreateInfo())));
	}
}

vk::Result vulkan_swapchain::acquire_next_image()
{
	current_index = (current_index + 1) % max_index;
	auto result = swapchain.acquireNextImage(std::numeric_limits<uint64_t>::max(), present_used.at(current_index), nullptr);
	return std::get<0>(static_cast<std::tuple<vk::Result&, uint32_t&>>(result));
}

void vulkan_swapchain::present_image(vulkan_commandbuffer& _commandbuffer, bool _immediately) const
{
	_commandbuffer.add_waited_info({ vk::SemaphoreSubmitInfo(*(present_used.at(current_index)), {}, vk::PipelineStageFlagBits2::eColorAttachmentOutput) });
	_commandbuffer.add_signal_info({ vk::SemaphoreSubmitInfo(*(present_waited.at(current_index)), {}, vk::PipelineStageFlagBits2::eColorAttachmentOutput) });
	_commandbuffer.submit(_immediately);

	try
	{
		vk::Result res = present_queue.get_queue().presentKHR(vk::PresentInfoKHR(*(present_waited.at(current_index)), (*swapchain), current_index, {}));
		if (res != vk::Result::eSuccess)
		{
			throw std::runtime_error("failed to present swap chain image!");
		}
	}
	catch (vk::OutOfDateKHRError e)
	{
#ifndef NDEBUG
		std::println("{}", e.what());
#endif // !NDEBUG
	}
	catch (std::system_error)
	{
		throw std::runtime_error("failed to present swap chain image!");
	}
}

const vulkan_queue& vulkan_swapchain::get_present_queue() const noexcept
{
	return present_queue;
}

const vk::raii::SwapchainKHR& vulkan_swapchain::get_swapchain() const noexcept
{
	return swapchain;
}

vk::Extent2D vulkan_swapchain::get_extent() const noexcept
{
	return extent;
}

vk::Format vulkan_swapchain::get_format() const noexcept
{
	return format;
}

const vk::Image vulkan_swapchain::get_current_image() const noexcept
{
	return images.at(current_index);
}

const vk::raii::ImageView& vulkan_swapchain::get_current_imageview() const noexcept
{
	return imageviews.at(current_index);
}