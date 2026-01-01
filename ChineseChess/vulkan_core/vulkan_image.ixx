export module vulkan_image;

import <cstdint>;
import std;
import vulkan_hpp;

export class vulkan_image
{
public:
	vulkan_image() = default;
	~vulkan_image() = default;
	vulkan_image(vulkan_image&) = delete;
	vulkan_image(vulkan_image&& _other) noexcept;
	vulkan_image& operator=(vulkan_image&) = delete;
	vulkan_image& operator=(vulkan_image&& _other) noexcept;

	void create(const vk::raii::PhysicalDevice& _physical_device, const vk::raii::Device& _device, vk::ImageCreateInfo& _image_info, vk::ImageViewCreateInfo& _imageview_info, vk::MemoryPropertyFlags _properties, const vk::ClearValue& _clear_values);
	vk::ImageMemoryBarrier2 transition_to_layout(vk::ImageLayout _new_layout, const vk::ImageSubresourceRange& _resource_range);

	vk::Format get_format() const;
	vk::Extent2D get_extent() const;
	vk::ImageLayout get_layout() const;
	vk::ClearValue get_clear_value() const;
	const vk::raii::Image& get_image() const;
	const vk::raii::ImageView& get_imageview() const;

	static vk::AccessFlags2 get_access_flags_from_image_layout(vk::ImageLayout _layout);
	static vk::PipelineStageFlags2 get_pipeline_stage_for_layout(vk::ImageLayout _layout);
	[[nodiscard("transition barrier need submit!")]] static vk::ImageMemoryBarrier2 transition_image_layout(const vk::Image& _image, vk::ImageLayout _old_layout, vk::ImageLayout _new_layout, const vk::ImageSubresourceRange& _resource_range);

private:
	vk::Format format = vk::Format::eUndefined;
	vk::Extent2D extent = vk::Extent2D{ 0, 0 };
	vk::ImageLayout layout = vk::ImageLayout::eUndefined;
	vk::ClearValue clear_value = vk::ClearColorValue(0.f, 0.f, 0.f, 1.f);

	vk::raii::Image image = nullptr;
	vk::raii::ImageView imageview = nullptr;
	vk::raii::DeviceMemory image_memory = nullptr;
};