#pragma once

#include <vulkan-memory-allocator-hpp/vk_mem_alloc_raii.hpp>

class vulkan_image
{
public:
	vulkan_image() = default;
	~vulkan_image() = default;
	vulkan_image(vulkan_image&) = delete;
	vulkan_image(vulkan_image&& _other) noexcept;
	vulkan_image& operator=(vulkan_image&) = delete;
	vulkan_image& operator=(vulkan_image&& _other) noexcept;

	void create(const vma::raii::Allocator& _allocator, const vk::raii::Device& _device, const vk::ImageCreateInfo& _image_info, vk::ImageViewCreateInfo& _imageview_info, vk::MemoryPropertyFlags _properties, const vk::ClearValue& _clear_value);

	[[nodiscard("transition barrier need submit!")]]
	vk::ImageMemoryBarrier2 set_layout(vk::ImageLayout _new_layout, const vk::ImageSubresourceRange& _resource_range) noexcept;

	vk::Format get_format() const noexcept;
	vk::Extent2D get_extent() const noexcept;
	vk::ImageLayout get_layout() const noexcept;
	vk::ClearValue get_clear_value() const noexcept;
	const vk::raii::Image& get_image() const noexcept;
	const vk::raii::ImageView& get_imageview() const noexcept;

	[[nodiscard("transition barrier need submit!")]]
	static vk::ImageMemoryBarrier2 transition_image_layout(const vk::Image& _image, vk::ImageLayout _old_layout, vk::ImageLayout _new_layout, const vk::ImageSubresourceRange& _resource_range) noexcept;

	static void copy_image_to_buffer(const vk::raii::CommandBuffer& _commandbuffer, const vk::Image& _image, const vk::Buffer& _buffer, vk::ImageLayout _layout, const vk::BufferImageCopy2& _copy_info) noexcept;
	static void copy_image_to_image(const vk::raii::CommandBuffer& _commandbuffer, const vk::Image& _image_src, const vk::Image& _image_dst, vk::ImageLayout _src_layout, vk::ImageLayout _dst_layout, const vk::ImageCopy2& _copy_info) noexcept;

private:
	static vk::AccessFlags2 get_access_flags_from_image_layout(vk::ImageLayout _layout) noexcept;
	static vk::PipelineStageFlags2 get_pipeline_stage_for_layout(vk::ImageLayout _layout) noexcept;

	vk::Format format = vk::Format::eUndefined;
	vk::Extent2D extent = vk::Extent2D{ 0, 0 };
	vk::ImageLayout layout = vk::ImageLayout::eUndefined;
	vk::ClearValue clear_value = vk::ClearColorValue(0.f, 0.f, 0.f, 0.f);

	vma::raii::Image image = nullptr;
	vk::raii::ImageView imageview = nullptr;
};