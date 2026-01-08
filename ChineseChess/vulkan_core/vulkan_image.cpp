module vulkan_image;

import vulkan_common;

vulkan_image::vulkan_image(vulkan_image&& _other) noexcept
	: format(std::move(_other.format)),
	extent(std::move(_other.extent)),
	layout(std::move(_other.layout)),
	clear_value(std::move(_other.clear_value)),
	image(std::move(_other.image)),
	imageview(std::move(_other.imageview)),
	image_memory(std::move(_other.image_memory))
{
}

vulkan_image& vulkan_image::operator=(vulkan_image&& _other) noexcept
{
	if (this != &_other)
	{
		std::swap(format, _other.format);
		std::swap(extent, _other.extent);
		std::swap(layout, _other.layout);
		std::swap(clear_value, _other.clear_value);
		std::swap(image, _other.image);
		std::swap(imageview, _other.imageview);
		std::swap(image_memory, _other.image_memory);
	}
	return *this;
}

void vulkan_image::create(const vk::raii::PhysicalDevice& _physical_device, const vk::raii::Device& _device, vk::ImageCreateInfo& _image_info, vk::ImageViewCreateInfo& _imageview_info, vk::MemoryPropertyFlags _properties, const vk::ClearValue& _clear_value)
{
	format = _image_info.format;
	extent = vk::Extent2D(_image_info.extent.width, _image_info.extent.height);
	layout = vk::ImageLayout::eUndefined;
	clear_value = _clear_value;

	image = vk::raii::Image(_device, _image_info);

	vk::MemoryRequirements memory_requirements = image.getMemoryRequirements();
	vk::MemoryAllocateInfo memory_info(memory_requirements.size, vulkan_common::find_memory_type(_physical_device, memory_requirements.memoryTypeBits, _properties));
	image_memory = vk::raii::DeviceMemory(_device, memory_info);
	image.bindMemory(image_memory, 0);

	_imageview_info.image = image;
	imageview = vk::raii::ImageView(_device, _imageview_info);
}

vk::ImageMemoryBarrier2 vulkan_image::transition_to_layout(vk::ImageLayout _new_layout, const vk::ImageSubresourceRange& _resource_range)
{
	auto barrier = transition_image_layout(image, layout, _new_layout, _resource_range);
	layout = _new_layout;
	return barrier;
}

vk::Format vulkan_image::get_format() const
{
	return format;
}

vk::Extent2D vulkan_image::get_extent() const
{
	return extent;
}

vk::ImageLayout vulkan_image::get_layout() const
{
	return layout;
}

vk::ClearValue vulkan_image::get_clear_value() const
{
	return clear_value;
}

const vk::raii::Image& vulkan_image::get_image() const
{
	return image;
}

const vk::raii::ImageView& vulkan_image::get_imageview() const
{
	return imageview;
}

vk::AccessFlags2 vulkan_image::get_access_flags_from_image_layout(vk::ImageLayout _layout)
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

vk::PipelineStageFlags2 vulkan_image::get_pipeline_stage_for_layout(vk::ImageLayout _layout)
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

vk::ImageMemoryBarrier2 vulkan_image::transition_image_layout(const vk::Image& _image, vk::ImageLayout _old_layout, vk::ImageLayout _new_layout, const vk::ImageSubresourceRange& _resource_range)
{
	return vk::ImageMemoryBarrier2(get_pipeline_stage_for_layout(_old_layout), get_access_flags_from_image_layout(_old_layout), get_pipeline_stage_for_layout(_new_layout), get_access_flags_from_image_layout(_new_layout), _old_layout, _new_layout, vk::QueueFamilyIgnored, vk::QueueFamilyIgnored, _image, _resource_range);
}

void vulkan_image::copy_image_to_buffer(const vk::raii::CommandBuffer& _commandbuffer, const vk::Image& _image, const vk::Buffer& _buffer, const vk::ImageSubresourceLayers& _subresource_layers, const vk::Offset3D& _image_offset, const vk::Extent3D& _image_extent, vk::ImageLayout _layout)
{
	vk::BufferImageCopy2 copy_regions(0, _image_extent.width, _image_extent.height, _subresource_layers, _image_offset, _image_extent);
	_commandbuffer.copyImageToBuffer2(vk::CopyImageToBufferInfo2(_image, _layout, _buffer, copy_regions));
}
