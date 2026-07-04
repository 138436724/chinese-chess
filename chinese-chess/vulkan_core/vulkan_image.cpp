#include "vulkan_image.h"

vulkan_image::vulkan_image(vulkan_image&& _other) noexcept
	:format(std::exchange(_other.format, {})),
	extent(std::exchange(_other.extent, {})),
	layout(std::exchange(_other.layout, {})),
	clear_value(std::exchange(_other.clear_value, {})),
	image(std::exchange(_other.image, nullptr)),
	imageview(std::exchange(_other.imageview, nullptr))
{
}

vulkan_image& vulkan_image::operator=(vulkan_image&& _other) noexcept
{
	if (this != &_other)
	{
		std::ranges::swap(format, _other.format);
		std::ranges::swap(extent, _other.extent);
		std::ranges::swap(layout, _other.layout);
		std::ranges::swap(clear_value, _other.clear_value);
		std::ranges::swap(image, _other.image);
		std::ranges::swap(imageview, _other.imageview);
	}
	return *this;
}

void vulkan_image::create(const vma::raii::Allocator& _allocator, const vk::raii::Device& _device, const vk::ImageCreateInfo& _image_info, vk::ImageViewCreateInfo& _imageview_info, vk::MemoryPropertyFlags _properties, const vk::ClearValue& _clear_value)
{
	if (_image_info.format != _imageview_info.format || _image_info.arrayLayers != _imageview_info.subresourceRange.layerCount)
	{
		throw std::runtime_error("Please check if image info and image view info match!");
	}

	format = _image_info.format;
	extent = vk::Extent2D(_image_info.extent.width, _image_info.extent.height);
	layout = vk::ImageLayout::eUndefined;
	clear_value = _clear_value;

	vma::AllocationCreateInfo create_info{};
	if (_properties & vk::MemoryPropertyFlagBits::eHostVisible)
	{
		create_info.setFlags(vma::AllocationCreateFlagBits::eHostAccessSequentialWrite | vma::AllocationCreateFlagBits::eMapped)
			.setUsage(vma::MemoryUsage::eAutoPreferHost);
	}
	else if (_properties & vk::MemoryPropertyFlagBits::eDeviceLocal)
	{
		create_info.setFlags(vma::AllocationCreateFlagBits::eDedicatedMemory /*| vma::AllocationCreateFlagBits::eMapped*/)
			.setUsage(vma::MemoryUsage::eGpuOnly);
	}
	else
	{
		create_info.setUsage(vma::MemoryUsage::eAuto);
	}

	image = _allocator.createImage(_image_info, create_info);

	_imageview_info.image = image;
	imageview = vk::raii::ImageView(_device, _imageview_info, _allocator.getAllocationCallbacks());
}

vk::ImageMemoryBarrier2 vulkan_image::set_layout(vk::ImageLayout _new_layout, const vk::ImageSubresourceRange& _resource_range) noexcept
{
	auto old_layout = layout;
	layout = _new_layout;
	return transition_image_layout(image, old_layout, layout, _resource_range);
}

vk::Format vulkan_image::get_format() const noexcept
{
	return format;
}

vk::Extent2D vulkan_image::get_extent() const noexcept
{
	return extent;
}

vk::ImageLayout vulkan_image::get_layout() const noexcept
{
	return layout;
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

vk::ImageMemoryBarrier2 vulkan_image::transition_image_layout(const vk::Image& _image, vk::ImageLayout _old_layout, vk::ImageLayout _new_layout, const vk::ImageSubresourceRange& _resource_range) noexcept
{
	return vk::ImageMemoryBarrier2(get_pipeline_stage_for_layout(_old_layout), get_access_flags_from_image_layout(_old_layout), get_pipeline_stage_for_layout(_new_layout), get_access_flags_from_image_layout(_new_layout), _old_layout, _new_layout, vk::QueueFamilyIgnored, vk::QueueFamilyIgnored, _image, _resource_range);
}

void vulkan_image::copy_image_to_buffer(const vk::raii::CommandBuffer& _commandbuffer, const vk::Image& _image, const vk::Buffer& _buffer, vk::ImageLayout _layout, const vk::BufferImageCopy2& _copy_info) noexcept
{
	_commandbuffer.copyImageToBuffer2(vk::CopyImageToBufferInfo2(_image, _layout, _buffer, _copy_info));
}

void vulkan_image::copy_image_to_image(const vk::raii::CommandBuffer& _commandbuffer, const vk::Image& _image_src, const vk::Image& _image_dst, vk::ImageLayout _src_layout, vk::ImageLayout _dst_layout, const vk::ImageCopy2& _copy_info) noexcept
{
	_commandbuffer.copyImage2(vk::CopyImageInfo2(_image_src, _src_layout, _image_dst, _dst_layout, _copy_info));
}

constexpr vk::AccessFlags2 vulkan_image::get_access_flags_from_image_layout(vk::ImageLayout _layout) noexcept
{
	switch (_layout)
	{
	case vk::ImageLayout::ePreinitialized:
		return vk::AccessFlagBits2::eHostWrite;
	case vk::ImageLayout::eTransferDstOptimal:
		return vk::AccessFlagBits2::eTransferWrite;
	case vk::ImageLayout::eTransferSrcOptimal:
		return vk::AccessFlagBits2::eTransferRead;
	case vk::ImageLayout::eColorAttachmentOptimal:
		return vk::AccessFlagBits2::eColorAttachmentWrite;
	case vk::ImageLayout::eDepthStencilAttachmentOptimal:
		return vk::AccessFlagBits2::eDepthStencilAttachmentWrite;
	case vk::ImageLayout::eShaderReadOnlyOptimal:
		return vk::AccessFlagBits2::eShaderRead;
	default:
		return vk::AccessFlags2();
	}
}

constexpr vk::PipelineStageFlags2 vulkan_image::get_pipeline_stage_for_layout(vk::ImageLayout _layout) noexcept
{
	switch (_layout)
	{
	case vk::ImageLayout::eTransferDstOptimal:
	case vk::ImageLayout::eTransferSrcOptimal:
		return vk::PipelineStageFlagBits2::eTransfer;
	case vk::ImageLayout::eColorAttachmentOptimal:
		return vk::PipelineStageFlagBits2::eColorAttachmentOutput;
	case vk::ImageLayout::eDepthStencilAttachmentOptimal:
		return vk::PipelineStageFlagBits2::eAllCommands; // We do this to allow queue other than graphic
		// return vk::PipelineStageFlagBits2::eEarlyFragmentTests;
	case vk::ImageLayout::eShaderReadOnlyOptimal:
		return vk::PipelineStageFlagBits2::eAllCommands; // We do this to allow queue other than graphic
		// return vk::PipelineStageFlagBits2::eFragmentShader;
	case vk::ImageLayout::ePreinitialized:
		return vk::PipelineStageFlagBits2::eHost;
	case vk::ImageLayout::eUndefined:
		return vk::PipelineStageFlagBits2::eTopOfPipe;
	default:
		return vk::PipelineStageFlagBits2::eBottomOfPipe;
	}
}