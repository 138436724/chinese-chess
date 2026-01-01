export module vulkan_common;

import <cstdint>;
import std;
import vulkan_hpp;

export class vulkan_common
{
private:
	vulkan_common() = delete;
	~vulkan_common() = delete;
	vulkan_common(const vulkan_common&) = delete;
	vulkan_common& operator=(const vulkan_common&) = delete;
	vulkan_common(const vulkan_common&&) = delete;
	vulkan_common& operator=(const vulkan_common&&) = delete;

public:
	inline static vk::SampleCountFlagBits MASS_SAMPLE_COUNT = vk::SampleCountFlagBits::e1;
	inline static vk::Format DEPTH_FORMAT = vk::Format::eUndefined;
	inline static const uint32_t MAX_FRAMES_IN_FLIGHT = 2;

	static uint32_t find_memory_type(const vk::raii::PhysicalDevice& _physical_device, uint32_t _type_filter, vk::MemoryPropertyFlags _properties)
	{
		auto memory_properties = _physical_device.getMemoryProperties();
		for (uint32_t i = 0; i < memory_properties.memoryTypeCount; i++)
		{
			if ((_type_filter & (1 << i)) && (memory_properties.memoryTypes[i].propertyFlags & _properties) == _properties)
			{
				return i;
			}
		}
		throw std::runtime_error("failed to find suitable memory type!");
	}

	static vk::Format find_supported_format(const vk::raii::PhysicalDevice& _physical_device, const std::vector<vk::Format>& _candidates, vk::ImageTiling _tiling, vk::FormatFeatureFlags _features)
	{
		auto format_iter = std::ranges::find_if(_candidates, [&](auto const format)
			{
				vk::FormatProperties props = _physical_device.getFormatProperties(format);
				return (_tiling == vk::ImageTiling::eLinear && (props.linearTilingFeatures & _features) == _features) ||
					(_tiling == vk::ImageTiling::eOptimal && (props.optimalTilingFeatures & _features) == _features); });

		if (format_iter == _candidates.end())
		{
			throw std::runtime_error("failed to find supported format!");
		}

		return *format_iter;
	}
};